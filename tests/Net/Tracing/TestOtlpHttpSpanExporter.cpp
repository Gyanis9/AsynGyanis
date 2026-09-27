// OTLP/HTTP 出口的用例：对端是自家 HTTP 服务器扮的采集端
//
// 判据分两侧：一侧看「投出去之后到底发出了什么」（正文、媒体类型、附加头部、状态码怎么算账），
// 另一侧看「构造期就该拒的写法」（地址畸形、占用本出口自己会写的头部、上界为 0）。
// 交付是异步的，所以断言前一律先有界等到条件成立，不赌调度时序。

#include "Net/Tracing/OtlpHttpSpanExporter.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Tracing/Span.h"
#include "Net/Tracing/Tracer.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using AsynGyanis::Net::OtlpHttpSpanExporter;
    using AsynGyanis::Net::Span;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanRecord;
    using AsynGyanis::Net::Tracer;
    using AsynGyanis::Net::TraceResource;
    using AsynGyanis::Net::HttpTestSupport::RunningHttpServerFixture;
    using AsynGyanis::Net::TestSupport::jsonTextOf;
    using AsynGyanis::Net::TestSupport::makeRecord;

    constexpr std::chrono::milliseconds kWaitBudget{5000};
    constexpr std::string_view          kTraceId        = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view          kSpanId         = "1111222233334444";
    constexpr std::string_view          kCollectorToken = "Bearer collector-token";

    /// 采集端收到的东西，以及本台假采集端该回的状态码
    struct CollectorState
    {
        std::mutex       m_mutex;
        std::string      m_body{};                  ///< 收到的最后一条正文
        std::string      m_contentType{};           ///< 收到的媒体类型
        std::string      m_authorization{};         ///< 收到的 Authorization 头部
        std::atomic<int> m_responseStatusCode{200}; ///< 该回的状态码
        std::atomic<int> m_requestCount{0};         ///< 收到过几条请求
    };

    /// 装一条只吃 POST /v1/traces 的路由
    void registerCollectorRoute(AsynGyanis::Net::Router &router, CollectorState *state)
    {
        router.post("/v1/traces",
                    [state](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        {
                            const std::lock_guard lock(state->m_mutex);
                            state->m_body          = std::string{request.body()};
                            state->m_contentType   = std::string{request.firstHeaderValueView("content-type").value_or(std::string_view{})};
                            state->m_authorization = std::string{request.firstHeaderValueView("authorization").value_or(std::string_view{})};
                        }
                        static_cast<void>(state->m_requestCount.fetch_add(1, std::memory_order_relaxed));
                        response.setStatus(state->m_responseStatusCode.load(std::memory_order_relaxed));
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
     *          那条用例会因此从「连不上」变成「连上了且回了别的东西」。
     */
    std::uint16_t borrowUnusedPort()
    {
        CollectorState state;
        const auto     fixture = startCollector(&state);
        EXPECT_TRUE(fixture != nullptr);
        const std::uint16_t port = fixture->listeningPort();
        return port;
    }

    /**
     * @brief 一个收下连接却永不作答的监听套接字
     * @details 「连得上、没人答」与「连不上」在客户端侧是两种失败：前者会等满整个时限，
     *          正好用来把「出口线程正握着一批」这段时间拉长到可以断言的程度。
     */
    class SilentListener
    {
    public:
        SilentListener()
        {
            const AsynGyanis::Platform::Socket::Initialization network;
            // Windows 上 ::socket 交回 SOCKET（64 位），本框架的套接字句柄一律按 int 传（与 Platform::Socket 同一口径）
            m_descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            EXPECT_NE(m_descriptor, -1);
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0U;
            EXPECT_EQ(::bind(m_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
            EXPECT_EQ(::listen(m_descriptor, 4), 0);

            sockaddr_in bound{};
            socklen_t   boundLength = static_cast<socklen_t>(sizeof(bound));
            EXPECT_EQ(::getsockname(m_descriptor, reinterpret_cast<sockaddr *>(&bound), &boundLength), 0);
            m_port = ntohs(bound.sin_port);
        }

        ~SilentListener()
        {
            if (m_descriptor != -1)
            {
                static_cast<void>(AsynGyanis::Platform::FileDescriptor::close(m_descriptor));
            }
        }

        SilentListener(const SilentListener &)            = delete;
        SilentListener &operator=(const SilentListener &) = delete;

        /// @brief 内核分配的监听端口
        [[nodiscard]] std::uint16_t port() const noexcept
        {
            return m_port;
        }

    private:
        int           m_descriptor{-1}; ///< 只 bind + listen，永不 accept
        std::uint16_t m_port{0};        ///< 实际端口
    };

    /// 有界等到条件成立：异步交付的断言前都要先自己把成立条件等出来
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

    /// 造一份指向给定端口的出口配置
    OtlpHttpSpanExporter::Configuration makeCollectorConfiguration(const std::uint16_t port)
    {
        OtlpHttpSpanExporter::Configuration configuration;
        configuration.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/v1/traces";
        return configuration;
    }

    /// 一批一条节
    std::vector<SpanRecord> makeSingleRecordBatch(const std::string &name = "collector-operation")
    {
        SpanRecord record = makeRecord(name, kTraceId, kSpanId);
        record.kind       = SpanKind::Server;
        return {record};
    }

    const TraceResource &testResource()
    {
        static const TraceResource resource{.serviceName = "otlp-exporter-test", .serviceVersion = {}};
        return resource;
    }
} // namespace

/**
 * @brief 投出去的一批真的落在采集端上：正文是合法的 OTLP JSON，媒体类型与附加头部都对
 */
TEST(OtlpHttpSpanExporter, PostsTheRenderedBatchAndCarriesTheExtraHeaders)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto configuration         = makeCollectorConfiguration(fixture->listeningPort());
    configuration.extraHeaders = {{"Authorization", std::string{kCollectorToken}}};
    OtlpHttpSpanExporter exporter{configuration};

    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch()));
    ASSERT_TRUE(waitUntil([&state] { return state.m_requestCount.load(std::memory_order_relaxed) >= 1; })) << "采集端一条请求都没收到";
    EXPECT_TRUE(waitUntil([&exporter] { return exporter.deliveredBatchCount() == 1U; }));

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
    EXPECT_EQ(exporter.exporterName(), "otlp-http");
    EXPECT_EQ(exporter.failedBatchCount(), 0U);
    EXPECT_EQ(exporter.droppedBatchCount(), 0U);
    EXPECT_EQ(exporter.pendingBatchCount(), 0U);

    const auto parsed = AsynGyanis::Base::parseConfigValue(body);
    ASSERT_TRUE(parsed.has_value()) << "采集端收到的正文不是合法 JSON：" << body;
    EXPECT_EQ(jsonTextOf(parsed->at("resourceSpans").at(0).at("resource").at("attributes").at(0).at("value").at("stringValue")), "otlp-exporter-test");
    EXPECT_EQ(jsonTextOf(parsed->at("resourceSpans").at(0).at("scopeSpans").at(0).at("spans").at(0).at("name")), "collector-operation");
}

/**
 * @brief 采集端回了非 2xx：这批算没送达，原因里带上那个状态码
 */
TEST(OtlpHttpSpanExporter, CountsNonSuccessStatusAsFailure)
{
    CollectorState state;
    state.m_responseStatusCode.store(429, std::memory_order_relaxed);
    const auto fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpHttpSpanExporter exporter{makeCollectorConfiguration(fixture->listeningPort())};
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch()));
    ASSERT_TRUE(waitUntil([&exporter] { return exporter.failedBatchCount() == 1U; })) << "429 没被算成失败";
    EXPECT_EQ(exporter.deliveredBatchCount(), 0U);
    EXPECT_NE(exporter.lastFailureReason().find("429"), std::string::npos) << exporter.lastFailureReason();
}

