#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/Detail/PlainTextLogLine.h"

#include <string>

namespace AsynGyanis::Base
{
    void DefaultFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        // 版式只在一处定义（见 PlainTextLogLine.h）：本类是它的无色用法，等级两侧不放修饰码
        Detail::appendPlainTextLogLine(out, event, std::string_view{}, std::string_view{});
    }

    std::string DefaultFormatter::format(const LogEvent &event)
    {
        std::string text;
        formatInto(text, event);
        return text;
    }
} // namespace AsynGyanis::Base
