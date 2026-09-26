// HTTP 出站客户端端到端用例
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Gzip.h"
namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        Core::Task<void> doGetTask(Core::EventLoop &loop, std::unique_ptr<HttpClientResponse> &result, std::string url, std::chrono::milliseconds requestTimeout)
        {
            result = co_await HttpClient::get(loop, url, requestTimeout);
            loop.stop();
        }

        static std::unique_ptr<HttpClientResponse> doGet(std::string_view url, std::chrono::milliseconds requestTimeout = HttpClient::kDefaultRequestTimeout)
        {
            Core::EventLoop                     loop;
            std::unique_ptr<HttpClientResponse> result;
            auto                                work = doGetTask(loop, result, std::string(url), requestTimeout);
            if (!work.isReady())
                loop.scheduler().schedule(work.handle());
            loop.run();
            return result;
        }

        /**
         * @brief 走 send() 发一次请求，把成功正文与失败原因分别落到调用方的两个串里
         * @details 参数按值/按引用落到协程帧里，闭包对象不参与（惰性协程帧记的是闭包地址）
         */
        Core::Task<void> sendOnceTask(Core::EventLoop &loop, std::string url, HttpClientRequest request, std::string &observedBody, std::string &observedReason,
                                      std::chrono::milliseconds requestTimeout)
        {
            try
            {
                const auto sent = co_await HttpClient::send(loop, url, std::move(request), requestTimeout);
                if (sent.has_value())
                {
                    observedBody = sent->body;
                } else
                {
                    observedReason = sent.error();
                }
            } catch (const std::exception &failure)
            {
                observedReason = failure.what();
            }
            loop.stop();
        }

        struct SendOutcome
        {
            std::string body;
            std::string reason;
        };

        SendOutcome runSend(std::string_view url, const HttpClientRequest &request, std::chrono::milliseconds requestTimeout = HttpClient::kDefaultRequestTimeout)
        {
            Core::EventLoop loop;
            SendOutcome     outcome;
            auto            task = sendOnceTask(loop, std::string(url), request, outcome.body, outcome.reason, requestTimeout);
            if (!task.isReady())
                loop.scheduler().schedule(task.handle());
            loop.run();
            return outcome;
        }

        /**
         * @brief 走 send() 但把整份响应留下（要断言头部本身有没有被改掉）
         * @details runSend 只留正文与原因，够不上「解压后头部要跟正文一致」这类判据
         */
        Core::Task<void> sendCapturingTask(Core::EventLoop &loop, std::string url, HttpClientRequest request, std::unique_ptr<HttpClientResponse> &result,
                                           std::string &observedReason, std::chrono::milliseconds requestTimeout)
        {
            try
            {
                auto sent = co_await HttpClient::send(loop, url, std::move(request), requestTimeout);
                if (sent.has_value())
                {
                    result = std::make_unique<HttpClientResponse>(std::move(*sent));
                } else
                {
                    observedReason = sent.error();
                }
            } catch (const std::exception &failure)
            {
                observedReason = failure.what();
            }
            loop.stop();
        }
    } // namespace

    TEST(HttpClientUrl, ParsesSimpleUrl)
    {
        auto u = parseUrl("http://example.com/path");
        EXPECT_EQ(u.scheme, "http");
        EXPECT_EQ(u.host, "example.com");
        EXPECT_EQ(u.port, 80);
        EXPECT_EQ(u.path, "/path");
    }

    TEST(HttpClientUrl, ParsesUrlWithPort)
    {
        auto u = parseUrl("http://localhost:8080/test");
        EXPECT_EQ(u.host, "localhost");
        EXPECT_EQ(u.port, 8080);
        EXPECT_EQ(u.path, "/test");
    }

    TEST(HttpClientUrl, ParsesHttpsScheme)
    {
        auto u = parseUrl("https://api.example.com/v1/data");
        EXPECT_EQ(u.scheme, "https");
        EXPECT_EQ(u.port, 443);
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
        auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));
        auto port = fixture->listeningPort();
        ASSERT_NE(port, 0U);
        auto url = "http://127.0.0.1:" + std::to_string(port) + "/hello";
        auto r   = doGet(url);
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
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{900};
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
        EXPECT_LT(elapsedMillis, kSlowRouteTime) << "客户端一直等到了服务器写完响应（用时 " << elapsedMillis.count() << " 毫秒）：时限没有生效";

        // 收尾前等这条慢路由自己跑完：夹具销毁会先让循环停手，不该把一条「要等定时器才肯结束」
        // 的在途请求留到那之后（既有用例都刻意避开这个状态）
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 钉住：send() 把调用方给的附加头部原样上线
     * @details 客户端此前没有自定义头部的入口（认证头与 Accept-* 一类根本发不出去）。这里让服务端把
     *          自己收到的取值回显出来再比对——比检查客户端拼出的字符串硬，能同时盯住名字不改大小写、
     *          值不转义这两条
     */
    TEST(HttpClient, SendsCallerSuppliedRequestHeaders)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/echo-token",
                                                        [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            // 客户端没把这条头部发上来时，回显的是占位串
                                                            response.setBody(request.getHeader("x-token").value_or("<missing>"));
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/echo-token";

        HttpClientRequest request;
        request.headers.emplace_back("x-token", "abc123");
        const SendOutcome outcome = runSend(url, request);
        ASSERT_TRUE(outcome.reason.empty()) << "请求失败：" << outcome.reason;
        EXPECT_EQ(outcome.body, "abc123") << "调用方给的附加头部没能原样上线";
    }

    /**
     * @brief 钉住：会撕裂请求行的头部写法一律拒绝，不转义也不静默丢掉
     * @details 值里带 CR/LF 等于自己结束这一行再插一条新字段；名字带空格或冒号会拼出第二个字段；
     *          方法名进的是请求行开头，同样只准是 token。保留头部（Host、Content-Length、Connection）
     *          由客户端按这次请求的实际情况写，调用方给了就拒收而不是覆盖或并存
     */
    TEST(HttpClient, RejectsHeadersThatCouldSplitTheRequestLine)
    {
        // 校验排在动套接字之前，所以这里只需要一个「形如可用」的地址；万一校验漏了，请求会真发到
        // 本机夹具并拿到响应，下面的断言就把「没拒」这件事报出来，而不是悄悄等一次网络超时
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";

        struct BadHeader
        {
            std::string name;
            std::string value;
        };
        const std::array<BadHeader, 2> brokenValues{
                BadHeader{"x-token", "abc\r\nX-Injected: 1"},
                BadHeader{"x-token", "line\nfeed"},
        };
        for (const BadHeader &bad: brokenValues)
        {
            HttpClientRequest request;
            request.headers.emplace_back(bad.name, bad.value);
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "这一项本该被拒（拿到响应说明校验没生效）：" << bad.value;
            EXPECT_NE(outcome.reason.find("请求行"), std::string::npos) << "原因要点明是撕裂请求行：" << outcome.reason;
        }

        const std::array<std::string, 2> brokenNames{std::string{}, std::string{"x token"}};
        for (const std::string &name: brokenNames)
        {
            HttpClientRequest request;
            request.headers.emplace_back(name, "abc");
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "这个名字本该被拒：" << name;
            EXPECT_NE(outcome.reason.find("token"), std::string::npos) << "原因要点明头部名必须是 HTTP token：" << outcome.reason;
        }

        const std::array<std::string, 2> brokenMethods{std::string{}, std::string{"GET /x HTTP/1.1\r\nHost: evil"}};
        for (const std::string &method: brokenMethods)
        {
            HttpClientRequest request;
            request.method            = method;
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "这个方法名本该被拒：「" << method << "」";
            EXPECT_NE(outcome.reason.find("token"), std::string::npos) << "原因要点明方法名必须是 HTTP token：" << outcome.reason;
        }

        const std::array<std::string, 3> reservedNames{std::string{"host"}, std::string{"Content-Length"}, std::string{"CONNECTION"}};
        for (const std::string &reserved: reservedNames)
        {
            HttpClientRequest request;
            request.headers.emplace_back(reserved, "whatever");
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "保留头部本该拒收：" << reserved;
            EXPECT_NE(outcome.reason.find("由客户端"), std::string::npos) << "原因要说明这三项由客户端写：" << outcome.reason;
        }
    }

    /**
     * @brief 钉住：拿不到响应时，原因说清断在哪一段
     * @details 旧契约只交回一个空指针，「域名解析不出来」与「连上但 TLS 不过」在调用方眼里是同一件事，
     *          重试策略与告警都分不开。这里挑一段能在本机确定复现又不碰 TLS 的：连一个刚关掉的端口。
     *          刻意不走 https——客户端的 SSL_CTX 是进程级缓存，本二进制里后跑的 TLS 用例要先装好
     *          受信文件，这里先建了上下文就会把它们的信任配置挡在后面（同一份 CA 环境只在首次
     *          建上下文时被读到）
     */
    TEST(HttpClient, NamesTheStageThatFailed)
    {
        // 「刚关掉的监听端口」是确定没人听的写法：连它一定被立刻拒掉，比挑一个自认为空闲的端口可靠
        std::uint16_t closedPort = 0;
        {
            RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
            ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
            closedPort = fixture.listeningPort();
            ASSERT_NE(closedPort, 0U);
        }

        const HttpClientRequest request;
        const SendOutcome       refused = runSend("http://127.0.0.1:" + std::to_string(closedPort) + "/", request, std::chrono::seconds{5});
        EXPECT_TRUE(refused.body.empty());
        EXPECT_NE(refused.reason.find("建立 TCP 连接失败"), std::string::npos) << "要指出断在连接这一段：" << refused.reason;
    }

    // ============================================================================
    // 透明解压：代为声明 Accept-Encoding，并把压缩正文解回来
    // ============================================================================

    namespace
    {
        /// 一份够长的正文：压得动，也能从长度上分出「解过」与「没解」
        constexpr std::string_view kCompressiblePayload = "decompressed-payload-decompressed-payload-decompressed-payload";

        /**
         * @brief 三条路都按「对端会压缩」的前提配好
         * @details /gzip 与 /br 都带 Content-Encoding；/echo-accept 把收到的 Accept-Encoding
         *          回显成「条数|首条取值」——多加一条同名声明光看取值看不出来。
         * @param router 待登记路由
         */
        void registerEncodingRoutes(Router &router, Core::EventLoop &)
        {
            router.get("/gzip",
                       [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                       {
                           const std::optional<std::string> packed = gzipCompress(kCompressiblePayload);
                           if (!packed.has_value())
                           {
                               response.setStatus(500);
                               co_return;
                           }
                           static_cast<void>(response.setHeader("content-encoding", "gzip"));
                           response.setBody(*packed);
                           co_return;
                       });
            router.get("/br",
                       [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                       {
                           // 对端发了本端没声明过的编码：字节看着像正文，其实是别的压缩格式
                           static_cast<void>(response.setHeader("content-encoding", "br"));
                           response.setBody("brotli-not-here");
                           co_return;
                       });
            router.get("/echo-accept",
                       [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                       {
                           const std::vector<std::string> values = request.headerValues("accept-encoding");
                           response.setBody(std::to_string(values.size()) + "|" + (values.empty() ? std::string("<none>") : values.front()));
                           co_return;
                       });
        }

        /**
         * @brief 响应头部里有没有某一条（名字大小写不敏感）
         * @param response 已拿到的响应
         * @param lowerName 小写形式的头部名
         * @return true 表示带着这一条
         */
        bool hasHeaderField(const HttpClientResponse &response, const std::string_view lowerName)
        {
            for (const HttpClientHeaderField &field: response.headers)
            {
                if (field.first.size() != lowerName.size())
                {
                    continue;
                }
                std::string folded = field.first;
                std::transform(folded.begin(), folded.end(), folded.begin(),
                               [](const unsigned char byte) { return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + 32) : byte; });
                if (folded == lowerName)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    /**
     * @brief 钉住：本端代加声明时，压缩正文被解回来，且两条描述压缩的头部一起消失
     * @details content-encoding 已被兑现、content-length 描述的是压缩后的字节数——留着任何一条
     *          都是让下游按错的口径处理一份新正文。
     */
    TEST(HttpClient, DecompressesGzipResponseBodyAndDropsTheTwoDescribingHeaders)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerEncodingRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/gzip";

        Core::EventLoop                     loop;
        std::unique_ptr<HttpClientResponse> result;
        std::string                         reason;
        auto                                task = sendCapturingTask(loop, url, HttpClientRequest{}, result, reason, kTimeout);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        ASSERT_TRUE(result != nullptr) << "请求失败：" << reason;
        EXPECT_EQ(result->statusCode, 200);
        EXPECT_EQ(result->body, kCompressiblePayload) << "正文没被解回来：透明解压没生效";
        EXPECT_FALSE(hasHeaderField(*result, "content-encoding")) << "解压后仍带着 content-encoding";
        EXPECT_FALSE(hasHeaderField(*result, "content-length")) << "解压后仍带着压缩前的 content-length";
    }

    /**
     * @brief 钉住：Accept-Encoding 只由本端声明一条，取值与本端真能解的编码一致
     */
    TEST(HttpClient, AdvertisesOnlyTheEncodingsItCanDecode)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerEncodingRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/echo-accept";

        const SendOutcome outcome = runSend(url, HttpClientRequest{});
        ASSERT_TRUE(outcome.reason.empty()) << "请求失败：" << outcome.reason;
        EXPECT_EQ(outcome.body, "1|gzip, deflate") << "声明的条数或取值不对：" << outcome.body;
    }

    /**
     * @brief 钉住：调用方自己声明了 Accept-Encoding，本端就不加声明也不碰正文
     * @details 两半都要钉：多加一条会让服务端在两个取值之间替调用方选；动了正文等于把调用方
     *          要拿去做别的用途的字节改掉。
     */
    TEST(HttpClient, LeavesTheBodyAloneWhenTheCallerAdvertisesItsOwnAcceptEncoding)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerEncodingRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string host = "http://127.0.0.1:" + std::to_string(fixture.listeningPort());

        HttpClientRequest request;
        request.headers.emplace_back("Accept-Encoding", "identity");
        const SendOutcome echoed = runSend(host + "/echo-accept", request);
        ASSERT_TRUE(echoed.reason.empty()) << "请求失败：" << echoed.reason;
        EXPECT_EQ(echoed.body, "1|identity") << "本端替调用方加了一条声明：" << echoed.body;

        Core::EventLoop                     loop;
        std::unique_ptr<HttpClientResponse> result;
        std::string                         reason;
        auto                                task = sendCapturingTask(loop, host + "/gzip", request, result, reason, kTimeout);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        ASSERT_TRUE(result != nullptr) << "请求失败：" << reason;
        const std::optional<std::string> packed = gzipCompress(kCompressiblePayload);
        ASSERT_TRUE(packed.has_value());
        EXPECT_EQ(result->body, *packed) << "调用方自己管编码时本端仍然解了正文";
        EXPECT_TRUE(hasHeaderField(*result, "content-encoding")) << "本端插手时才会删头部，这里不该删";
    }

    /**
     * @brief 钉住：解不了的编码判请求失败，不交回原样字节
     * @details 交回压缩字节的形状是「200 + 长度也对 + 内容是乱码」，比一个错误难查一个量级。
     */
    TEST(HttpClient, FailsInsteadOfHandingBackBytesItCannotDecode)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerEncodingRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/br";

        const SendOutcome unsupported = runSend(url, HttpClientRequest{});
        EXPECT_TRUE(unsupported.body.empty()) << "解不了的编码被原样交回业务了：" << unsupported.body;
        EXPECT_NE(unsupported.reason.find("br"), std::string::npos) << "要说清是哪一种编码：" << unsupported.reason;
    }
} // namespace AsynGyanis::Net
