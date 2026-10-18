// OTLP/HTTP metrics 出口的用例：对端是自家 HTTP 服务器扮的采集端
//
// 判据分两侧：一侧看「周期到了到底发出了什么」（正文、媒体类型、附加头部、空快照跳没跳过），
// 另一侧看「构造期就该拒的写法」（地址畸形、占用本出口自己会写的头部、周期与时限非正数、空快照来源）。
// 导出是自成周期的异步行为，所以断言前一律先有界等到条件成立，不赌调度时序。

#include "Net/Metrics/OtlpHttpMetricsExporter.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "ExpectedAssertions.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Net/Tracing/SpanExporter.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Core::ProcessMetricKind;
    using AsynGyanis::Core::ProcessMetricSample;
    using AsynGyanis::Net::OtlpHistogramSample;
    using AsynGyanis::Net::OtlpHttpMetricsExporter;
    using AsynGyanis::Net::OtlpLabeledPoint;
    using AsynGyanis::Net::OtlpLabeledSample;
    using AsynGyanis::Net::OtlpMetricsBatch;
    using AsynGyanis::Net::TraceResource;
    using AsynGyanis::Net::HttpTestSupport::RunningHttpServerFixture;
    using AsynGyanis::Net::TestSupport::jsonTextOf;

    constexpr std::chrono::milliseconds kWaitBudget{5000};
    constexpr std::string_view          kCollectorToken = "Bearer metrics-collector-token";

    /// 采集端收到的东西
    struct CollectorState
    {
        std::mutex       m_mutex;
        std::string      m_body{};          ///< 收到的最后一条正文
        std::string      m_contentType{};   ///< 收到的媒体类型
        std::string      m_authorization{}; ///< 收到的 Authorization 头部
        std::atomic<int> m_requestCount{0}; ///< 收到过几条请求
    };

    /// 装一条只吃 POST /v1/metrics 的路由
    void registerCollectorRoute(AsynGyanis::Net::Router &router, CollectorState *state)
    {
        router.post("/v1/metrics",
                    [state](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        {
                            const std::lock_guard lock(state->m_mutex);
                            state->m_body          = std::string{request.body()};
                            state->m_contentType   = std::string{request.firstHeaderValueView("content-type").value_or(std::string_view{})};
                            state->m_authorization = std::string{request.firstHeaderValueView("authorization").value_or(std::string_view{})};
                        }
                        static_cast<void>(state->m_requestCount.fetch_add(1, std::memory_order_relaxed));
                        response.setStatus(200);
                        response.setBody(R"({"partialSuccess":{}})");
                        co_return;
                    });
    }

    /// 起一台假采集端；没进入接受循环就交回空
    std::unique_ptr<RunningHttpServerFixture> startCollector(CollectorState *state)
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(
                AsynGyanis::Net::HttpServerLimits{}, std::chrono::milliseconds{1000}, AsynGyanis::Net::HttpTestSupport::SlowRouteOptions{},
                [state](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerCollectorRoute(router, state); });
        if (!fixture->awaitRunning(kWaitBudget))
        {
            return nullptr;
        }
        return fixture;
    }

    /**
     * @brief 借一个内核分配过、但已经没人监听的回环端口
     * @details 起一台服务器问出端口再立刻关掉：比随手挑一个号可靠——随手挑的那个数可能正被别人听着，
     *          那条用例会因此从「连不上」变成「连上了且回了别的东西」
     */
    std::uint16_t borrowUnusedPort()
    {
        CollectorState state;
        const auto     fixture = startCollector(&state);
        EXPECT_TRUE(fixture != nullptr);
        const std::uint16_t port = fixture->listeningPort();
        return port;
    }

    /// 有界等到条件成立：自成周期的异步行为，断言前都要先自己把成立条件等出来
    bool waitUntil(const std::function<bool()> &condition, const std::chrono::milliseconds timeout = kWaitBudget)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return condition();
    }

    /// 造一份指向给定端口的出口配置；周期取 50 毫秒：短到用例等得起，又长到一次导出有明确的观察窗口
    OtlpHttpMetricsExporter::Configuration makeCollectorConfiguration(const std::uint16_t port)
    {
        OtlpHttpMetricsExporter::Configuration configuration;
        configuration.endpoint       = "http://127.0.0.1:" + std::to_string(port) + "/v1/metrics";
        configuration.exportInterval = std::chrono::milliseconds{50};
        return configuration;
    }

    /// 固定出的出处
    TraceResource testResource()
    {
        return TraceResource{.serviceName = "otlp-metrics-exporter-test", .serviceVersion = {}};
    }

    /// 两条样本的快照：计数器 + 瞬时量各一条
    std::vector<ProcessMetricSample> twoSampleSnapshot()
    {
        return {
                ProcessMetricSample{.name = "asyn_test_requests_total", .help = "请求计数", .kind = ProcessMetricKind::Counter, .value = 42U},
                ProcessMetricSample{.name = "asyn_test_active_connections", .help = "活跃连接数", .kind = ProcessMetricKind::Gauge, .value = 7U},
        };
    }

    /// 固定返回同一份快照的来源（按值捕获，返回时拷贝一份；标量装进批的标量一路）
    OtlpHttpMetricsExporter::SnapshotProvider fixedSnapshot(std::vector<ProcessMetricSample> samples)
    {
        return [samples = std::move(samples)] { return OtlpMetricsBatch{.scalars = samples}; };
    }
} // namespace

