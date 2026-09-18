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
        nlohmann::json fields = nlohmann::json::object();
        fields["timestamp"]   = event.timestamp;

        // 等级名去掉尾部空格（文本版式靠 {:<5} 对齐，JSON 里只是噪声，会让按精确值取用的采集端踩空）
        std::string_view levelName{logLevelToString(event.level)};
        while (!levelName.empty() && levelName.back() == ' ')
        {
            levelName.remove_suffix(1);
        }
        fields["level"] = std::string(levelName);

        // 名字为空时省略该键，而不是每条日志带一个空字段
        if (const std::string_view loggerName = event.loggerNameView(); !loggerName.empty())
        {
            fields["logger"] = std::string(loggerName);
        }

        fields["thread"]  = std::string(event.threadIdView());
        fields["message"] = event.message;

        // 带栈的事件：栈作为独立字段（多帧文本），换行由 JSON 序列化转义；解析在 Sink 写入线程上发生
        if (!event.stackTrace.empty())
        {
            fields["stackTrace"] = formatStackTrace(event.stackTrace);
        }

#ifdef ASYN_DEBUG
        // 源码位置只在 Debug 出现，与文本格式化器同一口径
        fields["file"]     = std::string(event.location.shortFileName());
        fields["line"]     = static_cast<std::int64_t>(event.location.line);
        fields["function"] = std::string(event.location.functionName != nullptr ? event.location.functionName : "");
#endif

        try
        {
            // 紧凑单行（不带缩进参数即紧凑模式）：采集端按行切分，缩进只会撑大日志体积。
            // 原生序列化按严格 UTF-8 校验，合法 UTF-8 原样写出、NUL 转义为 \u0000
            return fields.dump();
        } catch (const nlohmann::json::exception &exception)
        {
            // 非法 UTF-8 会让 dump() 失败：宁可整条日志失败并说明原因，也不写出携带乱码字节的
            // 「合法 JSON」，否则采集端会在更靠后的环节拿到静默变形的日志
            throw Exception("JsonFormatter：日志消息含非法 UTF-8 字节，无法输出合法 JSON：" + std::string(exception.what()));
        }
    }
} // namespace AsynGyanis::Base
