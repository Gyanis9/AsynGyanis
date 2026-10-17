// HttpServerStats → OTLP 读数批的映射用例：名字、类型、状态码标签族与直方图的秒换算逐条钉住。
//
// 与 /metrics 的 Prometheus 渲染是同一份口径的两种编码，因此判据落在两处：换算与命名的逐条断言，
// 以及一条互为对照——同一份快照两路渲染，Prometheus 版里「服务器面字段」的名字必须能在批里找到
// （对照的是被既有用例钉过的渲染实现，不是自己对自己）。

#include "Net/Metrics/HttpStatsOtlp.h"

#include "Net/Http/HttpMetricsEndpoint.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using AsynGyanis::Core::ProcessMetricKind;
    using AsynGyanis::Core::ProcessMetricSample;
    using AsynGyanis::Net::buildOtlpMetricsFromHttpStats;
    using AsynGyanis::Net::formatPrometheusMetrics;
    using AsynGyanis::Net::HttpServerStats;
    using AsynGyanis::Net::kHttpLatencyBucketCount;
    using AsynGyanis::Net::OtlpMetricsBatch;

    /// 造一份各字段都非零的统计快照（错误路径也全开，好让映射的每条都落地）
    HttpServerStats makePopulatedStats()
    {
        HttpServerStats stats;
        stats.totalRequestCount                = 100U;
        stats.activeConnectionCount            = 7U;
        stats.badRequestCount                  = 3U;
        stats.timeoutClosedCount               = 2U;
        stats.writeAbortedConnectionCount      = 1U;
        stats.status1xxCount                   = 0U;
        stats.status2xxCount                   = 80U;
        stats.status3xxCount                   = 10U;
        stats.status4xxCount                   = 5U;
        stats.status5xxCount                   = 5U;
        stats.latencyBucketCounts              = {40U, 30U, 20U, 5U, 5U};
        stats.totalLatencyMicroseconds         = 2'500'000U;
        stats.webSocketUpgradeCount            = 4U;
        stats.webSocketMessageCount            = 900U;
        stats.webSocketProtocolErrorCloseCount = 1U;
        stats.webSocketPeerCloseCount          = 2U;
        stats.webSocketServerCloseCount        = 3U;
        stats.streamCancelledCount             = 6U;
        stats.zeroCopySendCount                = 55U;
        stats.admissionRejectedConnectionCount = 9U;
        stats.maximumConnections               = 4096U;
        stats.maximumConnectionsPerIp          = 256U;
        stats.overLimitRejectedConnectionCount = 1U;
        // 进程级与积压字段：刻意不在映射里，填上是为了双向核对「确实没有漏进来」
        stats.blockingTaskQueueDepth    = 12U;
        stats.blockingTaskRejectedCount = 34U;
        stats.residentMemoryBytes       = 56'789'012U;
        stats.processCpuSeconds         = 78U;
        stats.openHandleCount           = 90U;
        stats.processStartTimeSeconds   = 1'700'000'000U;
        return stats;
    }

    /// 在标量一路里找一个名字；找不到交回空
    const ProcessMetricSample *findScalar(const OtlpMetricsBatch &batch, const std::string_view name)
    {
        for (const ProcessMetricSample &sample: batch.scalars)
        {
            if (sample.name == name)
            {
                return &sample;
            }
        }
        return nullptr;
    }
} // namespace

/**
 * @brief 计数、瞬时量与两个分母逐条同名搬进标量一路（类型也照搬）
 */
