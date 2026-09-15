#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogLevel.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    std::string JsonFormatter::format(const LogEvent &event)
    {
        // 键写进 JSON 对象：object_t 是按键有序的映射，键序稳定、可逐字比对
        nlohmann::json fields = nlohmann::json::object();
        fields["timestamp"]   = event.timestamp;

        // 等级名去掉尾部空格：文本版式靠 {:<5} 补到 5 列对齐，JSON 里那只是噪声，
        // 留着会让采集端按「INFO 加空格」精确取值时踩空
        std::string_view levelName{logLevelToString(event.level)};
        while (!levelName.empty() && levelName.back() == ' ')
        {
            levelName.remove_suffix(1);
        }
        fields["level"] = std::string(levelName);

        // 根记录器的名字为空：省略该键而不是写一个空串，免得每条日志都带一个没信息量的字段
        if (const std::string_view loggerName = event.loggerNameView(); !loggerName.empty())
        {
            fields["logger"] = std::string(loggerName);
        }

        fields["thread"]  = event.threadId;
        fields["message"] = event.message;

#ifdef ASYN_DEBUG
        // 源码位置只在 Debug 构建里出现：与 DefaultFormatter 保持同一口径，
        // Release 下这些字段既不采集也不写出
        fields["file"]     = std::string(event.location.shortFileName());
        fields["line"]     = static_cast<std::int64_t>(event.location.line);
        fields["function"] = std::string(event.location.functionName != nullptr ? event.location.functionName : "");
#endif

        try
        {
            // 紧凑单行（不带缩进参数即是紧凑模式）：采集端按行切分，缩进只会白白撑大日志体积。
            // 原生序列化默认按严格 UTF-8 校验，合法 UTF-8 原样写出、NUL 转义为 \u0000
            return fields.dump();
        } catch (const nlohmann::json::exception &exception)
        {
            // 非法 UTF-8 会让 dump() 失败：宁可整条日志失败并说明原因，也不写出携带乱码字节的
            // 「合法 JSON」，否则采集端会在更靠后的环节拿到静默变形的日志
            throw Exception("JsonFormatter：日志消息含非法 UTF-8 字节，无法输出合法 JSON：" + std::string(exception.what()));
        }
    }
} // namespace AsynGyanis::Base
