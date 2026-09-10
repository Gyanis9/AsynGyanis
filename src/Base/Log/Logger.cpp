#include "Base/Log/Logger.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    Logger::Logger(std::string name) :
        m_name(std::move(name))
    {
    }

    Logger::~Logger()
    {
        clearSinks();
    }

    void Logger::log(const LogLevel level, const std::string_view message, const SourceLocation &location) const
    {
        if (!shouldLog(level))
        {
            return;
        }

        const LogEvent event{
                level,
                currentTimestamp(),
                threadIdString(),
                location,
                m_name,
                std::string(message)
        };

        writeToSinks(event);
    }

    void Logger::addSink(std::unique_ptr<LogSink> sink)
    {
        std::unique_lock lock(m_sinksMutex);
        m_sinks.push_back(std::move(sink));
    }

    void Logger::clearSinks()
    {
        std::unique_lock lock(m_sinksMutex);
        m_sinks.clear();
    }

    void Logger::setLevel(const LogLevel level)
    {
        m_level.store(level, std::memory_order_release);
    }

    LogLevel Logger::getLevel() const
    {
        return m_level.load(std::memory_order_acquire);
    }

    const std::string &Logger::name() const
    {
        return m_name;
    }

    void Logger::flush() const
    {
        std::shared_lock lock(m_sinksMutex);
        for (auto &sink: m_sinks)
        {
            if (sink)
            {
                sink->flush();
            }
        }
    }

    bool Logger::shouldLog(const LogLevel level) const
    {
        // Off 表示关闭全部输出，且它本身不是可用于记录消息的等级，因此一律不放行
        if (getLevel() == LogLevel::Off)
        {
            return false;
        }
        return level >= getLevel();
    }

    void Logger::writeToSinks(const LogEvent &event) const
    {
        std::shared_lock lock(m_sinksMutex);
        for (auto &sink: m_sinks)
        {
            if (sink && sink->shouldLog(event.level))
            {
                try
                {
                    sink->write(event);
                } catch (...)
                {
                    // 单个 Sink 异常不应阻止其他 Sink 接收日志
                }
            }
        }
    }
} // namespace AsynGyanis::Base
