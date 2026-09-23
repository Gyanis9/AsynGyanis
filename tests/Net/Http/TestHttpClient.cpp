// HTTP 出站客户端端到端用例
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        Core::Task<void> doGetTask(Core::EventLoop &loop,
                                   std::unique_ptr<HttpClientResponse> &result,
                                   std::string url,
                                   std::chrono::milliseconds requestTimeout)
        {
            result = co_await HttpClient::get(loop, url, requestTimeout);
            loop.stop();
        }

        static std::unique_ptr<HttpClientResponse> doGet(std::string_view url,
                                                          std::chrono::milliseconds requestTimeout = HttpClient::kDefaultRequestTimeout)
        {
            Core::EventLoop loop;
            std::unique_ptr<HttpClientResponse> result;
            auto work = doGetTask(loop, result, std::string(url), requestTimeout);
            if (!work.isReady()) loop.scheduler().schedule(work.handle());
            loop.run();
            return result;
        }
    }

    TEST(HttpClientUrl, ParsesSimpleUrl)
    {
        auto u = parseUrl("http://example.com/path");
        EXPECT_EQ(u.scheme, "http");  EXPECT_EQ(u.host, "example.com");
        EXPECT_EQ(u.port, 80);        EXPECT_EQ(u.path, "/path");
    }

    TEST(HttpClientUrl, ParsesUrlWithPort)
    {
        auto u = parseUrl("http://localhost:8080/test");
        EXPECT_EQ(u.host, "localhost"); EXPECT_EQ(u.port, 8080); EXPECT_EQ(u.path, "/test");
    }

    TEST(HttpClientUrl, ParsesHttpsScheme)
    {
        auto u = parseUrl("https://api.example.com/v1/data");
        EXPECT_EQ(u.scheme, "https"); EXPECT_EQ(u.port, 443);
    }

    TEST(HttpClientUrl, DefaultsPathToSlash)
    {
        auto u = parseUrl("http://example.com");
        EXPECT_EQ(u.path, "/");
    }

    /**
     * @brief 钉住：协议名大小写无关，写成 HTTPS 也走 TLS
     * @details 早先按精确串比 "https"，"HTTPS://host" 落到 http 那条路上 = 明文连出去，
     *          而调用方以为自己写的是加密地址（RFC 3986 §6.2.3 规定方案名大小写无关）
     */
    TEST(HttpClientUrl, RecognizesUppercaseHttpsScheme)
    {
        const ParsedUrl u = parseUrl("HTTPS://api.example.com/v1");
        EXPECT_EQ(u.scheme, "https") << "大写协议被降级成明文";
        EXPECT_EQ(u.port, 443);
        EXPECT_EQ(u.host, "api.example.com");
    }

    /**
     * @brief 钉住：写错的端口当场拒绝，不回落到 80
     * @details 这四形都是「冒号后面不是端口」：非数字、空、超出 65535、以及带尾巴的数字。
     *          回落会静默改掉对端地址，一个 https URL 能因此连到明文 80 端口上
     */
    TEST(HttpClientUrl, RejectsMalformedPort)
    {
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com:abc/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com:/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com:99999/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com:8080x/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com:0/")), std::invalid_argument);
    }

    /**
     * @brief 钉住：带方括号的 IPv6 字面量可用，括号在拆分时被去掉
     * @details 主机自带冒号，不先按 RFC 3986 §3.2.2 认方括号就分不清哪段是端口，
     *          「Host: ::1」也会把头部与端口分隔符混成一团
     */
    TEST(HttpClientUrl, ParsesBracketedIpv6Authority)
    {
        const ParsedUrl withPort = parseUrl("http://[::1]:8080/x");
        EXPECT_EQ(withPort.host, "::1") << "方括号该在拆分时去掉，交给底层按 IP 解析";
        EXPECT_EQ(withPort.port, 8080);
        EXPECT_EQ(withPort.path, "/x");

        const ParsedUrl withoutPort = parseUrl("https://[2001:db8::1]/");
        EXPECT_EQ(withoutPort.host, "2001:db8::1");
        EXPECT_EQ(withoutPort.port, 443);
    }

    /**
     * @brief 钉住：其余畸形 URL 一律拒绝而不是猜一个
     * @details 五条分别对应：没括号的 IPv6（拆出来的主机是半截地址）、括号没闭合、
     *          括号后跟了别的字符、没有主机、协议不是 http(s)
     */
    TEST(HttpClientUrl, RejectsMalformedAuthorityAndScheme)
    {
        ASSERT_THROW(static_cast<void>(parseUrl("http://::1:8080/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://[::1/x")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://[::1]extra/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://:8080/")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("ftp://example.com/x")), std::invalid_argument);
    }

    TEST(HttpClient, GetsLocalhostAndReceives200)
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(
                HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));
        auto port = fixture->listeningPort();  ASSERT_NE(port, 0U);
        auto url = "http://127.0.0.1:" + std::to_string(port) + "/hello";
        auto r = doGet(url);
        ASSERT_NE(r, nullptr) << "请求失败";
        EXPECT_EQ(r->statusCode, 200);
        EXPECT_NE(r->body.find("served-hello"), std::string::npos) << "正文：" << r->body;
    }

    /**
     * @brief 对端只连不应答时，请求级时限到点就掐断连接，调用方不会被永远挂住
     * @details 构造一条处理得比客户端时限慢得多的路由：客户端必须在自己的时限内收场，
     *          而不是一直等到服务器把响应写完
     */
    TEST(HttpClient, TimesOutAgainstSilentServer)
    {
        constexpr auto kSlowRouteTime = std::chrono::milliseconds{900};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        const std::uint16_t port = fixture.listeningPort();
        ASSERT_NE(port, 0U);
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/slow";

        const auto startTime     = std::chrono::steady_clock::now();
        const auto response      = doGet(url, std::chrono::milliseconds{150});
        const auto elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);

        EXPECT_EQ(response, nullptr) << "超时的请求不该给出响应";
        EXPECT_LT(elapsedMillis, kSlowRouteTime)
                << "客户端一直等到了服务器写完响应（用时 " << elapsedMillis.count() << " 毫秒）：时限没有生效";

        // 收尾前等这条慢路由自己跑完：夹具销毁会先让循环停手，不该把一条「要等定时器才肯结束」
        // 的在途请求留到那之后（既有用例都刻意避开这个状态）
        std::this_thread::sleep_for(kSlowRouteTime);
    }
} // namespace AsynGyanis::Net