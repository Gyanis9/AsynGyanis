#include "Base/Log/Logger.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    Logger::Logger(std::string name) : m_name(std::make_shared<const std::string>(std::move(name)))
    {
    }

    Logger::~Logger()
    {
        clearSinks();
    }

    void Logger::log(const LogLevel level, const std::string_view message, const SourceLocation &location) const
    {
        // 等级过滤排在正文落地之前：本入口以 string_view 收文本，若先拷成 std::string 再问等级，
        // 一条注定被挡下的记录也要为消息体取一块堆——按 INFO 跑的进程里，满代码库的 TRACE/DEBUG
        // 调用走的正是这条被丢弃的路径
        if (!shouldLog(level))
        {
            return;
        }

        // 走到这里说明本条已放行：公开入口以 string_view 收正文，这是它唯一一次被拷成 owning 字符串
        writeEvent(level, std::string(message), location);
    }

    void Logger::logWithStackTrace(const LogLevel level, const std::string_view message, const SourceLocation &location) const
    {
        // 采栈放在等级过滤之后：被过滤掉的日志不为采集付任何代价
        if (!shouldLog(level))
        {
            return;
        }
        // 跳过 1 帧：captureStackTrace 已跳过自身，这里再跳过本函数，首个保留帧即调用方
        writeEvent(level, std::string(message), location, captureStackTrace(1));
    }

    void Logger::logWithStackTrace(const LogLevel level, const std::string_view message, CapturedStackTrace stackTrace, const SourceLocation &location) const
    {
        // 与 log() 同一口径：栈已由调用方采好，正文拷贝仍要排在等级过滤之后
        if (!shouldLog(level))
        {
            return;
        }

        // 栈由调用方携带（异常的抛出点栈），此处不做采集
        writeEvent(level, std::string(message), location, std::move(stackTrace));
    }

    void Logger::writeEvent(const LogLevel level, std::string message, const SourceLocation &location, CapturedStackTrace stackTrace) const
    {
        if (!shouldLog(level))
        {
            return;
        }

        // 名字与线程号都是共享/缓存值，事件构造只搬指针；消息体按值移入避免二次拷贝。
        // 时刻只取一个 time_point：本地时间的折算与文本化留给各 Sink 的格式化器，
        // 于是事件循环线程上一行日志不为时间戳取堆块
        LogEvent event{level, std::chrono::system_clock::now(), threadIdString(), location, m_name, std::move(message)};
        event.stackTrace = std::move(stackTrace);

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
        // 阈值只读一次：读两次会在两次取值之间留出一个窗口，让并发的 setLevel(Off) 有机会被
        // 判定成「阈值还不是 Off」，刚被静音的记录器就又吐一行。判定本身与 Sink 侧共用一条函数
        return logLevelPassesFilter(getLevel(), level);
    }

    void Logger::writeToSinks(LogEvent &event) const
    {
        const auto snapshot = m_sinksSnapshot.load(std::memory_order_acquire);

        // 「只有一个 Sink 才交出事件本体」这条判据只看快照大小，不看过滤结果：异步日志正是
        // 挂在 Logger 上的唯一一个 Sink，这一趟就省掉整份事件拷贝。若改成数「有几个 Sink 愿意收」，
        // 判定就得问两遍，而并发的 setLevel() 能让两遍答案不一致——按「只有一个收」交出本体之后，
        // 后一个 Sink 读到的是已被搬空的消息，只留下一行有时间戳和级别的空日志
        const bool soleSink = snapshot->sinks.size() == 1U;

        // Fatal 之后调用方往往接着就 abort()/退出，不会替日志系统补那一次 flush，于是「最想知道的最后
        // 一条」整条留在文件流的用户态缓冲里丢掉。刷新因此只挂在 Fatal 上：常规等级不为它付这笔钱
        // （FileSink 的 flush 是一次 FlushFileBuffers/fsync）
        const bool flushAfterWrite = event.level == LogLevel::Fatal;

        for (const auto &sink: snapshot->sinks)
        {
            if (!sink || !sink->shouldLog(event.level))
            {
                continue;
            }
            try
            {
                if (soleSink)
                {
                    sink->write(std::move(event));
                } else
                {
                    sink->write(event);
                }
                // 只刷收下这条的 Sink：没写进去的 Sink 缓冲里没有它，刷了也只是白付一次系统调用
                if (flushAfterWrite)
                {
                    sink->flush();
                }
            } catch (const std::exception &sinkError)
            {
                // 单个 Sink 异常不应阻止其他 Sink 收日志，但绝不能静默：日志系统自己出了故障
                // 没有别处可报。会抛的写路径本就罕见（如滚动时无法重开文件），无需限流
                std::cerr << "Logger(" << name() << ")：某个 Sink 写入失败，该 Sink 的后续日志可能丢失：" << sinkError.what() << '\n';
            } catch (...)
            {
                std::cerr << "Logger(" << name() << ")：某个 Sink 写入时抛出未知异常，该 Sink 的后续日志可能丢失" << '\n';
            }
        }
    }
} // namespace AsynGyanis::Base
