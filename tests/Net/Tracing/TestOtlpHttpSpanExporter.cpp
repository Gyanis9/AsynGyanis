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
#include <limits>
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
        std::mutex                                         m_mutex;
        std::string                                        m_body{};                  ///< 收到的最后一条正文
        std::string                                        m_contentType{};           ///< 收到的媒体类型
        std::string                                        m_authorization{};         ///< 收到的 Authorization 头部
        std::atomic<int>                                   m_responseStatusCode{200}; ///< 该回的状态码
        std::atomic<long long>                             m_retryAfterSeconds{-1};   ///< 非负时回复带上 `Retry-After: <秒>`，-1 表示不带这条头
        std::atomic<int>                                   m_requestCount{0};         ///< 收到过几条请求
        std::vector<std::chrono::steady_clock::time_point> m_requestTimes{};          ///< 每条请求到达的刻

        /**
         * @brief 第二条请求落在第一条之后多久（毫秒）
         * @details 退避间隔在采集端这一侧量：两条请求到达的刻都记在同一台机器的同一根时钟上，
         *          不经过「用例线程轮询到条件成立」那一段，因此判据不受轮询周期与调度抖动支配。
         * @return long long 还没收到第二条时交回 -1
         */
        [[nodiscard]] long long secondRequestDelayMs()
        {
            const std::lock_guard lock(m_mutex);
            if (m_requestTimes.size() < 2)
            {
                return -1;
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(m_requestTimes[1] - m_requestTimes[0]).count();
        }
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
                            state->m_requestTimes.push_back(std::chrono::steady_clock::now());
                        }
                        static_cast<void>(state->m_requestCount.fetch_add(1, std::memory_order_relaxed));
                        response.setStatus(state->m_responseStatusCode.load(std::memory_order_relaxed));
                        // 这条头只对 429 与 503 有定义，用例把状态码与它配着给
                        const long long retryAfterSeconds = state->m_retryAfterSeconds.load(std::memory_order_relaxed);
                        if (retryAfterSeconds >= 0)
                        {
                            static_cast<void>(response.setHeader("Retry-After", std::to_string(retryAfterSeconds)));
                        }
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
 * @brief 一次失败之后停下并退避：队列里没发的那几批留在原地，等采集端缓过来再补发
 * @details 这条判据钉的是「故障时只丢一批」而不是「把整队列撞光」。间隔取 1 秒：短到用例等得起，
 *          又长到「停下」这件事能被确定地观察到——不退避的实现会在几毫秒内把三条全发完并全按失败计。
 */
TEST(OtlpHttpSpanExporter, KeepsTheQueueBackAfterAFailingCollector)
{
    CollectorState state;
    state.m_responseStatusCode.store(503, std::memory_order_relaxed);
    state.m_retryAfterSeconds.store(1, std::memory_order_relaxed);
    const auto fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpHttpSpanExporter exporter{makeCollectorConfiguration(fixture->listeningPort())};
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("first")));
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("second")));
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("third")));

    ASSERT_TRUE(waitUntil([&exporter] { return exporter.failedBatchCount() == 1U; })) << "503 没被算成失败";
    EXPECT_EQ(exporter.pendingBatchCount(), 2U) << "失败之后还在队列里等的那两条不该被撞掉";
    EXPECT_EQ(state.m_requestCount.load(std::memory_order_relaxed), 1) << "退避期内不该有第二条请求落到采集端上";

    // 采集端缓过来：留在队列里的两条接着发完，一条都不必因为故障而丢
    state.m_retryAfterSeconds.store(-1, std::memory_order_relaxed);
    state.m_responseStatusCode.store(200, std::memory_order_relaxed);
    ASSERT_TRUE(waitUntil([&exporter] { return exporter.deliveredBatchCount() == 2U; })) << "采集端恢复后队列里的批次没被补发";
    EXPECT_EQ(exporter.pendingBatchCount(), 0U);
    EXPECT_EQ(state.m_requestCount.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(exporter.droppedBatchCount(), 0U);
}

/**
 * @brief 退避时长取自响应里的 `Retry-After`，并且被封顶
 * @details 两头都要钉住，缺一头都是静默的错：只钉「等了很久」看不出它是不是在照一条越界的头干等，
 *          只钉「很快就重试」看不出这条头压根没进账。取 `Retry-After: 60` 让两个判据分开成立——
 *          间隔必须远大于 200 毫秒的地板（否则等于没读这条头），第二条请求又必须在几秒内到达
 *          （否则 60 秒被照单全收，链路易失静默停摆）。判据取采集端记下的到达时刻，间隔两侧各留五倍余量。
 */
