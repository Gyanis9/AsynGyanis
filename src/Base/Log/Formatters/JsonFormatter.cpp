#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Format/Json/JsonWriteOptions.h"
#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/Value/FormatValue.h"
#include "Base/Log/LogLevel.h"

#include <cstdint>
#include <string>

namespace AsynGyanis::Base
{
    std::string JsonFormatter::format(const LogEvent &event)
    {
        // 键放进 std::map：JSON 对象的键序由它决定（字典序），与采集端无关，但稳定可比对
        FormatValueObject fields;
        fields.emplace("timestamp", FormatValue(event.timestamp));

        // 等级名去掉尾部空格：文本版式靠 {:<5} 补到 5 列对齐，JSON 里那只是噪声，
        // 留着会让采集端按「INFO 加空格」精确取值时踩空
        std::string_view levelName{logLevelToString(event.level)};
        while (!levelName.empty() && levelName.back() == ' ')
        {
            levelName.remove_suffix(1);
        }
        fields.emplace("level", FormatValue(std::string(levelName)));

        // 根记录器的名字为空：省略该键而不是写一个空串，免得每条日志都带一个没信息量的字段
        if (const std::string_view loggerName = event.loggerNameView(); !loggerName.empty())
        {
            fields.emplace("logger", FormatValue(std::string(loggerName)));
        }

        fields.emplace("thread", FormatValue(event.threadId));
        fields.emplace("message", FormatValue(event.message));

#ifdef ASYN_DEBUG
        // 源码位置只在 Debug 构建里出现：与 DefaultFormatter 保持同一口径，
        // Release 下这些字段既不采集也不写出
        fields.emplace("file", FormatValue(std::string(event.location.shortFileName())));
        fields.emplace("line", FormatValue(static_cast<std::int64_t>(event.location.line)));
        fields.emplace("function", FormatValue(std::string(event.location.functionName != nullptr ? event.location.functionName : "")));
#endif

        // 紧凑单行：采集端按行切分，缩进只会白白撑大日志体积
        JsonWriteOptions options;
        options.indentWidth = 0;
        return JsonWriter::write(FormatValue(std::move(fields)), options);
    }
} // namespace AsynGyanis::Base
