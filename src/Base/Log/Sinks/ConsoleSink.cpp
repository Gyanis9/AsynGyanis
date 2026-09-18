#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/LogLevel.h"
#include "Platform/IO/Console.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace AsynGyanis::Base
{
    ConsoleSink::ConsoleSink(const bool enableColor) :
        m_colorEnabled(enableColor)
    {
        AsynGyanis::Platform::Console::ensureUtf8Output();
        // 构造只是「带初值」的一次切换，与运行期切换共用同一条 formatter 选择路径
        applyFormatter();
    }

    void ConsoleSink::write(const LogEvent &event)
    {
        std::lock_guard lock(m_mutex);
        std::string     formatted = formatEvent(event);
        // 换行并入同一缓冲后整行只做一次 <<：每次流插入都要构造 sentry（锁流缓冲、
        // 冲刷被 tie 的流），合并后只有一轮。format 结果串的容量通常够追加换行
        formatted.push_back('\n');
        if (event.level >= LogLevel::Warn)
        {
            // std::cerr 恒为 unitbuf：整行写出即落地，无需额外刷新
            std::cerr << formatted;
        } else
        {
            // 每条刷新：std::cout 重定向到文件或管道时是全缓冲，不刷就 tail 不到实时内容，
            // 异常退出还会丢掉尾部（实测 +1 µs/行，Release /O2），换来 write() 返回即已落地
            std::cout << formatted;
            std::cout.flush();
        }
    }

    void ConsoleSink::flush()
    {
        std::lock_guard lock(m_mutex);
        std::cout.flush();
        std::cerr.flush();
    }

    void ConsoleSink::setColorEnabled(const bool enabled)
    {
        std::lock_guard lock(m_mutex);
        m_colorEnabled = enabled;
        applyFormatter();
    }

    void ConsoleSink::applyFormatter()
    {
        // 终端不支持 ANSI 序列时即便请求了彩色也退回纯文本，避免输出乱码
        if (m_colorEnabled && AsynGyanis::Platform::Console::supportsAnsiEscapeCodes())
        {
            setFormatter(std::make_unique<ColorFormatter>());
        } else
        {
            setFormatter(std::make_unique<DefaultFormatter>());
        }
    }
} // namespace AsynGyanis::Base
