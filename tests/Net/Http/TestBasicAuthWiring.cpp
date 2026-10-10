// Basic 认证的**接线**证据：中间件挂到 router 上之后，真回环连接上确实拿到 401 挑战或 403 拒绝。
// 管道用例（TestMiddleware.cpp 的 BasicAuthMiddleware.*）是手工 `setOverTls()` 的，它证明这段逻辑本身；
// 「这条连接走没走 TLS」由会话从连接上取下来，挂错位置、或者只挂在某一条协议通道上，都只有真连接测得出。

#include "Net/Http/HttpRequest.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"

#include "Base/Coding/Base64.h"
#include "Base/Coding/SecureCompare.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    // 回环夹具与客户端跟其它服务端用例共用一份实现
    using namespace HttpTestSupport;

    namespace
    {
        /// 挑战取值：realm 要原样出现在线上，用例按整条头断言
        constexpr std::string_view kAuthRealm = "ops-backend";

        /// 这份凭据在两侧都被 verify 认下，只在「传输层事实」这一格上分出不同结论
        constexpr std::string_view kAcceptedCredential = "ops:s3cr3t";

        /**
         * @brief 装配一道只认 `ops:s3cr3t` 的 Basic 闸
         * @param requireSecureTransport 明文连接是否允许收凭据（用例要同时跑默认闸门与显式放开两侧）
         * @return BasicAuthOptions 配好的取值
         */
        [[nodiscard]] BasicAuthOptions makeAuthOptions(const bool requireSecureTransport)
        {
            BasicAuthOptions options;
            options.realm                  = std::string(kAuthRealm);
            options.protectedPaths         = {"/admin"};
            options.requireSecureTransport = requireSecureTransport;
            options.verify                 = [](const std::string_view user, const std::string_view secret)
            { return Base::constantTimeEquals(user, "ops") && Base::constantTimeEquals(secret, "s3cr3t"); };
            return options;
        }

        /**
         * @brief 登记两条路由：名单内的 `/admin` 与名单外的 `/public`，正文各带自己的名字
         * @details 正文写成「admin-granted」/「public-granted」而不是统一的 "ok"：被闸住时响应正文
         *          里有没有这个词就是「下游到底有没有被调用」的直接证据
         * @param router 待登记的路由表
         * @param eventLoop 未使用，与夹具的注册回调签名一致
         */
        void registerGuardedRoutes(Router &router, Core::EventLoop &eventLoop)
        {
            static_cast<void>(eventLoop);
            static_cast<void>(router.get("/admin",
                                         [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                         {
                                             response.setBody("admin-granted");
                                             co_return;
                                         }));
            static_cast<void>(router.get("/public",
                                         [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                         {
                                             response.setBody("public-granted");
                                             co_return;
                                         }));
        }

        /**
         * @brief 一条带 Basic 凭据的请求原文
         * @return std::string 供 `makeRequestText` 的头部行列表使用
         */
        [[nodiscard]] std::string credentialHeaderLine()
        {
            return "Authorization: Basic " + Base::base64Encode(kAcceptedCredential);
        }
    } // namespace

    /**
     * @brief 钉住：没给凭据的真连接拿到 401，挑战整条头原样上线，名单外的路径不受影响
     * @details `requireSecureTransport` 这里显式关掉：回环夹具是明文的，默认闸门会先把请求摁在 403，
     *          挑战通路（401 + `WWW-Authenticate`）就永远到不了线上。闸门本身由下一条钉
     */
    TEST(BasicAuthWiring, ChallengesUnauthenticatedRequestOnARealLoopbackConnection)
    {
        const BasicAuthOptions options = makeAuthOptions(false);

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, registerGuardedRoutes, HttpParserLimits{},
                                         [&options](TestHttpServer &server) { server.router().addMiddleware(basicAuthMiddleware(options)); });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const auto challenged = sendAndReadResponse(fixture.listeningPort(), makeRequestText("GET /admin HTTP/1.1"));
        ASSERT_TRUE(challenged.has_value()) << "未授权请求的响应没有落回来";
        EXPECT_TRUE(challenged->headers.starts_with("HTTP/1.1 401")) << "名单内的路径没被闸住：" << challenged->headers.substr(0, 60);
        EXPECT_NE(challenged->headers.find("www-authenticate: Basic realm=\"ops-backend\", charset=\"US-ASCII\""), std::string::npos)
                << "401 没带上可重试的挑战：" << challenged->headers;
        EXPECT_EQ(challenged->body.find("granted"), std::string::npos) << "被拒的请求还是把业务正文答了出去：" << challenged->body;

        const auto untouched = sendAndReadResponse(fixture.listeningPort(), makeRequestText("GET /public HTTP/1.1"));
        ASSERT_TRUE(untouched.has_value()) << "名单外请求的响应没有落回来";
        EXPECT_TRUE(untouched->headers.starts_with("HTTP/1.1 200")) << "名单外的路径被顺带闸住了：" << untouched->headers.substr(0, 60);
        EXPECT_EQ(untouched->body, "public-granted");
    }

    /**
     * @brief 钉住：明文真连接上带**正确**凭据也是 403 且不回挑战，显式放开之后同一条请求才走通
     * @details 两条断言是一对：只钉 403 那一侧，「闸门把所有请求都拒了」这种退化照样绿；
     *          只钉放开那一侧，又证明不了默认值站在收紧侧。403 不带 `WWW-Authenticate` 是有意为之——
     *          那等于邀请客户端把口令发进一条不加密的信道
     */
    TEST(BasicAuthWiring, RefusesCredentialsOverCleartextUntilTheOperatorOptsIn)
    {
        const BasicAuthOptions gated       = makeAuthOptions(true);
        const BasicAuthOptions relaxed     = makeAuthOptions(false);
        const std::string      requestText = makeRequestText("GET /admin HTTP/1.1", {credentialHeaderLine()});

        RunningHttpServerFixture strictFixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, registerGuardedRoutes, HttpParserLimits{},
                                               [&gated](TestHttpServer &server) { server.router().addMiddleware(basicAuthMiddleware(gated)); });
        ASSERT_TRUE(strictFixture.awaitRunning(kWaitTimeout)) << "默认闸门的服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const auto refused = sendAndReadResponse(strictFixture.listeningPort(), requestText);
        ASSERT_TRUE(refused.has_value()) << "明文带凭据的请求没有响应回来";
        EXPECT_TRUE(refused->headers.starts_with("HTTP/1.1 403")) << "明文连接收到了凭据（回的是 401/200 而不是 403）：" << refused->headers.substr(0, 60);
        EXPECT_EQ(refused->headers.find("www-authenticate"), std::string::npos) << "403 还附一句「请给 Basic 凭据」，等于一边拒一边催：" << refused->headers;
        EXPECT_EQ(refused->body.find("granted"), std::string::npos) << "明文闸门没挡住业务：" << refused->body;

        RunningHttpServerFixture openFixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, registerGuardedRoutes, HttpParserLimits{},
                                             [&relaxed](TestHttpServer &server) { server.router().addMiddleware(basicAuthMiddleware(relaxed)); });
        ASSERT_TRUE(openFixture.awaitRunning(kWaitTimeout)) << "显式放开的服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const auto admitted = sendAndReadResponse(openFixture.listeningPort(), requestText);
        ASSERT_TRUE(admitted.has_value()) << "放开之后的请求没有响应回来";
        EXPECT_TRUE(admitted->headers.starts_with("HTTP/1.1 200")) << "显式放开之后同一条请求仍不通：" << admitted->headers.substr(0, 60);
        EXPECT_EQ(admitted->body, "admin-granted");
    }
} // namespace AsynGyanis::Net
