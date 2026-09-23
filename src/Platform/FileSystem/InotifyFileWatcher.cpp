#include "Platform/FileSystem/InotifyFileWatcher.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <utility>
#include <vector>

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

        // 对外入口：调用方自己给的这一次注册要进自愈清单
        return registerWatch(absolutePath, recursive, true);
    }

    bool InotifyFileWatcher::registerWatch(const std::string &absolutePath, const bool recursive, const bool keepForSelfHeal)
    {
        std::error_code error;
        bool            registered   = true;
        bool            needsDescend = false;
        {
            std::lock_guard lock(m_watchMutex);

            // 自愈清单只记调用方（与框架自己的补挂）给的注册，不记递归枚举出来的子目录
            if (keepForSelfHeal)
            {
                m_selfHealPaths.insert(absolutePath);
            }

            // 递归根要记住：之后新建的子目录靠这份清单补挂监视（否则新目录里的变更永久丢失）
            if (recursive)
            {
                // 已经在清单里就不用再走一遍树：重复的递归注册，枚举出来的子目录都已各自挂好了
                needsDescend = !m_recursiveRoots.contains(absolutePath);
                m_recursiveRoots.insert(absolutePath);
            }

            // 「这条路径已经看过」不能直接返回：先按非递归注册、之后再要递归的那一次必须走到下面的
            // 枚举，否则那次升级会被静默吞掉——先就存在的子目录一个都挂不上，而自愈只复查清单上
            // 已有的路径，枚举不到的目录它根本不认识
            if (!m_pathToWatchDescriptor.contains(absolutePath))
            {
                // 此刻注册失败（路径还不存在）也保留清单里的那一条：自愈节拍会在它出现后补挂
                const int watchDescriptor = ::inotify_add_watch(m_inotifyFileDescriptor, absolutePath.c_str(), kWatchEventMask);
                if (watchDescriptor < 0)
                {
                    registered = false;
                } else
                {
                    m_watchDescriptors[watchDescriptor]   = absolutePath;
                    m_pathToWatchDescriptor[absolutePath] = watchDescriptor;
                }
            }
        }

        // 递归注册放在锁外，避免持锁期间遍历目录树
        if (needsDescend && registered && std::filesystem::is_directory(absolutePath, error))
        {
            for (const auto &entry: std::filesystem::recursive_directory_iterator(absolutePath, error))
            {
                if (error)
                {
                    break;
                }
                if (entry.is_directory())
                {
                    registerWatch(entry.path().string(), false, false);
                }
            }
        }

        return registered;
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
            // 「没有活监视」不等于「这条路径没被登记过」：注册失败（路径当时不存在）也保留清单里的那一条，
            // 目录之后出现时由自愈节拍补挂。显式撤销连这份意图一起清掉，否则调用方以为撤干净了，事件
            // 却在之后的某个时刻开始流过来。与 Windows 侧 dropWatch 同一口径
            const std::size_t revokedPendingEntries
                    = m_selfHealPaths.erase(absolutePath) + m_recursiveRoots.erase(absolutePath);
            return revokedPendingEntries > 0;
        }

        // 原生解除失败（例如监视已被内核回收）时同样清理映射，防止悬挂描述符
        [[maybe_unused]] const int removeResult = ::inotify_rm_watch(m_inotifyFileDescriptor, iterator->second);

        m_watchDescriptors.erase(iterator->second);
        m_pathToWatchDescriptor.erase(iterator);
        // 递归根清单要一起摘掉：留着它，下一拍的 rearmMissingWatchesIfDue 会把这条刚被撤销的
        // 监视重新挂回来（自愈的用途是「目录被删掉后又回来时补挂」，不是替调用方否决一次显式
        // 撤销）。那样 removeWatch() 虽然返回了 true，描述符却会重开、回调照旧派发
        m_recursiveRoots.erase(absolutePath);
        m_selfHealPaths.erase(absolutePath);

        // 子树的每一条监视也要一起解除。递归注册给每个子目录都挂了一只 inotify watch，只摘
        // 被点名的那一条，剩下的照旧派发回调（调用方以为撤销完成了），而且每只 watch 都钉住
        // 一枚 inode：整棵树删不掉、卷卸不掉，反复 add/remove 还会把 max_user_watches 耗尽，
        // 到那之后本实例的 addWatch() 会全部失败。与 Windows 侧 dropWatch 同一口径
        const std::string subtreePrefix = absolutePath.ends_with('/') ? absolutePath : absolutePath + '/';
        std::vector<std::pair<int, std::string>> coveredWatches;
        for (const auto &[watchDescriptor, watchedPath] : m_watchDescriptors)
        {
            if (watchedPath.size() > subtreePrefix.size()
                && watchedPath.compare(0, subtreePrefix.size(), subtreePrefix) == 0)
            {
                coveredWatches.emplace_back(watchDescriptor, watchedPath);
            }
        }
        for (const auto &[watchDescriptor, watchedPath] : coveredWatches)
        {
            [[maybe_unused]] const int removedDescriptor =
                    ::inotify_rm_watch(m_inotifyFileDescriptor, watchDescriptor);
            m_watchDescriptors.erase(watchDescriptor);
            m_pathToWatchDescriptor.erase(watchedPath);
            m_recursiveRoots.erase(watchedPath);
            m_selfHealPaths.erase(watchedPath);
        }

        return true;
    }

    void InotifyFileWatcher::removeWatchMapping(const int watchDescriptor)
    {
        // 内核摘除 watch 之后它不会再投递任何事件，两张表留着只会挡住同一路径的重新注册
        std::lock_guard lock(m_watchMutex);
        const auto      iterator = m_watchDescriptors.find(watchDescriptor);
        if (iterator == m_watchDescriptors.end())
        {
            return;
        }
        m_pathToWatchDescriptor.erase(iterator->second);
        m_watchDescriptors.erase(iterator);
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
                // 没有事件也走一遍自愈复查（下面在循环末尾统一做），这里只是别提前 continue 掉
                rearmMissingWatchesIfDue();
                continue;
            }

            if ((descriptor.revents & POLLIN) != 0)
            {
                processEvents();
            }
            rearmMissingWatchesIfDue();
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

            // IN_IGNORED：watch 被移除后内核必补的一条。它的掩码不参与本端的事件分类，
            // 却会落到下面「len == 0 → 监视目标本身」的分支上被当成一次「已修改」派发出去——
            // 删除之后再来一条假修改，会诱导热加载去重扫一个已经不存在的路径。
            // 映射必须跟着摘掉：内核已经不再监视这个 wd（目录被删、文件被替换都是这条路径），
            // 留着会让 addWatch(同一路径) 被「已经看过这个路径」挡下——重建出来的同名目录/文件
            // 从此再也挂不上监视，事件永久丢失（这里尚未取共享锁，可以安全地做清理）
            if ((event->mask & IN_IGNORED) != 0)
            {
                removeWatchMapping(event->wd);
                continue;
            }

            // IN_MOVE_SELF：被监视的目录**本身**被改名或移到别处（len==0 才代表目标自身，带名字的
            // 是子项改名）。与 IN_IGNORED 不同，内核到这里仍然持有这个 wd、不会补 IN_IGNORED，
            // 于是映射一直留着：原地重建同名目录后再 addWatch，会被「已经看过这个路径」挡下而不
            // 重新注册，新目录的事件从此永久丢失。这里按同样的方式摘掉映射——递归根随后由自愈
            // 复查补挂，非递归的由调用方下一次 addWatch 补挂
            if ((event->mask & IN_MOVE_SELF) != 0 && event->len == 0)
            {
                removeWatchMapping(event->wd);
                continue;
            }

            // 队列溢出（wd == -1）：内核来不及投递的事件已经丢了，而且不知道丢的是哪些路径。
            // 对每个受监视的目录各派发一次「要重扫了」，让消费方重新扫描，绝不静默停在旧状态
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

                // 递归根之下新出现的目录要在锁外补挂监视：新子目录里的变更否则永远不会上报。
                // 前缀比较必须落在路径分隔符边界上——纯前缀匹配会把「/data」当成「/database」的根，
                // 给监视范围外的目录补挂监视（并把它们记进递归根集合，范围越滚越大）
                shouldWatchNewDirectory =
                        changeType == FileChangeType::Created &&
                        std::ranges::any_of(m_recursiveRoots,
                                            [&watchedPath](const std::string &root)
                                            {
                                                if (watchedPath.size() < root.size() || watchedPath.compare(0, root.size(), root) != 0)
                                                {
                                                    return false;
                                                }
                                                // 完全相同，或下一个字符就是分隔符，才算「在根之下」
                                                return watchedPath.size() == root.size() ||
                                                       watchedPath[root.size()] == '/' ||
                                                       (!root.empty() && root.back() == '/');
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

    void InotifyFileWatcher::rearmMissingWatchesIfDue()
    {
        // 清单里的路径一旦失去内核监视就得补挂（监视的文件被替换或删除、被监视的目录被移走、注册时
        // 路径还不存在）：除了这条节拍，没有别的人会再为同一个路径调 addWatch。按秒复查一遍
        const auto now = std::chrono::steady_clock::now();
        if (now < m_rearmDeadline)
        {
            return;
        }
        m_rearmDeadline = now + kRearmInterval;

        std::vector<std::pair<std::string, bool> > missingWatches;
        {
            const std::shared_lock lock(m_watchMutex);
            for (const std::string &path: m_selfHealPaths)
            {
                if (!m_pathToWatchDescriptor.contains(path))
                {
                    // 补挂要照当初的 recursive 取值：给一条只要一层的路径按递归挂上，监视范围会越滚越大
                    missingWatches.emplace_back(path, m_recursiveRoots.contains(path));
                }
            }
        }

        // 锁外补挂：registerWatch() 要拿写锁；路径还没回来时它会失败，下一拍再试
        for (const auto &[path, recursive]: missingWatches)
        {
            static_cast<void>(registerWatch(path, recursive, true));
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
                callback(watchedPath, FileChangeType::NeedsRescan);
            }
        }
    }
} // namespace AsynGyanis::Platform
