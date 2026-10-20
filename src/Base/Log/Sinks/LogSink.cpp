#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Formatters/DefaultFormatter.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

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
        // 「Off 一律不放行」这条承诺落空（第一读过了 Off 的判定、第二读拿到 Off）。
        // 判定本身与 Logger 侧共用一条函数，两侧因此不会给出不同答案
        return logLevelPassesFilter(getLevel(), level);
    }

    void LogSink::setFormatter(std::unique_ptr<LogFormatter> formatter)
    {
        // 替换串行化在写侧互斥下：同一时刻只有一个写者动所有权槽与退休表
        const std::lock_guard lock(m_formatterWriteMutex);

        // 旧对象先移入退休表再让位：在途读者可能还攥着指向它的裸指针，提前释放会悬垂
        const std::shared_ptr<LogFormatter> previous = m_formatter.load(std::memory_order_acquire);
        if (previous)
        {
            m_retiredFormatters.push_back(previous);
        }

        // 先落所有权槽、再发布裸指针，读侧的 acquire 保证对象构造对读者可见
        const std::shared_ptr<LogFormatter> next = std::shared_ptr<LogFormatter>(std::move(formatter));
        m_formatter.store(next, std::memory_order_release);
        m_formatterPointer.store(next.get(), std::memory_order_release);
    }

    void LogSink::formatEventInto(std::string &out, const LogEvent &event) const
    {
        // 热路径只读裸指针：atomic<shared_ptr> 的 load 在 MSVC/libstdc++ 上要走内部锁池
        if (LogFormatter *const formatter = m_formatterPointer.load(std::memory_order_acquire))
        {
            formatter->formatInto(out, event);
            return;
        }
        kFallbackFormatter.formatInto(out, event);
    }

    std::string LogSink::formatEvent(const LogEvent &event) const
    {
        // 与 formatEventInto() 读同一个裸指针，产出逐字相同的文本
        if (LogFormatter *const formatter = m_formatterPointer.load(std::memory_order_acquire))
        {
            return formatter->format(event);
        }
        return kFallbackFormatter.format(event);
    }
} // namespace AsynGyanis::Base
