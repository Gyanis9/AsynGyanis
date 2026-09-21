#include "Platform/FileSystem/Win32FileWatcher.h"

#include "Platform/System/TextEncoding.h"

#include <filesystem>
#include <ranges>
#include <system_error>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 把 Windows 目录通知动作映射为平台无关的变更类型
         * @param action FILE_ACTION_* 常量
         * @return FileChangeType 映射后的变更类型
         */
        FileChangeType changeTypeFromAction(const DWORD action)
        {
            switch (action)
            {
                case FILE_ACTION_ADDED:
                    return FileChangeType::Created;
                case FILE_ACTION_REMOVED:
                    return FileChangeType::Deleted;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    return FileChangeType::Moved;
                case FILE_ACTION_MODIFIED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                default:
                    return FileChangeType::Modified;
            }
        }
    } // namespace

    Win32FileWatcher::Win32FileWatcher()
    {
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Win32FileWatcher::~Win32FileWatcher()
    {
        Win32FileWatcher::stop();

        if (m_stopEvent != nullptr)
        {
            ::CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
    }

    bool Win32FileWatcher::start()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            return true;
        }

        if (m_stopEvent == nullptr)
        {
            return false;
        }

        m_shouldStop.store(false, std::memory_order_release);
        ::ResetEvent(m_stopEvent);

        try
        {
            // jthread 直接作为成员启动，无需堆分配；循环靠 m_shouldStop 与停止事件退出
            m_watchThread = std::jthread([this]
            {
                watchLoop();
            });
        } catch (const std::system_error &)
        {
            return false;
        }

        m_running.store(true, std::memory_order_release);
        return true;
    }

    void Win32FileWatcher::stop()
    {
        m_shouldStop.store(true, std::memory_order_release);
        if (m_stopEvent != nullptr)
        {
            ::SetEvent(m_stopEvent);
        }

        if (m_watchThread.joinable())
        {
            // 赋默认构造的 jthread 会先 request_stop 再 join，阻塞式循环已因停止事件自行退出
            m_watchThread = std::jthread{};
        }

        {
            std::lock_guard lock(m_watchMutex);
            for (auto &entry: m_watches | std::views::values)
            {
                closeEntry(*entry);
            }
            m_watches.clear();
        }

        m_running.store(false, std::memory_order_release);
    }

    bool Win32FileWatcher::addWatch(const std::string_view path, const bool recursive)
    {
        std::error_code errorCode;
        const auto      absolutePath = std::filesystem::absolute(path, errorCode).string();
        if (errorCode)
        {
            return false;
        }

        {
            const std::string directoryPath = normalizeDirectoryPath(absolutePath);
            std::lock_guard   lock(m_watchMutex);

            // 递归根要记住：之后新建的子目录靠这份清单补挂监听（否则新目录里的变更永久丢失）
            if (recursive)
            {
                m_recursiveRoots.insert(directoryPath);
            }

            if (m_watches.contains(directoryPath))
            {
                return true;
            }

            auto entry  = std::make_unique<WatchEntry>();
            entry->path = directoryPath;
            entry->buffer.resize(kBufferSize);
            entry->directoryHandle = ::CreateFileW(TextEncoding::toWideString(absolutePath).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

            if (entry->directoryHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            entry->eventHandle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (entry->eventHandle == nullptr)
            {
                ::CloseHandle(entry->directoryHandle);
                entry->directoryHandle = INVALID_HANDLE_VALUE;
                return false;
            }

            entry->overlapped.hEvent = entry->eventHandle;

            // 首次投递读不出变更，这条监视就没有任何成立的形式：条目既不在等待集合里，也没有
            // 人会再给它投递一次，登记下来只会占住这个路径——之后同一目录再 addWatch 会被上面的
            // 去重分支挡下并返回 true，于是「注册成功」而事件永久收不到。当场失败返回，让调用方
            // 看得见这条监视没成立（最常见的触发形状是路径指向普通文件：CreateFileW 带着
            // FILE_FLAG_BACKUP_SEMANTICS 会成功，拒的是后面的 ReadDirectoryChangesW）
            if (!issueRead(*entry))
            {
                closeEntry(*entry);
                return false;
            }

            m_watches[directoryPath] = std::move(entry);
        }

        // 递归注册放在锁外，避免持锁期间遍历目录树
        if (recursive && std::filesystem::is_directory(absolutePath, errorCode))
        {
            for (const auto &directoryEntry: std::filesystem::recursive_directory_iterator(absolutePath, errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                if (directoryEntry.is_directory())
                {
                    addWatch(directoryEntry.path().string(), false);
                }
            }
        }

        return true;
    }

    bool Win32FileWatcher::removeWatch(const std::string_view path)
    {
        std::error_code errorCode;
        const auto      absolutePath = std::filesystem::absolute(path, errorCode).string();
        if (errorCode)
        {
            return false;
        }

        // 调用方撤销的是「这条监视」本身，递归根清单要一起摘掉，只留监视表的删除是不完整的
        return dropWatch(absolutePath, false);
    }

    bool Win32FileWatcher::dropWatch(const std::string_view path, const bool keepRecursiveRoot)
    {
        std::lock_guard lock(m_watchMutex);

        const std::string            normalizedPath = normalizeDirectoryPath(std::string(path));
        const auto                   iterator       = m_watches.find(normalizedPath);
        if (iterator == m_watches.end())
        {
            return false;
        }

        closeEntry(*iterator->second);
        m_watches.erase(iterator);
        // 内部清理（目录被删导致的死条目）刻意留着这份清单：自愈复查正是靠它把换掉重建的根重新挂上。
        // 而调用方显式撤销时必须摘掉——否则下一拍自愈会把刚撤销的监视悄悄加回来，removeWatch()
        // 虽然返回了 true，句柄却重开、回调照旧派发
        if (!keepRecursiveRoot)
        {
            m_recursiveRoots.erase(normalizedPath);
        }
        return true;
    }

    void Win32FileWatcher::setCallback(FileChangeCallback callback)
    {
        std::lock_guard lock(m_watchMutex);
        m_callback = std::move(callback);
    }

    bool Win32FileWatcher::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void Win32FileWatcher::watchLoop()
    {
        // 提到循环外并预留到系统上限：clear() 不回收容量，之后每轮收集不再产生堆分配。
        // 两组容器分工：all* 是本轮全部待等待的条目，eventHandles/pendingPaths 是真正交给
        // WaitForMultipleObjects 的那一批（最多 63 个目录 + 停止事件）
        std::vector<HANDLE>      allEventHandles;
        std::vector<std::string> allPendingPaths;
        std::vector<HANDLE>      eventHandles;
        std::vector<std::string> pendingPaths;
        eventHandles.reserve(MAXIMUM_WAIT_OBJECTS);
        pendingPaths.reserve(MAXIMUM_WAIT_OBJECTS - 1);

        // 单次最多等 63 个目录，超出的那部分靠**轮转**排下一轮：只截前 63 个的话，顺序一旦稳定
        // （m_watches 是有序表），尾部目录永远进不了等待集，它们的事件会永久收不到
        std::size_t rotationOffset = 0;

        while (!m_shouldStop.load(std::memory_order_acquire))
        {
            // 递归根自愈：按秒节拍复查一遍递归根都还在不在监听集合里。部署工具把目录整个换掉
            // （删掉再重建）是常见做法，而重建后的目录没有人会再调 addWatch——根上的监听一旦
            // 失效，它内部的变更就永久丢失。放在收集等待集之前：根刚被换掉时条目已被摘除、
            // 等待集为空，那条空转分支（sleep 后 continue）走不到本函数
            if (std::chrono::steady_clock::now() >= rootRecheckDeadline)
            {
                rootRecheckDeadline = std::chrono::steady_clock::now() + kRootRecheckInterval;
                rewatchMissingRecursiveRoots();
            }

            // 收集所有待等待的事件句柄及其对应监听路径，停止事件固定占据索引 0
            allEventHandles.clear();
            allPendingPaths.clear();
            eventHandles.clear();
            pendingPaths.clear();

            eventHandles.push_back(m_stopEvent);

            {
                std::shared_lock lock(m_watchMutex);
                for (const auto &[path, entry]: m_watches)
                {
                    if (entry->pending)
                    {
                        allEventHandles.push_back(entry->eventHandle);
                        allPendingPaths.push_back(path);
                    }
                }
            }

            // WaitForMultipleObjects 单次最多等待 MAXIMUM_WAIT_OBJECTS 个对象：从轮转起点截一段，
            // 下一轮从截断处接着排，保证每个待等待目录迟早进入等待集
            constexpr std::size_t kMaximumWatchedDirectoryCount = MAXIMUM_WAIT_OBJECTS - 1; // 去掉停止事件
            if (allEventHandles.size() <= kMaximumWatchedDirectoryCount)
            {
                rotationOffset = 0;
                eventHandles.insert(eventHandles.end(), allEventHandles.begin(), allEventHandles.end());
                pendingPaths.insert(pendingPaths.end(), allPendingPaths.begin(), allPendingPaths.end());
            } else
            {
                if (rotationOffset >= allEventHandles.size())
                {
                    rotationOffset = 0;
                }
                for (std::size_t index = 0; index < kMaximumWatchedDirectoryCount; ++index)
                {
                    const std::size_t position = (rotationOffset + index) % allEventHandles.size();
                    eventHandles.push_back(allEventHandles[position]);
                    pendingPaths.push_back(allPendingPaths[position]);
                }
                rotationOffset = (rotationOffset + kMaximumWatchedDirectoryCount) % allEventHandles.size();
            }

            if (eventHandles.size() <= 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const DWORD waitResult = ::WaitForMultipleObjects(static_cast<DWORD>(eventHandles.size()), eventHandles.data(), FALSE, 100);

            if (waitResult == WAIT_FAILED || waitResult == WAIT_TIMEOUT)
            {
                continue;
            }

            const DWORD index = waitResult - WAIT_OBJECT_0;
            if (index == 0)
            {
                break; // 停止事件被置位
            }

            if (index - 1 >= pendingPaths.size())
            {
                continue;
            }

            const std::string                                    targetPath = pendingPaths[index - 1];
            std::vector<std::pair<std::string, FileChangeType> > pendingCallbacks;
            FileChangeCallback                                   callbackSnapshot;
            std::string                                          deadWatchPath; // 本轮读失败的目录：锁外摘掉
            {
                std::shared_lock lock(m_watchMutex);

                const auto iterator = m_watches.find(targetPath);
                if (iterator == m_watches.end())
                {
                    continue; // 监听已在本轮等待期间被移除
                }

                WatchEntry &entry = *iterator->second;
                processEntry(entry, pendingCallbacks);
                callbackSnapshot = m_callback;

                // 处理完毕后重新发起下一次重叠读
                if (entry.isDead)
                {
                    // 目录已被删除/改名（句柄随之失效）：这条监听再也收不到事件，摘掉它。
                    // 若它是个递归根，下一秒的自愈复查会把它重新挂上
                    deadWatchPath = targetPath;
                } else
                {
                    ::ResetEvent(entry.eventHandle);
                    entry.pending = false;
                    if (!issueRead(entry))
                    {
                        // 投递失败同样是死条目（目录刚被删、句柄失效）：不摘掉的话它既不进等待集、
                        // 也没人会再投递——条目与两个句柄一起留到 stop()
                        deadWatchPath = targetPath;
                    }
                }
            }

            if (!deadWatchPath.empty())
            {
                // dropWatch 自带写锁：CancelIo → 关句柄 → 从监听集合摘掉。递归根清单要保留，
                // 下一秒的自愈复查靠它把这个根重新挂上（走公共 removeWatch() 会连清单一起摘掉，
                // 于是「目录被换掉重建」之后其内部变更永久丢失）
                static_cast<void>(dropWatch(deadWatchPath, true));
            }

            // 递归根之下新出现的目录要在锁外补挂监视：新子目录内部的变更否则永远不上报
            watchNewSubdirectories(pendingCallbacks);

            // 锁外批量触发回调，防止回调中增删监听路径造成死锁
            if (callbackSnapshot)
            {
                for (const auto &[changedPath, changeType]: pendingCallbacks)
                {
                    callbackSnapshot(changedPath, changeType);
                }
            }
        }
    }

    void Win32FileWatcher::watchNewSubdirectories(const std::vector<std::pair<std::string, FileChangeType> > &events)
    {
        for (const auto &[changedPath, changeType]: events)
        {
            if (changeType != FileChangeType::Created || !isUnderRecursiveRoot(changedPath))
            {
                continue;
            }

            std::error_code directoryError;
            if (std::filesystem::is_directory(changedPath, directoryError) && !directoryError)
            {
                // 锁外补挂：addWatch() 要拿写锁，持锁递归注册会自死锁
                static_cast<void>(addWatch(changedPath, true));
            }
        }
    }

    void Win32FileWatcher::rewatchMissingRecursiveRoots()
    {
        std::vector<std::string> missingRoots;
        {
            const std::shared_lock lock(m_watchMutex);
            for (const std::string &root: m_recursiveRoots)
            {
                if (!m_watches.contains(root))
                {
                    missingRoots.push_back(root);
                }
            }
        }

        // 锁外补挂：addWatch() 要拿写锁；目录还没回来时它会失败，下一拍再试
        for (const std::string &root: missingRoots)
        {
            static_cast<void>(addWatch(root, true));
        }
    }

    bool Win32FileWatcher::isUnderRecursiveRoot(const std::string &path) const
    {
        const std::shared_lock lock(m_watchMutex);
        return std::ranges::any_of(m_recursiveRoots,
                                   [&path](const std::string &root)
                                   {
                                       if (path.size() < root.size() || path.compare(0, root.size(), root) != 0)
                                       {
                                           return false;
                                       }
                                       // 完全相同，或下一个字符就是分隔符，或根本身带尾分隔符
                                       // （addWatch 规范化后总是带），才算「在根之下」
                                       return path.size() == root.size() || path[root.size()] == '\\' || path[root.size()] == '/' ||
                                              root.back() == '\\' || root.back() == '/';
                                   });
    }

    void Win32FileWatcher::processEntry(WatchEntry &entry, std::vector<std::pair<std::string, FileChangeType> > &events)
    {
        DWORD bytesTransferred = 0;
        if (!::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, FALSE))
        {
            // 缓冲区溢出（变更过快过多，通知被内核丢弃）时 Windows 报 ERROR_NOTIFY_ENUM_DIR：
            // 派发一次「已修改」让消费方重新扫描该目录，而不是静默漏掉这一批变更
            if (::GetLastError() != ERROR_NOTIFY_ENUM_DIR)
            {
                // 其余失败（目录已被删除/改名、句柄失效、访问被拒）：这条监听不会再产生事件，
                // 交给 watchLoop 摘掉它，而不是留一个永远不进等待集的僵尸条目
                entry.isDead = true;
                return;
            }
            events.emplace_back(entry.path, FileChangeType::Modified);
            return;
        }

        if (bytesTransferred == 0)
        {
            return;
        }

        const auto *information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(entry.buffer.data());
        while (true)
        {
            const std::wstring wideName(information->FileName, information->FileNameLength / sizeof(wchar_t));
            const std::string  fileName = TextEncoding::toUtf8String(wideName);

            std::string fullPath;
            fullPath.reserve(entry.path.size() + fileName.size());
            fullPath.append(entry.path);
            fullPath.append(fileName);

            const FileChangeType changeType = changeTypeFromAction(information->Action);

            // 防抖与过期记录清理由基类统一实现，仅监听线程调用
            if (shouldDispatchChange(fullPath))
            {
                events.emplace_back(std::move(fullPath), changeType);
            }

            if (information->NextEntryOffset == 0)
            {
                break;
            }
            information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(reinterpret_cast<const uint8_t *>(information) + information->NextEntryOffset);
        }
    }

    bool Win32FileWatcher::issueRead(WatchEntry &entry) const
    {
        entry.overlapped        = OVERLAPPED{};
        entry.overlapped.hEvent = entry.eventHandle;
        entry.pending           = true;

        DWORD      bytesReturned = 0;
        const BOOL success       = ::ReadDirectoryChangesW(entry.directoryHandle, entry.buffer.data(), static_cast<DWORD>(entry.buffer.size()), FALSE, kWatchFilter, &bytesReturned,
                                                     &entry.overlapped, nullptr);

        if (!success)
        {
            entry.pending = false;
        }
        return success != FALSE;
    }

    void Win32FileWatcher::closeEntry(WatchEntry &entry) const
    {
        if (entry.pending && entry.directoryHandle != INVALID_HANDLE_VALUE)
        {
            ::CancelIo(entry.directoryHandle);
            DWORD bytesTransferred = 0;
            ::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, TRUE);
        }
        if (entry.eventHandle != nullptr)
        {
            ::CloseHandle(entry.eventHandle);
            entry.eventHandle = nullptr;
        }
        if (entry.directoryHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(entry.directoryHandle);
            entry.directoryHandle = INVALID_HANDLE_VALUE;
        }
        entry.pending = false;
    }

    std::string Win32FileWatcher::normalizeDirectoryPath(const std::string &path)
    {
        // ReadDirectoryChangesW 报告的文件名相对于被监听目录，故目录路径须以反斜杠结尾
        if (path.empty() || path.back() == '\\')
        {
            return path;
        }
        return path + '\\';
    }
} // namespace AsynGyanis::Platform
