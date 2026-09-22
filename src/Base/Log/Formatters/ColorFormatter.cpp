#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
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
        // 与 DefaultFormatter 同一套时刻渲染：写进栈缓冲，本函数的两条版式分支只走其中一条
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
#ifdef ASYN_DEBUG
        // 与 DefaultFormatter 同一套源码位置文本（见 formatSourceLocationText）
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        std::string text = std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}",
                                       timestampText,
                                       event.threadIdView(),
                                       LogColor::colorForLevel(event.level),
                                       logLevelToString(event.level),
                                       LogColor::kReset,
                                       event.loggerNameView(),
                                       location,
                                       event.message);
#else
        std::string text = std::format("{} [{}{:<5}{}] [{}] {}",
                                       timestampText,
                                       LogColor::colorForLevel(event.level),
                                       logLevelToString(event.level),
                                       LogColor::kReset,
                                       event.loggerNameView(),
                                       event.message);
#endif
        // 与 DefaultFormatter 一致：栈的解析在 Sink 写入线程上发生
        appendStackTraceText(text, event.stackTrace);
        return text;
    }
} // namespace AsynGyanis::Base
