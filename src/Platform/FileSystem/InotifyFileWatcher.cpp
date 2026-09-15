#include "Platform/FileSystem/InotifyFileWatcher.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <stdexcept>

namespace AsynGyanis::Platform
{
    InotifyFileWatcher::InotifyFileWatcher()
    {
        m_inotifyFileDescriptor = ::inotify_init1(IN_CLOEXEC);
        if (m_inotifyFileDescriptor < 0)
        {
            throw std::runtime_error("inotify 初始化失败");
        }
    }

    InotifyFileWatcher::~InotifyFileWatcher()
    {
        stop();
        FileDescriptor::close(m_inotifyFileDescriptor);
    }

    bool InotifyFileWatcher::start()
    {
        if (m_isRunning.load(std::memory_order_acquire))
        {
            return true;
        }

        if (!FileDescriptor::isValid(m_inotifyFileDescriptor))
        {
            return false;
        }

        try
        {
            m_watchThread = std::jthread([this](const std::stop_token stopToken) { watchLoop(stopToken); });
        } catch (const std::system_error &)
        {
            return false;
        }

        m_isRunning.store(true, std::memory_order_release);
        return true;
    }

    void InotifyFileWatcher::stop()
    {
        if (!m_isRunning.load(std::memory_order_acquire))
        {
            return;
        }

        if (m_watchThread.joinable())
        {
            m_watchThread.request_stop();
            m_watchThread.join();
        }
        m_isRunning.store(false, std::memory_order_release);
    }

    bool InotifyFileWatcher::addWatch(const std::string_view path, const bool recursive)
    {
        std::error_code error;
        const auto      absolutePath = std::filesystem::absolute(path, error).string();
        if (error)
        {
            return false;
        }

        {
            std::lock_guard lock(m_watchMutex);

            // 递归根要记住：之后新建的子目录靠这份清单补挂监视（否则新目录里的变更永久丢失）
            if (recursive)
            {
                m_recursiveRoots.insert(absolutePath);
            }

            if (m_pathToWatchDescriptor.contains(absolutePath))
            {
                return true;
            }

            const int watchDescriptor = ::inotify_add_watch(m_inotifyFileDescriptor, absolutePath.c_str(), kWatchEventMask);
            if (watchDescriptor < 0)
            {
                return false;
            }

            m_watchDescriptors[watchDescriptor]   = absolutePath;
            m_pathToWatchDescriptor[absolutePath] = watchDescriptor;
        }

        // 递归注册放在锁外，避免持锁期间遍历目录树
        if (recursive && std::filesystem::is_directory(absolutePath, error))
        {
            for (const auto &entry: std::filesystem::recursive_directory_iterator(absolutePath, error))
            {
                if (error)
                {
                    break;
                }
                if (entry.is_directory())
                {
                    addWatch(entry.path().string(), false);
                }
            }
        }

        return true;
    }

    bool InotifyFileWatcher::removeWatch(const std::string_view path)
    {
        std::error_code error;
        const auto      absolutePath = std::filesystem::absolute(path, error).string();
        if (error)
        {
            return false;
        }

        std::lock_guard lock(m_watchMutex);

        const auto iterator = m_pathToWatchDescriptor.find(absolutePath);
        if (iterator == m_pathToWatchDescriptor.end())
        {
            return false;
        }

        // 原生解除失败（例如监视已被内核回收）时同样清理映射，防止悬挂描述符
        [[maybe_unused]] const int removeResult = ::inotify_rm_watch(m_inotifyFileDescriptor, iterator->second);

        m_watchDescriptors.erase(iterator->second);
        m_pathToWatchDescriptor.erase(iterator);

        return true;
    }

    void InotifyFileWatcher::setCallback(FileChangeCallback callback)
    {
        std::lock_guard lock(m_watchMutex);
        m_callback = std::move(callback);
    }

    bool InotifyFileWatcher::isRunning() const noexcept
    {
        return m_isRunning.load(std::memory_order_acquire);
    }

