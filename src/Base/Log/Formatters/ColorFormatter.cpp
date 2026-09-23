#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/Detail/PlainTextLogLine.h"
#include "Base/Log/LogColor.h"

#include <string>

namespace AsynGyanis::Base
{
    void ColorFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 与 DefaultFormatter 共用同一套版式，差别只在等级两侧夹了 ANSI 颜色码；
        // 复位码要等等级字段补齐之后才能收尾，否则宽度会算进转义序列
        Detail::appendPlainTextLogLine(out, event, LogColor::colorForLevel(event.level), LogColor::kReset);
    }

    std::string ColorFormatter::format(const LogEvent &event)
    {
        std::string text;
        formatInto(text, event);
        return text;
    }
} // namespace AsynGyanis::Base