TEST(HttpStatsOtlp, MapsCountersAndGaugesUnderTheSameNamesAsPrometheus)
{
    const HttpServerStats  stats = makePopulatedStats();
    const OtlpMetricsBatch batch = buildOtlpMetricsFromHttpStats(stats, "asyn_http");

    struct Case
    {
        std::string_view  name;
        ProcessMetricKind kind;
        std::uint64_t     value;
    };
    const std::vector<Case> cases{
            {"asyn_http_requests_total", ProcessMetricKind::Counter, 100U},
            {"asyn_http_bad_requests_total", ProcessMetricKind::Counter, 3U},
            {"asyn_http_timeout_closed_connections_total", ProcessMetricKind::Counter, 2U},
            {"asyn_http_write_aborted_connections_total", ProcessMetricKind::Counter, 1U},
            {"asyn_http_active_connections", ProcessMetricKind::Gauge, 7U},
            {"asyn_http_maximum_connections", ProcessMetricKind::Gauge, 4096U},
            {"asyn_http_maximum_connections_per_ip", ProcessMetricKind::Gauge, 256U},
            {"asyn_http_websocket_upgrades_total", ProcessMetricKind::Counter, 4U},
            {"asyn_http_websocket_messages_total", ProcessMetricKind::Counter, 900U},
            {"asyn_http_websocket_protocol_error_closes_total", ProcessMetricKind::Counter, 1U},
            {"asyn_http_websocket_peer_closes_total", ProcessMetricKind::Counter, 2U},
            {"asyn_http_websocket_server_closes_total", ProcessMetricKind::Counter, 3U},
            {"asyn_http_http2_stream_cancelled_total", ProcessMetricKind::Counter, 6U},
            {"asyn_http_zerocopy_sends_total", ProcessMetricKind::Counter, 55U},
            {"asyn_http_admission_rejected_connections_total", ProcessMetricKind::Counter, 9U},
            {"asyn_http_over_limit_rejected_connections_total", ProcessMetricKind::Counter, 1U},
    };
    for (const Case &testCase: cases)
    {
        const ProcessMetricSample *sample = findScalar(batch, testCase.name);
        ASSERT_NE(sample, nullptr) << "标量一路里缺少「" << testCase.name << "」";
        EXPECT_EQ(sample->kind, testCase.kind) << testCase.name;
        EXPECT_EQ(sample->value, testCase.value) << testCase.name;
        EXPECT_FALSE(sample->help.empty()) << testCase.name << " 没有说明文本";
    }
}

/**
 * @brief 状态码五类折成一条带 route + status_class 属性的读数（OTLP 侧的正统形状）
 * @details 快照没有路由明细时出口按 unmatched 合成一条；只输出实际出现过的类，因此 1xx 为零时不出现
 */
TEST(HttpStatsOtlp, FoldsTheStatusClassesIntoOneLabeledReading)
{
    const HttpServerStats  stats = makePopulatedStats();
    const OtlpMetricsBatch batch = buildOtlpMetricsFromHttpStats(stats, "asyn_http");

    ASSERT_EQ(batch.labeled.size(), 1U);
    const auto &responses = batch.labeled.front();
    EXPECT_EQ(responses.name, "asyn_http_responses_total");
    EXPECT_EQ(responses.kind, ProcessMetricKind::Counter);
    ASSERT_EQ(responses.points.size(), 4U) << "1xx 为零不输出，其余四类各一个数据点";
    const std::vector<std::uint64_t> expectedValues{80U, 10U, 5U, 5U};
    const std::vector<std::string>   expectedClasses{"2xx", "3xx", "4xx", "5xx"};
    for (std::size_t index = 0; index < responses.points.size(); ++index)
    {
        ASSERT_EQ(responses.points.at(index).attributes.size(), 2U);
        EXPECT_EQ(responses.points.at(index).attributes.at(0).first, "route");
        EXPECT_EQ(responses.points.at(index).attributes.at(0).second, "unmatched");
        EXPECT_EQ(responses.points.at(index).attributes.at(1).first, "status_class");
        EXPECT_EQ(responses.points.at(index).attributes.at(1).second, expectedClasses.at(index));
        EXPECT_EQ(responses.points.at(index).value, expectedValues.at(index));
    }
}

/**
 * @brief 延迟直方图折成秒单位的分布读数：每条路由一个点，每格条数照搬、上界毫秒换算、sum 微秒换算
 */
TEST(HttpStatsOtlp, ConvertsTheLatencyHistogramToSecondUnits)
{
    const HttpServerStats  stats = makePopulatedStats();
    const OtlpMetricsBatch batch = buildOtlpMetricsFromHttpStats(stats, "asyn_http");

    ASSERT_EQ(batch.histograms.size(), 1U);
    const auto &histogram = batch.histograms.front();
    EXPECT_EQ(histogram.name, "asyn_http_request_duration_seconds");
    ASSERT_EQ(histogram.explicitBoundsSeconds.size(), kHttpLatencyBucketCount - 1);
    EXPECT_DOUBLE_EQ(histogram.explicitBoundsSeconds.at(0), 0.001) << "1 毫秒换 0.001 秒";
    EXPECT_DOUBLE_EQ(histogram.explicitBoundsSeconds.at(3), 0.1);
    ASSERT_EQ(histogram.points.size(), 1U) << "没有路由明细时按 unmatched 合成一个点";
    const auto &point = histogram.points.front();
    ASSERT_EQ(point.attributes.size(), 1U);
    EXPECT_EQ(point.attributes.front().first, "route");
    EXPECT_EQ(point.attributes.front().second, "unmatched");
    ASSERT_EQ(point.bucketCounts.size(), kHttpLatencyBucketCount) << "末格是溢出档";
    EXPECT_EQ(point.bucketCounts.at(0), 40U) << "每格条数照搬（不做前缀和）";
    EXPECT_EQ(point.bucketCounts.at(4), 5U);
    EXPECT_EQ(point.count, stats.latencySampleCount());
    EXPECT_DOUBLE_EQ(point.sumSeconds, 2.5) << "2'500'000 微秒换秒";
}