/**
 * @brief 采集端根本连不上：这批算没送达而不是无声消失
 */
TEST(OtlpHttpSpanExporter, ReportsUnreachableCollectorAsFailure)
{
    auto configuration           = makeCollectorConfiguration(borrowUnusedPort());
    configuration.requestTimeout = std::chrono::milliseconds{1500};
    OtlpHttpSpanExporter exporter{configuration};

    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch()));
    EXPECT_TRUE(waitUntil([&exporter] { return exporter.failedBatchCount() == 1U; })) << "连不上也必须报出来：" << exporter.lastFailureReason();
    EXPECT_FALSE(exporter.lastFailureReason().empty());
    // 投递本身是成了的（已进队列并交给 HTTP 层），损失只在出口自己这一侧记账
    EXPECT_EQ(exporter.droppedBatchCount(), 0U);
}

/**
 * @brief 正文没能成形的一批当场拒收，且根本不会去碰网络；紧跟的正常批不受影响
 */
TEST(OtlpHttpSpanExporter, CountsUnformattableBatchAsDroppedWithoutSending)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpHttpSpanExporter    exporter{makeCollectorConfiguration(fixture->listeningPort())};
    std::vector<SpanRecord> poisoned = makeSingleRecordBatch();
    poisoned[0].name                 = std::string{"bad\xffname"};
    EXPECT_FALSE(exporter.exportSpans(testResource(), poisoned));
    EXPECT_EQ(exporter.droppedBatchCount(), 1U);
    EXPECT_EQ(exporter.pendingBatchCount(), 0U);
    EXPECT_NE(exporter.lastFailureReason().find("没能成形"), std::string::npos) << exporter.lastFailureReason();

    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch()));
    EXPECT_TRUE(waitUntil([&state] { return state.m_requestCount.load(std::memory_order_relaxed) >= 1; }));
}

