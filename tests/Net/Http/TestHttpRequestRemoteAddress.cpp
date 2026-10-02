// HttpRequest::remoteAddress() / remoteIp() 的直测：业务处理器只拿到请求与响应两个对象，来源地址必须由
// 会话在派发之前落进请求里。这里钉四层——字段本身的形状（默认空、设置后读得到、reset() 清掉）、
// remoteIp() 的剥端口规则（只认本框架产出的三种形状，认不出来就原样交回）、h1 真回环连接上业务读到的
// 是这条连接的地址、以及开了 PROXY 协议之后读到的是**代理交来的真实
// 来源**而不是代理记账。h2 与 h3 的同一判据分别落在 tests/Net/Http2 与 tests/Net/Http3。

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    // 回环夹具与客户端集中在 HttpTestSupport.h 里，与服务端限额、观测性用例共用一份实现
    using namespace HttpTestSupport;

    namespace
    {
        /**
         * @brief 把请求里的来源地址与不带端口的 IP 一起回显出去的路由
         * @param router 待注册的路由
         */
        void registerEchoRoute(Router &router)
        {
            static_cast<void>(router.get("/who",
                                         [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                         {
                                             // 两个字段一起回显：按来源限流要的键是不带端口的那一段，
                                             // 端口每条连接都换，拿整条地址当键会得到「每条连接一个桶」
                                             response.setBody("peer=" + request.remoteAddress() + " ip=" + request.remoteIp());
                                             co_return;
                                         }));
        }

        /**
         * @brief 从回显正文里取出指定标记后面那段
         * @param responseText 完整响应文本
         * @param marker 要读的标记（含结尾的 '='）
         * @return std::string 标记之后到下一个空白/换行为止的文本；没找到标记时返回空串
         */
        [[nodiscard]] std::string fieldOf(const std::string &responseText, const std::string_view marker)
        {
            const std::size_t markerPosition = responseText.find(marker);
            if (markerPosition == std::string::npos)
            {
                return {};
            }
            const std::size_t start = markerPosition + marker.size();
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

    /**
     * @brief remoteIp() 剥端口：只认本框架 InetAddress::toString() 产出的那三种形状
     * @details 认不出来时**原样交回整条文本**而不是切一刀——切错给的是一个静默失效的限额键，
     *          比交回原样难查得多（裸写的 "::1" 是这条判据的反例）
     */
    TEST(HttpRequestRemoteAddress, RemoteIpStripsThePortOnlyForTheShapesWeProduce)
    {
        struct Case
        {
            std::string_view address;
            std::string_view expectedIp;
        };

        const std::array<Case, 9> cases{{
                {"127.0.0.1:51234", "127.0.0.1"},                       // 点分四段 + 端口
                {"::ffff:198.51.100.7:50000", "::ffff:198.51.100.7"},   // 双栈打出来的映射形式
                {"[::ffff:198.51.100.7]:50000", "::ffff:198.51.100.7"}, // 双栈监听上映射地址也带着方括号
                {"[fe80::1%3]:8080", "fe80::1%3"},                      // IPv6 带作用域号：端口在右括号之后
                {"[2001:db8::1]", "2001:db8::1"},                       // 带方括号却没端口
                {"[fe80::1", "[fe80::1"},                               // 有左括号却没有右括号：认不出来，原样交回
                {"192.0.2.3", "192.0.2.3"},                             // 没端口
                {"::1", "::1"},                                         // 裸写的 IPv6：不许切成残段
                {"", ""},                                               // 会话没落定
        }};

        for (const Case &entry: cases)
        {
            HttpRequest request;
            request.setRemoteAddress(entry.address);
            EXPECT_EQ(request.remoteIp(), std::string(entry.expectedIp)) << "输入：" << entry.address;
        }
    }

    TEST(HttpRequestRemoteAddress, RemoteIpFollowsResetAlongWithTheAddress)
    {
        HttpRequest request;
        request.setRemoteAddress("203.0.113.7:44000");
        ASSERT_EQ(request.remoteIp(), "203.0.113.7");
        request.reset();
        EXPECT_TRUE(request.remoteIp().empty()) << "reset() 清了地址却没清 IP";
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
        const std::string peer = fieldOf(responseText, "peer=");
        // 回环上客户端的地址是确定的；端口由内核分配，因此只判「带端口且不是 0」
        EXPECT_NE(peer.find("127.0.0.1:"), std::string::npos) << "业务读到的来源不是这条连接的对端：" << peer;
        EXPECT_NE(peer, "127.0.0.1:0") << "端口没带上来：" << peer;
        // 按来源限流要的键：同一条连接上剥掉端口之后必须是纯 IP
        EXPECT_EQ(fieldOf(responseText, "ip="), "127.0.0.1") << "剥端口之后得到的不是这条连接的对端 IP";
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
        EXPECT_EQ(fieldOf(responseText, "peer="), "203.0.113.7:44000") << "业务读到的还是代理自己的地址";
        EXPECT_EQ(fieldOf(responseText, "ip="), "203.0.113.7") << "按来源限流的键没跟着 PROXY 头改写";
    }
} // namespace AsynGyanis::Net
