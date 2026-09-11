#include "Base/Log/Formatters/DefaultFormatter.h"

#include "Base/Log/LogLevel.h"

#include <format>
#include <string>

namespace AsynGyanis::Base
{
    std::string DefaultFormatter::format(const LogEvent &event)
    {
#ifdef ASYN_DEBUG
        return std::format("{} {} [{:<5}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadId,
                           logLevelToString(event.level),
                           event.loggerName,
                           std::string(event.location.shortFileName()) + ":" + std::to_string(event.location.line),
                           event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        return std::format("{} [{:<5}] [{}] {}",
                           event.timestamp,
                           logLevelToString(event.level),
                           event.loggerName,
                           event.message);
#endif
    }
} // namespace AsynGyanis::Base
