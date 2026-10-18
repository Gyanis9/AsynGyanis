// OTLP/HTTP metrics JSON 渲染的用例：只判正文形状，不涉及任何传输
//
// 判据落在两处：Counter 与 Gauge 折成的两种数据点形状（sum/gauge、isMonotonic、聚合时序），
// 以及 64 位取值与时间必须是字符串形态——与本仓链路出口同一条 JSON mapping 口径。

#include "Net/Metrics/OtlpMetricsJson.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "ExpectedAssertions.h"
#include "Net/Tracing/SpanExporter.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using AsynGyanis::Base::parseConfigValue;
    using AsynGyanis::Core::ProcessMetricKind;
    using AsynGyanis::Core::ProcessMetricSample;
    using AsynGyanis::Net::formatOtlpMetricsJson;
    using AsynGyanis::Net::kOtlpMetricsScopeName;
    using AsynGyanis::Net::OtlpHistogramPoint;
    using AsynGyanis::Net::OtlpHistogramSample;
    using AsynGyanis::Net::OtlpLabeledPoint;
    using AsynGyanis::Net::OtlpLabeledSample;
    using AsynGyanis::Net::OtlpMetricsBatch;
    using AsynGyanis::Net::TraceResource;
    using AsynGyanis::Net::TestSupport::jsonNumberOf;
    using AsynGyanis::Net::TestSupport::jsonTextOf;

    /// 采集时刻固定在一个两个平台都能精确表示的取值上，渲染结果因此可以逐位断言
    constexpr std::string_view kCollectionNanoText = "1700000000123456700";

    /// 与 kCollectionNanoText 同解的墙钟时刻
    std::chrono::system_clock::time_point fixedCollectionMoment()
    {
        return std::chrono::system_clock::time_point{} + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{1700000000123456700LL});
    }

    /// 造一份出处固定的 resource
    TraceResource makeResource(const std::string &version = {})
    {
        return TraceResource{.serviceName = "otlp-metrics-test-service", .serviceVersion = version};
    }

    /// 造一条读数样本
    ProcessMetricSample makeSample(std::string name, std::string help, const ProcessMetricKind kind, const std::uint64_t value)
    {
        return ProcessMetricSample{.name = std::move(name), .help = std::move(help), .kind = kind, .value = value};
    }
} // namespace

/**
 * @brief 一份快照（计数器 + 瞬时量各一条）渲染成 Collector 认的形状：两种数据点的键、聚合时序与取值类型都不许走样
 */
TEST(OtlpMetricsJson, RendersCounterAndGaugeDataPoints)
{
    const std::vector<ProcessMetricSample> samples{
            makeSample("asyn_test_requests_total", "请求计数", ProcessMetricKind::Counter, 42U),
            makeSample("asyn_test_active_connections", "活跃连接数", ProcessMetricKind::Gauge, 7U),
    };

    const std::string text   = formatOtlpMetricsJson(makeResource(), samples, fixedCollectionMoment());
    const auto        parsed = parseConfigValue(text);
    ASSERT_HAS_VALUE(parsed) << "渲染结果不是合法 JSON：" << text;

    const ConfigValue &resourceMetrics = parsed->at("resourceMetrics").at(0);
    const ConfigValue &attributes      = resourceMetrics.at("resource").at("attributes");
    ASSERT_EQ(attributes.size(), 1U);
    EXPECT_EQ(jsonTextOf(attributes.at(0).at("key")), "service.name");
    EXPECT_EQ(jsonTextOf(attributes.at(0).at("value").at("stringValue")), "otlp-metrics-test-service");

    const ConfigValue &scopeMetrics = resourceMetrics.at("scopeMetrics").at(0);
    EXPECT_EQ(jsonTextOf(scopeMetrics.at("scope").at("name")), kOtlpMetricsScopeName);

    const ConfigValue &metrics = scopeMetrics.at("metrics");
    ASSERT_EQ(metrics.size(), 2U);

    const ConfigValue &counter = metrics.at(0);
    EXPECT_EQ(jsonTextOf(counter.at("name")), "asyn_test_requests_total");
    EXPECT_EQ(jsonTextOf(counter.at("description")), "请求计数");
    EXPECT_FALSE(counter.contains("gauge")) << "计数器不算瞬时量：折成 sum 而不是 gauge";
    const ConfigValue &sum = counter.at("sum");
    EXPECT_TRUE(sum.at("isMonotonic").get<bool>());
    EXPECT_EQ(jsonNumberOf(sum.at("aggregationTemporality")), 2) << "注册表交的是自进程启动起的累计值，折成 CUMULATIVE(=2)";
    const ConfigValue &counterPoint = sum.at("dataPoints").at(0);
    EXPECT_EQ(jsonTextOf(counterPoint.at("asInt")), "42") << "64 位取值要发成字符串，与各语言 SDK 一致";
    EXPECT_EQ(jsonTextOf(counterPoint.at("timeUnixNano")), kCollectionNanoText);

    const ConfigValue &gauge = metrics.at(1);
    EXPECT_EQ(jsonTextOf(gauge.at("name")), "asyn_test_active_connections");
    EXPECT_EQ(jsonTextOf(gauge.at("description")), "活跃连接数");
    EXPECT_FALSE(gauge.contains("sum")) << "瞬时量不算计数器：折成 gauge 而不是 sum";
    const ConfigValue &gaugePoint = gauge.at("gauge").at("dataPoints").at(0);
    EXPECT_EQ(jsonTextOf(gaugePoint.at("asInt")), "7");
    EXPECT_EQ(jsonTextOf(gaugePoint.at("timeUnixNano")), kCollectionNanoText);
}