    void InotifyFileWatcher::watchLoop(const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            pollfd descriptor{};
            descriptor.fd     = m_inotifyFileDescriptor;
            descriptor.events = POLLIN;

            // 100ms 超时轮询，保证 stop() 请求后即使无事件也能及时退出
            const int pollResult = ::poll(&descriptor, 1, 100);

            if (pollResult < 0)
            {
                if (PlatformError::lastErrorCode() == PlatformError::kInterrupted)
                {
                    continue;
                }
                break;
            }

            if (pollResult == 0)
            {
                continue;
            }

            if ((descriptor.revents & POLLIN) != 0)
            {
                processEvents();
            }
        }
    }

    void InotifyFileWatcher::processEvents()
    {
        char buffer[kEventBufferBytes];

        const ssize_t bytesRead = FileDescriptor::read(m_inotifyFileDescriptor, buffer, sizeof(buffer));
        if (bytesRead < 0)
        {
            return;
        }

        for (ssize_t offset = 0; offset < bytesRead;)
        {
            const auto *event = reinterpret_cast<const inotify_event *>(&buffer[offset]);
            offset += sizeof(inotify_event) + event->len;

            // 队列溢出（wd == -1）：内核来不及投递的事件已经丢了，而且不知道丢的是哪些路径。
            // 对每个受监视的根各派发一次「已修改」让消费方重新扫描，绝不静默停在旧状态
            if (event->wd == -1)
            {
                dispatchOverflowRescan();
                continue;
            }

            // 锁内取出回调快照与目标路径，锁外触发，避免回调中操作监听器造成死锁
            FileChangeCallback callbackSnapshot;
            std::string        changedPath;
            FileChangeType     changeType = FileChangeType::Modified;
            bool               shouldWatchNewDirectory = false;

            {
                std::shared_lock lock(m_watchMutex);

                const auto watchIterator = m_watchDescriptors.find(event->wd);
                if (watchIterator == m_watchDescriptors.end())
                {
                    continue;
                }

                const std::string &watchedPath = watchIterator->second;
                // 监视单个文件时内核不给名字（event->len 恒为 0），变更的就是被监视的那个文件本身；
                // 监视目录时 event->name 才是目录内的条目名
                const std::size_t nameLength = event->len == 0 ? 0 : std::strlen(event->name);

                if (nameLength == 0)
                {
                    changedPath = watchedPath;
                } else
                {
                    const bool        needsSeparator = !watchedPath.empty() && watchedPath.back() != '/';
                    const std::size_t nameOffset     = watchedPath.size() + (needsSeparator ? 1U : 0U);

                    // 一次预留到位，避免「赋值 + 两次追加」引发的逐步扩容
                    changedPath.reserve(nameOffset + nameLength);
                    changedPath.append(watchedPath);
                    if (needsSeparator)
                    {
                        changedPath.push_back('/');
                    }
                    changedPath.append(event->name, nameLength);
                }

                if (!shouldDispatchChange(changedPath))
                {
                    continue;
                }

                callbackSnapshot = m_callback;

                if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0)
                {
                    changeType = FileChangeType::Created;
                } else if ((event->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
                {
                    changeType = FileChangeType::Deleted;
                }

                // 递归根之下新出现的目录要在锁外补挂监视：新子目录里的变更否则永远不会上报
                shouldWatchNewDirectory =
                        changeType == FileChangeType::Created &&
                        std::ranges::any_of(m_recursiveRoots,
                                            [&watchedPath](const std::string &root)
                                            {
                                                return watchedPath.size() >= root.size() && watchedPath.compare(0, root.size(), root) == 0;
                                            });
            }

            if (shouldWatchNewDirectory)
            {
                std::error_code directoryError;
                if (std::filesystem::is_directory(changedPath, directoryError) && !directoryError)
                {
                    // 锁外补挂：addWatch 要拿写锁，持锁递归注册会自死锁
                    static_cast<void>(addWatch(changedPath, true));
                }
            }

            if (callbackSnapshot)
            {
                callbackSnapshot(changedPath, changeType);
            }
        }
    }

    void InotifyFileWatcher::dispatchOverflowRescan()
    {
        // 快照照抄一份回调与路径再派发：回调里增删监听路径是常见写法，持锁派发会自死锁
        std::vector<std::pair<FileChangeCallback, std::string> > notifications;
        {
            std::shared_lock lock(m_watchMutex);
            notifications.reserve(m_watchDescriptors.size());
            for (const auto &entry: m_watchDescriptors)
            {
                notifications.emplace_back(m_callback, entry.second);
            }
        }

        for (const auto &[callback, watchedPath]: notifications)
        {
            if (callback)
            {
                callback(watchedPath, FileChangeType::Modified);
            }
        }
    }
} // namespace AsynGyanis::Platform
