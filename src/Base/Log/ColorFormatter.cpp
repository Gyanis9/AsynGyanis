#include "Base/Log/ColorFormatter.h"

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
                           std::string(event.location.shortFileName()) + ":" + std::to_string(event.location.line),
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
