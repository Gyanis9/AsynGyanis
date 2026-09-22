#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/PaddedFieldText.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogColor.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 除变长字段之外那截框架的开销：时刻文本 23 格、等级 5 格、方括号与分隔空格若干
        constexpr std::size_t kColorLineFrameOverheadBytes = 40U;
    } // namespace

    void ColorFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 与 DefaultFormatter 同一套时刻渲染：写进栈缓冲，本函数的两条版式分支只走其中一条
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
        const std::string_view                     levelText{logLevelToString(event.level)};
        const std::string_view                     levelColor   = LogColor::colorForLevel(event.level);
#ifdef ASYN_DEBUG
        // 与 DefaultFormatter 同一套源码位置文本（见 formatSourceLocationText）
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        // 逐字段追加（同 DefaultFormatter）：字段顺序与分隔符就是原来的格式串
        // "{} {} [{}{:<5}{}] [{}] {:<13} {}"，颜色码只夹在等级两侧，不参与补齐
        out.reserve(out.size() + kColorLineFrameOverheadBytes + levelColor.size() + event.threadIdView().size() +
                    event.loggerNameView().size() + location.size() + event.message.size());
        out.append(timestampText);
        out.push_back(' ');
        out.append(event.threadIdView());
        out.append(" [");
        out.append(levelColor);
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append(LogColor::kReset);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        appendPaddedField(out, location, kSourceLocationFieldWidth);
        out.push_back(' ');
        out.append(event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        out.reserve(out.size() + kColorLineFrameOverheadBytes + levelColor.size() + event.loggerNameView().size() +
                    event.message.size());
        out.append(timestampText);
        out.append(" [");
        out.append(levelColor);
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append(LogColor::kReset);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        out.append(event.message);
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
