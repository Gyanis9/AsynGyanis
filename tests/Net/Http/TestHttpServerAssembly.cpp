// server 段配置的装配入口：配置里的键要真的落到服务器上，特别是 expose_metrics——
// 它此前在库内没有任何消费方，写了也不注册端点，是最典型的「静默不生效」。

#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerAssembly.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        constexpr std::chrono::milliseconds kEndpointTimeout{2000};

        /**
         * @brief 抓一次指定路径，回响应首行与已到达的正文（连接用完即关）
         * @param port 目标端口
         * @param path 请求路径
         * @param extraHeaderLine 额外的一条请求头（空串表示不加），用于带 Authorization 这类判定
         */
        [[nodiscard]] std::string fetchPath(const std::uint16_t port, const std::string_view path, const std::string_view extraHeaderLine = {})
        {
            LoopbackClient client(port);
            if (!client.isValid())
            {
                return {};
            }
            std::string receivedText;
            if (!client.sendText(std::format("GET {} HTTP/1.1\r\nHost: localhost\r\n{}\r\nConnection: close\r\n\r\n", path,
                                             extraHeaderLine.empty() ? "" : std::string(extraHeaderLine) + "\r\n"),
                                 kEndpointTimeout))
            {
                return receivedText;
            }
            static_cast<void>(client.waitForText(receivedText, "HTTP/1.1 ", kEndpointTimeout));
            return receivedText;
        }
    } // namespace

    /**
     * @brief 钉住：配置里的超时与报文上限，装配之后从服务器读回来是同一份
     * @details 这条钉的是「装配真的下发了」而不是「setter 能用」——键与生效点分两处写时，
     *          漏接一个键就会静默保持默认值，而默认值往往正好像是对的
     */
    TEST(HttpServerAssembly, AppliesLimitsAndParserLimits)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        HttpServerConfiguration configuration;
        configuration.limits.idleTimeout              = std::chrono::seconds{41};
        configuration.parserLimits.maximumHeaderCount = 77;

        ASSERT_TRUE(applyHttpServerConfiguration(server, configuration).has_value());
        EXPECT_EQ(server.limits().idleTimeout, std::chrono::seconds{41}) << "limits.idle_timeout 没下发到服务器";
        EXPECT_EQ(server.parserLimits().maximumHeaderCount, 77u) << "parser_limits.header_count 没下发到服务器";
    }

    /**
     * @brief 钉住：共享限额器与配置标量不一致时当场拒绝
     * @details 限额器是多台的共用对象，静默挑一边会让「配置写了 16、实际跑 64」无任何痕迹
     */
    TEST(HttpServerAssembly, RejectsSharedLimiterThatDisagreesWithConfiguration)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        HttpServerAssemblyContext context;
        context.sharedPerIpLimiter = std::make_shared<PerIpConnectionLimiter>(64);

        HttpServerConfiguration configuration;
        configuration.maximumConnectionsPerIp = 16;

        const auto outcome = applyHttpServerConfiguration(server, configuration, context);
        ASSERT_FALSE(outcome.has_value()) << "两处上限不一致却被静默接受了";
        EXPECT_NE(outcome.error().find("装配冲突"), std::string::npos) << outcome.error();
    }

    /**
     * @brief 钉住：expose_metrics=true 装配后，/metrics、/healthz、/debug/loops 三个运维面都能答
     * @details 这条键此前没有任何消费方：配置里打勾、端点一个都不注册。三件套同开是因为它们都不鉴权，
     *          只开其一会让「抓不到数」与「以为没暴露」互相伪装
     */
    TEST(HttpServerAssembly, RegistersOperationEndpointsWhenConfigured)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server)
        {
            HttpServerConfiguration configuration;
            configuration.exposeMetrics = true;
            const auto outcome          = applyHttpServerConfiguration(server, configuration);
            ASSERT_TRUE(outcome.has_value()) << outcome.error();
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {}, HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::string metricsText = fetchPath(fixture.listeningPort(), "/metrics");
        EXPECT_NE(metricsText.find("HTTP/1.1 200"), std::string::npos) << metricsText;
        EXPECT_NE(metricsText.find("asyn_http_requests_total"), std::string::npos) << metricsText;

        const std::string healthText = fetchPath(fixture.listeningPort(), "/healthz");
        EXPECT_NE(healthText.find("HTTP/1.1 200"), std::string::npos) << healthText;

        const std::string loopsText = fetchPath(fixture.listeningPort(), "/debug/loops");
        EXPECT_NE(loopsText.find("HTTP/1.1 200"), std::string::npos) << loopsText;
    }

    /**
     * @brief 钉住：配了令牌的运维端点，没带对凭据进不来，而 /healthz 仍不查
     * @details /metrics 与 /debug/loops 各钉「缺凭据 401」「带错 401」「带对 200」，/healthz 钉「无凭据仍 200」——
     *          这条不对称是刻意的：进程存活探针要能被编排器无凭据访问，给它加令牌只会让人把探针关掉
     */
    TEST(HttpServerAssembly, GuardsOperationEndpointsWithBearerToken)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server)
        {
            HttpServerConfiguration configuration;
            configuration.exposeMetrics  = true;
            configuration.opsBearerToken = "assemble-test-token";
            const auto outcome           = applyHttpServerConfiguration(server, configuration);
            ASSERT_TRUE(outcome.has_value()) << outcome.error();
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {}, HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::string withoutToken = fetchPath(fixture.listeningPort(), "/metrics");
        EXPECT_NE(withoutToken.find("HTTP/1.1 401"), std::string::npos) << withoutToken;
        EXPECT_NE(withoutToken.find("www-authenticate"), std::string::npos) << "401 要一并给出怎么带凭据：" << withoutToken;

        const std::string wrongToken = fetchPath(fixture.listeningPort(), "/metrics", "Authorization: Bearer 另一个令牌");
        EXPECT_NE(wrongToken.find("HTTP/1.1 401"), std::string::npos) << wrongToken;

        // 方案名大小写不敏感（RFC 9110 §11.2）：只认 "Bearer" 的写法会把合规客户端拒在门外
        const std::string lowerCaseScheme = fetchPath(fixture.listeningPort(), "/metrics", "Authorization: bearer assemble-test-token");
        EXPECT_NE(lowerCaseScheme.find("HTTP/1.1 200"), std::string::npos) << lowerCaseScheme;

        const std::string loopsWithoutToken = fetchPath(fixture.listeningPort(), "/debug/loops");
        EXPECT_NE(loopsWithoutToken.find("HTTP/1.1 401"), std::string::npos) << loopsWithoutToken;

        const std::string healthWithoutToken = fetchPath(fixture.listeningPort(), "/healthz");
        EXPECT_NE(healthWithoutToken.find("HTTP/1.1 200"), std::string::npos) << "存活探针不该被鉴权挡住：" << healthWithoutToken;
    }

    /**
     * @brief 钉住：配了管理口端口号时，业务口上一个运维端点都不挂
     * @details 这条钉的是「收口真的收口」：端点留在业务口上就等于跟着业务口一起公开（业务口常开在
     *          0.0.0.0），而只起一台管理监听器不算证明——必须证明业务口那边确实空了。
     *          三个路径都钉 404：只钉 /metrics 会放过「漏挂 /debug/loops」这一半
     */
    TEST(HttpServerAssembly, KeepsOperationEndpointsOffTheBusinessPortWhenAdminPortIsSet)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server)
        {
            HttpServerConfiguration configuration;
            configuration.exposeMetrics = true;
            configuration.metricsPort   = 9101;
            const auto outcome          = applyHttpServerConfiguration(server, configuration);
            ASSERT_TRUE(outcome.has_value()) << outcome.error();
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {}, HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        for (const std::string_view path: {"/metrics", "/healthz", "/debug/loops"})
        {
            const std::string text = fetchPath(fixture.listeningPort(), path);
            EXPECT_NE(text.find("HTTP/1.1 404"), std::string::npos) << path << " 仍挂在业务口上：" << text;
        }
    }

    /**
     * @brief 钉住：整机限流摊到每台，且共享桶要按摊后的数比对
     * @details 与连接数那条判据同源：桶的速率在构造时定死，收下一份整机速率的桶而配置写着摊分，
     *          四个进程就放行四倍，而配置文件上看不出差别
     */
    TEST(HttpServerAssembly, SplitsWholeMachineRateAcrossWorkerProcesses)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        HttpServerConfiguration configuration;
        configuration.requestsPerSecond       = 100.0;
        configuration.rateLimitBurstCapacity  = 8.0;
        configuration.maximumConnections      = 0;
        configuration.maximumConnectionsPerIp = 0;

        HttpServerAssemblyContext matchingContext;
        matchingContext.workerProcessCount    = 4;
        matchingContext.sharedRateLimitBucket = std::make_shared<TokenBucket>(25.0, 2.0);
        ASSERT_TRUE(applyHttpServerConfiguration(server, configuration, matchingContext).has_value()) << "与摊分一致的共享桶被误拒";

        Core::EventLoop           rejectingLoop;
        TestHttpServer            rejectingServer(rejectingLoop, Core::InetAddress::localhost(0));
        HttpServerAssemblyContext mismatchedContext;
        mismatchedContext.workerProcessCount    = 4;
        mismatchedContext.sharedRateLimitBucket = std::make_shared<TokenBucket>(100.0, 8.0);
        const auto outcome                      = applyHttpServerConfiguration(rejectingServer, configuration, mismatchedContext);
        ASSERT_FALSE(outcome.has_value()) << "共享桶还是整机速率就直接收下了：那等于放行四倍";
        EXPECT_NE(outcome.error().find("25"), std::string::npos) << "拒因要点名摊后的速率：「" << outcome.error() << "」";
    }

    /**
     * @brief 钉住：限流摊分的三条取整规则
     * @details 速率走精确除法（0.5 摊两份是 0.25，向上取整会把每台抬成 1 请求/s 反而多放行），
     *          桶容量兜在 1.0（小于 1 的桶攒不满一枚令牌，TokenBucket 构造会当场抛），
     *          速率 0 时两个量都不摊（没有速率就没有桶）
     */
    TEST(HttpServerAssembly, SharesRateExactlyAndFloorsBurstAtOneToken)
    {
        EXPECT_DOUBLE_EQ(perProcessRateLimit(100.0, 8.0, 4).requestsPerSecond, 25.0);
        EXPECT_DOUBLE_EQ(perProcessRateLimit(100.0, 8.0, 4).burstCapacity, 2.0);
        EXPECT_DOUBLE_EQ(perProcessRateLimit(0.5, 1.0, 2).requestsPerSecond, 0.25) << "速率向上取整会把每台多放行一倍";
        EXPECT_DOUBLE_EQ(perProcessRateLimit(8.0, 2.0, 4).burstCapacity, 1.0) << "整机突发量小于进程数时容量必须兜在 1，否则一个请求都放不出";
        EXPECT_DOUBLE_EQ(perProcessRateLimit(30.0, 5.0, 1).requestsPerSecond, 30.0) << "单进程不做除法";
        EXPECT_DOUBLE_EQ(perProcessRateLimit(0.0, 5.0, 4).requestsPerSecond, 0.0) << "0 是不限流，摊成 0 才对";
        EXPECT_DOUBLE_EQ(perProcessRateLimit(0.0, 5.0, 4).burstCapacity, 1.0) << "不限流时的容量取默认桶容量，不该跟着整机数走";
    }

    /**
     * @brief 钉住：整机上限按 worker 进程数摊到每台，且共享限额器要按摊后的数比对
     * @details 每个进程只数自己那份账，配置里的整机数起 N 个进程就是 N 倍放行——这条钉的是
     *          「摊分真的发生」：100 摊给 4 个进程 → 本台 25；共享限额器给 100 要被拒，
     *          给 25 要接受（拿整机数比对的话，正确的 25 反而会被拒）
     */
    TEST(HttpServerAssembly, SplitsWholeMachineCapsAcrossWorkerProcesses)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        HttpServerConfiguration configuration;
        configuration.maximumConnections      = 100;
        configuration.maximumConnectionsPerIp = 100;

        HttpServerAssemblyContext matchingContext;
        matchingContext.workerProcessCount = 4;
        matchingContext.sharedPerIpLimiter = std::make_shared<PerIpConnectionLimiter>(25);
        ASSERT_TRUE(applyHttpServerConfiguration(server, configuration, matchingContext).has_value()) << "与摊分一致的共享限额器被误拒";
        EXPECT_EQ(server.maximumConnections(), 25u) << "整机 100 摊给 4 个进程，本台真正卡的应是 25";

        Core::EventLoop           rejectingLoop;
        TestHttpServer            rejectingServer(rejectingLoop, Core::InetAddress::localhost(0));
        HttpServerAssemblyContext mismatchedContext;
        mismatchedContext.workerProcessCount = 4;
        mismatchedContext.sharedPerIpLimiter = std::make_shared<PerIpConnectionLimiter>(100);
        const auto outcome                   = applyHttpServerConfiguration(rejectingServer, configuration, mismatchedContext);
        ASSERT_FALSE(outcome.has_value()) << "共享限额器还是整机数就直接收下了：那等于放行四倍";
        EXPECT_NE(outcome.error().find("25"), std::string::npos) << "拒因要点名摊后的数：「" << outcome.error() << "」";
    }

    /**
     * @brief 钉住：server 段的 memory_budget_bytes 真的落到服务器上，且落的是摊到本进程那一份
     * @details 预算对象此前只能由调用方手递（库内无人构造），配置里写字节数没有任何人读——键存在而
     *          生效点缺失，正是「配了不生效却不出声」那一类。四档一起钉：默认不建账、标量建摊后的账、
     *          共享对象与摊分一致时收且收的就是那一份、不一致时当场拒并点名摊后的数
     */
    TEST(HttpServerAssembly, AppliesMemoryBudgetFromConfiguration)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        // 没配就是不建账：空指针（没有这道账）与「有一份上限为 0 的账」读法不同，不能混
        ASSERT_TRUE(applyHttpServerConfiguration(server, HttpServerConfiguration{}).has_value());
        EXPECT_EQ(server.memoryBudget().get(), nullptr) << "没配 memory_budget_bytes 却建了一份账";

        HttpServerConfiguration configuration;
        configuration.memoryBudgetBytes = 1000;
        HttpServerAssemblyContext context;
        context.workerProcessCount = 4;
        ASSERT_TRUE(applyHttpServerConfiguration(server, configuration, context).has_value());
        ASSERT_NE(server.memoryBudget().get(), nullptr) << "配了字节数而服务器上仍是空指针：键没接到 setter";
        EXPECT_EQ(server.memoryBudget()->maximumTotalBytes(), 250u) << "整机 1000 摊给 4 个进程，本台真正卡的应是 250";

        const auto sharedBudget    = std::make_shared<HttpMemoryBudget>(250);
        context.sharedMemoryBudget = sharedBudget;
        ASSERT_TRUE(applyHttpServerConfiguration(server, configuration, context).has_value()) << "与摊分一致的共享预算被误拒";
        EXPECT_EQ(server.memoryBudget().get(), sharedBudget.get()) << "服务器上的账应当就是传进来的那一份，而不是另起的新账";

        Core::EventLoop           rejectingLoop;
        TestHttpServer            rejectingServer(rejectingLoop, Core::InetAddress::localhost(0));
        HttpServerAssemblyContext mismatchedContext;
        mismatchedContext.workerProcessCount = 4;
        mismatchedContext.sharedMemoryBudget = std::make_shared<HttpMemoryBudget>(1000);
        const auto outcome                   = applyHttpServerConfiguration(rejectingServer, configuration, mismatchedContext);
        ASSERT_FALSE(outcome.has_value()) << "共享预算还是整机数就直接收下了：四个进程合计放行四倍";
        EXPECT_NE(outcome.error().find("250"), std::string::npos) << "拒因要点名摊后的数：「" << outcome.error() << "」";
    }

    /**
     * @brief 钉住：workerProcessCount=0 被拒，而不是悄悄当成「不摊」
     */
    TEST(HttpServerAssembly, RejectsZeroWorkerProcessCount)
    {
        Core::EventLoop loop;
        TestHttpServer  server(loop, Core::InetAddress::localhost(0));

        HttpServerAssemblyContext context;
        context.workerProcessCount = 0;

        const auto outcome = applyHttpServerConfiguration(server, HttpServerConfiguration{}, context);
        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find("workerProcessCount"), std::string::npos) << outcome.error();
    }

    /**
     * @brief 钉住：摊分的取整与两个直通档
     * @details 向上取整（101/4=26）钉的是「余数不能凭空消失」；0 直通钉的是「显式不限不能被摊成 1」；
     *          count=1 直通钉的是单进程不做除法
     */
    TEST(HttpServerAssembly, SharesRoundUpAndPassThroughSpecialValues)
    {
        EXPECT_EQ(perProcessShare(100, 4), 25u);
        EXPECT_EQ(perProcessShare(101, 4), 26u) << "向下取整会让整机上限永远达不到，而差多少没人去算";
        EXPECT_EQ(perProcessShare(0, 4), 0u) << "0 是显式不限，摊成 1 就是把服务关掉";
        EXPECT_EQ(perProcessShare(100, 1), 100u);
        EXPECT_EQ(perProcessShare(100, 0), 100u) << "count=0 在这里按不摊处理，拒绝由装配出口负责";
    }

    /**
     * @brief 钉住：默认（expose_metrics=false）不注册任何运维端点，仍然走兜底 404
     * @details 与上一条成对：只钉「开了有」会放过「不开也有」，那只说明键没被消费而不是说明默认安全
     */
    TEST(HttpServerAssembly, LeavesOperationEndpointsUnregisteredByDefault)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server) { ASSERT_TRUE(applyHttpServerConfiguration(server, HttpServerConfiguration{}).has_value()); };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {}, HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::string metricsText = fetchPath(fixture.listeningPort(), "/metrics");
        EXPECT_NE(metricsText.find("HTTP/1.1 404"), std::string::npos) << "没打开 expose_metrics 却注册了指标端点：「" << metricsText << "」";
    }

} // namespace AsynGyanis::Net
