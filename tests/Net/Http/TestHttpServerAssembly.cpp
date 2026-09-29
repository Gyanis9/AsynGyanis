// server 段配置的装配入口：配置里的键要真的落到服务器上，特别是 expose_metrics——
// 它此前在库内没有任何消费方，写了也不注册端点，是最典型的「静默不生效」。

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

        /// 抓一次指定路径，回响应首行与已到达的正文（连接用完即关）
        [[nodiscard]] std::string fetchPath(const std::uint16_t port, const std::string_view path)
        {
            LoopbackClient client(port);
            if (!client.isValid())
            {
                return {};
            }
            std::string receivedText;
            if (!client.sendText(std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path), kEndpointTimeout))
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