/**
 * @brief 进程级字段刻意不进批：推模式一侧那是注册表的地盘，不能两边各报一份
 */
TEST(HttpStatsOtlp, LeavesProcessLevelFieldsOutOfTheBatch)
{
    const HttpServerStats  stats = makePopulatedStats();
    const OtlpMetricsBatch batch = buildOtlpMetricsFromHttpStats(stats, "asyn_http");

    for (const ProcessMetricSample &sample: batch.scalars)
    {
        EXPECT_EQ(sample.name.find("process"), std::string::npos) << "进程级读数不该从统计快照这一路推：" << sample.name;
        EXPECT_EQ(sample.name.find("resident"), std::string::npos) << sample.name;
        EXPECT_EQ(sample.name.find("blocking"), std::string::npos) << sample.name;
        EXPECT_EQ(sample.name.find("log_dropped"), std::string::npos) << sample.name;
    }
    EXPECT_EQ(batch.labeled.size() + batch.histograms.size(), 2U) << "带维度与直方图各恰一条，没有别的混进来";
}

/**
 * @brief 与 /metrics 的渲染互为对照：服务器面字段的名字两路必须同解；空前缀不带前缀
 */
TEST(HttpStatsOtlp, MatchesThePrometheusNamesForTheServerFacingFields)
{
    const HttpServerStats stats          = makePopulatedStats();
    const std::string     prometheusText = formatPrometheusMetrics(stats, "asyn_http");

    // 服务器面字段的名单（与 formatPrometheusMetrics 的逐条登记对齐；进程级与注册表那几段不算）
    const std::vector<std::string> serverFacingNames{
            "asyn_http_requests_total",
            "asyn_http_bad_requests_total",
            "asyn_http_timeout_closed_connections_total",
            "asyn_http_write_aborted_connections_total",
            "asyn_http_active_connections",
            "asyn_http_maximum_connections",
            "asyn_http_maximum_connections_per_ip",
            "asyn_http_responses_total",
            "asyn_http_request_duration_seconds",
            "asyn_http_websocket_upgrades_total",
            "asyn_http_websocket_messages_total",
            "asyn_http_websocket_protocol_error_closes_total",
            "asyn_http_websocket_peer_closes_total",
            "asyn_http_websocket_server_closes_total",
            "asyn_http_http2_stream_cancelled_total",
            "asyn_http_zerocopy_sends_total",
            "asyn_http_admission_rejected_connections_total",
            "asyn_http_over_limit_rejected_connections_total",
    };
    for (const std::string &name: serverFacingNames)
    {
        EXPECT_NE(prometheusText.find(name), std::string::npos) << "Prometheus 版里应当有「" << name << "」（名单本身过期了）";
    }

    const OtlpMetricsBatch batch = buildOtlpMetricsFromHttpStats(stats, "asyn_http");
    for (const std::string &name: serverFacingNames)
    {
        if (name == "asyn_http_responses_total")
        {
            ASSERT_EQ(batch.labeled.size(), 1U);
            EXPECT_EQ(batch.labeled.front().name, name);
            continue;
        }
        if (name == "asyn_http_request_duration_seconds")
        {
            ASSERT_EQ(batch.histograms.size(), 1U);
            EXPECT_EQ(batch.histograms.front().name, name);
            continue;
        }
        EXPECT_NE(findScalar(batch, name), nullptr) << "OTLP 批里缺少「" << name << "」";
    }

    // 空前缀：名字原样交给渲染层拼
    const OtlpMetricsBatch unprefixed = buildOtlpMetricsFromHttpStats(stats, "");
    ASSERT_NE(findScalar(unprefixed, "requests_total"), nullptr);
}
