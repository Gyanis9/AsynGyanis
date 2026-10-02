// HttpRequest::remoteAddress() 的直测：业务处理器只拿到请求与响应两个对象，来源地址必须由会话
// 在派发之前落进请求里。这里钉三层——字段本身的形状（默认空、设置后读得到、reset() 清掉）、
// h1 真回环连接上业务读到的是这条连接的地址、以及开了 PROXY 协议之后读到的是**代理交来的真实
// 来源**而不是代理记账。h2 与 h3 的同一判据分别落在 tests/Net/Http2 与 tests/Net/Http3。

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
    // 回环夹具与客户端集中在 HttpTestSupport.h 里，与服务端限额、观测性用例共用一份实现
    using namespace HttpTestSupport;

    namespace
    {
        /**
         * @brief 把请求里的来源地址原样回显出去的路由
         * @param router 待注册的路由
         */
        void registerEchoRoute(Router &router)
        {
            static_cast<void>(router.get("/who",
                                         [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                         {
                                             // 交回的是副本，因此这里连 response 的正文一起持有它即可
                                             response.setBody("peer=" + request.remoteAddress());
                                             co_return;
                                         }));
        }

        /**
         * @brief 从「peer=<地址>」里取出地址段
         * @param responseText 完整响应文本
         * @return std::string 标记之后的那段；没找到标记时返回空串
         */
        [[nodiscard]] std::string echoedPeerOf(const std::string &responseText)
        {
            const std::size_t marker = responseText.find("peer=");
            if (marker == std::string::npos)
            {
                return {};
            }
            const std::size_t start = marker + 5U;
            const std::size_t end   = responseText.find_first_of("\r\n ", start);
            return responseText.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    } // namespace

    // ============================================================================
    // 字段本身
    // ============================================================================

    TEST(HttpRequestRemoteAddress, DefaultsToEmptyUntilTheSessionSetsIt)
    {
        HttpRequest request;
        EXPECT_TRUE(request.remoteAddress().empty()) << "未经会话落定的请求谎称自己有来源";

        request.setRemoteAddress("203.0.113.7:44000");
        EXPECT_EQ(request.remoteAddress(), "203.0.113.7:44000");
    }

    TEST(HttpRequestRemoteAddress, IsClearedByResetBecauseItBelongsToThePreviousMessage)
    {
        HttpRequest request;
        request.setRemoteAddress("203.0.113.7:44000");
        request.reset();
        // 保活连接上请求对象按连接复用：留着它，下一条报文就挂着上一条的来源做限流与审计
        EXPECT_TRUE(request.remoteAddress().empty()) << "reset() 没有清掉来源地址";
    }

    // ============================================================================
    // h1：真回环连接上业务读到的来源
    // ============================================================================

    TEST(HttpRequestRemoteAddress, HandlerSeesThePeerAddressOnAnH1Connection)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, [](Router &router, Core::EventLoop &) { registerEchoRoute(router); });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /who HTTP/1.1"), kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "peer=", kWaitTimeout)) << "业务没有回显来源地址：" << responseText;
        const std::string peer = echoedPeerOf(responseText);
        // 回环上客户端的地址是确定的；端口由内核分配，因此只判「带端口且不是 0」
        EXPECT_NE(peer.find("127.0.0.1:"), std::string::npos) << "业务读到的来源不是这条连接的对端：" << peer;
        EXPECT_NE(peer, "127.0.0.1:0") << "端口没带上来：" << peer;
    }

    /**
     * @brief 开了 PROXY 协议时，业务读到的必须是代理交来的真实来源
     * @details 这条是 TcpServer::setProxyProtocolRequired() 的文档承诺：「限额键、
     *          `HttpRequest::remoteAddress()` 与日志都跟着改」。坐在代理后面时，回环上看到的
     *          对端永远是代理——按来源限流与审计落的是谁，全看这一步有没有跟过去
     */
    TEST(HttpRequestRemoteAddress, HandlerSeesTheSourceAdvertisedByTheProxyHeader)
    {
        RunningHttpServerFixture fixture(
                HttpServerLimits{}, std::chrono::milliseconds{50}, {}, [](Router &router, Core::EventLoop &) { registerEchoRoute(router); }, HttpParserLimits{},
                [](TestHttpServer &server) { server.setProxyProtocolRequired(true); });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        // 带头必须**单独成段**先到：与请求挤进同一次发送时多出来的字节无处安放，服务器按设计收口这条连接
        ASSERT_TRUE(client.sendText("PROXY TCP4 203.0.113.7 198.51.100.7 44000 " + std::to_string(listeningPort) + "\r\n", kWaitTimeout)) << "PROXY 头未能写入";
        ASSERT_TRUE(fixture.awaitConnectionAccepted(kWaitTimeout)) << "带头的连接没被放行：头没被认出来";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /who HTTP/1.1"), kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "peer=", kWaitTimeout)) << "业务没有回显来源地址：" << responseText;
        EXPECT_EQ(echoedPeerOf(responseText), "203.0.113.7:44000") << "业务读到的还是代理自己的地址";
    }
} // namespace AsynGyanis::Net
