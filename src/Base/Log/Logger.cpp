#include "Base/Log/Logger.h"

#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    Logger::Logger(std::string name) :
        m_name(std::make_shared<const std::string>(std::move(name)))
    {
    }

    Logger::~Logger()
    {
        clearSinks();
    }

    void Logger::log(const LogLevel level, const std::string_view message, const SourceLocation &location) const
    {
        // 公开入口以 string_view 接收（调用方无需构造字符串），此处做唯一一次拷贝
        writeEvent(level, std::string(message), location);
    }

    void Logger::writeEvent(const LogLevel level, std::string message, const SourceLocation &location) const
    {
        if (!shouldLog(level))
        {
            return;
        }

        // 名字与线程号都是共享/缓存值，事件构造只搬指针；消息体按值移入避免二次拷贝
        const LogEvent event{
                level,
                currentTimestamp(),
                threadIdString(),
                location,
                m_name,
                std::move(message)
        };

        writeToSinks(event);
    }

    std::shared_ptr<const Logger::SinkSnapshot> Logger::emptySnapshot()
    {
        return std::make_shared<const SinkSnapshot>();
    }

    void Logger::addSink(std::unique_ptr<LogSink> sink)
    {
        if (!sink)
        {
            return;
        }

        // 写者之间串行即可，读者全程无锁；复制上一代指针列表构成新一代快照
        std::lock_guard writeLock(m_sinksWriteMutex);

        auto next   = std::make_shared<SinkSnapshot>();
        next->sinks = m_sinksSnapshot.load(std::memory_order_acquire)->sinks;
        next->sinks.push_back(std::shared_ptr<LogSink>(std::move(sink)));
        m_sinksSnapshot.store(std::move(next), std::memory_order_release);
    }

    void Logger::clearSinks()
    {
        std::lock_guard writeLock(m_sinksWriteMutex);

        // 旧快照可能仍被并发写日志的线程持有，Sink 会在最后一个引用释放后才销毁
        m_sinksSnapshot.store(emptySnapshot(), std::memory_order_release);
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
        return *m_name;
    }

    void Logger::flush() const
    {
        const auto snapshot = m_sinksSnapshot.load(std::memory_order_acquire);
        for (const auto &sink: snapshot->sinks)
        {
            if (sink)
            {
                sink->flush();
            }
        }
    }

    bool Logger::shouldLog(const LogLevel level) const
    {
        const LogLevel currentLevel = getLevel();

        // Off 表示关闭全部输出，且它本身不是可用于记录消息的等级，因此一律不放行
        if (currentLevel == LogLevel::Off)
        {
            return false;
        }
        return level >= currentLevel;
    }

    void Logger::writeToSinks(const LogEvent &event) const
    {
        const auto snapshot = m_sinksSnapshot.load(std::memory_order_acquire);
        for (const auto &sink: snapshot->sinks)
        {
            if (sink && sink->shouldLog(event.level))
            {
                try
                {
                    sink->write(event);
                } catch (const std::exception &sinkError)
                {
                    // 单个 Sink 异常不应阻止其他 Sink 收日志，但绝不能静默：日志系统自己出了故障
                    // 没有别处可报。会抛的写路径本就罕见（如滚动时无法重开文件），无需限流
                    std::cerr << "Logger(" << name() << ")：某个 Sink 写入失败，该 Sink 的后续日志可能丢失："
                            << sinkError.what() << '\n';
                } catch (...)
                {
                    std::cerr << "Logger(" << name() << ")：某个 Sink 写入时抛出未知异常，该 Sink 的后续日志可能丢失" << '\n';
                }
            }
        }
    }
} // namespace AsynGyanis::Base
