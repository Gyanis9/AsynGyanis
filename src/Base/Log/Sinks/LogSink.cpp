#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Formatters/DefaultFormatter.h"

#include <memory>
#include <string>

namespace AsynGyanis::Base
{
    void LogSink::setLevel(const LogLevel level)
    {
        m_level.store(level, std::memory_order_release);
    }

    LogLevel LogSink::getLevel() const
    {
        return m_level.load(std::memory_order_acquire);
    }

    bool LogSink::shouldLog(const LogLevel level) const
    {
        // Off 表示关闭全部输出，且它本身不是可用于记录消息的等级，因此一律不放行
        if (getLevel() == LogLevel::Off)
        {
            return false;
        }
        return level >= getLevel();
    }

    void LogSink::setFormatter(std::unique_ptr<LogFormatter> formatter)
    {
        // 线程安全：使用 std::atomic<std::shared_ptr> 的 store/load 保护读写
        // 写侧与 formatEvent 的 load 以 release/acquire 语义配对，避免数据竞争
        m_formatter.store(std::shared_ptr<LogFormatter>(std::move(formatter)), std::memory_order_release);
    }

    std::string LogSink::formatEvent(const LogEvent &event) const
    {
        // load 确保读取时不会与 setFormatter 产生数据竞争
        if (const auto formatter = m_formatter.load(std::memory_order_acquire))
        {
            return formatter->format(event);
        }
        static DefaultFormatter defaultFormatter;
        return defaultFormatter.format(event);
    }
} // namespace AsynGyanis::Base
