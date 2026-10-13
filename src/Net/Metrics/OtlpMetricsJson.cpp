#include "Net/Metrics/OtlpMetricsJson.h"

#include "Base/Config/ConfigValue.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 墙钟时刻折成自 Unix 纪元起的纳秒文本
         * @details 用 floor 而不是 duration_cast：后者对 time_point::min 这类极端值会溢出成负数
         *          （与链路出口同一条换算）
         */
        std::string unixNanoTextOf(const std::chrono::system_clock::time_point &moment)
        {
            return std::to_string(std::chrono::floor<std::chrono::nanoseconds>(moment.time_since_epoch()).count());
        }

        /// @brief 一段文本折成 OTLP 的 AnyValue（stringValue 那一支）
        Base::ConfigValue textValueOf(std::string_view text)
        {
            Base::ConfigValue value = Base::ConfigValue::object();
            value["stringValue"]    = text;
            return value;
        }

        /// @brief 一个键值对折成 OTLP 的 KeyValue
        Base::ConfigValue keyValueOf(std::string_view key, Base::ConfigValue value)
        {
            Base::ConfigValue entry = Base::ConfigValue::object();
            entry["key"]            = key;
            entry["value"]          = std::move(value);
            return entry;
        }

        /// @brief 一条读数折成 OTLP 的 Metric 对象
        Base::ConfigValue metricObjectOf(const Core::ProcessMetricSample &sample, std::string_view timeUnixNano)
        {
            Base::ConfigValue point = Base::ConfigValue::object();
            point["asInt"]          = std::to_string(sample.value);
            point["timeUnixNano"]   = timeUnixNano;

            Base::ConfigValue dataPoints = Base::ConfigValue::array({std::move(point)});

            Base::ConfigValue metric = Base::ConfigValue::object();
            metric["name"]           = sample.name;
            metric["description"]    = sample.help;

            if (sample.kind == Core::ProcessMetricKind::Counter)
            {
                // 计数器：注册表交的是自进程启动起的累计值，折成单调的 CUMULATIVE sum
                // （AggregationTemporality 的取值按 OTLP 编号：UNSPECIFIED=0、DELTA=1、CUMULATIVE=2）
                Base::ConfigValue sum         = Base::ConfigValue::object();
                sum["dataPoints"]             = std::move(dataPoints);
                sum["aggregationTemporality"] = 2;
                sum["isMonotonic"]            = true;
                metric["sum"]                 = std::move(sum);
            } else
            {
                Base::ConfigValue gauge = Base::ConfigValue::object();
                gauge["dataPoints"]     = std::move(dataPoints);
                metric["gauge"]         = std::move(gauge);
            }
            return metric;
        }
    } // namespace

    std::string formatOtlpMetricsJson(const TraceResource &resource, const std::vector<Core::ProcessMetricSample> &samples, std::chrono::system_clock::time_point collectionMoment)
    {
        const std::string timeUnixNano = unixNanoTextOf(collectionMoment);

        Base::ConfigValue scope = Base::ConfigValue::object();
        scope["name"]           = kOtlpMetricsScopeName;

        Base::ConfigValue metricArray = Base::ConfigValue::array();
        for (const Core::ProcessMetricSample &sample: samples)
        {
            metricArray.push_back(metricObjectOf(sample, timeUnixNano));
        }

        Base::ConfigValue scopeMetrics = Base::ConfigValue::object();
        scopeMetrics["scope"]          = std::move(scope);
        scopeMetrics["metrics"]        = std::move(metricArray);

        // 出处只报 service.name / service.version 两项语义约定键：其余 resource 属性各有自己的
        // 采集口径，在这里硬塞一份反而会与 Collector 侧的 enrichment 撞键（与链路出口同口径）
        Base::ConfigValue resourceAttributes = Base::ConfigValue::array();
        resourceAttributes.push_back(keyValueOf("service.name", textValueOf(resource.serviceName)));
        if (!resource.serviceVersion.empty())
        {
            resourceAttributes.push_back(keyValueOf("service.version", textValueOf(resource.serviceVersion)));
        }

        Base::ConfigValue resourceObject = Base::ConfigValue::object();
        resourceObject["attributes"]     = std::move(resourceAttributes);

        Base::ConfigValue resourceMetrics = Base::ConfigValue::object();
        resourceMetrics["resource"]       = std::move(resourceObject);
        resourceMetrics["scopeMetrics"]   = Base::ConfigValue::array({std::move(scopeMetrics)});

        Base::ConfigValue document  = Base::ConfigValue::object();
        document["resourceMetrics"] = Base::ConfigValue::array({std::move(resourceMetrics)});

        // 交回空（非法 UTF-8、非有限浮点）时给的是空串：调用方按「这次导出失败」处置，不去猜哪一条坏了
        return Base::serializeConfigValue(document).value_or(std::string{});
    }
} // namespace AsynGyanis::Net
