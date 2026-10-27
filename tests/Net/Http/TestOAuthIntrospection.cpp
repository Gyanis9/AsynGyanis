// OAuthIntrospection 的用例：本地起一个「最小自省对端」（后台线程、记录完整请求、响应可换），
// 主线程一次 run() 跑完全部阶段。覆盖：
// - ReportsActiveTokenWithItsClaims（字段逐项 + 表单编码与 Basic 头的请求侧断言）
// - ReportsInactiveToken（active:false 是正常回答，不是失败）
// - FailsOnNon2xxAndOnMissingActiveField（协议不符要带原因，而不是猜成「不活跃」）
// - 字段类型不符（scope 是数字、exp 是字符串）：按失败返回并点名，不抛异常穿出 expected 契约

#include "Net/Http/Auth/OAuthIntrospection.h"

#include "Base/Exception/InvalidArgumentException.h"

#include "Base/Coding/Base64.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include "ExpectedAssertions.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Net/Http/Auth/OAuthError.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 最小自省对端：记录收到的完整请求（头 + 体），回一条可换的响应
         */
        class MiniIntrospectionServer
        {
        public:
            MiniIntrospectionServer() = default;

            MiniIntrospectionServer(const MiniIntrospectionServer &)            = delete;
            MiniIntrospectionServer &operator=(const MiniIntrospectionServer &) = delete;

            ~MiniIntrospectionServer()
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

            [[nodiscard]] std::string lastRequestBytes() const
            {
                const std::lock_guard guard(m_mutex);
                return m_lastRequest;
            }

            void setResponse(std::string statusLine, std::string body)
            {
                const std::lock_guard guard(m_mutex);
                m_statusLine = std::move(statusLine);
                m_body       = std::move(body);
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

                    // 读到请求头 + 按 Content-Length 读满正文：用例要断言「body 里的表单编码」
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

                    std::string statusLine;
                    std::string responseBody;
                    {
                        const std::lock_guard guard(m_mutex);
                        m_lastRequest = requestBytes;
                        statusLine    = m_statusLine;
                        responseBody  = m_body;
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

            std::thread                m_worker;
            std::atomic<std::uint16_t> m_port{0};
            std::atomic<bool>          m_isStopping{false};
            mutable std::mutex         m_mutex;
            std::string                m_lastRequest;
            std::string                m_statusLine{"200 OK"};
            std::string                m_body{"{}"};
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

        Core::Task<void> introspectionScenario(Core::EventLoop &loop, OAuthIntrospectionClient &client, MiniIntrospectionServer &server, std::string &lastFailure)
        {
            // 正例：active 令牌的字段逐项解析
            auto active = co_await client.introspect("tok+en/With=Specials");
            EXPECT_HAS_VALUE(active) << active.error().message;
            if (active.has_value())
            {
                EXPECT_TRUE(active->active);
                EXPECT_EQ(active->scope, "read write");
                EXPECT_EQ(active->clientId, "client-1");
                EXPECT_EQ(active->username, "alice");
                EXPECT_EQ(active->subject, "user-1");
                EXPECT_EQ(active->tokenType, "Bearer");
                EXPECT_EQ(active->expiresAt, 1791550800);
            }

            // 请求侧：表单编码与 Basic 头都要对
            const std::string recorded = server.lastRequestBytes();
            EXPECT_NE(recorded.find("POST /introspect HTTP/1.1"), std::string::npos) << recorded.substr(0, 200);
            EXPECT_NE(recorded.find("token=tok%2Ben%2FWith%3DSpecials"), std::string::npos) << "令牌原文没有按 form-urlencoded 编码";
            EXPECT_EQ(recorded.find("token=tok+en"), std::string::npos) << "加号没有被编码";
            EXPECT_NE(recorded.find("authorization: Basic " + Base::base64Encode("client-1:s3cret")), std::string::npos) << "Basic 凭据头不对";

            // 不活跃令牌：正常回答（200 + active:false），不是失败
            server.setResponse("200 OK", R"({"active":false})");
            auto inactive = co_await client.introspect("opaque-token");
            EXPECT_HAS_VALUE(inactive) << inactive.error().message;
            if (inactive.has_value())
            {
                EXPECT_FALSE(inactive->active);
            }

            // 非 2xx：协议不符，要带原因
            server.setResponse("503 Service Unavailable", R"({"error":"unavailable"})");
            auto unavailable = co_await client.introspect("opaque-token");
            EXPECT_FALSE(unavailable.has_value());
            if (!unavailable.has_value())
            {
                lastFailure = unavailable.error().message;
                EXPECT_NE(lastFailure.find("HTTP 503"), std::string::npos) << lastFailure;
            }

            // 2xx 但缺 active：RFC 7662 的必填字段缺席，同样算协议不符（不能猜成「不活跃」）
            server.setResponse("200 OK", R"({"scope":"read"})");
            auto missingActive = co_await client.introspect("opaque-token");
            EXPECT_FALSE(missingActive.has_value());
            if (!missingActive.has_value())
            {
                EXPECT_NE(missingActive.error().message.find("active"), std::string::npos) << missingActive.error().message;
                EXPECT_EQ(missingActive.error().kind, OAuthErrorKind::MalformedResponse) << "缺 active 这个必填布尔是 RFC 7662 §2.2 的形状违例";
            }

            // 字段类型不符：属协议不符、要按失败返回——绝不能抛异常穿出（expected 契约）。
            // 抛出来会顺着 co_await 链把本协程当场打死，后面的阶段一个也跑不到
            server.setResponse("200 OK", R"({"active":true,"scope":42})");
            bool        wrongScopeThrew    = false;
            bool        wrongScopeRejected = false;
            std::string wrongScopeReason;
            try
            {
                const auto wrongScope = co_await client.introspect("opaque-token");
                wrongScopeRejected    = !wrongScope.has_value();
                if (!wrongScope.has_value())
                {
                    wrongScopeReason = wrongScope.error().message;
                }
            } catch (const std::exception &)
            {
                wrongScopeThrew = true;
            }
            EXPECT_FALSE(wrongScopeThrew) << "字段类型不符要按失败返回，抛异常会穿出 expected 契约";
            if (!wrongScopeThrew)
            {
                EXPECT_TRUE(wrongScopeRejected) << "scope 是数字属协议不符，不能静默当成缺省";
                EXPECT_NE(wrongScopeReason.find("scope"), std::string::npos) << wrongScopeReason;
            }

            // exp 给了但不是整数：同样是协议不符（0 的语义是「端点未给出」，不让错类型借道）
            server.setResponse("200 OK", R"({"active":true,"exp":"1791550800"})");
            const auto wrongTypedExp = co_await client.introspect("opaque-token");
            EXPECT_FALSE(wrongTypedExp.has_value());
            if (!wrongTypedExp.has_value())
            {
                EXPECT_NE(wrongTypedExp.error().message.find("exp"), std::string::npos) << wrongTypedExp.error().message;
            }

            loop.stop();
        }
    } // namespace

    TEST(OAuthIntrospectionClientTest, ReportsActiveTokenWithItsClaims)
    {
        MiniIntrospectionServer server;
        server.setResponse("200 OK", R"({"active":true,"scope":"read write","client_id":"client-1","username":"alice","sub":"user-1","token_type":"Bearer","exp":1791550800})");
        ASSERT_TRUE(server.start());

        Core::EventLoop          loop;
        OAuthIntrospectionClient client(loop, OAuthIntrospectionOptions{.introspectionUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/introspect",
                                                                        .clientId         = "client-1",
                                                                        .clientSecret     = "s3cret"});

        std::string lastFailure;
        runControlTask(loop, introspectionScenario(loop, client, server, lastFailure));
        SUCCEED();
    }

    TEST(OAuthIntrospectionClientTest, RejectsPlaintextNonLoopbackEndpointAtConstruction)
    {
        Core::EventLoop           loop;
        OAuthIntrospectionOptions options;
        options.introspectionUrl = "http://example.com/introspect";
        EXPECT_THROW({ OAuthIntrospectionClient client(loop, options); }, Base::InvalidArgumentException);
    }

    TEST(OAuthIntrospectionClientTest, AcceptsHttpsAndLoopbackEndpointAtConstruction)
    {
        // 构造期只做取值校验：两个被接受的端点都不发请求
        Core::EventLoop           loop;
        OAuthIntrospectionOptions options;
        options.introspectionUrl = "https://example.com/introspect";
        EXPECT_NO_THROW({ OAuthIntrospectionClient client(loop, options); });

        options.introspectionUrl = "http://127.0.0.1:1/introspect";
        EXPECT_NO_THROW({ OAuthIntrospectionClient client(loop, options); });
    }

    TEST(OAuthIntrospectionClientTest, RejectsEmptyEndpointAtConstruction)
    {
        Core::EventLoop loop;
        try
        {
            OAuthIntrospectionClient client(loop, OAuthIntrospectionOptions{});
            ADD_FAILURE() << "没有自省端点时构造函数仍然成功";
        } catch (const Base::InvalidArgumentException &failure)
        {
            const std::string message = failure.what();
            EXPECT_NE(message.find("introspectionUrl"), std::string::npos) << "原因没有点名缺少的字段：" << message;
            EXPECT_NE(message.find("不能为空"), std::string::npos) << message;
        }
    }

} // namespace AsynGyanis::Net
