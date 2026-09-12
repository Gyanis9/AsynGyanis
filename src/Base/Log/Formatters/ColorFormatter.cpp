/**
 * @file ColorFormatter.cpp
 * @brief 彩色终端日志格式化器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/LogColor.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    std::string ColorFormatter::format(const LogEvent &event)
    {
#ifdef ASYN_DEBUG
        // 与 DefaultFormatter 共用 tryFormatSourceLocationText：短「文件:行号」写进栈缓冲、
        // 省掉每行一次堆分配，装不下时（返回空视图）才回退到分配路径。回退用的是与共用工具
        // 完全相同的格式串 "{}:{}"，因此命中与回退的输出逐字节一致（含 {:<13} 的右填充空格），
        // 两个格式化器也据此保持同一套源码位置文本
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        const std::string_view                         locationText = tryFormatSourceLocationText(event.location, locationBuffer);
        const std::string overflowLocation = locationText.empty()
                                                 ? std::format("{}:{}", event.location.shortFileName(), event.location.line)
                                                 : std::string();
        const std::string_view location = locationText.empty() ? std::string_view(overflowLocation) : locationText;

        return std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadId,
                           LogColor::colorForLevel(event.level),
                           logLevelToString(event.level),
                           LogColor::kReset,
                           event.loggerNameView(),
                           location,
                           event.message);
#else
        return std::format("{} [{}{:<5}{}] [{}] {}",
                           event.timestamp,
                           LogColor::colorForLevel(event.level),
                           logLevelToString(event.level),
                           LogColor::kReset,
                           event.loggerNameView(),
                           event.message);
#endif
    }
} // namespace AsynGyanis::Base