/**
 * @brief 服务版本非空才随 resource 报出：protojson 对缺省值是省略字段
 */
TEST(OtlpMetricsJson, ReportsServiceVersionOnlyWhenGiven)
{
    const std::vector<ProcessMetricSample> samples{makeSample("asyn_test_gauge", "瞬时量", ProcessMetricKind::Gauge, 1U)};

    const auto withoutVersion = parseConfigValue(formatOtlpMetricsJson(makeResource(), samples, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(withoutVersion);
    EXPECT_EQ(withoutVersion->at("resourceMetrics").at(0).at("resource").at("attributes").size(), 1U) << "没给版本就不该出现 service.version";

    const auto withVersion = parseConfigValue(formatOtlpMetricsJson(makeResource("2.1.0"), samples, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(withVersion);
    const ConfigValue &attributes = withVersion->at("resourceMetrics").at(0).at("resource").at("attributes");
    ASSERT_EQ(attributes.size(), 2U);
    EXPECT_EQ(jsonTextOf(attributes.at(1).at("key")), "service.version");
    EXPECT_EQ(jsonTextOf(attributes.at(1).at("value").at("stringValue")), "2.1.0");
}

/**
 * @brief 空快照也交出结构完整的正文：调用方据此判「渲染这一路是通的」
 */
TEST(OtlpMetricsJson, EmptySnapshotStillRendersTheResourceShell)
{
    // 空实参必须写全类型：标量入口与批入口两个重载之下，裸 {} 是二义的
    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), std::vector<ProcessMetricSample>{}, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(parsed);
    EXPECT_EQ(parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics").size(), 0U);
}

/**
 * @brief 取值进不了 JSON（正文含非法 UTF-8）时交回空串，而不是硬凑一份坏正文
 */
TEST(OtlpMetricsJson, RefusesToRenderAValueThatCannotEnterJson)
{
    const std::vector<ProcessMetricSample> poisoned{makeSample("asyn_test_bad", std::string{"bad\xffhelp"}, ProcessMetricKind::Gauge, 1U)};
    EXPECT_TRUE(formatOtlpMetricsJson(makeResource(), poisoned, fixedCollectionMoment()).empty()) << "含非法字节的正文不该交出来：这一次导出按失败处置，不去偷偷修那条坏数据";
}

/**
 * @brief 直方图读数渲染成 OTLP histogram 数据点：每格条数（非累计）、末格溢出、单位秒
 */
TEST(OtlpMetricsJson, RendersHistogramDataPointsWithSecondUnits)
{
    OtlpHistogramSample histogram;
    histogram.name                  = "asyn_test_request_duration_seconds";
    histogram.help                  = "已应答请求的耗时分布";
    histogram.explicitBoundsSeconds = {0.001, 0.005, 0.02, 0.1};
    histogram.bucketCounts          = {3U, 5U, 7U, 2U, 1U};
    histogram.count                 = 18U;
    histogram.sumSeconds            = 0.42;

    const OtlpMetricsBatch batch{.histograms = {histogram}};

    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), batch, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(parsed) << "渲染结果不是合法 JSON";
    const ConfigValue &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 1U);
    const ConfigValue &metric = metrics.at(0);
    EXPECT_EQ(jsonTextOf(metric.at("name")), "asyn_test_request_duration_seconds");
    EXPECT_EQ(jsonTextOf(metric.at("description")), "已应答请求的耗时分布");
    EXPECT_EQ(jsonTextOf(metric.at("unit")), "s") << "耗时一族按秒报（与 Prometheus 渲染同一换算）";
    EXPECT_FALSE(metric.contains("sum")) << "直方图不是计数器：折成 histogram 数据点而不是 sum";
    EXPECT_FALSE(metric.contains("gauge"));

    const ConfigValue &histogramObject = metric.at("histogram");
    EXPECT_EQ(jsonNumberOf(histogramObject.at("aggregationTemporality")), 2) << "统计快照是累计值，折成 CUMULATIVE";
    const ConfigValue &point = histogramObject.at("dataPoints").at(0);
    EXPECT_EQ(jsonTextOf(point.at("count")), "18") << "64 位计数按 proto3 的 JSON mapping 写成字符串";
    EXPECT_EQ(point.at("sum").get<double>(), 0.42) << "sum 是浮点形态、单位秒";
    ASSERT_EQ(point.at("bucketCounts").size(), 5U) << "bucketCounts 长度 = 界数 + 1，末格为溢出档";
    EXPECT_EQ(jsonTextOf(point.at("bucketCounts").at(0)), "3") << "是每格条数而不是累计（前缀和属于展示端的活）；元素同样是字符串";
    EXPECT_EQ(jsonTextOf(point.at("bucketCounts").at(4)), "1");
    const ConfigValue &bounds = point.at("explicitBounds");
    ASSERT_EQ(bounds.size(), 4U);
    EXPECT_DOUBLE_EQ(bounds.at(0).get<double>(), 0.001);
    EXPECT_DOUBLE_EQ(bounds.at(3).get<double>(), 0.1);
    EXPECT_EQ(jsonTextOf(point.at("timeUnixNano")), kCollectionNanoText);
}

/**
 * @brief 带维度的分布读数渲染成一条 metric 的多个 histogram 数据点，每点带 route 属性
 * @details 这条形状的判据是：同一条 `request_duration_seconds` 下按路由分点，
 *          而不是为每条路由各起一个同名 metric（后者在采集端是重复名字）
 */
TEST(OtlpMetricsJson, RendersMultiPointHistogramsWithRouteAttributes)
{
    OtlpHistogramSample histogram;
    histogram.name                  = "asyn_test_request_duration_seconds";
    histogram.help                  = "已应答请求的耗时分布，按路由模式（route）分组";
    histogram.explicitBoundsSeconds = {0.001, 0.005};
    histogram.points.push_back(OtlpHistogramPoint{.attributes = {{"route", "/user/:id"}}, .bucketCounts = {1U, 0U, 1U}, .count = 2U, .sumSeconds = 0.003});
    histogram.points.push_back(OtlpHistogramPoint{.attributes = {{"route", "unmatched"}}, .bucketCounts = {0U, 1U, 0U}, .count = 1U, .sumSeconds = 0.004});

    const OtlpMetricsBatch batch{.histograms = {histogram}};

    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), batch, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(parsed);
    const ConfigValue &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 1U) << "两条路由应当共用一条 metric，而不是各起一个同名 metric";
    const ConfigValue &dataPoints = metrics.at(0).at("histogram").at("dataPoints");
    ASSERT_EQ(dataPoints.size(), 2U);

    const ConfigValue &firstPoint = dataPoints.at(0);
    ASSERT_EQ(firstPoint.at("attributes").size(), 1U);
    EXPECT_EQ(jsonTextOf(firstPoint.at("attributes").at(0).at("key")), "route");
    EXPECT_EQ(jsonTextOf(firstPoint.at("attributes").at(0).at("value").at("stringValue")), "/user/:id");
    EXPECT_EQ(jsonTextOf(firstPoint.at("count")), "2");
    EXPECT_EQ(firstPoint.at("sum").get<double>(), 0.003);
    ASSERT_EQ(firstPoint.at("bucketCounts").size(), 3U);
    EXPECT_EQ(jsonTextOf(firstPoint.at("bucketCounts").at(2)), "1");

    const ConfigValue &secondPoint = dataPoints.at(1);
    ASSERT_EQ(secondPoint.at("attributes").size(), 1U);
    EXPECT_EQ(jsonTextOf(secondPoint.at("attributes").at(0).at("value").at("stringValue")), "unmatched");
    EXPECT_EQ(jsonTextOf(secondPoint.at("count")), "1");
    EXPECT_EQ(secondPoint.at("sum").get<double>(), 0.004);
}

