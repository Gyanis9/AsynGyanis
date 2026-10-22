#include "Net/Tracing/OtlpTraceJson.h"

#include "Net/Telemetry/OtlpJsonSupport.h"
#include "Base/Config/ConfigValue.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using Telemetry::keyValueOf;
        using Telemetry::textValueOf;
        using Telemetry::unixNanoTextOf;

        /**
         * @brief 把一个维度取值折成 OTLP 的 AnyValue
         * @details 整数写成字符串形态的 intValue：proto3 的 JSON mapping 里 64 位整数就是字符串，
         *          各家 SDK 的解码器都按这个形状发，跟着发最省事。
         * @param value 维度取值
         * @return ConfigValue 形如 {"stringValue":"…"} 或 {"intValue":"…"}
         */
        Base::ConfigValue anyValueOf(const SpanAttributeValue &value)
        {
            return std::visit(
                    [](const auto &rawValue) -> Base::ConfigValue
                    {
                        using RawValue           = std::decay_t<decltype(rawValue)>;
                        Base::ConfigValue result = Base::ConfigValue::object();
                        if constexpr (std::is_same_v<RawValue, std::string>)
                        {
                            result["stringValue"] = rawValue;
                        } else
                        {
                            result["intValue"] = std::to_string(rawValue);
                        }
                        return result;
                    },
                    value);
        }
        /**
         * @brief 结束时刻 = 开始时刻的纳秒数 + 时长
         * @details 在纳秒整数上相加，而不是先做 time_point 相加再折纳秒：两个平台的 system_clock
         *          刻度不同（MSVC 是 100 ns、libstdc++ 是 1 ns），后者会先共同类型换算一次，
         *          白把这条加法绕远。
         */
        std::string endUnixNanoTextOf(const SpanRecord &record)
        {
            const auto startNanoseconds = std::chrono::floor<std::chrono::nanoseconds>(record.startMoment.time_since_epoch()).count();
            return std::to_string(startNanoseconds + record.duration.count());
        }

        /// @brief 一条节折成 OTLP 的 Span 对象
        Base::ConfigValue spanObjectOf(const SpanRecord &record)
        {
            Base::ConfigValue span = Base::ConfigValue::object();
            span["traceId"]        = record.identity.traceIdText();
            span["spanId"]         = record.identity.spanIdText();
            // 根节不带 parentSpanId：protojson 对缺省值一律省略字段，写一个空串反而会被解码器判成非法标识
            if (record.identity.hasParent())
            {
                span["parentSpanId"] = record.identity.parentSpanIdText();
            }
            span["name"] = record.name;
            // 枚举取数值发：SpanKind 的取值已与 OTLP 对齐（UNSPECIFIED 空出 0），不必再做一层映射表
            span["kind"] = std::to_underlying(record.kind);

            Base::ConfigValue attributes = Base::ConfigValue::array();
            for (const SpanAttribute &attribute: record.attributes)
            {
                attributes.push_back(keyValueOf(attribute.key, anyValueOf(attribute.value)));
            }
            span["attributes"] = std::move(attributes);

            span["startTimeUnixNano"]      = unixNanoTextOf(record.startMoment);
            span["endTimeUnixNano"]        = endUnixNanoTextOf(record);
            span["droppedAttributesCount"] = record.droppedAttributeCount;

            Base::ConfigValue status = Base::ConfigValue::object();
            status["code"]           = std::to_underlying(record.status);
            if (!record.statusMessage.empty())
            {
                status["message"] = record.statusMessage;
            }
            span["status"] = std::move(status);
            return span;
        }
    } // namespace

    std::string formatOtlpTracesJson(const TraceResource &resource, const std::vector<SpanRecord> &spans)
    {
        Base::ConfigValue scope = Base::ConfigValue::object();
        scope["name"]           = kTracingScopeName;

        Base::ConfigValue spanArray = Base::ConfigValue::array();
        for (const SpanRecord &record: spans)
        {
            spanArray.push_back(spanObjectOf(record));
        }

        Base::ConfigValue scopeSpans = Base::ConfigValue::object();
        scopeSpans["scope"]          = std::move(scope);
        scopeSpans["spans"]          = std::move(spanArray);

        // 出处只报 service.name / service.version 两项语义约定键：其余 resource 属性（主机名、进程号、
        // 部署环境）各有自己的采集口径，在这里硬塞一份反而会与 Collector 侧的 enrichment 撞键
        Base::ConfigValue resourceAttributes = Base::ConfigValue::array();
        resourceAttributes.push_back(keyValueOf("service.name", textValueOf(resource.serviceName)));
        if (!resource.serviceVersion.empty())
        {
            resourceAttributes.push_back(keyValueOf("service.version", textValueOf(resource.serviceVersion)));
        }

        Base::ConfigValue resourceObject = Base::ConfigValue::object();
        resourceObject["attributes"]     = std::move(resourceAttributes);

        Base::ConfigValue resourceSpan = Base::ConfigValue::object();
        resourceSpan["resource"]       = std::move(resourceObject);
        resourceSpan["scopeSpans"]     = Base::ConfigValue::array({std::move(scopeSpans)});

        Base::ConfigValue document = Base::ConfigValue::object();
        document["resourceSpans"]  = Base::ConfigValue::array({std::move(resourceSpan)});

        // 交回空（非法 UTF-8、非有限浮点）时给的是空串：调用方按「整批没收」处置，不去猜哪一条坏了
        return Base::serializeConfigValue(document).value_or(std::string{});
    }
} // namespace AsynGyanis::Net
