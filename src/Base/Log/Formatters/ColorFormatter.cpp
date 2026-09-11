/**
 * @file ColorFormatter.cpp
 * @brief 彩色终端日志格式化器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/LogColor.h"
#include "Base/Log/LogLevel.h"

#include <format>
#include <string>

namespace AsynGyanis::Base
{
    std::string ColorFormatter::format(const LogEvent &event)
    {
#ifdef ASYN_DEBUG
        return std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadId,
                           LogColor::colorForLevel(event.level),
                           logLevelToString(event.level),
                           LogColor::kReset,
                           event.loggerName,
                           std::format("{}:{}", event.location.shortFileName(), event.location.line),
                           event.message);
#else
        return std::format("{} [{}{:<5}{}] [{}] {}",
                           event.timestamp,
                           LogColor::colorForLevel(event.level),
                           logLevelToString(event.level),
                           LogColor::kReset,
                           event.loggerName,
                           event.message);
#endif
    }
} // namespace AsynGyanis::Base