/**
 * @brief 带维度的读数渲染成一条 metric 多数据点：每点带 attributes（OTLP 侧的正统形状，不是拼进名字）
 */
TEST(OtlpMetricsJson, RendersLabeledSamplesAsMultiPointMetrics)
{
    OtlpLabeledSample responses;
    responses.name = "asyn_test_responses_total";
    responses.help = "已发出的响应条数，按状态码类聚合";
    responses.kind = ProcessMetricKind::Counter;
    responses.points.push_back(OtlpLabeledPoint{.attributes = {{"status_class", "2xx"}}, .value = 11U});
    responses.points.push_back(OtlpLabeledPoint{.attributes = {{"status_class", "5xx"}}, .value = 2U});

    const OtlpMetricsBatch batch{.labeled = {responses}};

    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), batch, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(parsed);
    const ConfigValue &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 1U);
    const ConfigValue &metric = metrics.at(0);
    EXPECT_EQ(jsonTextOf(metric.at("name")), "asyn_test_responses_total");
    const ConfigValue &sum = metric.at("sum");
    EXPECT_TRUE(sum.at("isMonotonic").get<bool>());
    EXPECT_EQ(jsonNumberOf(sum.at("aggregationTemporality")), 2);

    const ConfigValue &points = sum.at("dataPoints");
    ASSERT_EQ(points.size(), 2U) << "同名两点折成一条 metric 的两个数据点";
    EXPECT_EQ(jsonTextOf(points.at(0).at("asInt")), "11");
    ASSERT_EQ(points.at(0).at("attributes").size(), 1U);
    EXPECT_EQ(jsonTextOf(points.at(0).at("attributes").at(0).at("key")), "status_class");
    EXPECT_EQ(jsonTextOf(points.at(0).at("attributes").at(0).at("value").at("stringValue")), "2xx");
    EXPECT_EQ(jsonTextOf(points.at(1).at("asInt")), "2");
    EXPECT_EQ(jsonTextOf(points.at(1).at("attributes").at(0).at("value").at("stringValue")), "5xx");
}