/**
 * @brief 周期到了真的把快照推到采集端：正文是合法的 OTLP JSON，媒体类型与附加头部都对
 */
TEST(OtlpHttpMetricsExporter, PostsSnapshotsOnItsOwnCadenceWithTheExtraHeaders)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto configuration         = makeCollectorConfiguration(fixture->listeningPort());
    configuration.extraHeaders = {{"Authorization", std::string{kCollectorToken}}};
    OtlpHttpMetricsExporter exporter(testResource(), configuration, fixedSnapshot(twoSampleSnapshot()));

    ASSERT_TRUE(waitUntil([&exporter] { return exporter.deliveredExportCount() >= 1U; })) << "第一个周期内一条导出都没送达：" << exporter.lastFailureReason();
    EXPECT_EQ(exporter.failedExportCount(), 0U);

    std::string body;
    std::string contentType;
    std::string authorization;
    {
        const std::lock_guard lock(state.m_mutex);
        body          = state.m_body;
        contentType   = state.m_contentType;
        authorization = state.m_authorization;
    }
    EXPECT_EQ(contentType, "application/json");
    EXPECT_EQ(authorization, kCollectorToken);

    const auto parsed = AsynGyanis::Base::parseConfigValue(body);
    ASSERT_HAS_VALUE(parsed) << "采集端收到的正文不是合法 JSON：" << body;
    EXPECT_EQ(jsonTextOf(parsed->at("resourceMetrics").at(0).at("resource").at("attributes").at(0).at("value").at("stringValue")), "otlp-metrics-exporter-test");
    const auto &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(jsonTextOf(metrics.at(0).at("name")), "asyn_test_requests_total");
    EXPECT_EQ(jsonTextOf(metrics.at(0).at("sum").at("dataPoints").at(0).at("asInt")), "42");
    EXPECT_EQ(jsonTextOf(metrics.at(1).at("gauge").at("dataPoints").at(0).at("asInt")), "7");
}

/**
 * @brief 采集端根本连不上：这次导出算失败而不是无声消失，原因非空
 */
TEST(OtlpHttpMetricsExporter, CountsUnreachableCollectorAsFailure)
{
    auto configuration           = makeCollectorConfiguration(borrowUnusedPort());
    configuration.requestTimeout = std::chrono::milliseconds{500};
    OtlpHttpMetricsExporter exporter(testResource(), configuration, fixedSnapshot(twoSampleSnapshot()));

    ASSERT_TRUE(waitUntil([&exporter] { return exporter.failedExportCount() >= 1U; })) << "连不上也必须报出来";
    EXPECT_EQ(exporter.deliveredExportCount(), 0U);
    EXPECT_FALSE(exporter.lastFailureReason().empty());
}

/**
 * @brief 快照为空的周期直接跳过、一次网络都不碰：「没有指标」不是「通路坏了」
 */
TEST(OtlpHttpMetricsExporter, SkipsEmptySnapshotsWithoutHittingTheNetwork)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpHttpMetricsExporter exporter(testResource(), makeCollectorConfiguration(fixture->listeningPort()), fixedSnapshot({}));

    ASSERT_TRUE(waitUntil([&exporter] { return exporter.emptySnapshotCount() >= 1U; })) << "空快照的周期没有被记成跳过";
    EXPECT_EQ(exporter.deliveredExportCount(), 0U);
    EXPECT_EQ(state.m_requestCount.load(std::memory_order_relaxed), 0) << "空快照不该碰网络";
}

/**
 * @brief 收尾之后不再发起新的周期：计数停在原地
 * @details 停摆的下界是确定的——一个导出周期加一次在途请求时限；睡三个周期后计数若还在涨，
 *          那就是实现根本没停
 */
TEST(OtlpHttpMetricsExporter, StopsExportingAfterShutdown)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto exporter = std::make_unique<OtlpHttpMetricsExporter>(testResource(), makeCollectorConfiguration(fixture->listeningPort()), fixedSnapshot(twoSampleSnapshot()));
    ASSERT_TRUE(waitUntil([&exporter] { return exporter->deliveredExportCount() >= 1U; }));

    exporter->shutdown();
    exporter->shutdown(); ///< 收尾是幂等的：析构会再来一次，不该出错也不该卡住

    const std::uint64_t deliveredAtStop = exporter->deliveredExportCount();
    const std::uint64_t failedAtStop    = exporter->failedExportCount();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_EQ(exporter->deliveredExportCount(), deliveredAtStop) << "收尾之后还在发起新的导出";
    EXPECT_EQ(exporter->failedExportCount(), failedAtStop);
}

/**
 * @brief 构造期就拒的写法：地址不成形、占用本出口自己会写的头部、含会撕裂请求行的字节、周期与时限非正数、空快照来源
 */
