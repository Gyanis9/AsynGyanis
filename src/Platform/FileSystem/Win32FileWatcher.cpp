#include "Platform/FileSystem/Win32FileWatcher.h"

#include "Platform/System/TextEncoding.h"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string_view>
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
                    return FileChangeType::Modified;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    // 「改名落到这个位置」与 Linux 的 IN_MOVED_TO 是同一条事实，两侧要给同一个类型：
                    // 枚举把 Created 定义成「创建或原子替换后落位」，编辑器与配置发布走的正是这条；
                    // 递归监视补挂新目录的判据也只看 Created，报成 Modified 就漏挂
                    return FileChangeType::Created;
                default:
                    return FileChangeType::Modified;
            }
        }

        /**
         * @brief 按「同一个目录」判定两条路径：忽略大小写与末尾分隔符
         * @details NTFS 回报的是磁盘上的真实大小写，而注册时用的是调用方给的大小写，两者可以不同；
         *          目录键一律带尾分隔符、通知里的路径一律不带，也比不出差别。NTFS 对一个名字只存一份
         *          规范名，所以只在 ASCII 位上忽略大小写不会把两个不同目录判成同一个。
         * @param left 带或不带尾分隔符的路径
         * @param right 带或不带尾分隔符的路径
         * @return true 指向同一个目录
         */
        bool isSameDirectoryPath(const std::string_view left, const std::string_view right)
        {
            const auto trimSeparators = [](const std::string_view path)
            {
                std::size_t length = path.size();
                while (length > 0 && (path[length - 1] == '\\' || path[length - 1] == '/'))
                {
                    --length;
                }
                return path.substr(0, length);
            };

            const std::string_view leftTrimmed  = trimSeparators(left);
            const std::string_view rightTrimmed = trimSeparators(right);
            if (leftTrimmed.size() != rightTrimmed.size())
            {
                return false;
            }

            for (std::size_t index = 0; index < leftTrimmed.size(); ++index)
            {
                // 非 ASCII 字节按原值比较：NTFS 存的就是这一串字节，同一目录两次读回必然逐位相同
                const unsigned char leftByte  = static_cast<unsigned char>(leftTrimmed[index]);
                const unsigned char rightByte = static_cast<unsigned char>(rightTrimmed[index]);
                if (leftByte == rightByte)
                {
                    continue;
                }
                const unsigned char leftFolded  = leftByte >= 'A' && leftByte <= 'Z'
                                                      ? static_cast<unsigned char>(leftByte + ('a' - 'A'))
                                                      : leftByte;
                const unsigned char rightFolded = rightByte >= 'A' && rightByte <= 'Z'
                                                      ? static_cast<unsigned char>(rightByte + ('a' - 'A'))
                                                      : rightByte;
                if (leftFolded != rightFolded)
                {
                    return false;
                }
            }
            return true;
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
        // 对外入口：调用方自己给的这一次注册不算「递归树里枚举出来的一项」
        return registerWatch(path, recursive, false);
    }

    bool Win32FileWatcher::registerWatch(const std::string_view path, const bool recursive, const bool partOfRecursiveTree)
    {
        std::error_code errorCode;
        const auto      absolutePath = std::filesystem::absolute(path, errorCode).string();
        if (errorCode)
        {
            return false;
        }

        // 只有「这一次才把它变成递归根」才需要走一遍目录树；子目录自己再挂递归时同样要枚举它下面那层
        bool needsDescend = false;

        {
            const std::string directoryPath = normalizeDirectoryPath(absolutePath);
            std::lock_guard   lock(m_watchMutex);

            // 递归覆盖到的每个目录都留一份清单：条目一旦因目录被改名或删除而摘除，只有靠这份
            // 清单才会被自愈复查补回来。枚举出来的子目录同样要留——它们当时挂的是 recursive=false，
            // 但同属这条递归监视的范围
            if (recursive || partOfRecursiveTree)
            {
                // 已经在递归清单里就不用再走一遍树：重复的递归注册，枚举出来的子目录都在表里了
                needsDescend = recursive && !m_recursiveWatchPaths.contains(directoryPath);
                m_recursiveWatchPaths.insert(directoryPath);
            }

            // 调用方点过名的路径另记一份：非递归的那条不进上面那份清单（进了就会被当成递归范围去
            // 判定新建子目录），可它同样需要在目录被换掉后补挂回来
            if (!partOfRecursiveTree)
            {
                m_selfHealPaths.insert(directoryPath);
            }

            // 「这条路径已经挂过了」不能直接返回：先按非递归注册、之后再要递归的调用要走到下面的
            // 枚举，否则那次升级会被静默吞掉——先就存在的子目录一个都挂不上，自愈也找不到它们
            if (!m_watches.contains(directoryPath))
            {
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
                // 人会再给它投递一次，登记下来只会占住这个路径——之后同一目录再 addWatch 会被上面
                // 那条「已经挂过」的判断当成成立，于是「注册成功」而事件永久收不到。当场失败返回，
                // 让调用方看得见这条监视没成立（最常见的触发形状是路径指向普通文件：CreateFileW
                // 带着 FILE_FLAG_BACKUP_SEMANTICS 会成功，拒的是后面的 ReadDirectoryChangesW）
                if (!issueRead(*entry))
                {
                    closeEntry(*entry);
                    return false;
                }

                m_watches[directoryPath] = std::move(entry);
            }
        }

        // 递归注册放在锁外，避免持锁期间遍历目录树
        if (needsDescend && std::filesystem::is_directory(absolutePath, errorCode))
        {
            for (const auto &directoryEntry: std::filesystem::recursive_directory_iterator(absolutePath, errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                if (directoryEntry.is_directory())
                {
                    // 子目录自身不再往下枚举（外层迭代器已经把整棵树走了一遍，逐个递归注册会重复
                    // 走树），但要记进递归覆盖清单，让它享有和根本地一样的自愈补挂
                    static_cast<void>(registerWatch(directoryEntry.path().string(), false, true));
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

    bool Win32FileWatcher::dropWatch(const std::string_view path, const bool keepRecursiveWatchPath)
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
        // 内部清理（目录被删或被改名的死条目）刻意留着这份清单：自愈复查正是靠它把换掉重建的目录
        // 重新挂上。而调用方显式撤销时必须整棵子树一起摘掉——只摘被点名的那一条，枚举出来的子目录
        // 仍留在清单里，下一拍自愈会把它们逐个挂回来，等于把调用方的撤销否决掉
        if (!keepRecursiveWatchPath)
        {
            m_recursiveWatchPaths.erase(normalizedPath);
            m_selfHealPaths.erase(normalizedPath);
            std::erase_if(m_recursiveWatchPaths,
                          [&normalizedPath](const std::string &coveredPath)
                          {
                              // 清单里的路径一律带尾分隔符，前缀里那个分隔符本身就是边界判据：
                              // C:\a\b\ 不会是 C:\a\bc\ 的前缀
                              return coveredPath.size() > normalizedPath.size() &&
                                     isSameDirectoryPath(std::string_view(coveredPath).substr(0, normalizedPath.size()), normalizedPath);
                          });

            // 子树里已经挂上的活条目也要一并关掉。递归注册给每个子目录都建了一份监听上下文，
            // 只摘被点名的那一条，剩下的仍照旧派发回调（调用方以为撤销完成了，事件却一直在来），
            // 而且目录句柄不关就删不掉、也改不了那棵子树。先收集再关，免得在遍历里擦除
            std::vector<std::string> coveredWatchPaths;
            for (const auto &[watchedPath, watchEntry] : m_watches)
            {
                if (watchedPath.size() > normalizedPath.size()
                    && isSameDirectoryPath(std::string_view(watchedPath).substr(0, normalizedPath.size()), normalizedPath))
                {
                    coveredWatchPaths.push_back(watchedPath);
                }
            }
            for (const auto &coveredPath : coveredWatchPaths)
            {
                if (const auto covered = m_watches.find(coveredPath); covered != m_watches.end())
                {
                    closeEntry(*covered->second);
                    m_watches.erase(covered);
                }
            }
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
            // 递归监视的自愈：按秒节拍复查一遍「递归覆盖清单里该有、监听集合里却没有」的目录，
            // 把它们补挂上。部署工具把目录整个换掉（删掉再重建）是常见做法，而重建后的目录没有人
            // 会再调 addWatch——补挂不发生，它内部的变更就永久丢失。放在收集等待集之前：根刚被换掉
            // 时条目已被摘除、等待集为空，那条空转分支（sleep 后 continue）走不到本函数
            if (std::chrono::steady_clock::now() >= rootRecheckDeadline)
            {
                rootRecheckDeadline = std::chrono::steady_clock::now() + kRootRecheckInterval;
                rewatchMissingWatches();
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
            // 本批通知里成对出现的「目录改名」：旧名用来找到该目录自己的监视条目，新名给它的新键
            std::vector<std::pair<std::string, std::string> >  renamedPairs;
            {
                std::shared_lock lock(m_watchMutex);

                const auto iterator = m_watches.find(targetPath);
                if (iterator == m_watches.end())
                {
                    continue; // 监听已在本轮等待期间被移除
                }

                WatchEntry &entry = *iterator->second;
                processEntry(entry, pendingCallbacks, renamedPairs);
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
                // dropWatch 自带写锁：CancelIo → 关句柄 → 从监听集合摘掉。递归覆盖清单要保留，
                // 下一秒的自愈复查靠它把这个目录重新挂上（走公共 removeWatch() 会连清单一起摘掉，
                // 于是「目录被换掉重建」之后其内部变更永久丢失）
                static_cast<void>(dropWatch(deadWatchPath, true));
            }

            // 被改名走开的目录不能「摘掉重来」：它的未完成读挂在已经搬走的目录对象上，CancelIo 对
            // 这种 IRP 不会给出完成（实测：改名走开后不再往里写入时，closeEntry 的等待永久不返回，
            // 监听线程就此停摆）。改成让监视跟着目录走——父目录报出的那对旧名/新名正好给出去处，
            // 换键并同步条目里的路径，派发前缀因此重新对上真实的目录。旧键仍留在递归覆盖清单里，
            // 原地重建出同名目录时由自愈复查补挂上去；排在 watchNewSubdirectories 之前做，改名与
            // 重建落进同一批通知时，那批的 Created 记录才能在键位腾出来之后立刻挂上新目录
            for (const auto &[relocateFrom, relocateTo]: renamedPairs)
            {
                static_cast<void>(relocateWatchedDirectory(relocateFrom, relocateTo));
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
            if (changeType != FileChangeType::Created || !isUnderRecursiveWatch(changedPath))
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

    void Win32FileWatcher::rewatchMissingWatches()
    {
        std::vector<std::pair<std::string, bool> > missingWatches;
        {
            const std::shared_lock lock(m_watchMutex);
            for (const std::string &coveredPath: m_recursiveWatchPaths)
            {
                if (!m_watches.contains(coveredPath))
                {
                    missingWatches.emplace_back(coveredPath, true);
                }
            }

            for (const std::string &requestedPath: m_selfHealPaths)
            {
                // 已经由上面那份递归清单兜住的路径不再重复登记：两条路的 recursive 取值不同
                if (!m_watches.contains(requestedPath) && !m_recursiveWatchPaths.contains(requestedPath))
                {
                    // 落在自愈清单而不在递归清单里的，一定是调用方按 recursive=false 给的那条：
                    // 按递归挂上会把一条只要一层的请求悄悄扩成整棵树
                    missingWatches.emplace_back(requestedPath, false);
                }
            }
        }

        // 锁外补挂：registerWatch() 要拿写锁；目录还没回来时它会失败，下一拍再试。
        // 三条参数里的 partOfRecursiveTree 一律给 true：这些是框架的补挂动作，不该被记成新的调用方请求
        for (const auto &[missingPath, recursive]: missingWatches)
        {
            static_cast<void>(registerWatch(missingPath, recursive, true));
        }
    }

    bool Win32FileWatcher::relocateWatchedDirectory(const std::string &oldPath, const std::string &newPath)
    {
        // 监视表的键一律带尾分隔符、通知里的路径不带，因此找旧键比的是「同一个目录」；新键按同一
        // 形式规范化，之后拼出来的派发路径才和兄弟条目一致
        const std::string relocatedPath = normalizeDirectoryPath(newPath);
        const std::lock_guard lock(m_watchMutex);

        const auto iterator = std::find_if(m_watches.begin(), m_watches.end(),
                                           [&oldPath](const auto &item)
                                           {
                                               return isSameDirectoryPath(item.first, oldPath);
                                           });
        if (iterator == m_watches.end())
        {
            return false; // 改名的不是被监视的目录（普通文件的改名也走这两条记录）
        }

        if (m_watches.contains(relocatedPath))
        {
            return false; // 新位置上已经有一条监视，不覆盖它
        }

        auto node = m_watches.extract(iterator);
        node.key()          = relocatedPath;
        node.mapped()->path = relocatedPath;
        m_watches.insert(std::move(node));
        // 新位置同样纳入自愈范围；旧键**故意留着**——原地重建出同名目录时靠自愈复查补挂第二条监视
        m_recursiveWatchPaths.insert(relocatedPath);
        // 调用方点过名的那条监视搬到哪，自愈就该在新位置复查它；旧键同样留着，理由与上面一致
        if (std::ranges::any_of(m_selfHealPaths,
                               [&oldPath](const std::string &requestedPath)
                               {
                                   return isSameDirectoryPath(requestedPath, oldPath);
                               }))
        {
            m_selfHealPaths.insert(relocatedPath);
        }
        return true;
    }

    bool Win32FileWatcher::isUnderRecursiveWatch(const std::string &path) const
    {
        const std::shared_lock lock(m_watchMutex);
        return std::ranges::any_of(m_recursiveWatchPaths,
                                   [&path](const std::string &coveredPath)
                                   {
                                       if (path.size() < coveredPath.size() || path.compare(0, coveredPath.size(), coveredPath) != 0)
                                       {
                                           return false;
                                       }
                                       // 完全相同，或下一个字符就是分隔符，或清单里的路径本身带尾分隔符
                                       // （registerWatch 规范化后总是带），才算「落在这条递归监视之内」
                                       return path.size() == coveredPath.size() || path[coveredPath.size()] == '\\' ||
                                              path[coveredPath.size()] == '/' || coveredPath.back() == '\\' || coveredPath.back() == '/';
                                   });
    }

    void Win32FileWatcher::processEntry(WatchEntry &entry, std::vector<std::pair<std::string, FileChangeType> > &events,
                                        std::vector<std::pair<std::string, std::string> > &renamedPairs)
    {
        DWORD bytesTransferred = 0;
        if (!::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, FALSE))
        {
            // 缓冲区溢出（变更过快过多，通知被内核丢弃）时 Windows 报 ERROR_NOTIFY_ENUM_DIR：
            // 派发一次「该重扫了」让消费方重新扫描该目录，而不是静默漏掉这一批变更。用专门的
            // 事件种类而不是「目录被修改」：消费方按扩展名过滤事件时，前者会被一起滤掉
            if (::GetLastError() != ERROR_NOTIFY_ENUM_DIR)
            {
                // 其余失败（目录已被删除/改名、句柄失效、访问被拒）：这条监听不会再产生事件，
                // 交给 watchLoop 摘掉它，而不是留一个永远不进等待集的僵尸条目
                entry.isDead = true;
                return;
            }
            events.emplace_back(entry.path, FileChangeType::NeedsRescan);
            return;
        }

        if (bytesTransferred == 0)
        {
            return;
        }

        const auto *information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(entry.buffer.data());
        // 一次改名在通知里是相邻的两条记录（旧名、新名）。攒着旧名等下一条配对；万一分在两批里
        // （中间撞上缓冲区溢出），这一对就配不上，该条监视维持原状，下一批改名照样能配上
        std::string previousRenamedOldPath;
        while (true)
        {
            const std::wstring wideName(information->FileName, information->FileNameLength / sizeof(wchar_t));
            const std::string  fileName = TextEncoding::toUtf8String(wideName);

            std::string fullPath;
            fullPath.reserve(entry.path.size() + fileName.size());
            fullPath.append(entry.path);
            fullPath.append(fileName);

            const FileChangeType changeType = changeTypeFromAction(information->Action);

            // 改名配对要在防抖与 move 之前做：防抖可能把这两条压掉，但「被监视的目录搬去了哪」是与
            // 防抖无关的事实，丢了它就没法把派发前缀改回真实的目录
            if (information->Action == FILE_ACTION_RENAMED_OLD_NAME)
            {
                previousRenamedOldPath = fullPath;
            } else if (information->Action == FILE_ACTION_RENAMED_NEW_NAME && !previousRenamedOldPath.empty())
            {
                renamedPairs.emplace_back(std::move(previousRenamedOldPath), fullPath);
            }

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

            // 等完成包必须有上限。GetOverlappedResult 的 bWait=TRUE 是**无界**等待，而
            // CancelIo 对某些状态的目录根本不会给出完成（本文件上面就记着「目录被改名走开后
            // 不再往里写入时，等待永久不返回，监听线程就此停摆」这条实测）：调用 stop() 的线程
            // 会被一起钉死，热重载的启停与进程退出都可能因此挂住几十秒到永久。
            // 超时后直接关句柄——内核会在句柄回收时了结那条已取消的 IRP，我们不再等它。
            constexpr DWORD kCompletionDrainTimeoutMilliseconds = 200;
            static_cast<void>(::WaitForSingleObject(entry.eventHandle, kCompletionDrainTimeoutMilliseconds));

            DWORD bytesTransferred = 0;
            // bWait 给 FALSE：结果没到就作罢，这一句本身绝不阻塞
            static_cast<void>(::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, FALSE));
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
