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
         * @brief 本线程复用的字段对象
         * @details 一条 JSON 行的键形状固定就是那五到八对，每条重新建要占掉整行分配里的大头
         *          （实测一千行 16000 次）。留着复用的代价是每线程一份小对象，外加「历史上最长
         *          的那条消息」的串缓冲不还给分配器——不随日志量增长，只随单条长度上界增长。
         */
        nlohmann::json &reusableFields()
        {
            static thread_local nlohmann::json fields = nlohmann::json::object();
            return fields;
        }

        /**
         * @brief 往复用对象上落一个字符串字段
         * @details 槽位已是字符串时原地赋值，容量够用就不重新取堆（时间戳长度固定，日志器名、
         *          线程 id 与消息在同一线程上反复取同一长度）；不是字符串（本线程第一次用这一项）
         *          才整体替换成新的值。
         * @param fields 复用的字段对象
         * @param fieldName 键名
         * @param value 字段文本
         */
        void assignField(nlohmann::json &fields, const char *fieldName, const std::string_view value)
        {
            nlohmann::json &slot = fields[fieldName];
            if (slot.is_string())
            {
                slot.get_ref<std::string &>() = value;
                return;
            }
            slot = value;
        }

        /**
         * @brief 摘掉这一条不该出现的可选键
         * @details 复用对象意味着上一条的键会留在原地；可选键必须显式摘除，否则会出现
         *          「根日志器的行里带着上一个 logger 的名字」这类串味。
         * @param fields 复用的字段对象
         * @param fieldName 待摘除的键名
         */
        void removeField(nlohmann::json &fields, const char *fieldName)
        {
            static_cast<void>(fields.erase(fieldName));
        }

        /**
         * @brief 把事件字段填进复用的 JSON 对象
         * @param event 日志事件
         * @return nlohmann::json& 线程局域的复用对象（键序由 nlohmann 的对象容器按字典序落定）
         */
        nlohmann::json &fillFields(const LogEvent &event)
        {
            nlohmann::json &fields = reusableFields();

            // 时刻在本线程渲染成与文本版式同一口径的字符串：整条 JSON 本来就要落一份文本，
            // 这里的一次拷贝不参与事件产生线程的成本
            std::array<char, kTimestampTextBufferSize> timestampBuffer{};
            const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
            assignField(fields, "timestamp", timestampText);

            // 等级名去掉尾部空格（文本版式靠 {:<5} 对齐，JSON 里只是噪声，会让按精确值取用的采集端踩空）
            std::string_view levelName{logLevelToString(event.level)};
            while (!levelName.empty() && levelName.back() == ' ')
            {
                levelName.remove_suffix(1);
            }
            assignField(fields, "level", levelName);

            // 名字为空时省略该键，而不是每条日志带一个空字段——省略在复用对象上要落到 erase，
            // 否则留着的是上一条的名字
            if (const std::string_view loggerName = event.loggerNameView(); !loggerName.empty())
            {
                assignField(fields, "logger", loggerName);
            } else
            {
                removeField(fields, "logger");
            }

            assignField(fields, "thread", event.threadIdView());
            assignField(fields, "message", event.message);

            // 带栈的事件：栈作为独立字段（多帧文本），换行由 JSON 序列化转义；解析在 Sink 写入线程上发生
            if (!event.stackTrace.empty())
            {
                assignField(fields, "stackTrace", formatStackTrace(event.stackTrace));
            } else
            {
                removeField(fields, "stackTrace");
            }

#ifdef ASYN_DEBUG
            // 源码位置只在 Debug 出现，与文本格式化器同一口径
            assignField(fields, "file", event.location.shortFileName());
            fields["line"] = static_cast<std::int64_t>(event.location.line);
            assignField(fields,
                        "function",
                        event.location.functionName != nullptr ? std::string_view(event.location.functionName) : std::string_view());
#else
            // Release 里没有这三个键；复用的对象也不会有（同一进程内 #ifdef 的形态是常量）
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
        nlohmann::json &fields      = fillFields(event);
        const std::size_t enteredSize = out.size();
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
