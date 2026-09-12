/**
 * @file ConsoleSink.cpp
 * @brief 控制台日志输出目标
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
        // 与运行期切换共用同一条选择路径：构造只是「带初值」的一次切换，
        // 避免两处各写一份 formatter 选择逻辑而逐渐漂移
        applyFormatter();
    }

    void ConsoleSink::write(const LogEvent &event)
    {
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
        applyFormatter();
    }

    void ConsoleSink::applyFormatter()
    {
        // 读取 m_colorEnabled 决定 formatter：这就是该字段的消费点。
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
