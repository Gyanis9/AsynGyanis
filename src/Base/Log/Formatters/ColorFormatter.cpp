#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogColor.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    void ColorFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 与 DefaultFormatter 同一套时刻渲染：写进栈缓冲，本函数的两条版式分支只走其中一条
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
#ifdef ASYN_DEBUG
        // 与 DefaultFormatter 同一套源码位置文本（见 formatSourceLocationText）
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        // 直接追加进调用方留有容量的缓冲：std::format 交回一个新串要取两次堆
        std::format_to(std::back_inserter(out), "{} {} [{}{:<5}{}] [{}] {:<13} {}",
                       timestampText,
                       event.threadIdView(),
                       LogColor::colorForLevel(event.level),
                       logLevelToString(event.level),
                       LogColor::kReset,
                       event.loggerNameView(),
                       location,
                       event.message);
#else
        std::format_to(std::back_inserter(out), "{} [{}{:<5}{}] [{}] {}",
                       timestampText,
                       LogColor::colorForLevel(event.level),
                       logLevelToString(event.level),
                       LogColor::kReset,
                       event.loggerNameView(),
                       event.message);
#endif
        // 与 DefaultFormatter 一致：栈的解析在 Sink 写入线程上发生
        appendStackTraceText(out, event.stackTrace);
    }

    std::string ColorFormatter::format(const LogEvent &event)
    {
        std::string text;
        formatInto(text, event);
        return text;
    }
} // namespace AsynGyanis::Base
