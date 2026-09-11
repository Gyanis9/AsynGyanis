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
        // 控制台不支持 ANSI 序列时自动退回无颜色格式，避免输出乱码
        if (enableColor && AsynGyanis::Platform::Console::supportsAnsiEscapeCodes())
        {
            setFormatter(std::make_unique<ColorFormatter>());
        } else
        {
            setFormatter(std::make_unique<DefaultFormatter>());
        }
    }

    void ConsoleSink::write(const LogEvent &event)
    {
        AsynGyanis::Platform::Console::ensureUtf8Output();
        std::lock_guard   lock(m_mutex);
        const std::string formatted = formatEvent(event);
        if (event.level >= LogLevel::Warn)
        {
            std::cerr << formatted << '\n';
        } else
        {
            std::cout << formatted << '\n';
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
        if (enabled && AsynGyanis::Platform::Console::supportsAnsiEscapeCodes())
        {
            setFormatter(std::make_unique<ColorFormatter>());
        } else
        {
            setFormatter(std::make_unique<DefaultFormatter>());
        }
    }
} // namespace AsynGyanis::Base
