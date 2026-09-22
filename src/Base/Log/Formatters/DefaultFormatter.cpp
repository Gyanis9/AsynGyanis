#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/PaddedFieldText.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
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
        constexpr std::size_t kTextLineFrameOverheadBytes = 40U;
    } // namespace

    void DefaultFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 时刻在本线程就地渲染成文本：一块栈缓冲，不取堆。两条版式分支只走其中一条，
        // 因此一次渲染一份缓冲就够
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
        const std::string_view                     levelText{logLevelToString(event.level)};
#ifdef ASYN_DEBUG
        // 源码位置经共用工具生成：短「文件:行号」写进栈缓冲，装不下才回退到会分配的路径
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        // 逐字段追加而不是 std::format_to + back_insert_iterator：后者要一个字符一个字符地喂给
        // 输出迭代器并当场解析格式串，整行下来比按字段 memcpy 慢数倍（实测 301 → 见微基准）。
        // 字段顺序与分隔符就是原来的格式串 "{} {} [{:<5}] [{}] {:<13} {}"
        out.reserve(out.size() + kTextLineFrameOverheadBytes + event.threadIdView().size() +
                    event.loggerNameView().size() + location.size() + event.message.size());
        out.append(timestampText);
        out.push_back(' ');
        out.append(event.threadIdView());
        out.append(" [");
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        appendPaddedField(out, location, kSourceLocationFieldWidth);
        out.push_back(' ');
        out.append(event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        out.reserve(out.size() + kTextLineFrameOverheadBytes + event.loggerNameView().size() + event.message.size());
        out.append(timestampText);
        out.append(" [");
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        out.append(event.message);
#endif
        // 栈的符号解析在这里发生：本函数的调用方（Sink）决定它跑在哪个线程上
        appendStackTraceText(out, event.stackTrace);
    }

    std::string DefaultFormatter::format(const LogEvent &event)
    {
        std::string text;
        formatInto(text, event);
        return text;
    }
} // namespace AsynGyanis::Base
