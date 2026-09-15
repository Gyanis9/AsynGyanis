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
        const auto currentTime = std::chrono::steady_clock::now();
        const auto debounceInterval = std::chrono::milliseconds(m_debounceIntervalMilliseconds.load(std::memory_order_relaxed));

        if (const auto lastIterator = m_lastEventTime.find(filePath); lastIterator != m_lastEventTime.end())
        {
            if (currentTime - lastIterator->second < debounceInterval)
            {
                return false;
            }
        }

        m_lastEventTime[filePath] = currentTime;

        if (m_lastEventTime.size() > kMaximumDebouncedPaths)
        {
            // 已过防抖窗口的路径再来事件本就该立即触发，丢弃其记录不改变行为
            for (auto iterator = m_lastEventTime.begin(); iterator != m_lastEventTime.end();)
            {
                if (currentTime - iterator->second >= debounceInterval)
                {
                    iterator = m_lastEventTime.erase(iterator);
                } else
                {
                    ++iterator;
                }
            }

            if (m_lastEventTime.size() > kMaximumDebouncedPaths)
            {
                // 极端场景（窗口内持续涌入全新路径）退化为整表清空：
                // 宁可少抑制几次重复事件，也不让内存无界增长
                m_lastEventTime.clear();
            }
        }

        return true;
    }
} // namespace AsynGyanis::Platform
