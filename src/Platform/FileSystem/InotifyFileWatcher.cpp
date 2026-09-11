/**
 * @file InotifyFileWatcher.cpp
 * @brief Linux 平台文件监听器，基于内核 inotify 机制
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/InotifyFileWatcher.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

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

            if (event->len == 0)
            {
                continue;
            }

            // 锁内取出回调快照与目标路径，锁外触发，避免回调中操作监听器造成死锁
            FileChangeCallback callbackSnapshot;
            std::string        changedPath;
            FileChangeType     changeType = FileChangeType::Modified;

            {
                std::shared_lock lock(m_watchMutex);

                const auto watchIterator = m_watchDescriptors.find(event->wd);
                if (watchIterator == m_watchDescriptors.end())
                {
                    continue;
                }

                const std::string &watchedPath = watchIterator->second;
                const bool needsSeparator = !watchedPath.empty() && watchedPath.back() != '/';
                // event->name 是柔性数组，event->len 含结尾 NUL 与对齐填充，故按实际字符串长度取
                const std::size_t nameLength = std::strlen(event->name);

                // 一次预留到位，避免「赋值 + 两次追加」引发的逐步扩容
                changedPath.reserve(watchedPath.size() + (needsSeparator ? 1U : 0U) + nameLength);
                changedPath.append(watchedPath);
                if (needsSeparator)
                {
                    changedPath.push_back('/');
                }
                changedPath.append(event->name, nameLength);

                if (!shouldDispatchChange(changedPath))
                {
                    continue;
                }

                callbackSnapshot = m_callback;

                if ((event->mask & IN_CLOSE_WRITE) != 0)
                {
                    changeType = FileChangeType::Modified;
                } else if ((event->mask & IN_MOVED_TO) != 0)
                {
                    changeType = FileChangeType::Created;
                } else if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
                {
                    changeType = FileChangeType::Deleted;
                }
            }

            if (callbackSnapshot)
            {
                callbackSnapshot(changedPath, changeType);
            }
        }
    }
} // namespace AsynGyanis::Platform
