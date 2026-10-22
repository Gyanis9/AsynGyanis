// OAuthAuthorizationCode 的用例：PKCE 与授权 URL 是纯函数（跨实现判据取 RFC 7636 附录 B 向量），
// 换取走本地「最小令牌端点」（后台线程、记录请求、响应脚本化），主线程一次 run() 跑完全部阶段。
// 覆盖：
// - MatchesTheRfc7636AppendixBVector（S256：verifier → challenge 的官方测试向量）
// - BuildsAuthorizationUrlCarryingPkceStateAndEncodedRedirect（URL 逐段 + 挑战与 verifier 自洽 + 两次调用互异）
// - ParsesCallbackFromFullUrlAndFromBareQuery（片段剥掉、%2B 与 + 解码）
// - RejectsMalformedCallbacksWithNamedReasons（既没有 code 也没有 error）
// - RejectsStateMismatchWithoutTouchingTheNetwork（state 不符 → 拒；对端零请求）
// - RefusesCallbackErrorsWithoutExchanging（回调带 error → 拒并带描述；对端零请求）
// - ExchangesCodeCarryingVerifierAndRedirect（换取成功：请求侧编码断言 + 共用令牌形状）
// - SurfacesTokenEndpointErrors（400 invalid_grant 与 §5.2 描述透传）
// - RejectsEmptyConfigurationAtConstruction（四个必填取值）

#include "Net/Http/Auth/OAuthAuthorizationCode.h"

#include "Base/Exception/InvalidArgumentException.h"

#include "Core/EventLoop/EventLoop.h"

