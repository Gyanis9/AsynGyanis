// 安全响应头的**接线**证据：中间件挂在 router 上之后，真回环连接上拿到的响应里确实带着这些头，
// 且明文连接上不带 HSTS。纯管道用例（TestMiddleware.cpp 的 SecurityHeadersMiddleware.*）只证明
// 这段逻辑本身正确，证明不了它真的在服务器上跑——挂错位置、只在某一条协议通道上注册，都会让
// 「配了安全头」这件事在浏览器里悄悄不生效。

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
    // 回环夹具与客户端与服务端限额、观测性用例共用一份实现
    using namespace HttpTestSupport;

    namespace
    {
        /**
         * @brief 把响应头那一截取出来（正文之前、按 CRLF 分行的那一段）
         * @param responseText 完整响应文本
         * @return std::string 头部段原文；没有 CRLFCRLF 时交回整段
         */
        [[nodiscard]] std::string headerSectionOf(const std::string &responseText)
        {
            const std::size_t endOfHeaders = responseText.find("\r\n\r\n");
            return endOfHeaders == std::string::npos ? responseText : responseText.substr(0, endOfHeaders);
        }
    } // namespace

    /**
     * @brief 钉住：中间件挂在 router 上之后，真连接上的响应带着默认的那三条收紧头
     * @details 这一条同时是 `Router::addMiddleware()` 与响应序列化通路的证据：头要是被
     *          `HttpResponse` 存下却没写上线，管道用例照样全绿
     */
    TEST(SecurityHeadersWiring, ServesTheDefaultHeadersOnARealLoopbackConnection)
    {
        SecurityHeadersOptions options;
        options.contentSecurityPolicy                    = "default-src 'none'";
        options.strictTransportSecurityMaxAgeSeconds     = 31'536'000;
        options.strictTransportSecurityIncludeSubDomains = true;

        RunningHttpServerFixture fixture(
                HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                [](Router &router, Core::EventLoop &)
                {
                    static_cast<void>(router.get("/guarded",
                                                 [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                 {
                                                     response.setBody("ok");
                                                     co_return;
                                                 }));
                },
                HttpParserLimits{}, [&options](TestHttpServer &server) { server.router().addMiddleware(securityHeadersMiddleware(options)); });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /guarded HTTP/1.1"), kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "ok", kWaitTimeout)) << "响应没有落回来：" << responseText;
        const std::string headers = headerSectionOf(responseText);

        EXPECT_NE(headers.find("x-content-type-options: nosniff"), std::string::npos) << "线上响应没带 nosniff：" << headers;
        EXPECT_NE(headers.find("x-frame-options: DENY"), std::string::npos) << "线上响应没带 X-Frame-Options：" << headers;
        EXPECT_NE(headers.find("referrer-policy: no-referrer"), std::string::npos) << "线上响应没带 Referrer-Policy：" << headers;
        EXPECT_NE(headers.find("content-security-policy: default-src 'none'"), std::string::npos) << "配了策略却没上线：" << headers;
        // 明文回环这一跳：配了 HSTS 也**不该**出现这一行（RFC 6797 §7.2 让浏览器忽略明文收到的它）
        EXPECT_EQ(headers.find("strict-transport-security"), std::string::npos) << "明文连接发出了 HSTS：" << headers;

        // 错误应答同样要挂上：头是在下游之前写的，路由器写 404 正文时它们已经就位——
        // 「给每条响应」这句话要有 404 这一半才完整（漏掉时未命中的页面正是最容易被探测的那一类）
        ASSERT_TRUE(client.sendText(makeRequestText("GET /nope HTTP/1.1"), kWaitTimeout)) << "未命中路由的那条请求未能写入";
        std::string notFoundText;
        ASSERT_TRUE(client.waitForText(notFoundText, "404", kWaitTimeout)) << "未命中路由的应答没落回来：" << notFoundText;
        EXPECT_NE(headerSectionOf(notFoundText).find("x-content-type-options: nosniff"), std::string::npos) << "404 的响应没带安全头：" << headerSectionOf(notFoundText);
    }

    /**
     * @brief 钉住：没有那条中间件时，这些头一条都不出现
     * @details 上一条的对照。没有它，「头是被别处顺手加上的」这种假绿就分不出来——
     *          本仓此前一条安全响应头都不发，这一格必须是空的
     */
    TEST(SecurityHeadersWiring, ServesNothingOfTheKindWithoutTheMiddleware)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             static_cast<void>(router.get("/plain",
                                                                          [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                                          {
                                                                              response.setBody("ok");
                                                                              co_return;
                                                                          }));
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /plain HTTP/1.1"), kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "ok", kWaitTimeout)) << "响应没有落回来：" << responseText;
        const std::string headers = headerSectionOf(responseText);

        EXPECT_EQ(headers.find("x-content-type-options"), std::string::npos) << "没挂中间件却发出了 nosniff：" << headers;
        EXPECT_EQ(headers.find("x-frame-options"), std::string::npos) << "没挂中间件却发出了 X-Frame-Options：" << headers;
        EXPECT_EQ(headers.find("referrer-policy"), std::string::npos) << "没挂中间件却发出了 Referrer-Policy：" << headers;
    }
} // namespace AsynGyanis::Net