/**
 * @brief 构造期就拒的写法：地址不成形、占用本出口自己会写的头部、含会撕裂请求行的字节、上界为 0
 */
TEST(OtlpHttpSpanExporter, RejectsUnusableConfigurationAtConstruction)
{
    struct Case
    {
        OtlpHttpSpanExporter::Configuration configuration;
        std::string                         expectedFragment;
    };

    std::vector<Case> cases;
    cases.push_back({OtlpHttpSpanExporter::Configuration{}, "采集端地址为空"});

    auto garbageUrl     = OtlpHttpSpanExporter::Configuration{};
    garbageUrl.endpoint = "http://"; ///< 有协议名却没有主机：parseUrl 判非法
    cases.push_back({garbageUrl, "采集端地址"});

    auto reservedConnection         = makeCollectorConfiguration(4318);
    reservedConnection.extraHeaders = {{"Connection", "close"}};
    cases.push_back({reservedConnection, "Connection"});

    auto reservedContentType         = makeCollectorConfiguration(4318);
    reservedContentType.extraHeaders = {{"CONTENT-TYPE", "text/plain"}};
    cases.push_back({reservedContentType, "占用"});

    auto unsafeValue         = makeCollectorConfiguration(4318);
    unsafeValue.extraHeaders = {{"Authorization", "Bearer bad\r\nvalue"}};
    cases.push_back({unsafeValue, "CR"});

    auto zeroBound                     = makeCollectorConfiguration(4318);
    zeroBound.maximumPendingBatchCount = 0U;
    cases.push_back({zeroBound, "批数上界"});

    auto zeroTimeout            = makeCollectorConfiguration(4318);
    zeroTimeout.shutdownTimeout = std::chrono::milliseconds::zero();
    cases.push_back({zeroTimeout, "收尾期限"});

    for (const Case &testCase: cases)
    {
        try
        {
            OtlpHttpSpanExporter exporter{testCase.configuration};
            FAIL() << "这份配置本该在构造期就被拒：" << testCase.expectedFragment;
        } catch (const AsynGyanis::Base::InvalidArgumentException &exception)
        {
            EXPECT_NE(std::string{exception.what()}.find(testCase.expectedFragment), std::string::npos)
                    << "拒绝原因里要点名「" << testCase.expectedFragment << "」：" << exception.what();
        }
    }
}

/**
 * @brief 收尾之后的投递当场拒收并计数：那时再收新批等于永远发不完
 */