#include "ExpectedAssertions.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Net/Http/Auth/OAuthError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 最小令牌端点：记录收到的完整请求（头 + 体），响应按脚本逐条发放（最后一条重复）
         */
        class MiniTokenEndpoint
        {
        public:
            MiniTokenEndpoint() = default;

            MiniTokenEndpoint(const MiniTokenEndpoint &)            = delete;
            MiniTokenEndpoint &operator=(const MiniTokenEndpoint &) = delete;

            ~MiniTokenEndpoint()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            [[nodiscard]] bool start()
            {
                m_worker            = std::thread([this] { run(); });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                while (m_port.load(std::memory_order_acquire) == 0U && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return m_port.load(std::memory_order_acquire) != 0U;
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::uint64_t requestCount() const noexcept
            {
                return m_requests.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::string lastRequestBytes() const
            {
                const std::lock_guard guard(m_mutex);
                return m_lastRequest;
            }

            void setResponses(std::vector<std::pair<std::string, std::string>> responses)
            {
                const std::lock_guard guard(m_mutex);
                m_responses     = std::move(responses);
                m_responseIndex = 0;
            }

        private:
            void run()
            {
                const Platform::Socket::Initialization network;
                const int                              listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (listener < 0)
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
                sockaddr_in bound{};
                socklen_t   boundLength = static_cast<socklen_t>(sizeof(bound));
                if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    return;
                }
                m_port.store(ntohs(bound.sin_port), std::memory_order_release);

                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet;
                    FD_ZERO(&readSet);
                    FD_SET(listener, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(listener + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }

                    const int connection = static_cast<int>(::accept(listener, nullptr, nullptr));
                    if (connection < 0)
                    {
                        continue;
                    }

                    std::string requestBytes;
                    char        chunk[1024];
                    std::size_t expectedTotal = std::string::npos;
                    while (true)
                    {
                        const int received = ::recv(connection, chunk, static_cast<int>(sizeof(chunk)), 0);
                        if (received <= 0)
                        {
                            break;
                        }
                        requestBytes.append(chunk, static_cast<std::size_t>(received));

                        if (expectedTotal == std::string::npos)
                        {
                            const std::size_t headEnd = requestBytes.find("\r\n\r\n");
                            if (headEnd != std::string::npos)
                            {
                                std::size_t contentLength  = 0;
                                const auto  lengthPosition = requestBytes.find("Content-Length: ");
                                if (lengthPosition != std::string::npos)
                                {
                                    contentLength = static_cast<std::size_t>(std::stoul(requestBytes.substr(lengthPosition + 16)));
                                }
                                expectedTotal = headEnd + 4 + contentLength;
                            }
                        }
                        if (expectedTotal != std::string::npos && requestBytes.size() >= expectedTotal)
                        {
                            break;
                        }
                    }

                    // 计数在回响应之前发布：客户端拿到响应后立刻断言 requestCount 时，
                    // 不能还停在这条 fetch_add 之前（并行跑全量时实测过一次 1≠2 的假红）
                    m_requests.fetch_add(1, std::memory_order_acq_rel);

                    std::string statusLine;
                    std::string responseBody;
                    {
                        const std::lock_guard guard(m_mutex);
                        m_lastRequest = requestBytes;
                        if (!m_responses.empty())
                        {
                            const std::size_t scripted = std::min(m_responseIndex, m_responses.size() - 1);
                            statusLine                 = m_responses[scripted].first;
                            responseBody               = m_responses[scripted].second;
                            if (m_responseIndex + 1 < m_responses.size())
                            {
                                ++m_responseIndex;
                            }
                        } else
                        {
                            statusLine   = "500 Internal Server Error";
                            responseBody = "{}";
                        }
                    }

                    const std::string response  = "HTTP/1.1 " + statusLine + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(responseBody.size()) +
                                                  "\r\nConnection: close\r\n\r\n" + responseBody;
                    std::size_t       sentTotal = 0;
                    while (sentTotal < response.size())
                    {
                        const int justSent = ::send(connection, response.data() + sentTotal, static_cast<int>(response.size() - sentTotal), 0);
                        if (justSent <= 0)
                        {
                            break;
                        }
                        sentTotal += static_cast<std::size_t>(justSent);
                    }
                    Platform::FileDescriptor::close(connection);
                }
                Platform::FileDescriptor::close(listener);
            }

            std::thread                                      m_worker;
            std::atomic<std::uint16_t>                       m_port{0};
            std::atomic<std::uint64_t>                       m_requests{0};
            std::atomic<bool>                                m_isStopping{false};
            mutable std::mutex                               m_mutex;
            std::string                                      m_lastRequest;
            std::vector<std::pair<std::string, std::string>> m_responses;
            std::size_t                                      m_responseIndex{0};
        };

        /// 起主控协程并跑到它自己停循环：Task 必须活到 run() 之后（临时对象取 handle 会当场析构帧）
        void runControlTask(Core::EventLoop &loop, Core::Task<void> control)
        {
            if (!control.isReady())
            {
                loop.scheduler().schedule(control.handle());
            }
            loop.run();
        }

        /// 手拼一份「已发出的授权请求」：state 与 verifier 定死，换取路径因此可完全确定地复核
        [[nodiscard]] OAuthAuthorizationRequest makeRecordedRequest()
        {
            return OAuthAuthorizationRequest{.url = "https://auth.example/authorize?state=st-1", .state = "st-1", .verifier = "verifier-1"};
        }

        /// 正例：换取成功 + 400 invalid_grant 透传（同一对端，两条脚本）
        Core::Task<void> exchangeScenario(Core::EventLoop &loop, OAuthAuthorizationCodeClient &client, MiniTokenEndpoint &endpoint)
        {
            const OAuthAuthorizationCallback callback{.code = "code-1", .state = "st-1"};

            const auto token = co_await client.exchangeCode(makeRecordedRequest(), callback);
            EXPECT_HAS_VALUE(token) << token.error().message;
            if (token.has_value())
            {
                EXPECT_EQ(token->accessToken, "at-code");
                EXPECT_EQ(token->tokenType, "Bearer");
                EXPECT_EQ(token->refreshToken, "rt-code");
                EXPECT_EQ(token->expiresIn.count(), 1800);
            }

            // 请求侧：grant_type 逐字、四个参数都在且按表单编码（回调地址的冒号与斜杠要编掉）
            const std::string recorded = endpoint.lastRequestBytes();
            EXPECT_NE(recorded.find("POST /token HTTP/1.1"), std::string::npos) << recorded.substr(0, 120);
            EXPECT_NE(recorded.find("grant_type=authorization_code"), std::string::npos) << recorded;
            EXPECT_NE(recorded.find("code=code-1"), std::string::npos) << recorded;
            EXPECT_NE(recorded.find("redirect_uri=https%3A%2F%2Fapp.example%2Fcb"), std::string::npos) << recorded;
            EXPECT_NE(recorded.find("client_id=client-1"), std::string::npos) << recorded;
            EXPECT_NE(recorded.find("code_verifier=verifier-1"), std::string::npos) << recorded;

            // 令牌端点回错误：原样透传 error 与描述，不猜可重试性
            endpoint.setResponses({{"400 Bad Request", R"({"error":"invalid_grant","error_description":"授权码已过期或已被使用"})"}});
            const auto reused = co_await client.exchangeCode(makeRecordedRequest(), callback);
            EXPECT_FALSE(reused.has_value());
            if (!reused.has_value())
            {
                EXPECT_NE(reused.error().message.find("invalid_grant"), std::string::npos) << reused.error().message;
                EXPECT_NE(reused.error().message.find("授权码已过期或已被使用"), std::string::npos) << reused.error().message;
            }
            EXPECT_EQ(endpoint.requestCount(), 2U);
            loop.stop();
        }

        /// state 不符：本地拒，不触网
        Core::Task<void> stateMismatchScenario(Core::EventLoop &loop, OAuthAuthorizationCodeClient &client, MiniTokenEndpoint &endpoint)
        {
            const OAuthAuthorizationCallback forged{.code = "code-1", .state = "st-other"};
            const auto                       token = co_await client.exchangeCode(makeRecordedRequest(), forged);
            EXPECT_FALSE(token.has_value());
            if (!token.has_value())
            {
                EXPECT_NE(token.error().message.find("state"), std::string::npos) << token.error().message;
                EXPECT_EQ(token.error().kind, OAuthErrorKind::StateMismatch) << "回调的 state 与本次请求不一致，这一格必须能不看文案就拒掉";
            }
            EXPECT_EQ(endpoint.requestCount(), 0U) << "state 不符就不该发出换取请求";
            loop.stop();
        }

        /// 回调本身带 error：按对端给的原因拒，不触网
        Core::Task<void> callbackErrorScenario(Core::EventLoop &loop, OAuthAuthorizationCodeClient &client, MiniTokenEndpoint &endpoint)
        {
            const OAuthAuthorizationCallback denied{.state = "st-1", .error = "access_denied", .errorDescription = "用户点了拒绝"};
            const auto                       token = co_await client.exchangeCode(makeRecordedRequest(), denied);
            EXPECT_FALSE(token.has_value());
            if (!token.has_value())
            {
                EXPECT_NE(token.error().message.find("access_denied"), std::string::npos) << token.error().message;
                EXPECT_EQ(token.error().kind, OAuthErrorKind::AuthorizationDenied) << "用户在同意页点了拒绝，不是故障";
                EXPECT_NE(token.error().message.find("用户点了拒绝"), std::string::npos) << token.error().message;
            }
            EXPECT_EQ(endpoint.requestCount(), 0U) << "回调已带 error 就不该再发换取请求";
            loop.stop();
        }
    } // namespace

    TEST(OAuthAuthorizationCodeClientTest, MatchesTheRfc7636AppendixBVector)
    {
        // RFC 7636 附录 B 的官方测试向量：S256 的挑战是 verifier 的 SHA-256 的 base64url（无填充）
        const auto challenge = OAuthAuthorizationCodeClient::computePkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
        ASSERT_HAS_VALUE(challenge) << challenge.error().message;
        EXPECT_EQ(*challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    }

    TEST(OAuthAuthorizationCodeClientTest, BuildsAuthorizationUrlCarryingPkceStateAndEncodedRedirect)
    {
        Core::EventLoop                    loop;
        const OAuthAuthorizationCodeClient client(loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize",
                                                                                      .tokenUrl         = "http://127.0.0.1:1/token",
                                                                                      .clientId         = "client-1",
                                                                                      .redirectUri      = "https://app.example/cb",
                                                                                      .scope            = "read write"});

        const auto first = client.beginAuthorization();
        ASSERT_HAS_VALUE(first) << first.error().message;
        const std::string &url = first->url;
        EXPECT_EQ(url.find("https://auth.example/authorize?"), 0U) << url;
        EXPECT_NE(url.find("response_type=code"), std::string::npos) << url;
        EXPECT_NE(url.find("client_id=client-1"), std::string::npos) << url;
        EXPECT_NE(url.find("redirect_uri=https%3A%2F%2Fapp.example%2Fcb"), std::string::npos) << url;
        EXPECT_NE(url.find("scope=read%20write"), std::string::npos) << url;
        EXPECT_NE(url.find("state=" + first->state), std::string::npos) << url;

        // 挑战与 verifier 自洽（computePkceChallenge 本身已被附录 B 向量钉住）
        const auto expectedChallenge = OAuthAuthorizationCodeClient::computePkceChallenge(first->verifier);
        ASSERT_HAS_VALUE(expectedChallenge);
        EXPECT_NE(url.find("code_challenge=" + *expectedChallenge), std::string::npos) << url;
        EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos) << url;

        // verifier 与 state 的形状：43 字符的 base64url（32 字节随机）；两次调用必须互不相同
        EXPECT_EQ(first->verifier.size(), 43U) << first->verifier;
        EXPECT_TRUE(first->verifier.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos) << first->verifier;
        EXPECT_FALSE(first->state.empty());
        EXPECT_NE(first->state, first->verifier);

        const auto second = client.beginAuthorization();
        ASSERT_HAS_VALUE(second) << second.error().message;
        EXPECT_NE(second->verifier, first->verifier) << "两次授权请求的 PKCE verifier 不该相同";
        EXPECT_NE(second->state, first->state) << "两次授权请求的 state 不该相同";
    }

    TEST(OAuthAuthorizationCodeClientTest, ParsesCallbackFromFullUrlAndFromBareQuery)
    {
        // 整条回调 URL：片段要剥掉，%2B 解成 '+'（不是空格）
        const auto fromUrl = OAuthAuthorizationCodeClient::parseCallback("https://app.example/cb?code=abc%2B123&state=st-1#access_token=x");
        ASSERT_HAS_VALUE(fromUrl) << fromUrl.error().message;
        EXPECT_EQ(fromUrl->code, "abc+123");
        EXPECT_EQ(fromUrl->state, "st-1");
        EXPECT_TRUE(fromUrl->error.empty());

        // 裸查询串：与整 URL 同解；'+' 按表单约定解成空格
        const auto fromQuery = OAuthAuthorizationCodeClient::parseCallback("code=abc%2B123&state=st+1");
        ASSERT_HAS_VALUE(fromQuery) << fromQuery.error().message;
        EXPECT_EQ(fromQuery->code, "abc+123");
        EXPECT_EQ(fromQuery->state, "st 1");

        // error 回调：错误码与描述都要取回（描述里的 '+' 解成空格）
        const auto denied = OAuthAuthorizationCodeClient::parseCallback("?error=access_denied&error_description=user+said+no&state=st-1");
        ASSERT_HAS_VALUE(denied) << denied.error().message;
        EXPECT_EQ(denied->error, "access_denied");
        EXPECT_EQ(denied->errorDescription, "user said no");
        EXPECT_EQ(denied->state, "st-1");
        EXPECT_TRUE(denied->code.empty());
    }

    TEST(OAuthAuthorizationCodeClientTest, RejectsMalformedCallbacksWithNamedReasons)
    {
        for (const char *malformed: {"?foo=bar", "https://app.example/cb"})
        {
            const auto callback = OAuthAuthorizationCodeClient::parseCallback(malformed);
            EXPECT_FALSE(callback.has_value()) << malformed;
            if (!callback.has_value())
            {
                EXPECT_NE(callback.error().message.find("code"), std::string::npos) << callback.error().message;
                EXPECT_EQ(callback.error().kind, OAuthErrorKind::MissingAuthorizationCode) << "既没有 code 也没有 error 的回跳不是授权响应";
            }
        }
    }

    TEST(OAuthAuthorizationCodeClientTest, RejectsStateMismatchWithoutTouchingTheNetwork)
    {
        MiniTokenEndpoint endpoint;
        ASSERT_TRUE(endpoint.start());

        Core::EventLoop              loop;
        OAuthAuthorizationCodeClient client(loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize",
                                                                                .tokenUrl         = "http://127.0.0.1:" + std::to_string(endpoint.port()) + "/token",
                                                                                .clientId         = "client-1",
                                                                                .redirectUri      = "https://app.example/cb"});
        runControlTask(loop, stateMismatchScenario(loop, client, endpoint));
    }

    TEST(OAuthAuthorizationCodeClientTest, RefusesCallbackErrorsWithoutExchanging)
    {
        MiniTokenEndpoint endpoint;
        ASSERT_TRUE(endpoint.start());

        Core::EventLoop              loop;
        OAuthAuthorizationCodeClient client(loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize",
                                                                                .tokenUrl         = "http://127.0.0.1:" + std::to_string(endpoint.port()) + "/token",
                                                                                .clientId         = "client-1",
                                                                                .redirectUri      = "https://app.example/cb"});
        runControlTask(loop, callbackErrorScenario(loop, client, endpoint));
    }

    TEST(OAuthAuthorizationCodeClientTest, ExchangesCodeCarryingVerifierAndRedirect)
    {
        MiniTokenEndpoint endpoint;
        endpoint.setResponses({{"200 OK", R"({"access_token":"at-code","token_type":"Bearer","expires_in":1800,"refresh_token":"rt-code"})"},
                               {"400 Bad Request", R"({"error":"invalid_grant","error_description":"授权码已过期或已被使用"})"}});
        ASSERT_TRUE(endpoint.start());

        Core::EventLoop              loop;
        OAuthAuthorizationCodeClient client(loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize",
                                                                                .tokenUrl         = "http://127.0.0.1:" + std::to_string(endpoint.port()) + "/token",
                                                                                .clientId         = "client-1",
                                                                                .redirectUri      = "https://app.example/cb"});
        runControlTask(loop, exchangeScenario(loop, client, endpoint));
    }

    TEST(OAuthAuthorizationCodeClientTest, RejectsEmptyConfigurationAtConstruction)
    {
        Core::EventLoop loop;
        // 授权端点、令牌端点、客户端标识、回调地址四个取值各自缺一即抛
        EXPECT_THROW(static_cast<void>(OAuthAuthorizationCodeClient(
                             loop, OAuthAuthorizationCodeOptions{.tokenUrl = "http://127.0.0.1:1/token", .clientId = "client-1", .redirectUri = "https://app.example/cb"})),
                     Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(OAuthAuthorizationCodeClient(
                             loop,
                             OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize", .clientId = "client-1", .redirectUri = "https://app.example/cb"})),
                     Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(OAuthAuthorizationCodeClient(loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize",
                                                                                                        .tokenUrl         = "http://127.0.0.1:1/token",
                                                                                                        .redirectUri      = "https://app.example/cb"})),
                     Base::InvalidArgumentException);
        EXPECT_THROW(
                static_cast<void>(OAuthAuthorizationCodeClient(
                        loop, OAuthAuthorizationCodeOptions{.authorizationUrl = "https://auth.example/authorize", .tokenUrl = "http://127.0.0.1:1/token", .clientId = "client-1"})),
                Base::InvalidArgumentException);
    }

} // namespace AsynGyanis::Net
