// OAuthDeviceCode 的用例：本地起一个「最小设备授权对端」（后台线程，两条路径
// /device_authorization 与 /token；令牌响应按脚本逐条发放并记录每次轮询的到达时刻），
// 主线程一次 run() 跑完全部阶段。每例都挂一枚看门狗：实现若把轮询写成无界循环，
// 看门狗会停掉循环并让用例带着原因变红，而不是挂到 ctest 超时。覆盖：
// - ExposesTheGrantAfterAnAuthorizationPendingPoll（字段逐项 + 请求侧编码 + 首次轮询也在等待之后）
// - SlowDownAddsFiveSecondsToThisAndEverySubsequentInterval（纯函数：+5 且此后叠加）
// - PollingAfterSlowDownWaitsTheIncreasedInterval（端到端实测：slow_down 后两轮间隔不小于 +5 秒后的新值）
// - AccessDeniedEndsPollingWithTheServerReason（被拒即止、带对端原因）
// - StopsAtTheDeviceCodeLifetimeWhenTheUserNeverFinishes（有效期到点即止，不把有界性外包给对端）
// - RejectsWrongShapedPeerPayloadsWithNamedReasons（缺必填字段、interval 非正、缺 access_token/token_type）
// - RejectsEmptyConfigurationAtConstruction（三个必填取值）

#include "Net/Http/Auth/OAuthDeviceCode.h"