TEST(OtlpHttpSpanExporter, PausesForTheCollectorsRetryAfterWindow)
{
    CollectorState state;
    state.m_responseStatusCode.store(429, std::memory_order_relaxed);
    state.m_retryAfterSeconds.store(60, std::memory_order_relaxed);
    const auto fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto configuration            = makeCollectorConfiguration(fixture->listeningPort());
    configuration.shutdownTimeout = std::chrono::milliseconds{100}; ///< 收尾不必等满整个退避窗口：期限到就按丢弃计
    OtlpHttpSpanExporter exporter{configuration};
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("a")));
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("b")));

    ASSERT_TRUE(waitUntil([&state] { return state.m_requestCount.load(std::memory_order_relaxed) >= 2; }, std::chrono::milliseconds{15000}))
            << "封顶后的退避之后必须再来一次：60 秒不该照单全收";
    const long long delayMs = state.secondRequestDelayMs();
    EXPECT_GE(delayMs, 1000) << "Retry-After 被当成了不存在，等于这条头没进账";
    EXPECT_LE(delayMs, 10000) << "封顶没生效：对端写多少就干等多少";
}

/**
 * @brief 大到装不下的 `Retry-After` 仍然按封顶退避，而不是折成负数后立刻重撞
 * @details `Retry-After: 9223372036854775807` 是一条语法合法的头（RFC 9110 §10.1.2 只要若干个十进制数字），
 *          而秒折成毫秒要乘 1000：先折再钳会在 int64 上有符号溢出（UB），折出来是负数、clamp 取地板
 *          200 毫秒——对端明确说了限流，重试频率反而抬到最高。判据与上一条同形（间隔既不能短到头没进账，
 *          也不能长过封顶），差别只在喂进来的取值：把折算挪到钳之前，这条会在「太短」那一侧变红。
 */
TEST(OtlpHttpSpanExporter, CapsAnUnrepresentableRetryAfterInsteadOfRetryingImmediately)
{
    CollectorState state;
    state.m_responseStatusCode.store(429, std::memory_order_relaxed);
    state.m_retryAfterSeconds.store(std::numeric_limits<long long>::max(), std::memory_order_relaxed);
    const auto fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    auto configuration            = makeCollectorConfiguration(fixture->listeningPort());
    configuration.shutdownTimeout = std::chrono::milliseconds{100}; ///< 收尾不必等满整个退避窗口
    OtlpHttpSpanExporter exporter{configuration};
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("a")));
    ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch("b")));

    ASSERT_TRUE(waitUntil([&state] { return state.m_requestCount.load(std::memory_order_relaxed) >= 2; }, std::chrono::milliseconds{15000})) << "越界的 Retry-After 不该让出口停摆";
    const long long delayMs = state.secondRequestDelayMs();
    EXPECT_GE(delayMs, 1000) << "越界的取值被折成负数再钳到地板：等于对端说了限流而我们反而加速去撞";
    EXPECT_LE(delayMs, 10000) << "封顶没生效：对端写多少就干等多少";
}

/**
 * @brief 收尾要一路发到队列真的空了才算发完：失败退避这条出口不能把「还剩几条没发」报成「发完了」
 * @details 判据是「一条都不能凭空消失」：队列里三条全被采集端拒掉，那这三条都必须落在失败计数上。
 *          只按「不再收新批」判定排空的话，收尾会在队列还有两条时提前返回，而那两条既不计失败也不计丢弃。
 */
TEST(OtlpHttpSpanExporter, ShutdownKeepsTryingUntilTheQueueIsActuallyEmpty)
{
    CollectorState state;
    state.m_responseStatusCode.store(503, std::memory_order_relaxed);
    const auto fixture = startCollector(&state);
    ASSERT_TRUE(fixture != nullptr);

    OtlpHttpSpanExporter exporter{makeCollectorConfiguration(fixture->listeningPort())};
    for (const char *name: {"first", "second", "third"})
    {
        ASSERT_TRUE(exporter.exportSpans(testResource(), makeSingleRecordBatch(name)));
    }
    // 至少失败一条之后再叫停：这里判的是「>= 1」而不是「== 1」，因为「恰好停在第一条」是退避带来的时序，
    // 拿它当前提会把这条用例和上面那条钉同一个性质；本用例要钉的是收尾的排空判据
    ASSERT_TRUE(waitUntil([&exporter] { return exporter.failedBatchCount() >= 1U; })) << "503 没被算成失败";
    exporter.shutdown();

    EXPECT_EQ(exporter.failedBatchCount(), 3U) << "每条都该有一次明确的失败记账";
    EXPECT_EQ(exporter.droppedBatchCount(), 0U) << "采集端答得上话（回 503），就没该按丢弃计的批次";
    EXPECT_EQ(exporter.pendingBatchCount(), 0U) << "收尾返回时队列必须真的空了";
    EXPECT_EQ(state.m_requestCount.load(std::memory_order_relaxed), 3);
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
