#include "Net/Metrics/OtlpMetricsJson.h"

#include "Net/Telemetry/OtlpJsonSupport.h"
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
        using Telemetry::keyValueOf;
        using Telemetry::textValueOf;
        using Telemetry::unixNanoTextOf;

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

        /// @brief 一条带维度读数折成 OTLP 的 Metric（多数据点，每点带 attributes）
        Base::ConfigValue labeledMetricObjectOf(const OtlpLabeledSample &sample, std::string_view timeUnixNano)
        {
            Base::ConfigValue dataPoints = Base::ConfigValue::array();
            for (const OtlpLabeledPoint &point: sample.points)
            {
                Base::ConfigValue pointObject = Base::ConfigValue::object();
                pointObject["asInt"]          = std::to_string(point.value);
                pointObject["timeUnixNano"]   = timeUnixNano;

                Base::ConfigValue attributes = Base::ConfigValue::array();
                for (const auto &[key, value]: point.attributes)
                {
                    attributes.push_back(keyValueOf(key, textValueOf(value)));
                }
                pointObject["attributes"] = std::move(attributes);
                dataPoints.push_back(std::move(pointObject));
            }

            Base::ConfigValue metric = Base::ConfigValue::object();
            metric["name"]           = sample.name;
            metric["description"]    = sample.help;
            if (sample.kind == Core::ProcessMetricKind::Counter)
            {
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

        /**
         * @brief 一条分布读数折成 OTLP 的 Metric（histogram 数据点）
         * @details 64 位计数按 proto3 JSON mapping 的规矩写成字符串（`count` 与 `bucketCounts`
         *          的元素都是 uint64；`sum` 与 `explicitBounds` 是 double，保持数字形态）；
         *          桶是「每格条数」不累计、末格溢出，与产出方同形、渲染层不做换算。
         */
        Base::ConfigValue histogramMetricObjectOf(const OtlpHistogramSample &sample, std::string_view timeUnixNano)
        {
            const auto pointObjectOf = [&sample, &timeUnixNano](const std::vector<std::pair<std::string, std::string>> &pointAttributes,
                                                                const std::vector<std::uint64_t> &pointBucketCounts, const std::uint64_t pointCount, const double pointSumSeconds)
            {
                Base::ConfigValue point = Base::ConfigValue::object();
                point["count"]          = std::to_string(pointCount);
                point["sum"]            = pointSumSeconds;

                Base::ConfigValue bucketCounts = Base::ConfigValue::array();
                for (const std::uint64_t bucketCount: pointBucketCounts)
                {
                    bucketCounts.push_back(std::to_string(bucketCount));
                }
                point["bucketCounts"] = std::move(bucketCounts);

                Base::ConfigValue explicitBounds = Base::ConfigValue::array();
                for (const double upperBound: sample.explicitBoundsSeconds)
                {
                    explicitBounds.push_back(upperBound);
                }
                point["explicitBounds"] = std::move(explicitBounds);
                point["timeUnixNano"]   = timeUnixNano;

                Base::ConfigValue attributes = Base::ConfigValue::array();
                for (const auto &[key, value]: pointAttributes)
                {
                    attributes.push_back(keyValueOf(key, textValueOf(value)));
                }
                if (!pointAttributes.empty())
                {
                    point["attributes"] = std::move(attributes);
                }
                return point;
            };

            Base::ConfigValue dataPoints = Base::ConfigValue::array();
            if (!sample.points.empty())
            {
                // 带维度的分布：一条 metric、多个数据点，每点自带 attributes（如 route）
                for (const OtlpHistogramPoint &point: sample.points)
                {
                    dataPoints.push_back(pointObjectOf(point.attributes, point.bucketCounts, point.count, point.sumSeconds));
                }
            } else
            {
                // 单点模式：沿用无属性的那一份字段（旧调用方的兼容路径）
                dataPoints.push_back(pointObjectOf({}, sample.bucketCounts, sample.count, sample.sumSeconds));
            }

            Base::ConfigValue histogram         = Base::ConfigValue::object();
            histogram["dataPoints"]             = std::move(dataPoints);
            histogram["aggregationTemporality"] = 2; // 统计快照是自启动起的累计值，折成 CUMULATIVE

            Base::ConfigValue metric = Base::ConfigValue::object();
            metric["name"]           = sample.name;
            metric["description"]    = sample.help;
            metric["unit"]           = "s"; // sum 与 explicitBounds 都已换算成秒（与 Prometheus 渲染同一口径）
            metric["histogram"]      = std::move(histogram);
            return metric;
        }
    } // namespace

    std::string formatOtlpMetricsJson(const TraceResource &resource, const OtlpMetricsBatch &batch, std::chrono::system_clock::time_point collectionMoment)
    {
        const std::string timeUnixNano = unixNanoTextOf(collectionMoment);

        Base::ConfigValue scope = Base::ConfigValue::object();
        scope["name"]           = kOtlpMetricsScopeName;

        // 标量一路
        Base::ConfigValue metricArray = Base::ConfigValue::array();
        for (const Core::ProcessMetricSample &sample: batch.scalars)
        {
            metricArray.push_back(metricObjectOf(sample, timeUnixNano));
        }
        // 带维度一路：一条 metric、多数据点、每点带 attributes
        for (const OtlpLabeledSample &labeledSample: batch.labeled)
        {
            metricArray.push_back(labeledMetricObjectOf(labeledSample, timeUnixNano));
        }
        // 分布一路
        for (const OtlpHistogramSample &histogramSample: batch.histograms)
        {
            metricArray.push_back(histogramMetricObjectOf(histogramSample, timeUnixNano));
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

    std::string formatOtlpMetricsJson(const TraceResource &resource, const std::vector<Core::ProcessMetricSample> &samples, std::chrono::system_clock::time_point collectionMoment)
    {
        return formatOtlpMetricsJson(resource, OtlpMetricsBatch{.scalars = samples}, collectionMoment);
    }
} // namespace AsynGyanis::Net
