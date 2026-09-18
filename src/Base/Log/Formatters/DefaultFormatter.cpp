#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    std::string DefaultFormatter::format(const LogEvent &event)
    {
#ifdef ASYN_DEBUG
        // 源码位置经共用工具生成：短「文件:行号」写进栈缓冲，装不下才回退到会分配的路径
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        return std::format("{} {} [{:<5}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadIdView(),
                           logLevelToString(event.level),
                           event.loggerNameView(),
                           location,
                           event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        return std::format("{} [{:<5}] [{}] {}",
                           event.timestamp,
                           logLevelToString(event.level),
                           event.loggerNameView(),
                           event.message);
#endif
    }
} // namespace AsynGyanis::Base
