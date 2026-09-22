#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    void DefaultFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 时刻在本线程就地渲染成文本：一块栈缓冲，不取堆。两条版式分支只走其中一条，
        // 因此一次渲染一份缓冲就够
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
#ifdef ASYN_DEBUG
        // 源码位置经共用工具生成：短「文件:行号」写进栈缓冲，装不下才回退到会分配的路径
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        // 写进调用方的缓冲而不是 std::format 造一个新串：后者交回结果要取两次堆，
        // 而 Sink 那侧本来就有留了容量的行缓冲，追加进它是零次
        std::format_to(std::back_inserter(out), "{} {} [{:<5}] [{}] {:<13} {}",
                       timestampText,
                       event.threadIdView(),
                       logLevelToString(event.level),
                       event.loggerNameView(),
                       location,
                       event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        std::format_to(std::back_inserter(out), "{} [{:<5}] [{}] {}",
                       timestampText,
                       logLevelToString(event.level),
                       event.loggerNameView(),
                       event.message);
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
