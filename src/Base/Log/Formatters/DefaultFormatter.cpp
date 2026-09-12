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
        // 「文件:行号」先写进栈上缓冲：嵌套 std::format 既要多跑一次格式化，又让每行
        // 多出一次堆分配（实测每行分配 2 次，其中一次就来自这段临时串）。
        // 调用共用的 tryFormatSourceLocationText：命中就直接以返回的视图参与外层格式化，
        // 未命中（空视图）才回退到原来的分配路径。两条路径使用同一个 format_to_n 格式串
        // "{}:{}"，所以文本内容与长度必然逐字节相同（含 {:<13} 用空格补齐到 13 列），
        // 差别仅在内存来源，让「命中」与「回退」对输出不可见
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        const std::string_view locationText = tryFormatSourceLocationText(event.location, locationBuffer);
        const std::string overflowLocation = locationText.empty()
                                                 ? std::format("{}:{}", event.location.shortFileName(), event.location.line)
                                                 : std::string();
        const std::string_view location = locationText.empty() ? std::string_view(overflowLocation) : locationText;

        return std::format("{} {} [{:<5}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadId,
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
