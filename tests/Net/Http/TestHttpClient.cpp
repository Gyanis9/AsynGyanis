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
#include "Net/Http/Client/HttpCookieJar.h"
#include "Net/Http/Client/OutboundCircuitBreaker.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpDate.h"
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
     * @brief 钉住：协议名缺失一律拒绝，不替调用方补一个 http
     * @details 「host:port」这种写法以前能过——schemeSeparator 找不到 `://` 就顺着把整串当 authority，
     *          结果 protocol 静默成 http：一个想连 TLS 端口的地址被明文发出去，而且失败方式与「连上了
     *          但对方不回话」难以区分。补默认值这条路比直接拒绝危险，故与「写错的端口不回落到 80」同判
     */
    TEST(HttpClientUrl, RequiresAnExplicitScheme)
    {
        ASSERT_THROW(static_cast<void>(parseUrl("127.0.0.1:8080/x")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("api.example.com")), std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("example.com:443/v1")), std::invalid_argument) << "看着像 https 的地址会被静默按明文发出";

        try
        {
            static_cast<void>(parseUrl("127.0.0.1:8080/x"));
            FAIL() << "缺协议名的 URL 本该被拒";
        } catch (const std::exception &failure)
        {
            EXPECT_NE(std::string{failure.what()}.find("协议名"), std::string::npos) << failure.what();
        }
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

    /**
     * @brief 钉住：响应头能按大小写不敏感读出来，读不到时是空而不是猜
     * @details `headers` 按对端给什么留什么（不折叠、不重排），而 RFC 9110 §5.1 规定头部名
     *          大小写不敏感。此前消费方要么自己写一份折小写的循环（本仓写过三份），要么
     *          按字面名比——比不中的那次就是静默漏读：拿不到 `Set-Cookie`、拿不到限流答复。
     *          服务端在这里刻意用混合大小写写下头部名，正向对照与反向缺席都要断到。
     */
    TEST(HttpClient, ReadsResponseHeadersIgnoringNameCase)
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                                                  [](Router &router, Core::EventLoop &)
                                                                  {
                                                                      router.get("/hdr",
                                                                                 [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                                                 {
                                                                                     response.setStatus(200);
                                                                                     static_cast<void>(response.setHeader("X-RateLimit-Limit", "7"));
                                                                                     response.setBody("ok");
                                                                                     co_return;
                                                                                 });
                                                                  });
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));

        const auto response = doGet("http://127.0.0.1:" + std::to_string(fixture->listeningPort()) + "/hdr");
        ASSERT_NE(response, nullptr) << "请求失败";
        for (const std::string_view spelling: {std::string_view{"x-ratelimit-limit"}, std::string_view{"X-RateLimit-Limit"}, std::string_view{"X-RATELIMIT-LIMIT"}})
        {
            const auto value = response->headerValue(spelling);
            ASSERT_TRUE(value.has_value()) << "按这个写法没读到：" << spelling;
            EXPECT_EQ(value.value(), "7");
        }
        EXPECT_FALSE(response->headerValue("x-ratelimit-remaining").has_value()) << "缺的字段要读成空，不能读成空串冒充「有但为空」";
    }

    /**
     * @brief 钉住：对端按什么大小写写来都能读到，同名多条取第一条
     * @details 本框架的服务端把头部名归一化成小写再发出去，所以端到端那一条测不出「对端混合大小写」
     *          这一档——而线上拿到的应答正是那种形状。这里直接按消费方会看到的形态造响应：
     *          名字原样留着（`headers` 的契约就是不折叠不重排），读口才负责按 ASCII 折叠比对。
     *          同名多条取第一条是刻意口径：`Set-Cookie` 那种要靠遍历，读单值时给一个确定的答案
     *          比给最后一个（取决于对端顺序）更好解释。
     */
    TEST(HttpClient, MatchesHeaderNamesByAsciiCaseInsensitiveRule)
    {
        HttpClientResponse peerResponse;
        peerResponse.statusCode = 429;
        peerResponse.headers    = {{"Retry-AFTER", "3600"}, {"X-Trace", "one"}, {"x-trace", "two"}};

        EXPECT_EQ(peerResponse.headerValue("retry-after").value_or(std::string_view{}), "3600");
        EXPECT_EQ(peerResponse.headerValue("RETRY-AFTER").value_or(std::string_view{}), "3600");
        EXPECT_EQ(peerResponse.headerValue("x-trace").value_or(std::string_view{}), "one") << "同名多条应当读到第一条，且读数确定";
        EXPECT_FALSE(peerResponse.headerValue("x-trac").has_value()) << "前缀不算命中";
        EXPECT_FALSE(peerResponse.headerValue("").has_value()) << "空名字不该匹配到任何东西";
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
    namespace
    {
        /**
         * @brief 挂上罐子后连发三次请求，把三次的正文分别落下来
         * @details 走的是实例这一路（带池、带 jar），静态 send() 没有状态可挂
         */
        Core::Task<void> jarRoundTripTask(Core::EventLoop &loop, const std::string loginUrl, const std::string whoamiUrl, const std::string callerCookieUrl, std::string &firstBody,
                                          std::string &secondBody, std::string &thirdBody, std::size_t &observedJarCount)
        {
            HttpClient client(loop);
            auto       jar = std::make_shared<HttpCookieJar>();
            client.setCookieJar(jar);

            const std::unique_ptr<HttpClientResponse> loginResponse = co_await client.get(loginUrl);
            firstBody                                               = loginResponse != nullptr ? loginResponse->body : "<请求失败>";
            observedJarCount                                        = jar->cookieCount();

            const std::unique_ptr<HttpClientResponse> whoamiResponse = co_await client.get(whoamiUrl);
            secondBody                                               = whoamiResponse != nullptr ? whoamiResponse->body : "<请求失败>";

            HttpClientRequest explicitRequest;
            explicitRequest.headers.emplace_back("cookie", "mine=1");
            const std::unique_ptr<HttpClientResponse> callerResponse = co_await client.send(callerCookieUrl, explicitRequest);
            thirdBody                                                = callerResponse != nullptr ? callerResponse->body : "<请求失败>";

            loop.stop();
        }
    } // namespace

    /**
     * @brief 钉住：罐子会在同一台主机的一次会话里自动收发放到的 Cookie，而调用方自己写的头部优先
     * @details 三条判据各挡一种错：收了不发（jar 只写不读）、发了但把调用方的 cookie 头改掉
     *          （静默覆盖明确意图）、以及服务端 Set-Cookie 根本没被认出来。
     *          夹具是真实 HTTP/1.1 服务端与真实套接字，因此这条也顺带盯住序列化侧有没有把
     *          set-cookie 与 cookie 原样搬上线。
     */
    TEST(HttpClient, CarriesCookiesAcrossRequestsWhenJarIsAttached)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/login",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            response.setCookie(HttpCookie("sid", "42"));
                                                            response.setBody("logged-in");
                                                            co_return;
                                                        });
                                             router.get("/whoami",
                                                        [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            response.setBody(request.getHeader("cookie").value_or("<none>"));
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string base = "http://127.0.0.1:" + std::to_string(fixture.listeningPort());

        Core::EventLoop loop;
        std::string     firstBody;
        std::string     secondBody;
        std::string     thirdBody;
        std::size_t     observedJarCount = 0;
        auto            task             = jarRoundTripTask(loop, base + "/login", base + "/whoami", base + "/whoami", firstBody, secondBody, thirdBody, observedJarCount);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_EQ(firstBody, "logged-in");
        EXPECT_EQ(observedJarCount, 1U) << "服务端发的 Set-Cookie 没有收进罐子";
        EXPECT_EQ(secondBody, "sid=42") << "收了却不发：第二次请求应当自动带上罐子里那条";
        EXPECT_EQ(thirdBody, "mine=1") << "调用方自己写了 cookie 头，罐子不该把他那份改掉";
    }
    /**
     * @brief 用带熔断器的实例连发三次同一端点，把三份结果与结束时的开闸端点数落下来
     */
    Core::Task<void> breakerProbeTask(Core::EventLoop &loop, const std::string url, std::vector<std::unique_ptr<HttpClientResponse>> &results, std::size_t &openCountAtEnd)
    {
        HttpClient                            client(loop);
        OutboundCircuitBreaker::Configuration configuration;
        configuration.consecutiveFailureThreshold = 2;
        configuration.openDuration                = std::chrono::minutes{5};
        client.setCircuitBreaker(std::make_shared<OutboundCircuitBreaker>(configuration));

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            results.push_back(co_await client.send(url, HttpClientRequest{}));
        }
        openCountAtEnd = client.circuitBreaker()->openEndpointCount();
        loop.stop();
    }

    /**
     * @brief 熔断器在实例这一路上生效：连续 5xx 之后，下一次请求连套接字都不建
     * @details 挡的是「这笔握手的钱」。判据要能被证伪：阈值是 2，因此前两次必须真拿到 500 响应、
     *          第三次才是被挡下的——如果熔断器根本没接进通路，第三次也会拿到响应而不是空指针
     */
    TEST(HttpClient, BreakerOpensAfterRepeatedServerErrorAndStopsDialing)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/broken",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            response.setStatus(500);
                                                            response.setBody("nope");
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/broken";

        Core::EventLoop                                  loop;
        std::vector<std::unique_ptr<HttpClientResponse>> results;
        std::size_t                                      openCountAtEnd = 0;
        auto                                             task           = breakerProbeTask(loop, url, results, openCountAtEnd);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        ASSERT_EQ(results.size(), 3U);
        ASSERT_TRUE(results[0] != nullptr) << "第一条请求就该拿到 500 响应";
        ASSERT_TRUE(results[1] != nullptr);
        EXPECT_EQ(results[1]->statusCode, 500);
        EXPECT_TRUE(results[2] == nullptr) << "阈值已过却没挡住第三次请求：熔断器没接进这条通路";
        EXPECT_EQ(openCountAtEnd, 1U);
    }

    /**
     * @brief 钉住：出站响应的 `Retry-After` 两种写法都认，且读不懂时交回空而不是 0
     * @details 这条读口此前没有直测（连 `headerValue` 都只在端到端用例里被顺带跑到），而它要处理
     *          的三种形状里最危险的是「读不懂」：把它折成 0 秒等于对着一台明确说了要限流的上游
     *          立刻再撞一次。绝对的过去折成 0 是对的——那正是「等待已结束」。
     */
    TEST(HttpClientResponseReads, RetryAfterHandlesBothFormsAndNeverInventsZero)
    {
        const auto         now = std::chrono::system_clock::time_point(std::chrono::seconds{2'000'000'000});
        HttpClientResponse response;

        EXPECT_FALSE(response.retryAfter(now).has_value()) << "没有这条头就该交回空";

        response.headers.emplace_back("Retry-After", "3600");
        EXPECT_EQ(response.retryAfter(now), std::optional{std::chrono::seconds{3600}}) << "头名按 RFC 9110 §5.1 大小写不敏感";

        // 绝对的 HTTP-date：未来按差值交回，已经过去按「等待已结束」交回 0
        response.headers.clear();
        response.headers.emplace_back("retry-after", formatHttpDate(now + std::chrono::seconds{120}));
        EXPECT_EQ(response.retryAfter(now), std::optional{std::chrono::seconds{120}});
        response.headers.clear();
        response.headers.emplace_back("retry-after", formatHttpDate(now - std::chrono::seconds{5}));
        EXPECT_EQ(response.retryAfter(now), std::optional{std::chrono::seconds{0}}) << "已经过去的绝对时刻是「现在就能再试」，不是负数也不是空";

        // 拒绝面：读不懂就不编造对方没给过的等待时长
        for (const char *const garbage: {"soon", "-5", "1.5", "99999999999999999999999", ""})
        {
            response.headers.clear();
            response.headers.emplace_back("retry-after", garbage);
            EXPECT_FALSE(response.retryAfter(now).has_value()) << "这条本该判读不懂：" << garbage;
        }
    }

    /**
     * @brief 钉住：绝对形式与网络路径形式自己带主机，而网络路径形式的协议跟着基准
     * @details `//cdn/x` 用基准的协议是 RFC 3986 §5.3 的写法；把它按 http 拼出去等于把一次本应加密的
     *          跳转降级成明文，而这正是「配置文件看着完全正确、线上跑明文」的那一类
     */
    TEST(HttpClientUrlResolution, AbsoluteAndNetworkPathReferencesCarryTheirOwnAuthority)
    {
        const ParsedUrl base = parseUrl("http://a.example:8080/docs/page.html?x=1");

        EXPECT_EQ(resolveUrlReference(base, "https://b.example/other"), "https://b.example/other") << "443 是 https 的默认端口，不该写出来";
        EXPECT_EQ(resolveUrlReference(base, "https://b.example:9443/a"), "https://b.example:9443/a");
        EXPECT_EQ(resolveUrlReference(base, "//c.example/y"), "http://c.example/y") << "网络路径引用的协议必须跟基准";
    }

    /**
     * @brief 钉住：路径引用按 RFC 9110 §5.3 合并目录并折到根为止
     * @details 点段折叠此前没人写过：少折一步打到错的目录，多折一步跳出本站，两种都不报错
     */
    TEST(HttpClientUrlResolution, PathReferencesMergeWithTheBaseDirectoryAndCollapseDotSegments)
    {
        const ParsedUrl base = parseUrl("http://a.example/docs/page.html?x=1");

        EXPECT_EQ(resolveUrlReference(base, "/login?next=%2Fhome"), "http://a.example/login?next=%2Fhome");
        EXPECT_EQ(resolveUrlReference(base, "sub/x.png"), "http://a.example/docs/sub/x.png");
        EXPECT_EQ(resolveUrlReference(base, "./a/../b"), "http://a.example/docs/b");
        EXPECT_EQ(resolveUrlReference(base, "../../etc/passwd"), "http://a.example/etc/passwd") << "越根的 .. 折到根为止，不能跳出这台主机";
        EXPECT_EQ(resolveUrlReference(base, "/docs/a/../b"), "http://a.example/docs/b") << "绝对形式的 Location 也要折：同一个文件不该有两种写法";
        EXPECT_EQ(resolveUrlReference(base, "/docs//deep"), "http://a.example/docs//deep") << "中段的双斜杠是规范里的一个空段，必须原样留着：按段过滤会把下一跳指向另一个资源";
        EXPECT_EQ(resolveUrlReference(base, "/docs/./x/"), "http://a.example/docs/x/") << "末尾斜杠按输入保留";
        EXPECT_EQ(resolveUrlReference(base, "/docs/x/.."), "http://a.example/docs/") << "以 .. 收尾时结果是它所在的那个目录";
    }

    /**
     * @brief 钉住：只有查询的引用换掉基准的查询、片段丢得干净、空查询与没有查询不是一回事
     */
    TEST(HttpClientUrlResolution, QueryOnlyAndFragmentOnlyReferencesAreToldApart)
    {
        const ParsedUrl base = parseUrl("http://a.example/docs/page.html?x=1");

        EXPECT_EQ(resolveUrlReference(base, "?page=2"), "http://a.example/docs/page.html?page=2") << "基准的旧查询要被换掉，而不是拼成 ?x=1?page=2";
        EXPECT_EQ(resolveUrlReference(base, "/x#frag"), "http://a.example/x") << "片段对一次 HTTP 跳转没用，留着只会让下一跳的 Origin 对不上";
        EXPECT_FALSE(resolveUrlReference(base, "#frag").has_value()) << "去掉片段之后什么都不剩，那不构成一跳";
        EXPECT_EQ(resolveUrlReference(base, "/x?"), "http://a.example/x?") << "空查询是引用给的原样，不该被当成「没有查询」抹掉";
    }

    /**
     * @brief 钉住：一切可能撕裂下一跳请求行、或把流量引到 http(s) 之外的写法都在此刻拒
     * @details 这条读的是**对端给的字节**，所以对端写歪时交回空而不是抛异常；判拒的六种形状各有一个
     *          真实后果：换协议（ftp/data/javascript）、把口令带到另一台主机、CR/LF 注入第二条头部、
     *          空格让请求行分成两段、空引用指向自己（一跳就是一次死循环）
     */
    TEST(HttpClientUrlResolution, RejectsAnythingThatCouldTearTheNextRequestLine)
    {
        const ParsedUrl base = parseUrl("http://a.example/docs/page.html");

        for (const std::string_view rejected: {"", "   ", "/a b", "/a\tb", "/a\r\nX-Injected: 1", "ftp://h/p", "data:text/plain,x", "javascript:alert(1)", "http://ad***@h/p",
                                               "//guest@h/p", "http://h:0/p", "http://bad host/p"})
        {
            EXPECT_FALSE(resolveUrlReference(base, rejected).has_value()) << "这条本该拒：" << rejected;
        }
    }

    /**
     * @brief 钉住：交回的串总能被 parseUrl 再拆一遍，且主机、端口、IPv6 方括号都回到原位
     * @details 这条是整件事的闭环判据：调用方拿到结果就喂给 `send()`，拼不出可拆的 URL 就等于这条读口没用
     */
    TEST(HttpClientUrlResolution, ResultsAlwaysParseBackIntoTheSameAuthority)
    {
        const ParsedUrl ipv6Base = parseUrl("http://[::1]:8080/docs/page.html");
        const auto      resolved = resolveUrlReference(ipv6Base, "sub/x.png");
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, "http://[::1]:8080/docs/sub/x.png") << "IPv6 主机必须带回方括号，否则端口分隔符会糊进地址里";

        const ParsedUrl reparsed = parseUrl(*resolved);
        EXPECT_EQ(reparsed.host, "::1");
        EXPECT_EQ(reparsed.port, 8080U);
        EXPECT_EQ(reparsed.path, "/docs/sub/x.png");

        const ParsedUrl defaultPortBase = parseUrl("https://a.example/docs/page.html");
        EXPECT_EQ(resolveUrlReference(defaultPortBase, "/x"), "https://a.example/x") << "443 是默认端口，写出来只会让 Host 头与基准不一致";
    }
} // namespace AsynGyanis::Net
