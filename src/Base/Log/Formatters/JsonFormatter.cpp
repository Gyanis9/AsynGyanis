#include "Base/Log/Formatters/JsonFormatter.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogContext.h"
#include "Base/Log/LogLevel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
         * @brief 是否为日志骨架自己的键（上下文撞名时要加前缀，不能覆盖它们）
         * @param key 待判定的键名
         * @return true 表示撞上了框架字段
         */
        bool isReservedLogField(const std::string_view key)
        {
            static constexpr std::array<std::string_view, 10> kReservedFields{"timestamp",  "level", "logger", "thread",   "message",
                                                                              "stackTrace", "file",  "line",   "function", "context"};
            return std::ranges::find(kReservedFields, key) != kReservedFields.end();
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
         * @brief 用已有的键名串在复用对象上落一个字符串字段
         * @details 查找直接复用调用方手里那份键名串，不为长键再造临时 std::string：
         *          上下文键名本来就在跨行缓冲里，交给 `const char *` 重载会先构造一份临时串。
         * @param fields 复用的字段对象
         * @param fieldName 键名（由调用方持有，生命周期覆盖本次调用）
         * @param value 字段文本
         */
        void assignField(nlohmann::json &fields, const std::string &fieldName, const std::string_view value)
        {
            const auto iterator = fields.find(fieldName);
            if (iterator == fields.end())
            {
                fields[fieldName] = value;
                return;
            }
            nlohmann::json &slot = iterator.value();
            if (slot.is_string())
            {
                slot.get_ref<std::string &>() = value;
                return;
            }
            slot = value;
        }

        /**
         * @brief 上下文键名的跨行复用缓冲
         * @details previous 记录上一行写进 JSON 对象的键名，next 是为这一行重建的键名。
         *          两个向量互换而不清空：清空会销毁串、丢掉容量，下一行又要为每个键重新取堆。
         *          字符串按位置赋值复用缓冲；键数变少时多余的元素随 resize 销毁，向量容量仍留给后面的行。
         *          复用对象不会自己忘掉键：previous 与 next 的集差就是本行要摘掉的旧键，
         *          否则没有上下文的下一条日志会带着上一条的 trace_id，看起来像「链路串了」。
         */
        struct ContextKeyScratch
        {
            std::vector<std::string> previous; ///< 上一行写进对象的上下文键名
            std::vector<std::string> next;     ///< 本行要写入的上下文键名，字符串缓冲跨行复用
        };

        /**
         * @brief 取本线程的上下文键名缓冲
         * @return ContextKeyScratch& 线程局域的两份键名表
         */
        ContextKeyScratch &contextKeyScratch()
        {
            static thread_local ContextKeyScratch scratch;
            return scratch;
        }

        /**
         * @brief 把键名写进复用缓冲，必要时继续加前缀
         * @details 先按原始键名赋值，容量够时原地改写；对保留字与本次已分配的名字反复加
         *          ctx. 前缀。只看一次保留字不够：上下文里同时有 message 与 ctx.message 时，
         *          前者的前缀结果正好是后者，两条会落到同一个 JSON 键上。
         * @param fieldName 复用的键名缓冲
         * @param key 上下文里的原始键名
         * @param usedNames 本行已分配的键名表
         * @param usedCount usedNames 里有效的个数（只有前这么多个是本行已经分配的）
         */
        void assignContextFieldName(std::string &fieldName, const std::string_view key, const std::vector<std::string> &usedNames, const std::size_t usedCount)
        {
            fieldName.assign(key);
            const auto usedEnd = usedNames.begin() + static_cast<std::ptrdiff_t>(usedCount);
            while (isReservedLogField(fieldName) || std::find(usedNames.begin(), usedEnd, fieldName) != usedEnd)
            {
                fieldName.insert(0, "ctx.");
            }
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
         * @brief 用已有的键名串摘掉一个可选键
         * @details 直接复用调用方持有的键名串，不为摘除再造临时 std::string；换到 `const char *`
         *          重载会在内嵌 NUL 处截断，旧键摘不掉，还可能误摘同前缀的另一个键。
         * @param fields 复用的字段对象
         * @param fieldName 待摘除的键名（由调用方持有，生命周期覆盖本次调用）
         */
        void removeField(nlohmann::json &fields, const std::string &fieldName)
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

            // 时刻在本线程渲染成与文本版式同一口径的字符串：整条 JSON 本就要落一份文本，这次拷贝不参与产生线程的成本
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
            assignField(fields, "function", event.location.functionName != nullptr ? std::string_view(event.location.functionName) : std::string_view());
#else
            // Release 里没有这三个键；复用的对象也不会有（同一进程内 #ifdef 的形态是常量）
#endif

            // 上下文：顶层键，与原有字段并列。键名按名字重建、同名键只更新取值；上一行有而本行没有的键才摘除。
            // 撞上框架字段或已分配的上下文键都继续加 `ctx.` 前缀，上下文既不能改写日志骨架，也不能与另一条上下文落到同一个键上
            ContextKeyScratch &scratch      = contextKeyScratch();
            std::size_t        currentCount = 0U;
            // 优先读事件自带的快照：异步 Sink 的工作线程上 TLS 是空的，读它会丢掉日志点的上下文
            const LogContextPtr &context = event.context ? event.context : currentLogContext();
            try
            {
                if (context != nullptr)
                {
                    for (const auto &[key, value]: context->fields())
                    {
                        if (currentCount == scratch.next.size())
                        {
                            scratch.next.emplace_back();
                        }
                        std::string &fieldName = scratch.next[currentCount];
                        assignContextFieldName(fieldName, key, scratch.next, currentCount);
                        // 找键时复用这份键名串：长键不必每行再为查找造一份临时串
                        assignField(fields, fieldName, value);
                        ++currentCount;
                    }
                }
            } catch (...)
            {
                // 失败行已经写进复用对象的键必须摘掉：previous 仍是上一行的集合，
                // 不摘就会在下一条无上下文日志里留下这一行的幽灵字段。抛错的键可能
                // 只写了一半，也一并尝试摘除；若该槽位还留着上一行的名字，多摘一次是
                // 自愈的——下一条会重建框架字段与本行上下文。erase 不分配，兜底不会再抛
                const std::size_t writtenCount = currentCount < scratch.next.size() ? currentCount + 1U : currentCount;
                const auto        writtenEnd   = scratch.next.begin() + static_cast<std::ptrdiff_t>(writtenCount);
                for (auto iterator = scratch.next.begin(); iterator != writtenEnd; ++iterator)
                {
                    removeField(fields, *iterator);
                }
                throw;
            }

            // 旧键按集差摘除：键集合没变时这里一次 erase 都不做，JSON 节点原地保留
            const auto nextEnd = scratch.next.begin() + static_cast<std::ptrdiff_t>(currentCount);
            for (const std::string &staleKey: scratch.previous)
            {
                if (std::find(scratch.next.begin(), nextEnd, staleKey) == nextEnd)
                {
                    // 摘除同样复用键名串：经 c_str() 会在内嵌 NUL 处截断，还会先造一份临时串
                    removeField(fields, staleKey);
                }
            }
            scratch.next.resize(currentCount);
            scratch.previous.swap(scratch.next);
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
            nlohmann::detail::serializer<nlohmann::json> emitter{nlohmann::detail::output_adapter<char, std::string>(out), ' ', nlohmann::json::error_handler_t::strict};
            emitter.dump(fields, false, false, 0);
        }
    } // namespace

    void JsonFormatter::formatInto(std::string &out, const LogEvent &event)
    {
        nlohmann::json   &fields      = fillFields(event);
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
