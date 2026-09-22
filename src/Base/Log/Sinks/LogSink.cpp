#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Formatters/DefaultFormatter.h"

#include <memory>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 未设置格式化器时的回退实例。constinit + 命名空间作用域省掉了函数内 static 的
        /// 初始化守卫（每次调用一次 TSS 读 + 分支）与首次调用时的 atexit 注册。
        /// 不加 const 是因为 LogFormatter::format() 按契约是非 const 成员；该对象无状态
        constinit DefaultFormatter kFallbackFormatter{};
    } // namespace

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
        // 阈值只读一次：读两次就在两次取值之间留出一个窗口，并发的 setLevel(Off) 会让
        // 「Off 一律不放行」这条承诺落空（第一读过了 Off 的判定、第二读拿到 Off，
        // 而任何真实级别都 >= Off），刚被静音的 Sink 还会再吐一行
        const LogLevel currentLevel = getLevel();
        // Off 表示关闭全部输出，且它本身不是可用于记录消息的等级，因此一律不放行
        if (currentLevel == LogLevel::Off)
        {
            return false;
        }
        return level >= currentLevel;
    }

    void LogSink::setFormatter(std::unique_ptr<LogFormatter> formatter)
    {
        // 写侧与此后 formatEvent 的 load 以 release/acquire 配对，避免数据竞争
        m_formatter.store(std::shared_ptr<LogFormatter>(std::move(formatter)), std::memory_order_release);
    }

    void LogSink::formatEventInto(std::string &out, const LogEvent &event) const
    {
        // 与 formatEvent() 选同一份格式化器快照，只是把去处换成调用方的行缓冲
        if (const auto formatter = m_formatter.load(std::memory_order_acquire))
        {
            formatter->formatInto(out, event);
            return;
        }
        kFallbackFormatter.formatInto(out, event);
    }

    std::string LogSink::formatEvent(const LogEvent &event) const
    {
        if (const auto formatter = m_formatter.load(std::memory_order_acquire))
        {
            return formatter->format(event);
        }
        return kFallbackFormatter.format(event);
    }
} // namespace AsynGyanis::Base