TEST(OtlpHttpMetricsExporter, RejectsUnusableConfigurationAtConstruction)
{
    struct Case
    {
        OtlpHttpMetricsExporter::Configuration    configuration;
        OtlpHttpMetricsExporter::SnapshotProvider provider;
        std::string                               expectedFragment;
    };

    std::vector<Case> cases;
    cases.push_back({OtlpHttpMetricsExporter::Configuration{}, fixedSnapshot(twoSampleSnapshot()), "采集端地址为空"});

    auto garbageUrl     = makeCollectorConfiguration(4318);
    garbageUrl.endpoint = "http://"; ///< 有协议名却没有主机：parseUrl 判非法
    cases.push_back({garbageUrl, fixedSnapshot(twoSampleSnapshot()), "采集端地址"});

    auto reservedConnection         = makeCollectorConfiguration(4318);
    reservedConnection.extraHeaders = {{"Connection", "close"}};
    cases.push_back({reservedConnection, fixedSnapshot(twoSampleSnapshot()), "Connection"});

    auto reservedContentType         = makeCollectorConfiguration(4318);
    reservedContentType.extraHeaders = {{"CONTENT-TYPE", "text/plain"}};
    cases.push_back({reservedContentType, fixedSnapshot(twoSampleSnapshot()), "占用"});

    auto unsafeValue         = makeCollectorConfiguration(4318);
    unsafeValue.extraHeaders = {{"Authorization", "Bearer bad\r\nvalue"}};
    cases.push_back({unsafeValue, fixedSnapshot(twoSampleSnapshot()), "CR"});

    auto zeroInterval           = makeCollectorConfiguration(4318);
    zeroInterval.exportInterval = std::chrono::milliseconds::zero();
    cases.push_back({zeroInterval, fixedSnapshot(twoSampleSnapshot()), "周期"});

    auto zeroTimeout           = makeCollectorConfiguration(4318);
    zeroTimeout.requestTimeout = std::chrono::milliseconds::zero();
    cases.push_back({zeroTimeout, fixedSnapshot(twoSampleSnapshot()), "时限"});

    cases.push_back({makeCollectorConfiguration(4318), OtlpHttpMetricsExporter::SnapshotProvider{}, "快照"});

    for (const Case &testCase: cases)
    {
        try
        {
            OtlpHttpMetricsExporter exporter(testResource(), testCase.configuration, testCase.provider);
            FAIL() << "这份配置本该在构造期就被拒：" << testCase.expectedFragment;
        } catch (const AsynGyanis::Base::InvalidArgumentException &exception)
        {
            EXPECT_NE(std::string{exception.what()}.find(testCase.expectedFragment), std::string::npos)
                    << "拒绝原因里要点名「" << testCase.expectedFragment << "」：" << exception.what();
        }
    }
}

/**
 * @brief 带维度读数与直方图随同一周期发出：正文里 attributes 与直方图桶逐项在位
 */
TEST(OtlpHttpMetricsExporter, PostsLabeledAndHistogramReadingsOnTheSameCadence)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpMetricsBatch batch;
    batch.scalars.push_back(ProcessMetricSample{.name = "asyn_test_scalar", .help = "标量", .kind = ProcessMetricKind::Gauge, .value = 5U});
    batch.labeled.push_back(OtlpLabeledSample{
            .name = "asyn_test_responses_total", .help = "按状态码类聚合", .kind = ProcessMetricKind::Counter, .points = {{.attributes = {{"status_class", "2xx"}}, .value = 9U}}});
    batch.histograms.push_back(OtlpHistogramSample{
            .name = "asyn_test_duration_seconds", .help = "耗时分布", .explicitBoundsSeconds = {0.001, 0.1}, .bucketCounts = {4U, 2U, 1U}, .count = 7U, .sumSeconds = 0.3});

    OtlpHttpMetricsExporter exporter(testResource(), makeCollectorConfiguration(fixture->listeningPort()), [batch] { return batch; });
    ASSERT_TRUE(waitUntil([&exporter] { return exporter.deliveredExportCount() >= 1U; })) << "含三路条目的批没能送达：" << exporter.lastFailureReason();

    std::string body;
    {
        const std::lock_guard lock(state.m_mutex);
        body = state.m_body;
    }
    const auto parsed = AsynGyanis::Base::parseConfigValue(body);
    ASSERT_HAS_VALUE(parsed) << body;
    const auto &metrics = parsed->at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    ASSERT_EQ(metrics.size(), 3U) << "三路条目应当都在同一份正文里";
    EXPECT_EQ(jsonTextOf(metrics.at(0).at("name")), "asyn_test_scalar");
    EXPECT_EQ(jsonTextOf(metrics.at(1).at("sum").at("dataPoints").at(0).at("attributes").at(0).at("value").at("stringValue")), "2xx");
    const auto &histogramPoint = metrics.at(2).at("histogram").at("dataPoints").at(0);
    EXPECT_EQ(jsonTextOf(histogramPoint.at("count")), "7");
    EXPECT_EQ(histogramPoint.at("bucketCounts").size(), 3U) << "每格条数 + 末格溢出";
    EXPECT_DOUBLE_EQ(histogramPoint.at("sum").get<double>(), 0.3);
}
