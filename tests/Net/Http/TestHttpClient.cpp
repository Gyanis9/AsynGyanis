// HTTP 出站客户端端到端用例
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "ExpectedAssertions.h"
#include "HttpTestSupport.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpCookieJar.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/Client/OutboundCircuitBreaker.h"
#include "Net/Http/Features/Gzip.h"
#include "Net/Http/Message/HttpDate.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
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
         * @brief 只回一份预设字节、回完仍不关连接的假对端
         * @details 真实服务端造不出「畸形响应之后还握着连接」这一种形状：它要么回合规响应，要么收线。
         *          这里按请求头的空行认一次请求，回预设字节后继续等对端先关，好让「客户端有没有在
         *          解析失败时自己收口」成为可直接观测的判据。
         */
        class MalformedResponsePeer
        {
        public:
            explicit MalformedResponsePeer(std::string payload) : m_payload(std::move(payload))
            {
            }

            MalformedResponsePeer(const MalformedResponsePeer &)            = delete;
            MalformedResponsePeer &operator=(const MalformedResponsePeer &) = delete;

            ~MalformedResponsePeer()
            {
                stop();
            }

            /// 起工作线程并等它把端口报回来
            bool start(const std::chrono::milliseconds waitTimeout)
            {
                m_worker            = std::thread([this] { run(); });
                const auto deadline = std::chrono::steady_clock::now() + waitTimeout;
                while (m_port.load(std::memory_order_acquire) == 0U && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return m_port.load(std::memory_order_acquire) != 0U;
            }

            /// 收摊：置停止标志并等线程退出（各处轮询都带超时）
            void stop()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

        private:
            void run()
            {
                const Platform::Socket::Initialization network;
                const int                              listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(listener))
                {
                    return;
                }
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = 0;
                if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    return;
                }
                m_port.store(queryBoundPort(listener), std::memory_order_release);

                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(listener, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(listener + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }
                    const int client = static_cast<int>(::accept(listener, nullptr, nullptr));
                    if (!Platform::FileDescriptor::isValid(client))
                    {
                        continue;
                    }
                    serve(client);
                    Platform::FileDescriptor::close(client);
                }
                Platform::FileDescriptor::close(listener);
            }

            void serve(const int client)
            {
                std::string            buffered;
                std::array<char, 1024> chunk{};
                bool                   isPayloadSent = false;
                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(client, &readSet);
                    timeval   pollInterval{0, 50 * 1000};
                    const int readyCount = ::select(client + 1, &readSet, nullptr, nullptr, &pollInterval);
                    if (readyCount < 0)
                    {
                        return;
                    }
                    if (readyCount == 0)
                    {
                        continue;
                    }
                    const ssize_t received = ::recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
                    if (received <= 0)
                    {
                        return;
                    }
                    buffered.append(chunk.data(), static_cast<std::size_t>(received));
                    if (isPayloadSent || buffered.find("\r\n\r\n") == std::string::npos)
                    {
                        continue;
                    }
                    isPayloadSent             = true;
                    std::size_t sentByteCount = 0;
                    while (sentByteCount < m_payload.size())
                    {
                        const int sent = ::send(client, m_payload.data() + sentByteCount, static_cast<int>(m_payload.size() - sentByteCount), 0);
                        if (sent <= 0)
                        {
                            return;
                        }
                        sentByteCount += static_cast<std::size_t>(sent);
                    }
                }
            }

            std::string                m_payload;           ///< 每份请求要回的预设字节
            std::thread                m_worker;            ///< 接受与应答线程
            std::atomic<bool>          m_isStopping{false}; ///< 收摊标志
            std::atomic<std::uint16_t> m_port{0};           ///< 实际监听端口，0 表示还没起来
        };

        /// 半开探测名额用例的读数
        struct HalfOpenProbeOutcome
        {
            bool routeEntered{false};     ///< 第一条请求确实进了被按住的路由
            bool secondRejected{false};   ///< 第二条请求被本地并发闸挡下
            bool nextProbeAllowed{false}; ///< 名额没有被第二条白占：下一次询问仍放行
        };

        /**
         * @brief 在一条循环上造出「半开时并发闸已满」的那一刻，并读下一次探测还放不放行
         * @details 顺序按用例需要排定：第一条请求在闭闸期出去并占住在途名额（它不占探测名额），
         *          随后把熔断器推到半开，再发第二条。第二条被并发闸挡下时，旧实现已经先向熔断器
         *          占走了一个探测名额；新实现把这次询问排到闸之后，名额应当还在。
         * @param loop 客户端事件循环
         * @param client 被测客户端
         * @param breaker 直接读写的熔断器（与客户端共用同一份）
         * @param endpointKey 目标端点
         * @param url 被按住的路由
         * @param routeEntered 路由处理器进门时置位
         * @param routeRelease 用例置位后路由才回响应
         * @param outcome 就地收集读数
         */
        Core::Task<void> runHalfOpenGateProbeTask(Core::EventLoop &loop, HttpClient &client, OutboundCircuitBreaker &breaker, const HttpOutboundEndpointKey &endpointKey,
                                                  const std::string &url, const std::shared_ptr<std::atomic<bool>> &routeEntered,
                                                  const std::shared_ptr<std::atomic<bool>> &routeRelease, HalfOpenProbeOutcome &outcome)
        {
            Core::Task<std::unique_ptr<HttpClientResponse>> firstTask  = client.get(url, std::chrono::seconds{10});
            Core::Task<std::unique_ptr<HttpClientResponse>> secondTask = client.get(url, std::chrono::seconds{2});
            loop.scheduler().schedule(firstTask.handle());

            Core::Timer pollTimer(loop);
            const auto  enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!routeEntered->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enteredDeadline)
            {
                co_await pollTimer.waitFor(std::chrono::milliseconds{5});
            }
            outcome.routeEntered = routeEntered->load(std::memory_order_acquire);
            if (outcome.routeEntered)
            {
                // 第一条在闭闸期拿到名额，没有占探测名额；把它推到半开，模拟上一条请求仍在途中时窗口到点
                breaker.reportFailure(endpointKey);
                static_cast<void>(breaker.stateOf(endpointKey, OutboundCircuitBreaker::Clock::now() + std::chrono::milliseconds{5}));

                loop.scheduler().schedule(secondTask.handle());
                const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                while (!secondTask.isReady() && std::chrono::steady_clock::now() < secondDeadline)
                {
                    co_await pollTimer.waitFor(std::chrono::milliseconds{5});
                }
                if (secondTask.isReady())
                {
                    outcome.secondRejected = secondTask.handle().promise().result() == nullptr;
                }
                // 此刻第一条仍被路由按住：探测名额若被第二条白占，这里一定读回 false
                outcome.nextProbeAllowed = breaker.allowRequest(endpointKey);
            }

            routeRelease->store(true, std::memory_order_release);
            const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!firstTask.isReady() && std::chrono::steady_clock::now() < firstDeadline)
            {
                co_await pollTimer.waitFor(std::chrono::milliseconds{5});
            }
            loop.stop();
            co_return;
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
     * @brief 钉住：URL 里任何可能撕裂请求行的字节都被拒，而不只是原先那四个
     * @details 头文件承诺「不接受空白与控制字符」，实现却只认 CR/LF/TAB/空格：NUL、0x01、DEL、0x0B
     *          照样进得去，随后被原样拼成请求目标。NUL 会让按 C 字符串处理这段的接口把目标截断，
     *          这正是请求分裂的形状。与下一跳那条引用（`resolveUrlReference()`）共用同一份判据——
     *          两处各写一条时，注释里那句「同一口径」在收紧的那一天就会变成假话。
     */
    TEST(HttpClientUrl, RejectsEveryTearingCharacter)
    {
        const std::string nulInTarget = std::string("http://example.com/a") + '\0' + "b"; // 按字节拼：字面量会在 NUL 处被截掉
        ASSERT_THROW(static_cast<void>(parseUrl(std::string_view(nulInTarget))), std::invalid_argument) << "NUL 混在请求目标里照样放行";
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com/a\x01"
                                                "b")),
                     std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com/a\x0b"
                                                "b")),
                     std::invalid_argument);
        ASSERT_THROW(static_cast<void>(parseUrl("http://example.com/a\x7f"
                                                "b")),
                     std::invalid_argument)
                << "DEL 不是可见字符，放行等于让对端自己判断这段到哪为止";

        // 正向对照：百分号编码与可打印字符照常收（把合规 URL 也挡了就是自伤），片段按规范剥掉
        const ParsedUrl encoded = parseUrl("http://example.com/a%20b?x=1#frag");
        EXPECT_EQ(encoded.path, "/a%20b?x=1") << "百分号编码的路径被误挡，或片段没剥掉";
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
     * @brief 钉住：带 userinfo 的 URL 被明确拒绝，异常文本里不出现用户名或口令
     * @details 先按冒号数量判 IPv6 会把 `user:pass@host:port` 误报成「IPv6 没加方括号」，
     *          而那条文案会把 authority 整段回显。这里同时钉两件事：拒绝的理由是 userinfo
     *          （RFC 9110 §4.2.4），以及异常文本里没有任何输入里的凭据字节。
     * @note 正向对照：`@` 出现在路径或查询里不是 userinfo，必须照常解析
     */
    TEST(HttpClientUrl, RejectsUserInfoWithoutEchoingCredentials)
    {
        const auto expectRejectedWithoutSecret = [](const std::string_view url, const std::string_view secret, const std::string_view expectedReason)
        {
            try
            {
                static_cast<void>(parseUrl(url));
                FAIL() << "带 userinfo 的 URL 被放行了：" << url;
            } catch (const std::invalid_argument &failure)
            {
                const std::string text = failure.what();
                EXPECT_NE(text.find(expectedReason), std::string::npos) << "拒绝理由没点明问题：" << text;
                EXPECT_EQ(text.find(secret), std::string::npos) << "异常文本回显了凭据：" << text;
                EXPECT_EQ(text.find("alice"), std::string::npos) << "异常文本回显了用户名：" << text;
            }
        };

        expectRejectedWithoutSecret("http://alice:topsecret@example.com:8080/x", "topsecret", "userinfo");
        expectRejectedWithoutSecret("https://alice@example.com/x", "alice", "userinfo");
        expectRejectedWithoutSecret("http://alice:topsecret@[::1]:8080/x", "topsecret", "userinfo");
        // 撕裂请求行的错误发生在 userinfo 识别之前：这条也必须是脱敏后的文本
        expectRejectedWithoutSecret("http://alice:topsecret@example.com/a b", "topsecret", "空白或控制字符");

        const ParsedUrl pathAt = parseUrl("http://example.com/a@b?mail=a@b");
        EXPECT_EQ(pathAt.host, "example.com") << "路径/查询里的 @ 被误当成 userinfo";
        EXPECT_EQ(pathAt.path, "/a@b?mail=a@b");
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
            ASSERT_HAS_VALUE(value) << "按这个写法没读到：" << spelling;
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
     * @brief 钉住超时后的连接归属：掐断的那条不能还回池里，同一客户端随后仍能发新请求
     * @details 旧读循环在时限到点时只把请求判失败；连接若被当作「用完」还池，下一条请求会从一条
     *          已经半途而废的通路上读响应。判据取池的空闲计数（直接读连接归属）与下一条请求的
     *          200（证明客户端还能重新建连），两处缺一都抓不到「失败后状态被污染」。
     */
    TEST(HttpClient, TimedOutRequestDoesNotLeaveAReusableConnection)
    {
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{900};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        const std::string slowUrl = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";
        const std::string fastUrl = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";

        Core::EventLoop loop;
        HttpClient      client(loop);
        bool            firstWasEmpty     = false;
        std::size_t     idleAfterTimeout  = 1U;
        bool            secondAnswered200 = false;
        auto            drive             = [&]() -> Core::Task<void>
        {
            const std::unique_ptr<HttpClientResponse> first  = co_await client.get(slowUrl, std::chrono::milliseconds{150});
            firstWasEmpty                                    = first == nullptr;
            idleAfterTimeout                                 = client.idleConnectionCount();
            const std::unique_ptr<HttpClientResponse> second = co_await client.get(fastUrl, std::chrono::seconds{2});
            secondAnswered200                                = second != nullptr && second->statusCode == 200;
            loop.stop();
            co_return;
        };
        auto work = drive();
        loop.scheduler().schedule(work.handle());
        loop.run();

        EXPECT_TRUE(firstWasEmpty) << "慢路由没在时限内失败";
        EXPECT_EQ(idleAfterTimeout, 0U) << "超时的连接被还回了空闲池：下一条请求可能拿它当干净通路";
        EXPECT_TRUE(secondAnswered200) << "超时之后同一客户端没能重新建连并拿到 200";
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 对端回畸形响应又不收线时，客户端按解析失败立即收场，不陪它耗到请求时限
     * @details 解析器一进入 Failed 就该停：旧读循环只认 isComplete()，把失败态当成「还没收齐」
     *          继续读，于是每条畸形响应都白等满时限。夹具回完畸形头部后保持连接不关，时限给 5 秒，
     *          断言耗时远小于它，失败原因还要点明响应不合规范。
     */
    TEST(HttpClient, FailsFastWhenMalformedResponseArrivesOnAConnectionThePeerKeepsOpen)
    {
        MalformedResponsePeer peer("HTTP/1.1 200 OK\r\nContent-Length: not-a-number\r\n\r\n");
        ASSERT_TRUE(peer.start(kTimeout)) << "假对端未在时限内进入监听";
        const std::string url = "http://127.0.0.1:" + std::to_string(peer.port()) + "/malformed";

        const auto        started       = std::chrono::steady_clock::now();
        const SendOutcome outcome       = runSend(url, HttpClientRequest{}, std::chrono::seconds{5});
        const auto        elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

        EXPECT_TRUE(outcome.body.empty()) << "畸形响应被当成了成功响应";
        EXPECT_NE(outcome.reason.find("响应不合规范"), std::string::npos) << "失败原因没有点明响应不合规范：" << outcome.reason;
        EXPECT_LT(elapsedMillis, std::chrono::milliseconds{2000}) << "客户端等到了时限（用时 " << elapsedMillis.count() << " 毫秒）：解析失败没有让读循环停下来";
    }

    /**
     * @brief 绝对截止时刻优先于相对时限：剩余预算到点就收口，不等到 requestTimeout
     */
    TEST(HttpClient, AbsoluteDeadlineCapsTheRequestBudget)
    {
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{900};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";

        {
            // 截止时刻比 requestTimeout 更严：应在 ~150ms 收口，而不是 5 秒
            HttpClientRequest request;
            request.deadline   = std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
            const auto started = std::chrono::steady_clock::now();
            const auto outcome = runSend(url, request, std::chrono::milliseconds{5000});
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            EXPECT_TRUE(outcome.body.empty()) << "截止时刻已到却拿到了正文";
            EXPECT_FALSE(outcome.reason.empty()) << "失败没有原因";
            EXPECT_LT(elapsed, kSlowRouteTime) << "请求等到了慢路由写完（" << elapsed.count() << " ms）：截止时刻没有生效";
        }

        {
            // requestTimeout 更严：取更严者，截止时刻不放大它
            HttpClientRequest request;
            request.deadline   = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            const auto started = std::chrono::steady_clock::now();
            const auto outcome = runSend(url, request, std::chrono::milliseconds{100});
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            EXPECT_TRUE(outcome.body.empty());
            EXPECT_LT(elapsed, kSlowRouteTime) << "requestTimeout 没有被取为更严者";
        }

        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 剩余预算不足 1 毫秒按 1 毫秒发：截止时刻刚过也不能翻成「不设限」
     */
    TEST(HttpClient, SubMillisecondBudgetStillStopsAtTheDeadline)
    {
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{900};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";
        HttpClientRequest request;
        request.deadline = std::chrono::steady_clock::now() + std::chrono::microseconds{500};

        const auto started = std::chrono::steady_clock::now();
        const auto outcome = runSend(url, request, std::chrono::milliseconds{5000});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

        EXPECT_TRUE(outcome.body.empty()) << "预算不足 1 毫秒时不该拿到正文";
        EXPECT_LT(elapsed, kSlowRouteTime) << "不足 1 毫秒的预算被翻成了不设限（用了 " << elapsed.count() << " ms）";
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 出向并发闸：上限 1 时第二条按 Fail 立即失败，Wait 时等第一条让出名额
     */
    TEST(HttpClient, OutboundInFlightGateRejectsOrWaitsPerConfiguration)
    {
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{250};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";

        // 在同一个循环上同时起两条实例请求，等两条协程都结束
        const auto runTwo = [&url](const std::size_t inFlightLimit, const InFlightOverflowPolicy policy, std::unique_ptr<HttpClientResponse> &first,
                                   std::unique_ptr<HttpClientResponse> &second, std::uint64_t &rejections)
        {
            Core::EventLoop                    loop;
            HttpOutboundConnectionPool::Config poolConfig;
            poolConfig.maximumInFlightPerEndpoint = inFlightLimit;
            poolConfig.inFlightOverflowPolicy     = policy;
            HttpClient client(loop, poolConfig);

            Core::Task<std::unique_ptr<HttpClientResponse>> firstTask  = client.get(url, std::chrono::seconds{2});
            Core::Task<std::unique_ptr<HttpClientResponse>> secondTask = client.get(url, std::chrono::seconds{2});
            loop.scheduler().schedule(firstTask.handle());
            loop.scheduler().schedule(secondTask.handle());

            Core::Timer timer(loop);
            // 先把闭包命名再调用：惰性协程帧持有的是闭包地址，立即调用的临时闭包在开跑前就销毁了
            auto watchdogBody = [&]() -> Core::Task<void>
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                while ((!firstTask.isReady() || !secondTask.isReady()) && std::chrono::steady_clock::now() < deadline)
                {
                    co_await timer.waitFor(std::chrono::milliseconds{10});
                }
                loop.stop();
                co_return;
            };
            Core::Task<void> watchdog = watchdogBody();
            loop.scheduler().schedule(watchdog.handle());
            loop.run();
            if (firstTask.isReady())
            {
                first = firstTask.handle().promise().result();
            }
            if (secondTask.isReady())
            {
                second = secondTask.handle().promise().result();
            }
            rejections = client.outboundGateRejectionCount();
        };

        {
            std::unique_ptr<HttpClientResponse> first;
            std::unique_ptr<HttpClientResponse> second;
            std::uint64_t                       rejections = 0;
            runTwo(1, InFlightOverflowPolicy::Fail, first, second, rejections);
            // 调度器不保证两条协程的起跑先后：只断言「恰好一条成功、一条被闸挡下」
            EXPECT_NE(first == nullptr, second == nullptr) << "上限 1 时应恰好一条成功、一条被闸挡下";
            EXPECT_EQ(rejections, 1U) << "拒绝计数没有加一";
        }

        {
            std::unique_ptr<HttpClientResponse> first;
            std::unique_ptr<HttpClientResponse> second;
            std::uint64_t                       rejections = 0;
            runTwo(1, InFlightOverflowPolicy::Wait, first, second, rejections);
            EXPECT_NE(first, nullptr);
            EXPECT_NE(second, nullptr) << "Wait 处置应在第一条让出名额后放行第二条";
            EXPECT_EQ(rejections, 0U) << "等到了名额就不该记拒绝";
        }

        {
            std::unique_ptr<HttpClientResponse> first;
            std::unique_ptr<HttpClientResponse> second;
            std::uint64_t                       rejections = 0;
            runTwo(0, InFlightOverflowPolicy::Fail, first, second, rejections);
            EXPECT_NE(first, nullptr);
            EXPECT_NE(second, nullptr) << "上限 0 是「不设限」，不是「一条都不许发」";
            EXPECT_EQ(rejections, 0U);
        }
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 钉住并发闸排队也吃请求时限：排在 Wait 队列里的第二条到点就失败，不等前一条跑完
     * @details 「整条请求的预算」包括闸门等待那一段；旧实现只把预算用在连接与读写上，排队的请求
     *          会一直等到前一条让出名额，调用方的 150 ms 时限变成 600 ms 以上。判据先让第一条真占住
     *          名额（服务端慢路由已进入处理），再发第二条，因此不靠调度顺序。
     */
    TEST(HttpClient, WaitPolicyHonoursTheRequestDeadlineWhileQueued)
    {
        constexpr auto    kSlowRouteTime = std::chrono::milliseconds{600};
        std::atomic<bool> slowRouteEntered{false};
        SlowRouteOptions  slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        slowRoute.handlerStarted = &slowRouteEntered;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";

        Core::EventLoop                    loop;
        HttpOutboundConnectionPool::Config poolConfig;
        poolConfig.maximumInFlightPerEndpoint = 1;
        poolConfig.inFlightOverflowPolicy     = InFlightOverflowPolicy::Wait;
        HttpClient client(loop, poolConfig);

        bool                      firstAnswered      = false;
        bool                      secondQueuedFailed = false;
        bool                      routeEntered       = false;
        std::chrono::milliseconds secondElapsed{0};
        auto                      drive = [&]() -> Core::Task<void>
        {
            Core::Task<std::unique_ptr<HttpClientResponse>> firstTask = client.get(url, std::chrono::seconds{3});
            loop.scheduler().schedule(firstTask.handle());

            Core::Timer pollTimer(loop);
            const auto  enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (!slowRouteEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < enteredDeadline)
            {
                co_await pollTimer.waitFor(std::chrono::milliseconds{5});
            }
            routeEntered = slowRouteEntered.load(std::memory_order_acquire);
            if (!routeEntered)
            {
                loop.stop();
                co_return;
            }

            const auto                                      secondStarted = std::chrono::steady_clock::now();
            Core::Task<std::unique_ptr<HttpClientResponse>> secondTask    = client.get(url, std::chrono::milliseconds{150});
            loop.scheduler().schedule(secondTask.handle());
            const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (!secondTask.isReady() && std::chrono::steady_clock::now() < secondDeadline)
            {
                co_await pollTimer.waitFor(std::chrono::milliseconds{5});
            }
            secondElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - secondStarted);
            if (secondTask.isReady())
            {
                const std::unique_ptr<HttpClientResponse> secondResponse = secondTask.handle().promise().result();
                secondQueuedFailed                                       = secondResponse == nullptr;
            }

            const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (!firstTask.isReady() && std::chrono::steady_clock::now() < firstDeadline)
            {
                co_await pollTimer.waitFor(std::chrono::milliseconds{5});
            }
            if (firstTask.isReady())
            {
                const std::unique_ptr<HttpClientResponse> firstResponse = firstTask.handle().promise().result();
                firstAnswered                                           = firstResponse != nullptr && firstResponse->statusCode == 200;
            }
            loop.stop();
            co_return;
        };
        auto work = drive();
        loop.scheduler().schedule(work.handle());
        loop.run();

        EXPECT_TRUE(routeEntered) << "第一条请求没进入慢路由：名额归属不成立";
        EXPECT_TRUE(secondQueuedFailed) << "排队中的第二条没有被时限拦下";
        EXPECT_LT(secondElapsed, kSlowRouteTime) << "第二条等到了前一条跑完（用时 " << secondElapsed.count() << " ms）：闸门等待没有吃请求时限";
        EXPECT_TRUE(firstAnswered) << "第一条被排队者拖累，没能正常拿到 200";
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
     * @brief 出站 tracestate：给了才发、原样按列表写出；与手写头部冲突时拒绝
     */
    TEST(HttpClient, WritesTracestateOnlyWhenGivenAndRejectsConflicts)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/echo-state",
                                                        [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            response.setBody(request.getHeader("tracestate").value_or("<missing>"));
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/echo-state";

        const TraceIdentifiers identifiers = Traceparent::generate();
        const auto             parsedState = TraceState::parse("vendora=abc,vendorb=def");
        ASSERT_HAS_VALUE(parsedState);

        {
            HttpClientRequest request;
            request.traceContext      = identifiers;
            request.traceState        = *parsedState;
            const SendOutcome outcome = runSend(url, request);
            ASSERT_TRUE(outcome.reason.empty()) << outcome.reason;
            EXPECT_EQ(outcome.body, "vendora=abc,vendorb=def") << "tracestate 没有按原顺序上线";
        }

        {
            // 只给 traceContext：不发空的 tracestate 头
            HttpClientRequest request;
            request.traceContext      = identifiers;
            const SendOutcome outcome = runSend(url, request);
            ASSERT_TRUE(outcome.reason.empty()) << outcome.reason;
            EXPECT_EQ(outcome.body, "<missing>") << "没给 tracestate 却发出了这条头";
        }

        {
            // 手写 tracestate + traceState：一处请求只能有一份
            HttpClientRequest request;
            request.traceContext = identifiers;
            request.traceState   = *parsedState;
            request.headers.emplace_back("tracestate", "hand=written");
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "冲突组合却发出去了";
            EXPECT_NE(outcome.reason.find("tracestate"), std::string::npos) << outcome.reason;
        }
    }

    /**
     * @brief 端到端：入站 traceparent+tracestate 原样带到下游那一跳
     */
    TEST(HttpClient, PropagatesInboundTracestateToTheDownstreamHop)
    {
        // 下游只回显收到的 tracestate
        RunningHttpServerFixture downstream(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                            [](Router &router, Core::EventLoop &)
                                            {
                                                router.get("/echo-state",
                                                           [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                           {
                                                               response.setBody(request.getHeader("tracestate").value_or("<missing>"));
                                                               co_return;
                                                           });
                                            });
        ASSERT_TRUE(downstream.awaitRunning(kTimeout));
        const std::string downstreamUrl = "http://127.0.0.1:" + std::to_string(downstream.listeningPort()) + "/echo-state";

        // 上游收到请求后把入站链路上下文原样交给下游
        RunningHttpServerFixture upstream(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                          [downstreamUrl](Router &router, Core::EventLoop &loop)
                                          {
                                              router.get("/hop",
                                                         [&loop, downstreamUrl](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             const std::optional<TraceIdentifiers> identifiers = extractTraceContext(request);
                                                             const std::optional<TraceState>       state       = extractTraceState(request);
                                                             if (!identifiers.has_value())
                                                             {
                                                                 response.setStatus(500);
                                                                 response.setBody("no traceparent");
                                                                 co_return;
                                                             }
                                                             HttpClientRequest outbound;
                                                             outbound.traceContext = *identifiers;
                                                             outbound.traceState   = state;
                                                             const auto sent       = co_await HttpClient::send(loop, downstreamUrl, outbound, std::chrono::seconds{2});
                                                             response.setBody(sent.has_value() ? sent->body : "<upstream failed>");
                                                             co_return;
                                                         });
                                          });
        ASSERT_TRUE(upstream.awaitRunning(kTimeout));
        const std::string upstreamUrl = "http://127.0.0.1:" + std::to_string(upstream.listeningPort()) + "/hop";

        HttpClientRequest request;
        request.headers.emplace_back("traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
        request.headers.emplace_back("tracestate", "vendora=abc");
        request.headers.emplace_back("tracestate", "vendorb=def");

        const SendOutcome outcome = runSend(upstreamUrl, request);
        ASSERT_TRUE(outcome.reason.empty()) << outcome.reason;
        EXPECT_EQ(outcome.body, "vendora=abc,vendorb=def") << "入站 tracestate（多条合并）没能一路带到下游";
    }

    /**
     * @brief 钉住：会撕裂请求行的头部写法一律拒绝，不转义也不静默丢掉
     * @details 值里带 CR/LF 等于自己结束这一行再插一条新字段；名字带空格或冒号会拼出第二个字段；
     *          方法名进的是请求行开头，同样只准是 token。保留头部（Host、Content-Length、Connection、
     *          Transfer-Encoding）由客户端按这次请求的实际情况写，调用方给了就拒收而不是覆盖或并存
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

        const std::array<std::string, 4> reservedNames{std::string{"host"}, std::string{"Content-Length"}, std::string{"CONNECTION"}, std::string{"Transfer-Encoding"}};
        for (const std::string &reserved: reservedNames)
        {
            HttpClientRequest request;
            request.headers.emplace_back(reserved, "whatever");
            const SendOutcome outcome = runSend(url, request);
            EXPECT_TRUE(outcome.body.empty()) << "保留头部本该拒收：" << reserved;
            EXPECT_NE(outcome.reason.find("由客户端"), std::string::npos) << "原因要说明这几项由客户端写：" << outcome.reason;
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

    /**
     * @brief 钉住解析失败与建连失败在原因里分得开
     * @details 超长标签在 DNS 协议里必然非法，getaddrinfo 不问网络就会拒；这条既不依赖本机解析器
     *          答什么，也不依赖网络是否可用。旧文案只给一句「请求失败」时，重试策略只能靠猜。
     */
    TEST(HttpClient, NamesDnsFailureStage)
    {
        const std::string invalidHost(200U, 'a');
        const SendOutcome outcome = runSend("http://" + invalidHost + "/", HttpClientRequest{}, std::chrono::seconds{2});
        EXPECT_TRUE(outcome.body.empty()) << "非法主机名不该拿到响应";
        EXPECT_NE(outcome.reason.find("解析地址"), std::string::npos) << "原因要点明断在解析这一段：" << outcome.reason;
        EXPECT_EQ(outcome.reason.find("建立 TCP 连接失败"), std::string::npos) << "解析失败被折成了建连失败：" << outcome.reason;
    }

    /**
     * @brief 钉住 TLS 握手失败与 TCP 建连失败在原因里分得开
     * @details 对端是一台明文 HTTP 服务器：TCP 连得上，TLS 一定握手不过。这正是两种故障在诊断上
     *          必须分开的形状——前者查证书/协议，后者查网络与端口。
     */
    TEST(HttpClient, NamesTlsHandshakeFailureStage)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/";

        const SendOutcome outcome = runSend(url, HttpClientRequest{}, std::chrono::seconds{5});
        EXPECT_TRUE(outcome.body.empty()) << "明文对端不该完成 TLS 握手";
        EXPECT_NE(outcome.reason.find("TLS 握手"), std::string::npos) << "原因要点明断在 TLS 握手这一段：" << outcome.reason;
        EXPECT_EQ(outcome.reason.find("建立 TCP 连接失败"), std::string::npos) << "握手失败被折成了建连失败：" << outcome.reason;
    }

    /**
     * @brief 钉住请求时限到点时原因仍点明「时限」，不折成建连或解析失败
     */
    TEST(HttpClient, NamesTimeoutStageDistinctFromConnectFailure)
    {
        constexpr auto   kSlowRouteTime = std::chrono::milliseconds{900};
        SlowRouteOptions slowRoute;
        slowRoute.processingTime = kSlowRouteTime;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        const std::string url     = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";
        const SendOutcome outcome = runSend(url, HttpClientRequest{}, std::chrono::milliseconds{150});
        EXPECT_TRUE(outcome.body.empty()) << "慢路由不该在时限内交出响应";
        EXPECT_NE(outcome.reason.find("时限"), std::string::npos) << "原因要点明是时限到点：" << outcome.reason;
        EXPECT_EQ(outcome.reason.find("建立 TCP 连接失败"), std::string::npos) << "时限失败被折成了建连失败：" << outcome.reason;
        EXPECT_EQ(outcome.reason.find("解析地址"), std::string::npos) << "时限失败被折成了解析失败：" << outcome.reason;
        std::this_thread::sleep_for(kSlowRouteTime);
    }

    /**
     * @brief 钉住：DNS 卡住时请求在自身时限内失败，且客户端随后仍能发新请求
     * @details 真实解析器在本机与 CI 上都会很快返回；这条用一个阻塞的注入 lookup 制造
     *          「解析卡住」的形状，并检查失败原因没有被折成建连失败。
     */
    TEST(HttpClient, DnsLookupTimeoutIsReportedAndLeavesClientUsable)
    {
        auto                           release  = std::make_shared<std::promise<void>>();
        const std::shared_future<void> released = release->get_future().share();
        Core::AsyncResolver::setLookupFunction(
                [released](const std::string &, const std::uint16_t)
                {
                    released.wait();
                    return std::vector<Core::InetAddress>{};
                });
        struct LookupReset
        {
            ~LookupReset()
            {
                Core::AsyncResolver::clearLookupFunction();
            }
        } reset;

        const auto        started = std::chrono::steady_clock::now();
        const SendOutcome outcome = runSend("http://blocked-lookup.test:18080/", HttpClientRequest{}, std::chrono::milliseconds{100});
        const auto        elapsed = std::chrono::steady_clock::now() - started;

        EXPECT_TRUE(outcome.body.empty()) << "DNS 卡住时不该拿到响应";
        EXPECT_NE(outcome.reason.find("解析地址"), std::string::npos) << "原因没有点明解析阶段：" << outcome.reason;
        EXPECT_LT(elapsed, std::chrono::milliseconds{500}) << "DNS 卡住把请求拖过了 100 ms 时限";

        release->set_value();
        ASSERT_TRUE(waitForCondition([] { return Core::AsyncResolver::activeLookupCount() == 0; }, std::chrono::seconds{2})) << "阻塞的解析线程结束后名额没有归还";
        Core::AsyncResolver::clearLookupFunction();

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string url   = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";
        const SendOutcome after = runSend(url, HttpClientRequest{}, std::chrono::seconds{2});
        EXPECT_TRUE(after.reason.empty()) << "DNS 超时之后客户端没有恢复：" << after.reason;
        EXPECT_EQ(after.body, "served-hello") << "DNS 超时之后后续请求没有拿到预期正文";
    }

    /**
     * @brief 钉住网关把入站预算传给下游：下游慢于入站时限时，出站请求先失败，上游按时返回
     * @details 没有这条通路时，处理器只能给下游一个独立的相对时限；上游 200 ms 的预算
     *          会被下游 700 ms 的处理拖穿，整条调用链的超时逐层放大。
     */
    TEST(HttpClient, GatewayPropagatesInboundDeadlineToDownstream)
    {
        std::atomic<bool> downstreamEntered{false};
        SlowRouteOptions  downstreamSlow;
        downstreamSlow.processingTime = std::chrono::milliseconds{700};
        downstreamSlow.handlerStarted = &downstreamEntered;
        RunningHttpServerFixture downstream(HttpServerLimits{}, std::chrono::milliseconds{100}, downstreamSlow);
        ASSERT_TRUE(downstream.awaitRunning(kTimeout)) << "下游服务器未在时限内进入接受循环";
        const std::string downstreamUrl = "http://127.0.0.1:" + std::to_string(downstream.listeningPort()) + "/slow";

        bool                     upstreamSawDeadline = false;
        RunningHttpServerFixture upstream(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                          [&downstreamUrl, &upstreamSawDeadline](Router &router, Core::EventLoop &loop)
                                          {
                                              router.addMiddleware(timeoutMiddleware(loop, std::chrono::milliseconds{200}));
                                              router.get("/proxy",
                                                         [&loop, &downstreamUrl, &upstreamSawDeadline](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             const auto deadline = request.deadline();
                                                             upstreamSawDeadline = deadline.has_value();
                                                             if (!deadline.has_value())
                                                             {
                                                                 response.setStatus(500);
                                                                 response.setBody("missing-deadline");
                                                                 co_return;
                                                             }

                                                             HttpClientRequest outbound;
                                                             outbound.deadline = deadline;
                                                             const auto sent   = co_await HttpClient::send(loop, downstreamUrl, outbound, std::chrono::seconds{2});
                                                             if (sent.has_value())
                                                             {
                                                                 response.setStatus(200);
                                                                 response.setBody(sent->body);
                                                             } else
                                                             {
                                                                 response.setStatus(504);
                                                                 response.setBody(sent.error());
                                                             }
                                                             co_return;
                                                         });
                                          });
        ASSERT_TRUE(upstream.awaitRunning(kTimeout)) << "上游服务器未在时限内进入接受循环";
        const std::string upstreamUrl = "http://127.0.0.1:" + std::to_string(upstream.listeningPort()) + "/proxy";

        const auto        started = std::chrono::steady_clock::now();
        const SendOutcome outcome = runSend(upstreamUrl, HttpClientRequest{}, std::chrono::seconds{2});
        const auto        elapsed = std::chrono::steady_clock::now() - started;

        EXPECT_TRUE(upstreamSawDeadline) << "上游处理器没有拿到入站截止时刻";
        EXPECT_TRUE(waitForCondition([&downstreamEntered] { return downstreamEntered.load(std::memory_order_acquire); }, std::chrono::seconds{1}))
                << "下游慢路由没有被进入：出站请求可能在建连前就失败了";
        EXPECT_EQ(outcome.body.find("missing-deadline"), std::string::npos) << "缺少入站截止时刻，网关路径没有建立";
        EXPECT_EQ(outcome.body.find("served-slow"), std::string::npos) << "下游跑完了整段处理：入站预算没有传下去";
        EXPECT_LT(elapsed, std::chrono::milliseconds{600}) << "上游被下游拖过了自己的 200 ms 预算";
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
        ASSERT_HAS_VALUE(packed);
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
     * @brief 半开时被本地并发闸挡下的请求不得占掉探测名额
     * @details 半开态下 allowRequest 会占一个探测名额，而它只在 reportSuccess/reportFailure 里归还。
     *          并发闸上限 1 时，第一条请求在闭闸期出去并占住在途名额（不占探测名额）；随后把熔断器
     *          推到半开，第二条请求被并发闸挡下。旧实现先问熔断再问闸，第二条因此白占了一个探测名额；
     *          新实现把熔断询问排到闸之后，名额应当还在，下一次询问仍放行。
     */
    TEST(HttpClient, GateRejectedRequestDoesNotConsumeTheHalfOpenProbe)
    {
        auto                     routeEntered = std::make_shared<std::atomic<bool>>(false);
        auto                     routeRelease = std::make_shared<std::atomic<bool>>(false);
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [routeEntered, routeRelease](Router &router, Core::EventLoop &loop)
                                         {
                                             router.get("/held",
                                                        [&loop, routeEntered, routeRelease](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            routeEntered->store(true, std::memory_order_release);
                                                            Core::Timer holdTimer(loop);
                                                            while (!routeRelease->load(std::memory_order_acquire))
                                                            {
                                                                co_await holdTimer.waitFor(std::chrono::milliseconds{5});
                                                            }
                                                            response.setBody("held");
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        Core::EventLoop                    loop;
        HttpOutboundConnectionPool::Config poolConfig;
        poolConfig.maximumInFlightPerEndpoint = 1;
        poolConfig.inFlightOverflowPolicy     = InFlightOverflowPolicy::Fail;
        HttpClient client(loop, poolConfig);

        OutboundCircuitBreaker::Configuration breakerConfiguration;
        breakerConfiguration.consecutiveFailureThreshold = 1;
        breakerConfiguration.openDuration                = std::chrono::milliseconds{1};
        auto breaker                                     = std::make_shared<OutboundCircuitBreaker>(breakerConfiguration);
        client.setCircuitBreaker(breaker);

        const std::string             url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/held";
        const HttpOutboundEndpointKey endpointKey{"127.0.0.1", fixture.listeningPort(), false};

        HalfOpenProbeOutcome outcome;
        auto                 task = runHalfOpenGateProbeTask(loop, client, *breaker, endpointKey, url, routeEntered, routeRelease, outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        ASSERT_TRUE(outcome.routeEntered) << "第一条请求没有进入被按住的路由，用例前提不成立";
        EXPECT_TRUE(outcome.secondRejected) << "并发闸上限 1 时第二条请求本该被挡下";
        EXPECT_EQ(client.outboundGateRejectionCount(), 1U) << "第二条失败不是并发闸挡下的";
        EXPECT_TRUE(outcome.nextProbeAllowed) << "探测名额被本地闸挡下的请求白占了：半开态的端点此后会一直拒绝探测";
    }

    /**
     * @brief 钉住重发闸门那一份判据：三态合起来决定「重来一次」还是「就此失败」
     * @details 这份判据过去埋在 HttpClient.cpp 的匿名命名空间里，只有 h1 那条端到端用例
     *          （`HttpOutboundConnectionPool.DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection`）
     *          间接压到它的一条边。RFC 9113 §8.7 的两种「保证没处理过」接进来之后判据多了一维，
     *          八种组合各有各的答案，端到端用例铺不满，因此把它提成一处公开判据逐格钉住
     * @note 证伪：把 `isGuaranteedUnprocessed` 那一项从判据里摘掉，「非幂等 + 有保证」两格红；
     *       把「对端答过话」那一条挪到保证之后判，最后两格红
     */
    TEST(HttpClientRetryRule, OnlyRetriesWhenTheSpecSaysItIsSafe)
    {
        // 请求压根没写上通路：对端无从执行它，重来与幂等性无关
        EXPECT_TRUE(HttpClient::isRetrySafeAfterFailure("POST", false, false, false));
        // 写上了通路、对端没答话：幂等方法按 RFC 9112 §9.3.2 重来
        EXPECT_TRUE(HttpClient::isRetrySafeAfterFailure("GET", true, false, false));
        // 同一种形状换成非幂等方法：默认不重来（可能已经执行过）
        EXPECT_FALSE(HttpClient::isRetrySafeAfterFailure("POST", true, false, false));
        // 方法 token 大小写敏感（RFC 9110 §9）：小写的 post 不在幂等集合里
        EXPECT_FALSE(HttpClient::isRetrySafeAfterFailure("post", true, false, false));
        // RFC 9113 §8.7 的保证压倒幂等性：REFUSED_STREAM／GOAWAY 之上那条流，非幂等也能重来
        EXPECT_TRUE(HttpClient::isRetrySafeAfterFailure("POST", true, false, true));
        EXPECT_TRUE(HttpClient::isRetrySafeAfterFailure("PATCH", true, false, true));
        // 对端答过话就一概不重来：换一条通路不会换一个答案，「保证」也不例外
        EXPECT_FALSE(HttpClient::isRetrySafeAfterFailure("GET", true, true, true));
        EXPECT_FALSE(HttpClient::isRetrySafeAfterFailure("POST", false, true, false));
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
     * @brief 钉住：authority 的边界认 `/`、`?`、`#` 三个，查询不被吞、片段不进请求目标
     * @details 此前只认 `/`，于是 `http://host?a=1` 把 `?a=1` 整段当成了主机：Host 头随之畸形，
     *          而查询静默消失——一个「看着配好了」的查询请求打到了不带参数的根上。片段按 RFC 9110
     *          §7.1 剥掉（它不属于一次 HTTP 请求），空路径按 RFC 9112 §3.2.1 补成 `/`，只带查询时是 `/?a=1`。
     */
    TEST(HttpClientUrlParsing, SplitsAuthorityAtQueryAndFragmentNotJustSlash)
    {
        const ParsedUrl queryOnly = parseUrl("http://host?a=1");
        EXPECT_EQ(queryOnly.host, "host") << "查询串被当成主机的一部分，Host 头就畸形了";
        EXPECT_EQ(queryOnly.port, 80U);
        EXPECT_EQ(queryOnly.path, "/?a=1") << "只带查询时请求目标是 /?a=1，不是空串";

        const ParsedUrl withFragment = parseUrl("https://host:8443/x?y=1#frag");
        EXPECT_EQ(withFragment.port, 8443U);
        EXPECT_EQ(withFragment.path, "/x?y=1") << "片段不该被交给服务器，也不该混进请求目标";

        const ParsedUrl ipv6Query = parseUrl("http://[::1]:8080?a=1");
        EXPECT_EQ(ipv6Query.host, "::1");
        EXPECT_EQ(ipv6Query.path, "/?a=1") << "方括号形式的主机后面直接跟查询时同样要拆对";

        EXPECT_EQ(parseUrl("http://host/#frag").path, "/") << "只有片段的地址，请求目标就是根";
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
     * @details 这条读的是对端给的字节，所以对端写歪时交回空而不是抛异常；判拒的六种形状各有一个
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
        ASSERT_HAS_VALUE(resolved);
        EXPECT_EQ(*resolved, "http://[::1]:8080/docs/sub/x.png") << "IPv6 主机必须带回方括号，否则端口分隔符会糊进地址里";

        const ParsedUrl reparsed = parseUrl(*resolved);
        EXPECT_EQ(reparsed.host, "::1");
        EXPECT_EQ(reparsed.port, 8080U);
        EXPECT_EQ(reparsed.path, "/docs/sub/x.png");

        const ParsedUrl defaultPortBase = parseUrl("https://a.example/docs/page.html");
        EXPECT_EQ(resolveUrlReference(defaultPortBase, "/x"), "https://a.example/x") << "443 是默认端口，写出来只会让 Host 头与基准不一致";
    }
    namespace
    {
        /// 服务端一侧逐跳记下的事实：请求目标、方法是不是 GET、有没有带凭据与 Cookie、正文多长
        struct RedirectHop
        {
            std::string target;
            bool        isGet{false};
            bool        hasAuthorization{false};
            bool        hasCookie{false};
            std::string cookie;
            std::size_t bodyLength{0};
        };

        /// 多条用例的处理器与断言在两条线程上读写同一张台账，所以按锁过
        class RedirectLedger
        {
        public:
            void record(RedirectHop hop)
            {
                const std::lock_guard guard(m_mutex);
                m_hops.push_back(std::move(hop));
            }
            [[nodiscard]] std::vector<RedirectHop> snapshot() const
            {
                const std::lock_guard guard(m_mutex);
                return m_hops;
            }

        private:
            mutable std::mutex       m_mutex;
            std::vector<RedirectHop> m_hops;
        };

        /// 一次跟随的读数：状态码（0 是没拿到响应）、正文、失败原因
        struct RedirectOutcome
        {
            int         status{0};
            std::string body;
            std::string reason;
        };

        Core::Task<void> followOnceTask(Core::EventLoop &loop, RedirectOutcome &outcome, const std::string url, HttpClientRequest request, const std::chrono::milliseconds timeout)
        {
            try
            {
                const auto sent = co_await HttpClient::send(loop, url, std::move(request), timeout);
                if (sent.has_value())
                {
                    outcome.status = sent->statusCode;
                    outcome.body   = sent->body;
                } else
                {
                    outcome.reason = sent.error();
                }
            } catch (const Base::Exception &failure)
            {
                outcome.reason = failure.what();
            }
            loop.stop();
        }

        /// 在调用方的线程上起一条循环跑一次请求（与 doGet 同一形状）
        RedirectOutcome followOnce(const std::string &url, HttpClientRequest request, const std::chrono::milliseconds timeout = HttpClient::kDefaultRequestTimeout)
        {
            Core::EventLoop loop;
            RedirectOutcome outcome;
            auto            work = followOnceTask(loop, outcome, url, std::move(request), timeout);
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            return outcome;
        }

        /// 记一笔台账并答复：location 为空就不带这一条头（那是「304/坏应答」那一格的形状）
        void answerRedirect(const std::shared_ptr<RedirectLedger> &ledger, HttpRequest &request, HttpResponse &response, const int status, const std::string_view location)
        {
            RedirectHop hop;
            hop.target           = std::string(request.path());
            hop.isGet            = request.method() == HttpMethod::GET;
            hop.hasAuthorization = request.firstHeaderValueView("authorization").has_value();
            if (const auto cookie = request.firstHeaderValueView("cookie"); cookie.has_value())
            {
                hop.hasCookie = true;
                hop.cookie    = std::string(*cookie);
            }
            hop.bodyLength = request.body().size();
            ledger->record(std::move(hop));
            response.setStatus(status);
            if (!location.empty())
            {
                static_cast<void>(response.setHeader("location", location));
            }
            response.setBody("redirected");
        }

        /// 记一笔台账并答复 200
        void answerLanded(const std::shared_ptr<RedirectLedger> &ledger, HttpRequest &request, HttpResponse &response)
        {
            RedirectHop hop;
            hop.target           = std::string(request.path());
            hop.isGet            = request.method() == HttpMethod::GET;
            hop.hasAuthorization = request.firstHeaderValueView("authorization").has_value();
            if (const auto cookie = request.firstHeaderValueView("cookie"); cookie.has_value())
            {
                hop.hasCookie = true;
                hop.cookie    = std::string(*cookie);
            }
            hop.bodyLength = request.body().size();
            ledger->record(std::move(hop));
            response.setStatus(200);
            response.setBody("landed");
        }

        std::string urlFor(const RunningHttpServerFixture &fixture, const std::string_view path)
        {
            return "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + std::string(path);
        }

        HttpClientRequest followingRequest(const std::string_view method, const std::size_t hopLimit = 10)
        {
            HttpClientRequest request;
            request.method               = std::string(method);
            request.followRedirects      = true;
            request.maximumRedirectCount = hopLimit;
            return request;
        }
    } // namespace

    /**
     * @brief 钉住：开关关着时那条 302 原样交回，一条都不跟
     * @details 默认关是本改动的兼容底线——已有调用方靠「拿到 3xx 自己处置」活着，
     *          替它们改成自动跟随等于把响应体换掉还不打招呼。
     */
    TEST(HttpClientRedirects, OffByDefaultReturnsTheRedirectItself)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/r302",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerRedirect(ledger, request, response, 302, "/landed");
                                                            co_return;
                                                        });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r302"), HttpClientRequest{});
        EXPECT_EQ(outcome.status, 302) << outcome.reason;
        EXPECT_EQ(ledger->snapshot().size(), 1U) << "关着就不该有第二跳";
    }

    TEST(HttpClientRedirects, FollowsARelativeLocationWhenEnabled)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/r302",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerRedirect(ledger, request, response, 302, "/landed");
                                                            co_return;
                                                        });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r302"), followingRequest("GET"));
        EXPECT_EQ(outcome.status, 200) << outcome.reason;
        EXPECT_EQ(outcome.body, "landed");
        const auto hops = ledger->snapshot();
        ASSERT_EQ(hops.size(), 2U);
        EXPECT_EQ(hops[1].target, "/landed") << "相对引用要按基准解析成这条路径";
        EXPECT_TRUE(hops[1].isGet);
    }

    /**
     * @brief 钉住：303 之后一律见 GET（RFC 9110 §15.4.4），正文与正文声明一起丢
     * @details 带着正文的 GET 是非法形状：留着 Content-Length 而正文没了，对端会等一段不存在的字节。
     */
    TEST(HttpClientRedirects, Status303FoldsPostIntoGetAndDropsTheBody)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.post("/r303",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerRedirect(ledger, request, response, 303, "/landed");
                                                             co_return;
                                                         });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        auto request                  = followingRequest("POST");
        request.body                  = "payload=1";
        request.contentType           = "application/x-www-form-urlencoded";
        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r303"), request);
        EXPECT_EQ(outcome.status, 200) << outcome.reason;
        const auto hops = ledger->snapshot();
        ASSERT_EQ(hops.size(), 2U);
        EXPECT_FALSE(hops[0].isGet);
        EXPECT_EQ(hops[0].bodyLength, 9U);
        EXPECT_TRUE(hops[1].isGet) << "303 的下一跳必须是 GET";
        EXPECT_EQ(hops[1].bodyLength, 0U) << "换 GET 还带着正文，等于发一条非法请求";
    }

    TEST(HttpClientRedirects, Status301FoldsOnlyPostIntoGet)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.post("/r301",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerRedirect(ledger, request, response, 301, "/landed");
                                                             co_return;
                                                         });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        auto request                  = followingRequest("POST");
        request.body                  = "x=1";
        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r301"), request);
        EXPECT_EQ(outcome.status, 200) << outcome.reason;
        const auto hops = ledger->snapshot();
        ASSERT_EQ(hops.size(), 2U);
        EXPECT_TRUE(hops[1].isGet) << "301 把 POST 折成 GET 是 §15.4.2 写下的历史一致行为";
    }

    /**
     * @brief 钉住：307 原样重放方法与正文（§15.4.8 明写不许改方法）
     * @details 这一条与 303 是反着的：把 307 也折成 GET 就等于替调用方换掉了语义（同一个请求再问一次）。
     */
    TEST(HttpClientRedirects, Status307ReplaysMethodAndBody)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.post("/r307",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerRedirect(ledger, request, response, 307, "/landed");
                                                             co_return;
                                                         });
                                             router.post("/landed",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerLanded(ledger, request, response);
                                                             co_return;
                                                         });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        auto request                  = followingRequest("POST");
        request.body                  = "again";
        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r307"), request);
        EXPECT_EQ(outcome.status, 200) << outcome.reason;
        const auto hops = ledger->snapshot();
        ASSERT_EQ(hops.size(), 2U);
        EXPECT_FALSE(hops[1].isGet) << "307 的下一跳仍是 POST";
        EXPECT_EQ(hops[1].bodyLength, 5U) << "正文要重放同一份字节";
    }

    /**
     * @brief 钉住：跨源重定向剥掉 Authorization，同源的不剥
     * @details RFC 9110 §11.5.3 要求凭据只能发给原请求那台主机。端口不同也算跨源——
     *          「同一台机器上的另一个服务」正是这种泄漏最容易生效的形状。
     */
    TEST(HttpClientRedirects, CrossOriginRedirectStripsCredentialsWhileSameOriginKeepsThem)
    {
        auto                     sourceLedger = std::make_shared<RedirectLedger>();
        auto                     targetLedger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture target(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [targetLedger](Router &router, Core::EventLoop &)
                                        {
                                            router.get("/landed",
                                                       [targetLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerLanded(targetLedger, request, response);
                                                           co_return;
                                                       });
                                        });
        ASSERT_TRUE(target.awaitRunning(kTimeout));

        RunningHttpServerFixture source(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [sourceLedger, targetPort = target.listeningPort()](Router &router, Core::EventLoop &)
                                        {
                                            router.get("/away",
                                                       [sourceLedger, targetPort](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerRedirect(sourceLedger, request, response, 302, "http://127.0.0.1:" + std::to_string(targetPort) + "/landed");
                                                           co_return;
                                                       });
                                            router.get("/keep",
                                                       [sourceLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerRedirect(sourceLedger, request, response, 302, "/landed");
                                                           co_return;
                                                       });
                                            router.get("/landed",
                                                       [sourceLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerLanded(sourceLedger, request, response);
                                                           co_return;
                                                       });
                                        });
        ASSERT_TRUE(source.awaitRunning(kTimeout));

        auto crossing = followingRequest("GET");
        crossing.headers.emplace_back("authorization", "Bearer secret-token");
        const RedirectOutcome crossOutcome = followOnce(urlFor(source, "/away"), crossing);
        EXPECT_EQ(crossOutcome.status, 200) << crossOutcome.reason;
        const auto crossHops = targetLedger->snapshot();
        ASSERT_EQ(crossHops.size(), 1U);
        EXPECT_FALSE(crossHops[0].hasAuthorization) << "跨源还带着凭据，等于把令牌交给对端挑的那台机器";

        auto staying = followingRequest("GET");
        staying.headers.emplace_back("authorization", "Bearer secret-token");
        const RedirectOutcome sameOutcome = followOnce(urlFor(source, "/keep"), staying);
        EXPECT_EQ(sameOutcome.status, 200) << sameOutcome.reason;
        const auto sameHops = sourceLedger->snapshot();
        ASSERT_EQ(sameHops.size(), 3U) << "两次请求共用同一张台账：/away 那跳也在里面";
        EXPECT_TRUE(sameHops.back().hasAuthorization) << "同源下一跳仍该带上调用方给的凭据";
    }

    /**
     * @brief 钉住：同源重定向链逐跳从罐子取 Cookie、逐跳收 Set-Cookie
     * @details 首跳发的会话必须在下一跳就带上，而不是等整条链跟完才补；后续对同一主机的请求
     *          同样要能从罐子里带上去。
     */
    TEST(HttpClientRedirects, JarSendsAndStoresCookiesOnEverySameOriginHop)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/start",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            HttpCookie session("sid", "abc");
                                                            session.setPath("/");
                                                            response.setCookie(session);
                                                            response.setStatus(302);
                                                            static_cast<void>(response.setHeader("location", "/next"));
                                                            response.setBody("redirected");
                                                            co_return;
                                                        });
                                             router.get("/next",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                             router.get("/whoami",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        Core::EventLoop   loop;
        int               followedStatus = 0;
        int               laterStatus    = 0;
        const std::string startUrl       = urlFor(fixture, "/start");
        const std::string whoamiUrl      = urlFor(fixture, "/whoami");
        auto              task           = [&loop, &followedStatus, &laterStatus, startUrl, whoamiUrl](HttpClient &client) -> Core::Task<void>
        {
            const std::unique_ptr<HttpClientResponse> followed = co_await client.send(startUrl, followingRequest("GET"));
            followedStatus                                     = followed != nullptr ? followed->statusCode : 0;
            const std::unique_ptr<HttpClientResponse> later    = co_await client.get(whoamiUrl);
            laterStatus                                        = later != nullptr ? later->statusCode : 0;
            loop.stop();
            co_return;
        };
        HttpClient client(loop);
        client.setCookieJar(std::make_shared<HttpCookieJar>());
        auto work = task(client);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(followedStatus, 200);
        EXPECT_EQ(laterStatus, 200);
        const auto hops = ledger->snapshot();
        ASSERT_EQ(hops.size(), 2U) << "第一跳之后的 /next 与后续的 /whoami 各一笔";
        EXPECT_EQ(hops[0].target, "/next");
        EXPECT_TRUE(hops[0].hasCookie) << "首跳发的 Set-Cookie 没有在下一跳带上";
        EXPECT_EQ(hops[0].cookie, "sid=abc");
        EXPECT_EQ(hops[1].target, "/whoami");
        EXPECT_TRUE(hops[1].hasCookie) << "后续请求没有从罐子里带上会话";
        EXPECT_EQ(hops[1].cookie, "sid=abc");
    }

    /**
     * @brief 钉住：跨源重定向时罐里的 Cookie 只发给它自己的源，终跳的 Set-Cookie 记在终跳的域下
     * @details 罐子按主机名判作用域，不按端口：两台服务器若都落在 127.0.0.1，A 的 Cookie 对 B
     *          本来就合法（RFC 6265 不看端口），因此目标机监听另一个回环地址，让「跨源」同时是
     *          「跨域」。A 的 a=1 不得出现在 B 的请求上；B 回的 b=2 也不得回灌到 A。
     */
    TEST(HttpClientRedirects, CrossOriginRedirectKeepsJarCookiesScopedToTheirOwnOrigin)
    {
        auto                                      targetLedger = std::make_shared<RedirectLedger>();
        std::unique_ptr<RunningHttpServerFixture> target;
        try
        {
            target = std::make_unique<RunningHttpServerFixture>(
                    HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                    [targetLedger](Router &router, Core::EventLoop &)
                    {
                        router.get("/landed",
                                   [targetLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                   {
                                       HttpCookie cookie("b", "2");
                                       cookie.setPath("/");
                                       response.setCookie(cookie);
                                       answerLanded(targetLedger, request, response);
                                       co_return;
                                   });
                        router.get("/whoami",
                                   [targetLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                   {
                                       answerLanded(targetLedger, request, response);
                                       co_return;
                                   });
                    },
                    HttpParserLimits{}, ServerConfigurator{}, Core::InetAddress{0, "127.0.0.2"});
        } catch (const std::exception &)
        {
            GTEST_SKIP() << "127.0.0.2 绑不上：这台机器没有第二个可用的回环地址";
        }
        ASSERT_TRUE(target->awaitRunning(kTimeout));
        const std::string targetBase = "http://127.0.0.2:" + std::to_string(target->listeningPort());

        auto                     sourceLedger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture source(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [sourceLedger, targetPort = target->listeningPort()](Router &router, Core::EventLoop &)
                                        {
                                            router.get("/away",
                                                       [sourceLedger, targetPort](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           HttpCookie cookie("a", "1");
                                                           cookie.setPath("/");
                                                           response.setCookie(cookie);
                                                           answerRedirect(sourceLedger, request, response, 302, "http://127.0.0.2:" + std::to_string(targetPort) + "/landed");
                                                           co_return;
                                                       });
                                            router.get("/whoami",
                                                       [sourceLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerLanded(sourceLedger, request, response);
                                                           co_return;
                                                       });
                                        });
        ASSERT_TRUE(source.awaitRunning(kTimeout));

        Core::EventLoop   loop;
        int               followedStatus    = 0;
        int               sourceLaterStatus = 0;
        int               targetLaterStatus = 0;
        const std::string awayUrl           = urlFor(source, "/away");
        const std::string sourceWhoamiUrl   = urlFor(source, "/whoami");
        const std::string targetWhoamiUrl   = targetBase + "/whoami";
        auto              task = [&loop, &followedStatus, &sourceLaterStatus, &targetLaterStatus, awayUrl, sourceWhoamiUrl, targetWhoamiUrl](HttpClient &client) -> Core::Task<void>
        {
            const std::unique_ptr<HttpClientResponse> followed    = co_await client.send(awayUrl, followingRequest("GET"));
            followedStatus                                        = followed != nullptr ? followed->statusCode : 0;
            const std::unique_ptr<HttpClientResponse> sourceLater = co_await client.get(sourceWhoamiUrl);
            sourceLaterStatus                                     = sourceLater != nullptr ? sourceLater->statusCode : 0;
            const std::unique_ptr<HttpClientResponse> targetLater = co_await client.get(targetWhoamiUrl);
            targetLaterStatus                                     = targetLater != nullptr ? targetLater->statusCode : 0;
            loop.stop();
            co_return;
        };
        HttpClient client(loop);
        client.setCookieJar(std::make_shared<HttpCookieJar>());
        auto work = task(client);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(followedStatus, 200);
        EXPECT_EQ(sourceLaterStatus, 200);
        EXPECT_EQ(targetLaterStatus, 200);

        const auto targetHops = targetLedger->snapshot();
        ASSERT_EQ(targetHops.size(), 2U) << "跨源那一跳与后续的 /whoami 各一笔";
        EXPECT_EQ(targetHops[0].target, "/landed");
        EXPECT_FALSE(targetHops[0].hasCookie) << "A 的 Cookie 被带去 B：跨源跳没剥干净";
        EXPECT_EQ(targetHops[1].target, "/whoami");
        EXPECT_EQ(targetHops[1].cookie, "b=2") << "B 的 Set-Cookie 没记在 B 的域下";

        const auto sourceHops = sourceLedger->snapshot();
        ASSERT_EQ(sourceHops.size(), 2U) << "/away 那一跳与后续的 /whoami 各一笔";
        EXPECT_EQ(sourceHops[0].target, "/away");
        EXPECT_EQ(sourceHops[1].target, "/whoami");
        EXPECT_EQ(sourceHops[1].cookie, "a=1") << "A 的 Cookie 被终跳的入库改到了别的域下";
    }

    /**
     * @brief 钉住：调用方显式写的 Cookie 头在同源跳保留，跨源跳剥掉，罐子不覆盖调用方的值
     * @details 罐子里先种一条源主机的会话；调用方仍写自己的 cookie 头时，同源下一跳必须原样带着
     *          调用方那份，而不是被罐子里的值换掉。
     */
    TEST(HttpClientRedirects, CallerCookieHeaderSurvivesSameOriginButNotCrossOriginHop)
    {
        auto                     targetLedger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture target(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [targetLedger](Router &router, Core::EventLoop &)
                                        {
                                            router.get("/landed",
                                                       [targetLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerLanded(targetLedger, request, response);
                                                           co_return;
                                                       });
                                        });
        ASSERT_TRUE(target.awaitRunning(kTimeout));

        auto                     sourceLedger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture source(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [sourceLedger, targetPort = target.listeningPort()](Router &router, Core::EventLoop &)
                                        {
                                            router.get("/keep",
                                                       [sourceLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerRedirect(sourceLedger, request, response, 302, "/landed");
                                                           co_return;
                                                       });
                                            router.get("/landed",
                                                       [sourceLedger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerLanded(sourceLedger, request, response);
                                                           co_return;
                                                       });
                                            router.get("/away",
                                                       [sourceLedger, targetPort](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           answerRedirect(sourceLedger, request, response, 302, "http://127.0.0.1:" + std::to_string(targetPort) + "/landed");
                                                           co_return;
                                                       });
                                        });
        ASSERT_TRUE(source.awaitRunning(kTimeout));

        Core::EventLoop   loop;
        int               sameStatus  = 0;
        int               crossStatus = 0;
        const std::string keepUrl     = urlFor(source, "/keep");
        const std::string awayUrl     = urlFor(source, "/away");
        auto              task        = [&loop, &sameStatus, &crossStatus, keepUrl, awayUrl](HttpClient &client) -> Core::Task<void>
        {
            HttpClientRequest staying = followingRequest("GET");
            staying.headers.emplace_back("cookie", "mine=1");
            const std::unique_ptr<HttpClientResponse> same = co_await client.send(keepUrl, staying);
            sameStatus                                     = same != nullptr ? same->statusCode : 0;

            HttpClientRequest crossing = followingRequest("GET");
            crossing.headers.emplace_back("cookie", "mine=1");
            const std::unique_ptr<HttpClientResponse> cross = co_await client.send(awayUrl, crossing);
            crossStatus                                     = cross != nullptr ? cross->statusCode : 0;

            loop.stop();
            co_return;
        };
        HttpClient client(loop);
        auto       jar = std::make_shared<HttpCookieJar>();
        // 罐子里先种一条源主机的会话：同源下一跳若改由罐子取值，收到的就会是 sid=abc
        jar->storeFromResponse("127.0.0.1", false, "/", {"sid=abc; Path=/"});
        client.setCookieJar(jar);
        auto work = task(client);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(sameStatus, 200);
        EXPECT_EQ(crossStatus, 200);

        const auto sourceHops = sourceLedger->snapshot();
        ASSERT_EQ(sourceHops.size(), 3U) << "/keep、同源跳 /landed、/away 各一笔";
        EXPECT_EQ(sourceHops[0].target, "/keep");
        EXPECT_EQ(sourceHops[0].cookie, "mine=1");
        EXPECT_EQ(sourceHops[1].target, "/landed");
        EXPECT_EQ(sourceHops[1].cookie, "mine=1") << "同源跳把调用方的 cookie 头换成了罐子里的值";
        EXPECT_EQ(sourceHops[2].target, "/away");
        EXPECT_EQ(sourceHops[2].cookie, "mine=1");

        const auto targetHops = targetLedger->snapshot();
        ASSERT_EQ(targetHops.size(), 1U);
        EXPECT_EQ(targetHops[0].target, "/landed");
        EXPECT_FALSE(targetHops[0].hasCookie) << "跨源跳仍带着调用方写的 cookie 头";
    }

    /**
     * @brief 钉住：要重放一次性拉取的正文时不跟随，把那条 307 原样交回
     * @details 拉取来源已经被抽干了；再拉一遍交出去的是另一份字节，而对端以为收到的是同一份请求。
     *          宁可不跟，也不悄悄发一条没正文或内容不同的请求。
     */
    TEST(HttpClientRedirects, DoesNotReplayAOneShotBodySource)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.post("/r307",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerRedirect(ledger, request, response, 307, "/landed");
                                                             co_return;
                                                         });
                                             router.post("/landed",
                                                         [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             answerLanded(ledger, request, response);
                                                             co_return;
                                                         });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        auto request = followingRequest("POST");
        auto cursor  = std::make_shared<bool>(false);
        // 一次性来源：第一口给出字节，第二口就是终点——抽干了就没有「同一份正文」可重放
        request.bodySource = [cursor]() -> Core::Task<std::optional<std::string>>
        {
            if (*cursor)
            {
                co_return std::nullopt;
            }
            *cursor = true;
            co_return std::optional<std::string>{"streamed-once"};
        };
        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r307"), request);
        EXPECT_EQ(outcome.status, 307) << outcome.reason;
        EXPECT_EQ(ledger->snapshot().size(), 1U) << "不该发出第二跳";
    }

    TEST(HttpClientRedirects, DoesNotFollowWhileAResponseBodyReceiverIsAttached)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/r303",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerRedirect(ledger, request, response, 303, "/landed");
                                                            co_return;
                                                        });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        auto        request = followingRequest("GET");
        std::string delivered;
        request.responseBodyReceiver = [&delivered](const HttpResponseInfo &, const std::string_view batch, const bool) -> Core::Task<bool>
        {
            delivered.append(batch);
            co_return true;
        };
        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/r303"), request);
        EXPECT_EQ(outcome.status, 303) << outcome.reason;
        EXPECT_EQ(delivered, "redirected") << "接收口该只收到那条 303 自己的正文";
        EXPECT_EQ(ledger->snapshot().size(), 1U) << "同一个接收口不能收两段互不相干的正文";
    }

    /**
     * @brief 钉住：跳数上限就是环检测，停在顶上时交回最后那条 302
     * @details `Location` 指回自己不需要识别环：数到顶就停。交回 3xx 而不是编一个失败——
     *          那几条都是对端真实给过的响应，调用方看得见停在第几跳。
     */
    TEST(HttpClientRedirects, StopsAtTheHopLimitAndReturnsTheLastRedirect)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/loop",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerRedirect(ledger, request, response, 302, "/loop");
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/loop"), followingRequest("GET", 2));
        EXPECT_EQ(outcome.status, 302) << outcome.reason;
        EXPECT_EQ(ledger->snapshot().size(), 3U) << "上限 2 跳 = 首发一次 + 跟随两次，第三次仍带着 302 交回";
    }

    /**
     * @brief 钉住：跟随重定向不会把总时限乘成「时限 × 跳数」
     * @details 这条断的是结局（整条链必须在调用方给的预算内收场），不是某一行算术：包里有两道闸
     *          在做同一件事——发下一跳前先看剩余（不够就当场停），以及把「此刻剩余」当作下一跳的时限。
     *          只拆掉后者（每跳重新给一份满预算）这条用例照样绿，因为前者仍然把总时长兜住；要把这条
     *          用例逼红，得两道一起拆（`no-budget-at-all` 那次突变就是这么做的，实测红在本条）。
     *          慢机器只会让它更早停下来，所以断言里不放时间上界。
     */
    TEST(HttpClientRedirects, SharesOneTimeoutBudgetAcrossHops)
    {
        constexpr auto           kHopDelay    = std::chrono::milliseconds{80};
        constexpr auto           kWholeBudget = std::chrono::milliseconds{250};
        constexpr int            kChainLength = 5;
        auto                     ledger       = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &loop)
                                         {
                                             for (int index = 0; index < kChainLength; ++index)
                                             {
                                                 const std::string from = "/h" + std::to_string(index);
                                                 const std::string to   = index + 1 == kChainLength ? "/landed" : "/h" + std::to_string(index + 1);
                                                 router.get(from,
                                                            [ledger, &loop, to](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                            {
                                                                Core::Timer pause(loop);
                                                                co_await pause.waitFor(kHopDelay);
                                                                answerRedirect(ledger, request, response, 302, to);
                                                                co_return;
                                                            });
                                             }
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        const RedirectOutcome outcome = followOnce(urlFor(fixture, "/h0"), followingRequest("GET", 10), kWholeBudget);
        EXPECT_EQ(outcome.status, 0) << "共享预算下这条链该在时限内跑不完，交回 200 就是每跳重新给了预算";
        EXPECT_NE(outcome.reason.find("时限"), std::string::npos) << outcome.reason;
        const auto hops = ledger->snapshot();
        EXPECT_GE(hops.size(), 1U) << "至少要走完第一跳，才有「预算被跳数吃掉」这件事";
        EXPECT_LE(hops.size(), static_cast<std::size_t>(kChainLength - 1)) << "走完全部跳数说明预算没被共享";
    }

    /**
     * @brief 钉住：带池那一条出口也吃到同一份跟随逻辑（不是只接了静态 send()）
     */
    TEST(HttpClientRedirects, PooledClientFollowsToo)
    {
        auto                     ledger = std::make_shared<RedirectLedger>();
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [ledger](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/r302",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerRedirect(ledger, request, response, 302, "/landed");
                                                            co_return;
                                                        });
                                             router.get("/landed",
                                                        [ledger](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            answerLanded(ledger, request, response);
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout));

        Core::EventLoop loop;
        int             observedStatus = 0;
        std::string     observedBody;
        auto            task = [&loop, &observedStatus, &observedBody, url = urlFor(fixture, "/r302")](HttpClient &client) -> Core::Task<void>
        {
            const std::unique_ptr<HttpClientResponse> response = co_await client.send(url, followingRequest("GET"));
            if (response != nullptr)
            {
                observedStatus = response->statusCode;
                observedBody   = response->body;
            }
            loop.stop();
            co_return;
        };
        HttpClient client(loop);
        // 协程帧按值收引用参数会被销毁的闭包顶在手里（这条形状此前咬过一次），所以把客户端活到循环结束
        auto work = task(client);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();
        EXPECT_EQ(observedStatus, 200);
        EXPECT_EQ(observedBody, "landed");
        EXPECT_EQ(ledger->snapshot().size(), 2U);
    }
} // namespace AsynGyanis::Net
