#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Formatters/DefaultFormatter.h"

#include <memory>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 未设置格式化器时的回退实例。放在命名空间作用域并用 constinit 常量初始化，
        /// 相比函数内 static：不再为每次调用生成线程安全初始化守卫（TSS 读 + 比较 + 分支，
        /// 已用生成的汇编确认），也不需要在首次调用时注册 atexit。
        /// 此处不能加 const：LogFormatter::format() 按接口契约是非 const 成员，
        /// 而该对象无状态、运行期从不被修改，去掉 const 只影响静态检查
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
        return kFallbackFormatter.format(event);
    }
} // namespace AsynGyanis::Base
