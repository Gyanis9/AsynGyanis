// 按 Host 选站的端到端用例：真实 h1 服务器上三个站点各回各的正文，中间件按层生效
#include "HttpTestSupport.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        /**
         * @brief 组装一条只带 Host 的请求报文
         * @details 不用 makeRequestText()：它固定补一条 `host: test`，再自己写一条就成了同名两条，
         *          那时用例判的就不是「选站」而是「重复 Host 头部怎么办」了
         * @param path 请求路径
         * @param hostHeader Host 头部原文（可以带端口、大写、结尾根点）
         * @return std::string 完整报文文本
         */
        [[nodiscard]] std::string hostRequestText(const std::string_view path, const std::string_view hostHeader)
        {
            return "GET " + std::string(path) + " HTTP/1.1\r\nhost: " + std::string(hostHeader) + "\r\nconnection: close\r\n\r\n";
        }

        /**
         * @brief 生成一个「只写固定正文」的处理函数
         * @param bodyText 响应正文
         * @return Router::Handler 处理函数
         */
        [[nodiscard]] Router::Handler makeTextHandler(std::string bodyText)
        {
            return [bodyText = std::move(bodyText)]([[maybe_unused]] HttpRequest &, HttpResponse &response) -> Core::Task<void>
            {
                response.setBody(bodyText);
                co_return;
            };
        }

        /**
         * @brief 登记三个站点：精确名、通配名与一张兜底根表，外加两层中间件
         * @param router 待登记的路由器（服务器的根路由）
         * @param loop 服务器的事件循环（本用例不需要，按 RouteRegistrar 的形状收着）
         */
        void registerThreeSites(Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(loop);

            router.get("/who", makeTextHandler("default-site"));
            router.get("/root-only", makeTextHandler("root-only"));
            // 根中间件对三个站点都生效：它跑在最外层
            router.addMiddleware(
                    [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                    {
                        response.setHeader("x-root", "1");
                        co_await next();
                    });

            Router &apiSite = router.virtualHost("api.example.com");
            apiSite.get("/who", makeTextHandler("api-site"));
            apiSite.addMiddleware(
                    [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                    {
                        response.setHeader("x-site", "api");
                        co_await next();
                    });

            router.virtualHost("*.example.org").get("/who", makeTextHandler("wildcard-site"));
        }

        /**
         * @brief 起一台三站点的服务器
         * @return std::unique_ptr<RunningHttpServerFixture> 已投递 start() 的夹具，起不来时为空
         */
        [[nodiscard]] std::unique_ptr<RunningHttpServerFixture> makeThreeSiteServer()
        {
            return std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerThreeSites);
        }
    } // namespace

    /**
     * @brief 钉住线上行为：同一个端口按 Host 落到三个不同站点
     * @details 单元测试走的是内存里的请求对象，这条证明对端送来的 Host 原文经过解析与选站
     *          之后仍然落在同一张表上
     */
    TEST(HttpVirtualHostEndToEnd, ServesDistinctSitesByHostHeader)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeThreeSiteServer();
        ASSERT_TRUE(fixture->awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        struct Case
        {
            std::string host;
            std::string expectedBody;
        };
        const std::vector<Case> cases{
                Case{"api.example.com", "api-site"},       Case{"API.Example.COM:443", "api-site"}, Case{"api.example.com.", "api-site"},
                Case{"cdn.example.org", "wildcard-site"},  Case{"example.org", "default-site"}, // 通配不收顶点域名
                Case{"other.example.net", "default-site"},
        };
        for (const Case &testCase: cases)
        {
            const std::optional<ParsedResponse> reply = sendAndReadResponse(fixture->listeningPort(), hostRequestText("/who", testCase.host));
            ASSERT_TRUE(reply.has_value()) << "Host「" << testCase.host << "」没拿到响应";
            EXPECT_EQ(reply->body, testCase.expectedBody) << "Host「" << testCase.host << "」落到了别的站点";
            EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 200")) << "Host「" << testCase.host << "」的响应不是 200：" << reply->headers.substr(0, 40);
        }
    }

    /**
     * @brief 根中间件对每个站点都写头部，站点自己的中间件只对自己写
     * @details 横切逻辑（CORS、访问日志）登记在根路由上、加了主机就被绕过，是安全事故的形状
     */
    TEST(HttpVirtualHostEndToEnd, RootMiddlewareAppliesToEverySiteAndSiteMiddlewareOnlyToItsOwn)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeThreeSiteServer();
        ASSERT_TRUE(fixture->awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        const std::optional<ParsedResponse> apiReply = sendAndReadResponse(fixture->listeningPort(), hostRequestText("/who", "api.example.com"));
        ASSERT_TRUE(apiReply.has_value());
        EXPECT_NE(apiReply->headers.find("x-root"), std::string::npos) << "外层中间件的头部没到线上，说明选站跳过了根管道";
        EXPECT_NE(apiReply->headers.find("x-site"), std::string::npos) << "本主机自己的中间件没跑";

        const std::optional<ParsedResponse> defaultReply = sendAndReadResponse(fixture->listeningPort(), hostRequestText("/who", "other.example.net"));
        ASSERT_TRUE(defaultReply.has_value());
        EXPECT_NE(defaultReply->headers.find("x-root"), std::string::npos);
        EXPECT_EQ(defaultReply->headers.find("x-site"), std::string::npos) << "api 主机的中间件跑到了别的站点上，站点之间就漏了";
    }

    /**
     * @brief 命中的主机没有这条路径时回 404，且仍带着根中间件写下的头部
     * @details 404 不能回落去问根表（那等于站点隔离漏一半），也不能把外层头部抹掉
     */
    TEST(HttpVirtualHostEndToEnd, MissingPathOnHostSiteAnswersNotFoundWithoutAskingRootTable)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeThreeSiteServer();
        ASSERT_TRUE(fixture->awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        const std::optional<ParsedResponse> reply = sendAndReadResponse(fixture->listeningPort(), hostRequestText("/root-only", "api.example.com"));
        ASSERT_TRUE(reply.has_value());
        EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 404")) << "根表上的 /root-only 被 api 站点拿去应答，等于两个域名共用一套路由：" << reply->headers.substr(0, 40);
        EXPECT_NE(reply->headers.find("x-root"), std::string::npos) << "回 404 时把外层中间件写下的头部复位掉了";
    }
} // namespace AsynGyanis::Net