TEST(OtlpHttpSpanExporter, DropsBatchesOfferedAfterShutdown)
{
    OtlpHttpSpanExporter exporter{makeCollectorConfiguration(borrowUnusedPort())};
    exporter.shutdown();
    EXPECT_FALSE(exporter.exportSpans(testResource(), makeSingleRecordBatch()));
    EXPECT_EQ(exporter.droppedBatchCount(), 1U);
    // 收尾是幂等的：析构会再来一次，不该出错也不该卡住
    exporter.shutdown();
}

/**
 * @brief 收尾要把队列里已收下的发完再走：进程正常退出时最后那批链路最需要看
 */
TEST(OtlpHttpSpanExporter, ShutdownDrainsThePendingQueue)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto exporter = std::make_unique<OtlpHttpSpanExporter>(makeCollectorConfiguration(fixture->listeningPort()));
    ASSERT_TRUE(exporter->exportSpans(testResource(), makeSingleRecordBatch("first")));
    ASSERT_TRUE(exporter->exportSpans(testResource(), makeSingleRecordBatch("second")));
    exporter.reset(); ///< 析构走同一条收尾路径

    EXPECT_EQ(state.m_requestCount.load(std::memory_order_relaxed), 2) << "两条都该在收尾前发出去";
}

/**
 * @brief 队列到上界只丢新到的并计数：采集端卡住时内存有用，而不是无限攒
 * @details 成立条件由用例自己造：对端收下连接却永不作答，第一条请求就得等满整个时限，
 *          「队列里压着一条、又来一条、第三条撞界」因此是确定的，不靠调度时序。
 */
TEST(OtlpHttpSpanExporter, DropsBatchesBeyondTheQueueBound)
{
    const SilentListener blackHole;
    auto                 configuration     = makeCollectorConfiguration(blackHole.port());
    configuration.requestTimeout           = std::chrono::milliseconds{3000};
    configuration.shutdownTimeout          = std::chrono::milliseconds{100};
    configuration.maximumPendingBatchCount = 1U;
    OtlpHttpSpanExporter exporter{configuration};

    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("in-flight")));
    // 等第一条确实被取走（离开队列、卡在 HTTP 层里）：这一刻起队列是空的，界才好判
    ASSERT_TRUE(waitUntil([&exporter] { return exporter.pendingBatchCount() == 0U; })) << "出口线程没把第一条取走";

    EXPECT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("queued")));
    EXPECT_EQ(exporter.pendingBatchCount(), 1U);
    EXPECT_FALSE(exporter.exportSpans(testResource(), makeSingleRecordBatch("overflow")));
    EXPECT_EQ(exporter.droppedBatchCount(), 1U);
    EXPECT_EQ(exporter.deliveredBatchCount(), 0U);
}

/**
 * @brief 与 Tracer 接线：收口的节经编排器的出口线程一路发到采集端
 */
TEST(OtlpHttpSpanExporter, TracerDeliversThroughTheExporter)
{
    CollectorState state;
    const auto     fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    Tracer::Configuration tracerConfiguration;
    tracerConfiguration.serviceName          = "wired-through-tracer";
    tracerConfiguration.exportBatchSpanCount = 1U;
    tracerConfiguration.exportInterval       = std::chrono::milliseconds{60000};
    const auto tracer                        = Tracer::create(tracerConfiguration);
    const auto exporter                      = std::make_shared<OtlpHttpSpanExporter>(makeCollectorConfiguration(fixture->listeningPort()));
    tracer->addExporter(exporter);

    Span span = tracer->startSpan("through-the-wire", SpanKind::Server);
    span.setAttribute("http.request.method", std::string_view{"GET"});
    span.finish();
    tracer->flush();

    ASSERT_TRUE(waitUntil([&exporter] { return exporter->deliveredBatchCount() == 1U; })) << "Tracer 交出去的一批没能发到采集端：" << exporter->lastFailureReason();
    EXPECT_EQ(tracer->exportedSpanCount(), 1U);
    EXPECT_TRUE(waitUntil(
            [&state]
            {
                const std::lock_guard lock(state.m_mutex);
                return state.m_body.find("through-the-wire") != std::string::npos;
            }))
            << "采集端收到的正文里没有那条节的名字";
}
