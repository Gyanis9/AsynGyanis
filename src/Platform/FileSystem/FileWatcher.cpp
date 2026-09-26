#include "Platform/FileSystem/FileWatcher.h"

#if ASYN_PLATFORM_WIN32
#include "Platform/FileSystem/Win32FileWatcher.h"
#else
#include "Platform/FileSystem/InotifyFileWatcher.h"
#endif

#include <chrono>

namespace AsynGyanis::Platform
{
    std::unique_ptr<FileWatcher> FileWatcher::create()
    {
#if ASYN_PLATFORM_WIN32
        return std::make_unique<Win32FileWatcher>();
#else
        return std::make_unique<InotifyFileWatcher>();
#endif
    }

    void FileWatcher::setDebounceInterval(const std::chrono::milliseconds interval) noexcept
    {
        // 原子写：监听线程可能正同时在读它（见成员声明处的线程约定）
        m_debounceIntervalMilliseconds.store(interval.count(), std::memory_order_relaxed);
    }

    bool FileWatcher::shouldDispatchChange(const std::string &filePath)
    {
        const auto currentTime      = std::chrono::steady_clock::now();
        const auto debounceInterval = std::chrono::milliseconds(m_debounceIntervalMilliseconds.load(std::memory_order_relaxed));

        if (const auto indexIterator = m_lastEventTime.find(filePath); indexIterator != m_lastEventTime.end())
        {
            const std::list<DebounceRecord>::iterator recordIterator = indexIterator->second;
            if (currentTime - recordIterator->lastTime < debounceInterval)
            {
                return false;
            }

            // 过窗后再次触发：把这条挪到序表最前端并刷新时间。窗口始终从「上一次派发出去」算起，
            // 抑制期间不刷新时间（与早期实现一致）——否则一路持续变更的文件会被无限抑制下去
            m_recentDebouncedPaths.splice(m_recentDebouncedPaths.begin(), m_recentDebouncedPaths, recordIterator);
            recordIterator->lastTime = currentTime;
            return true;
        }

        // 表满、且连表尾那条都还在窗口里：这条不记账，照常派发但本窗口内不受抑制。硬挤是白挤——
        // 被挤掉的那条转过眼就当「新路径」再插回来，每次插入都付一趟分配与摘除，实测这种抖动能把
        // 单次判定从约 0.7 µs 顶到 37 µs；监听线程被自己的记账拖停，就是通知缓冲被憋爆那一族丢事件
        if (m_lastEventTime.size() >= kMaximumDebouncedPaths && currentTime - m_recentDebouncedPaths.back().lastTime < debounceInterval)
        {
            return true;
        }

        m_recentDebouncedPaths.emplace_front(DebounceRecord{filePath, currentTime});
        m_lastEventTime[filePath] = m_recentDebouncedPaths.begin();

        if (m_lastEventTime.size() > kMaximumDebouncedPaths)
        {
            // 走到这里说明表尾那条已过窗：它的记录再不抑制任何事件，让位给新路径不改变行为。
            // 更早的实现是「每次插入都重扫整表」，实测满表后的单次判定从 934 ns 涨到 8115 ns
            m_lastEventTime.erase(m_recentDebouncedPaths.back().path);
            m_recentDebouncedPaths.pop_back();
        }

        return true;
    }
} // namespace AsynGyanis::Platform
