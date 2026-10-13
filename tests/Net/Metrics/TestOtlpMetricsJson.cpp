// OTLP/HTTP metrics JSON 渲染的用例：只判正文形状，不涉及任何传输
//
// 判据落在两处：Counter 与 Gauge 折成的两种数据点形状（sum/gauge、isMonotonic、聚合时序），
// 以及 64 位取值与时间必须是字符串形态——与本仓链路出口同一条 JSON mapping 口径。

#include "Net/Metrics/OtlpMetricsJson.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
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
    ASSERT_TRUE(parsed.has_value()) << "渲染结果不是合法 JSON：" << text;

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
    ASSERT_TRUE(withoutVersion.has_value());
    EXPECT_EQ(withoutVersion->at("resourceMetrics").at(0).at("resource").at("attributes").size(), 1U) << "没给版本就不该出现 service.version";

    const auto withVersion = parseConfigValue(formatOtlpMetricsJson(makeResource("2.1.0"), samples, fixedCollectionMoment()));
    ASSERT_TRUE(withVersion.has_value());
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
    const auto parsed = parseConfigValue(formatOtlpMetricsJson(makeResource(), {}, fixedCollectionMoment()));
    ASSERT_TRUE(parsed.has_value());
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