/**
 * @brief 三路混合按标量、带维度、直方图的固定顺序进同一份正文
 */
TEST(OtlpMetricsJson, KeepsTheThreeLanesInAFixedOrderInOneDocument)
{
    OtlpMetricsBatch batch;
    batch.scalars.push_back(makeSample("asyn_test_scalar", "标量", ProcessMetricKind::Gauge, 1U));
    batch.labeled.push_back(
            OtlpLabeledSample{.name = "asyn_test_labeled", .help = "带维度", .kind = ProcessMetricKind::Gauge, .points = {{.attributes = {{"k", "v"}}, .value = 2U}}});
    batch.histograms.push_back(
            OtlpHistogramSample{.name = "asyn_test_histogram", .help = "分布", .explicitBoundsSeconds = {0.001}, .bucketCounts = {1U, 1U}, .count = 2U, .sumSeconds = 0.5});

    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), batch, fixedCollectionMoment()));
    ASSERT_HAS_VALUE(parsed);
    const ConfigValue &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 3U);
    EXPECT_EQ(jsonTextOf(metrics.at(0).at("name")), "asyn_test_scalar");
    EXPECT_EQ(jsonTextOf(metrics.at(1).at("name")), "asyn_test_labeled");
    EXPECT_EQ(jsonTextOf(metrics.at(2).at("name")), "asyn_test_histogram");
}