#include "Base/Exception/InvalidArgumentException.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

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
         * @brief 最小设备授权对端：/device_authorization 回固定响应，/token 按脚本逐条发放（最后一条重复）
         * @details 记录每条请求的完整字节与到达时刻（steady_clock）：用例要断言「请求侧编码」与
         *          「两次轮询之间的实际间隔」。只在后台线程碰套接字。
         */
        class MiniDeviceServer
        {
        public:
            MiniDeviceServer() = default;

            MiniDeviceServer(const MiniDeviceServer &)            = delete;
            MiniDeviceServer &operator=(const MiniDeviceServer &) = delete;

            ~MiniDeviceServer()
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

            void setDeviceAuthorizationResponse(std::string statusLine, std::string body)
            {
                const std::lock_guard guard(m_mutex);
                m_deviceAuthorizationStatus = std::move(statusLine);
                m_deviceAuthorizationBody   = std::move(body);
            }

            /// 令牌端点的响应脚本：按序发放，最后一条之后重复最后一条
            void setTokenResponses(std::vector<std::pair<std::string, std::string>> responses)
            {
                const std::lock_guard guard(m_mutex);
                m_tokenResponses     = std::move(responses);
                m_tokenResponseIndex = 0;
            }

            [[nodiscard]] std::size_t tokenPollCount() const
            {
                const std::lock_guard guard(m_mutex);
                return m_tokenPolls.size();
            }

            [[nodiscard]] std::string tokenPollRequestBytes(std::size_t index) const
            {
                const std::lock_guard guard(m_mutex);
                return index < m_tokenPolls.size() ? m_tokenPolls[index].second : std::string{};
            }

            [[nodiscard]] std::chrono::steady_clock::time_point tokenPollArrival(std::size_t index) const
            {
                const std::lock_guard guard(m_mutex);
                return index < m_tokenPolls.size() ? m_tokenPolls[index].first : std::chrono::steady_clock::time_point{};
            }

            [[nodiscard]] std::string deviceRequestBytes() const
            {
                const std::lock_guard guard(m_mutex);
                return m_deviceRequest;
            }

            [[nodiscard]] std::chrono::steady_clock::time_point deviceRequestArrival() const
            {
                const std::lock_guard guard(m_mutex);
                return m_deviceArrival;
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

                    // 读到请求头 + 按 Content-Length 读满正文：用例要断言表单体的编码
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
                    const auto arrival = std::chrono::steady_clock::now();

                    std::string statusLine;
                    std::string responseBody;
                    {
                        const std::lock_guard guard(m_mutex);
                        if (requestBytes.find("/device_authorization ") != std::string::npos)
                        {
                            m_deviceRequest = requestBytes;
                            m_deviceArrival = arrival;
                            statusLine      = m_deviceAuthorizationStatus;
                            responseBody    = m_deviceAuthorizationBody;
                        } else
                        {
                            m_tokenPolls.emplace_back(arrival, requestBytes);
                            if (!m_tokenResponses.empty())
                            {
                                const std::size_t scripted = std::min(m_tokenResponseIndex, m_tokenResponses.size() - 1);
                                statusLine                 = m_tokenResponses[scripted].first;
                                responseBody               = m_tokenResponses[scripted].second;
                                if (m_tokenResponseIndex + 1 < m_tokenResponses.size())
                                {
                                    ++m_tokenResponseIndex;
                                }
                            } else
                            {
                                statusLine   = "500 Internal Server Error";
                                responseBody = "{}";
                            }
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

            std::thread                                                                m_worker;
            std::atomic<std::uint16_t>                                                 m_port{0};
            std::atomic<bool>                                                          m_isStopping{false};
            mutable std::mutex                                                         m_mutex;
            std::string                                                                m_deviceAuthorizationStatus{"200 OK"};
            std::string                                                                m_deviceAuthorizationBody;
            std::string                                                                m_deviceRequest;
            std::chrono::steady_clock::time_point                                      m_deviceArrival{};
            std::vector<std::pair<std::string, std::string>>                           m_tokenResponses;
            std::size_t                                                                m_tokenResponseIndex{0};
            std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> m_tokenPolls;
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

        /// 看门狗：实现若把轮询写成无界循环，到点停掉循环并置位，用例带着原因变红而不是挂到超时
        Core::Task<void> watchdogTask(Core::EventLoop &loop, std::chrono::milliseconds limit, bool &fired)
        {
            Core::Timer timer(loop);
            co_await timer.waitFor(limit);
            fired = true;
            loop.stop();
        }

        constexpr std::string_view kHappyDeviceCodeJson = R"({"device_code":"dev-123","user_code":"WDJB-MJHT","verification_uri":"https://example.com/device",)"
                                                          R"("verification_uri_complete":"https://example.com/device?user_code=WDJB-MJHT","expires_in":30,"interval":1})";

        /// 正例：设备授权 → 一次 authorization_pending → 拿到令牌；顺带断言两处请求编码与首次轮询的等待
        Core::Task<void> pendingThenGrantScenario(Core::EventLoop &loop, OAuthDeviceCodeClient &client, MiniDeviceServer &server)
        {
            const auto authorization = co_await client.requestDeviceAuthorization();
            EXPECT_HAS_VALUE(authorization) << authorization.error().message;
            if (authorization.has_value())
            {
                EXPECT_EQ(authorization->deviceCode, "dev-123");
                EXPECT_EQ(authorization->userCode, "WDJB-MJHT");
                EXPECT_EQ(authorization->verificationUri, "https://example.com/device");
                EXPECT_EQ(authorization->verificationUriComplete, "https://example.com/device?user_code=WDJB-MJHT");
                EXPECT_EQ(authorization->expiresIn.count(), 30);
                EXPECT_EQ(authorization->interval.count(), 1);
            }

            // 请求侧：设备授权请求的表单编码（空格编 %20、client_id 与 scope 都在）
            const std::string deviceRequest = server.deviceRequestBytes();
            EXPECT_NE(deviceRequest.find("POST /device_authorization HTTP/1.1"), std::string::npos) << deviceRequest.substr(0, 120);
            EXPECT_NE(deviceRequest.find("client_id=client-1"), std::string::npos) << deviceRequest;
            EXPECT_NE(deviceRequest.find("scope=read%20write"), std::string::npos) << deviceRequest;

            const auto token = co_await client.waitForToken(*authorization);
            EXPECT_HAS_VALUE(token) << token.error().message;
            if (token.has_value())
            {
                EXPECT_EQ(token->accessToken, "at-1");
                EXPECT_EQ(token->tokenType, "Bearer");
                EXPECT_EQ(token->expiresIn.count(), 3600);
                EXPECT_EQ(token->refreshToken, "rt-1");
                EXPECT_EQ(token->scope, "read");
            }

            // 令牌请求侧：grant_type 逐字（冒号编 %3A）、device_code 与 client_id 都在；公开客户端不带 Basic
            EXPECT_EQ(server.tokenPollCount(), 2U);
            const std::string firstPoll = server.tokenPollRequestBytes(0);
            EXPECT_NE(firstPoll.find("POST /token HTTP/1.1"), std::string::npos) << firstPoll.substr(0, 120);
            EXPECT_NE(firstPoll.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"), std::string::npos) << firstPoll;
            EXPECT_NE(firstPoll.find("device_code=dev-123"), std::string::npos) << firstPoll;
            EXPECT_NE(firstPoll.find("client_id=client-1"), std::string::npos) << firstPoll;
            EXPECT_EQ(firstPoll.find("uthorization"), std::string::npos) << "公开客户端不该带认证头";

            // 首次轮询也在等待之后：§3.5「每次新请求之前都要等满 interval」，第一次同样按它发
            const auto sinceDeviceRequest = std::chrono::duration_cast<std::chrono::milliseconds>(server.tokenPollArrival(0) - server.deviceRequestArrival());
            EXPECT_GE(sinceDeviceRequest.count(), 990) << "首次轮询没有等满 interval，实测 " << sinceDeviceRequest.count() << " ms";

            loop.stop();
        }

        /// slow_down 端到端：上一轮被要求降速后，下一轮的实际间隔要不小于 +5 秒后的新值
        Core::Task<void> slowDownScenario(Core::EventLoop &loop, OAuthDeviceCodeClient &client, MiniDeviceServer &server)
        {
            const auto authorization = co_await client.requestDeviceAuthorization();
            EXPECT_HAS_VALUE(authorization) << authorization.error().message;
            if (!authorization.has_value())
            {
                loop.stop();
                co_return;
            }

            const auto token = co_await client.waitForToken(*authorization);
            EXPECT_HAS_VALUE(token) << token.error().message;
            if (token.has_value())
            {
                EXPECT_EQ(token->accessToken, "at-slow");
            }

            EXPECT_EQ(server.tokenPollCount(), 2U);
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(server.tokenPollArrival(1) - server.tokenPollArrival(0));
            EXPECT_GE(gap.count(), 5900) << "slow_down 之后没有等满 +5 秒的新间隔，实测两轮相距 " << gap.count() << " ms";
            loop.stop();
        }

        /// 被拒即止：只发一次轮询，失败原因带对端的 error 与 error_description
        Core::Task<void> accessDeniedScenario(Core::EventLoop &loop, OAuthDeviceCodeClient &client, MiniDeviceServer &server, std::string &failure)
        {
            const auto authorization = co_await client.requestDeviceAuthorization();
            EXPECT_HAS_VALUE(authorization) << authorization.error().message;
            if (!authorization.has_value())
            {
                loop.stop();
                co_return;
            }

            const auto token = co_await client.waitForToken(*authorization);
            EXPECT_FALSE(token.has_value());
            if (!token.has_value())
            {
                failure = token.error().message;
                EXPECT_NE(failure.find("access_denied"), std::string::npos) << failure;
                EXPECT_NE(failure.find("用户点了拒绝"), std::string::npos) << failure;
            }
            EXPECT_EQ(server.tokenPollCount(), 1U) << "被拒之后不该再轮询";
            loop.stop();
        }

        /// 有效期上界：服务端一直回 pending，本端到点必须自己收口（不把有界性外包给对端）
        Core::Task<void> lifetimeScenario(Core::EventLoop &loop, OAuthDeviceCodeClient &client, MiniDeviceServer &server, std::string &failure)
        {
            const auto authorization = co_await client.requestDeviceAuthorization();
            EXPECT_HAS_VALUE(authorization) << authorization.error().message;
            if (!authorization.has_value())
            {
                loop.stop();
                co_return;
            }

            const auto token = co_await client.waitForToken(*authorization);
            EXPECT_FALSE(token.has_value());
            if (!token.has_value())
            {
                failure = token.error().message;
                EXPECT_NE(failure.find("有效期"), std::string::npos) << failure;
            }
            EXPECT_GE(server.tokenPollCount(), 1U) << "有效期内该发的轮询一枚都没发";
            loop.stop();
        }

        /// 协议不符点名：缺 device_code、interval 非正、缺 access_token、缺 token_type
        Core::Task<void> protocolScenario(Core::EventLoop &loop, OAuthDeviceCodeClient &client, MiniDeviceServer &server)
        {
            // 缺 device_code：必填字段缺席要指名道姓
            server.setDeviceAuthorizationResponse("200 OK", R"({"user_code":"WDJB-MJHT","verification_uri":"https://example.com/device","expires_in":30})");
            const auto missingDeviceCode = co_await client.requestDeviceAuthorization();
            EXPECT_FALSE(missingDeviceCode.has_value());
            if (!missingDeviceCode.has_value())
            {
                EXPECT_NE(missingDeviceCode.error().message.find("device_code"), std::string::npos) << missingDeviceCode.error().message;
                EXPECT_EQ(missingDeviceCode.error().kind, OAuthErrorKind::MalformedResponse) << "缺 device_code 是应答形状不对";
            }

            // interval 给了但不是正数：只有「没给」才回退默认 5，给了荒谬值按协议不符拒
            server.setDeviceAuthorizationResponse(
                    "200 OK", R"({"device_code":"dev-123","user_code":"WDJB-MJHT","verification_uri":"https://example.com/device","expires_in":30,"interval":0})");
            const auto badInterval = co_await client.requestDeviceAuthorization();
            EXPECT_FALSE(badInterval.has_value());
            if (!badInterval.has_value())
            {
                EXPECT_NE(badInterval.error().message.find("interval"), std::string::npos) << badInterval.error().message;
            }

            // 令牌响应缺 access_token / token_type：RFC 6749 §5.1 的必填字段，各自点名
            server.setDeviceAuthorizationResponse("200 OK", std::string{kHappyDeviceCodeJson});
            server.setTokenResponses({{"200 OK", R"({"token_type":"Bearer"})"}, {"200 OK", R"({"access_token":"at-x"})"}});

            const auto authorization = co_await client.requestDeviceAuthorization();
            EXPECT_HAS_VALUE(authorization) << authorization.error().message;
            if (!authorization.has_value())
            {
                loop.stop();
                co_return;
            }

            const auto missingAccessToken = co_await client.waitForToken(*authorization);
            EXPECT_FALSE(missingAccessToken.has_value());
            if (!missingAccessToken.has_value())
            {
                EXPECT_NE(missingAccessToken.error().message.find("access_token"), std::string::npos) << missingAccessToken.error().message;
                EXPECT_EQ(missingAccessToken.error().kind, OAuthErrorKind::MalformedResponse) << "缺 access_token 同属应答形状";
            }

            const auto missingTokenType = co_await client.waitForToken(*authorization);
            EXPECT_FALSE(missingTokenType.has_value());
            if (!missingTokenType.has_value())
            {
                EXPECT_NE(missingTokenType.error().message.find("token_type"), std::string::npos) << missingTokenType.error().message;
            }
            loop.stop();
        }
    } // namespace

    TEST(OAuthDeviceCodeClientTest, ExposesTheGrantAfterAnAuthorizationPendingPoll)
    {
        MiniDeviceServer server;
        server.setDeviceAuthorizationResponse("200 OK", std::string{kHappyDeviceCodeJson});
        server.setTokenResponses({{"400 Bad Request", R"({"error":"authorization_pending"})"},
                                  {"200 OK", R"({"access_token":"at-1","token_type":"Bearer","expires_in":3600,"refresh_token":"rt-1","scope":"read"})"}});
        ASSERT_TRUE(server.start());

        Core::EventLoop       loop;
        OAuthDeviceCodeClient client(loop, OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/device_authorization",
                                                                  .tokenUrl               = "http://127.0.0.1:" + std::to_string(server.port()) + "/token",
                                                                  .clientId               = "client-1",
                                                                  .scope                  = "read write"});
        bool                  watchdogFired = false;
        Core::Task<void>      watchdog      = watchdogTask(loop, std::chrono::seconds{8}, watchdogFired);
        loop.scheduler().schedule(watchdog.handle());

        runControlTask(loop, pendingThenGrantScenario(loop, client, server));
        EXPECT_FALSE(watchdogFired) << "用例被看门狗掐停——正例的两枚轮询没有在有界时间内收口";
    }

    TEST(OAuthDeviceCodeClientTest, SlowDownAddsFiveSecondsToThisAndEverySubsequentInterval)
    {
        // RFC 8628 §3.5：slow_down 要求间隔加 5 秒，「本请求与之后所有请求」都用新值；其余错误不动间隔
        EXPECT_EQ(OAuthDeviceCodeClient::pollingIntervalAfter(std::chrono::seconds{1}, "slow_down").count(), 6);
        EXPECT_EQ(OAuthDeviceCodeClient::pollingIntervalAfter(std::chrono::seconds{6}, "slow_down").count(), 11);
        EXPECT_EQ(OAuthDeviceCodeClient::pollingIntervalAfter(std::chrono::seconds{6}, "authorization_pending").count(), 6);
        EXPECT_EQ(OAuthDeviceCodeClient::pollingIntervalAfter(std::chrono::seconds{1}, "").count(), 1);
    }

    TEST(OAuthDeviceCodeClientTest, PollingAfterSlowDownWaitsTheIncreasedInterval)
    {
        MiniDeviceServer server;
        server.setDeviceAuthorizationResponse("200 OK", std::string{kHappyDeviceCodeJson});
        server.setTokenResponses({{"400 Bad Request", R"({"error":"slow_down"})"}, {"200 OK", R"({"access_token":"at-slow","token_type":"Bearer"})"}});
        ASSERT_TRUE(server.start());

        Core::EventLoop       loop;
        OAuthDeviceCodeClient client(loop, OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/device_authorization",
                                                                  .tokenUrl               = "http://127.0.0.1:" + std::to_string(server.port()) + "/token",
                                                                  .clientId               = "client-1"});
        bool                  watchdogFired = false;
        Core::Task<void>      watchdog      = watchdogTask(loop, std::chrono::seconds{12}, watchdogFired);
        loop.scheduler().schedule(watchdog.handle());

        runControlTask(loop, slowDownScenario(loop, client, server));
        EXPECT_FALSE(watchdogFired) << "用例被看门狗掐停——slow_down 后的轮询没有在有界时间内收口";
    }

    TEST(OAuthDeviceCodeClientTest, AccessDeniedEndsPollingWithTheServerReason)
    {
        MiniDeviceServer server;
        server.setDeviceAuthorizationResponse("200 OK", std::string{kHappyDeviceCodeJson});
        server.setTokenResponses({{"400 Bad Request", R"({"error":"access_denied","error_description":"用户点了拒绝"})"}});
        ASSERT_TRUE(server.start());

        Core::EventLoop       loop;
        OAuthDeviceCodeClient client(loop, OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/device_authorization",
                                                                  .tokenUrl               = "http://127.0.0.1:" + std::to_string(server.port()) + "/token",
                                                                  .clientId               = "client-1"});
        bool                  watchdogFired = false;
        Core::Task<void>      watchdog      = watchdogTask(loop, std::chrono::seconds{6}, watchdogFired);
        loop.scheduler().schedule(watchdog.handle());

        std::string failure;
        runControlTask(loop, accessDeniedScenario(loop, client, server, failure));
        EXPECT_FALSE(watchdogFired) << "用例被看门狗掐停——access_denied 没有被当成终止条件";
    }

    TEST(OAuthDeviceCodeClientTest, StopsAtTheDeviceCodeLifetimeWhenTheUserNeverFinishes)
    {
        MiniDeviceServer server;
        // 有效期 2 秒、间隔 1 秒，服务端一直回 pending：到点必须自己停（第 N 枚轮询发不发得出去看时序，
        // 但绝不能越过有效期继续轮询、更不能无界轮询）
        server.setDeviceAuthorizationResponse("200 OK",
                                              R"({"device_code":"dev-123","user_code":"WDJB-MJHT","verification_uri":"https://example.com/device","expires_in":2,"interval":1})");
        server.setTokenResponses({{"400 Bad Request", R"({"error":"authorization_pending"})"}});
        ASSERT_TRUE(server.start());

        Core::EventLoop       loop;
        OAuthDeviceCodeClient client(loop, OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/device_authorization",
                                                                  .tokenUrl               = "http://127.0.0.1:" + std::to_string(server.port()) + "/token",
                                                                  .clientId               = "client-1"});
        bool                  watchdogFired = false;
        Core::Task<void>      watchdog      = watchdogTask(loop, std::chrono::seconds{8}, watchdogFired);
        loop.scheduler().schedule(watchdog.handle());

        std::string failure;
        const auto  begin = std::chrono::steady_clock::now();
        runControlTask(loop, lifetimeScenario(loop, client, server, failure));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
        EXPECT_FALSE(watchdogFired) << "用例被看门狗掐停——有效期没有被当成本端的轮询上界";
        EXPECT_LT(elapsed.count(), 5000) << "有效期 2 秒的流程跑了 " << elapsed.count() << " ms，" << failure;
    }

    TEST(OAuthDeviceCodeClientTest, RejectsWrongShapedPeerPayloadsWithNamedReasons)
    {
        MiniDeviceServer server;
        ASSERT_TRUE(server.start());

        Core::EventLoop       loop;
        OAuthDeviceCodeClient client(loop, OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/device_authorization",
                                                                  .tokenUrl               = "http://127.0.0.1:" + std::to_string(server.port()) + "/token",
                                                                  .clientId               = "client-1"});
        bool                  watchdogFired = false;
        Core::Task<void>      watchdog      = watchdogTask(loop, std::chrono::seconds{8}, watchdogFired);
        loop.scheduler().schedule(watchdog.handle());

        runControlTask(loop, protocolScenario(loop, client, server));
        EXPECT_FALSE(watchdogFired) << "用例被看门狗掐停——协议不符没有被当成终止条件";
    }

    TEST(OAuthDeviceCodeClientTest, RejectsEmptyConfigurationAtConstruction)
    {
        Core::EventLoop loop;
        // 三个必填取值各自缺一即抛：没有端点、没有客户端标识就无从发起设备授权，文案点名缺的那一项
        const auto expectMissingField = [&loop](OAuthDeviceCodeOptions options, const std::string_view field)
        {
            try
            {
                OAuthDeviceCodeClient client(loop, std::move(options));
                ADD_FAILURE() << "缺少 " << field << " 时构造函数仍然成功";
            } catch (const Base::InvalidArgumentException &failure)
            {
                const std::string message = failure.what();
                EXPECT_NE(message.find(field), std::string::npos) << "原因没有点名缺少的字段：" << message;
                EXPECT_NE(message.find("不能为空"), std::string::npos) << message;
            }
        };
        expectMissingField(OAuthDeviceCodeOptions{.tokenUrl = "http://127.0.0.1:1/token", .clientId = "client-1"}, "deviceAuthorizationUrl");
        expectMissingField(OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:1/device_authorization", .clientId = "client-1"}, "tokenUrl");
        expectMissingField(OAuthDeviceCodeOptions{.deviceAuthorizationUrl = "http://127.0.0.1:1/device_authorization", .tokenUrl = "http://127.0.0.1:1/token"}, "clientId");
    }

} // namespace AsynGyanis::Net
