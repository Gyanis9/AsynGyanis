#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogLevel.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 除消息体之外那截框架的开销余量：时间戳文本、级别名、线程与日志器名连同分隔符的上界。
        /// 消息通常才是整条 JSON 的主要长度，按它加一份就够第一轮不必重分配
        constexpr std::size_t kJsonFrameOverheadBytes = 128U;

        /**
         * @brief 按事件的字段表组装 JSON 对象
         * @param event 日志事件
         * @return nlohmann::json 待序列化的对象（键序由 nlohmann 的对象容器按字典序落定）
         */
        nlohmann::json buildFields(const LogEvent &event)
        {
            // 时刻在本线程渲染成与文本版式同一口径的字符串：整条 JSON 本来就要落一份文本，
            // 这里的一次拷贝不参与事件产生线程的成本
            std::array<char, kTimestampTextBufferSize> timestampBuffer{};
            const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);

            nlohmann::json fields = nlohmann::json::object();
            fields["timestamp"]   = std::string{timestampText};

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
            return fields;
        }

        /**
         * @brief 用 nlohmann 的序列化器把对象直接追加进目标缓冲
         * @details 函数体照搬 `basic_json::dump()`（紧凑模式、ensure_ascii=false、strict 错误处理），
         *          只把出口从它内部新建的临时串换成调用方的缓冲：转义表、键序与非法 UTF-8 的抛点
         *          都由同一份序列化代码负责，因此产物与 `dump()` 逐字节相同。
         * @param out 目标缓冲，已有内容保留
         * @param fields 待序列化的对象
         */
        void dumpInto(std::string &out, const nlohmann::json &fields)
        {
            // 取库的 detail 命名空间是因为 `dump()` 没有「追加到既有缓冲」的公开出口；
            // 版本由 Conan 锁住，接口真漂移时是编译不过，不会静默改变日志内容
            nlohmann::detail::serializer<nlohmann::json> emitter{nlohmann::detail::output_adapter<char, std::string>(out),
                                                                ' ',
                                                                nlohmann::json::error_handler_t::strict};
            emitter.dump(fields, false, false, 0);
        }
    } // namespace

    void JsonFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        const nlohmann::json fields      = buildFields(event);
        const std::size_t    enteredSize = out.size();
        // 先按消息长度留一次容量：Sink 的行缓冲跨行留着不缩，稳态下这一段不碰堆；
        // 全新缓冲也只走这一次分配，而不是逐字符几何增长重分配七八次
        out.reserve(enteredSize + event.message.size() + kJsonFrameOverheadBytes);
        try
        {
            // 原生序列化按严格 UTF-8 校验，合法 UTF-8 原样写出、NUL 转义为 \u0000
            dumpInto(out, fields);
        } catch (const nlohmann::json::exception &exception)
        {
            // 非法 UTF-8 会让序列化中途失败：宁可整条日志失败并说明原因，也不写出携带乱码字节的
            // 「合法 JSON」，否则采集端会在更靠后的环节拿到静默变形的日志。
            // 擦回入口长度是保住「要么整条、要么一个字都不加」这条既有语义——半途而废的半条 JSON
            // 不能留在调用方的缓冲里
            out.resize(enteredSize);
            throw Exception("JsonFormatter：日志消息含非法 UTF-8 字节，无法输出合法 JSON：" + std::string(exception.what()));
        }
    }

    std::string JsonFormatter::format(const LogEvent &event)
    {
        std::string text;
        formatInto(text, event);
        return text;
    }
} // namespace AsynGyanis::Base
