// 消费者旅程：仓库外的工程只经 find_package 使用公开 API，把「装出来的包能干什么」串成一条链。
// 每一步打印一行机读结果；环境造不出来的步骤显式 SKIP 并写原因，不静默算通过。

#include "AsynGyanisFeatures.h"

#include "Base/Exception/Exception.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPeerIdentity.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Config/HttpServerLimits.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Server/HttpServer.h"
#include "Net/Http/Server/HttpsServer.h"
#include "Net/WebSocket/WebSocketClient.h"
#include "Net/WebSocket/WebSocketFrame.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /// 旅程的固定口令：认证步用它证明「带对了才放行」
    constexpr std::string_view kJourneyToken = "journey-token";

    /// WebSocket 回显文案：发什么就该原样回来
    constexpr std::string_view kJourneyMessage = "journey-upgrade";

    /// 单步网络动作的时限：本机回环，超过这个时长就是没通
    constexpr std::chrono::milliseconds kStepTimeout{5000};

    /// 就绪与收尾的有界轮询间隔
    constexpr std::chrono::milliseconds kPollInterval{5};

    /**
     * @brief 等到条件成立或时限到点
     * @param condition 判定体
     * @param timeout 等待上限
     * @return bool 时限到点时条件是否已成立
     */
    [[nodiscard]] bool waitUntil(const std::function<bool()> &condition, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return true;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        return condition();
    }

    /**
     * @brief 一步的结论
     */
    struct StepOutcome
    {
        bool        isOk{false}; ///< 这一步是否通过
        std::string detail;      ///< 机读明细：关键读数都在这里
    };

    /**
     * @brief 机读报告：每步一行，最后一行给总数
     */
    class JourneyReport
    {
    public:
        /**
         * @brief 记一步通过
         * @param name 步骤名
         * @param detail 关键读数
         */
        void pass(const std::string_view name, std::string detail)
        {
            ++m_passedCount;
            record(name, "PASS", std::move(detail));
        }

        /**
         * @brief 记一步失败
         * @param name 步骤名
         * @param detail 失败原因与读数
         */
        void fail(const std::string_view name, std::string detail)
        {
            ++m_failedCount;
            record(name, "FAIL", std::move(detail));
        }

        /**
         * @brief 记一步显式跳过
         * @param name 步骤名
         * @param reason 为什么这一步在本环境造不出来
         */
        void skip(const std::string_view name, std::string reason)
        {
            ++m_skippedCount;
            record(name, "SKIP", std::move(reason));
        }

        /**
         * @brief 打总数并按结果给出退出码
         * @return int 0 表示没有失败步；显式跳过的步骤不改退出码
         */
        [[nodiscard]] int summarize() const
        {
            const bool isOk = m_failedCount == 0;
            std::printf("JOURNEY_RESULT=%s PASS=%zu FAIL=%zu SKIP=%zu\n", isOk ? "PASS" : "FAIL", m_passedCount, m_failedCount, m_skippedCount);
            std::fflush(stdout);
            return isOk ? 0 : 1;
        }

    private:
        /**
         * @brief 写一行机读结果
         * @details 明细里的换行会撕开机读行，落笔前折成空格
         */
        void record(const std::string_view name, const char *result, std::string detail)
        {
            for (char &character: detail)
            {
                if (character == '\n' || character == '\r' || character == '\t')
                {
                    character = ' ';
                }
            }
            std::printf("JOURNEY_STEP=%s RESULT=%s DETAIL=%s\n", std::string(name).c_str(), result, detail.c_str());
            std::fflush(stdout);
        }

        std::size_t m_passedCount{0};  ///< 通过步数
        std::size_t m_failedCount{0};  ///< 失败步数
        std::size_t m_skippedCount{0}; ///< 跳过步数
    };

    /**
     * @brief 旅程要用的证书材料
     */
    struct JourneyMaterial
    {
        std::string serverCertificate;  ///< 服务端证书
        std::string serverKey;          ///< 服务端私钥
        std::string clientCertificate;  ///< 客户端身份证书（mTLS）
        std::string clientKey;          ///< 客户端身份私钥
        std::string trustedCertificate; ///< 客户端信任的服务端根
    };

    /**
     * @brief 服务端处理器与客户端共享的状态
     */
    struct JourneyState
    {
        std::atomic<bool>                 slowEntered{false};        ///< /slow 处理器是否已经跑起来
        std::atomic<bool>                 slowHeadersEntered{false}; ///< /slow-headers 处理器是否已经跑起来
        std::shared_ptr<Net::TokenBucket> rateLimitBucket;           ///< /limited 的令牌桶
    };

    /**
     * @brief 计算证书 DER 的 SHA-256（小写十六进制）
     * @details 用 OpenSSL 现算而不是从服务端抄：身份判据要有独立来源
     * @param certificateFile 证书文件（PEM）
     * @return std::string 64 位小写十六进制；读不出来时为空
     */
    std::string certificateSha256(const std::string &certificateFile)
    {
        BIO *source = BIO_new_file(certificateFile.c_str(), "rb");
        if (source == nullptr)
        {
            return {};
        }
        X509 *certificate = PEM_read_bio_X509(source, nullptr, nullptr, nullptr);
        BIO_free(source);
        if (certificate == nullptr)
        {
            return {};
        }
        unsigned char *der       = nullptr;
        const int      derLength = i2d_X509(certificate, &der);
        X509_free(certificate);
        if (derLength <= 0)
        {
            return {};
        }
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int  digestLength = 0;
        const bool    isHashed     = EVP_Digest(der, static_cast<std::size_t>(derLength), digest, &digestLength, EVP_sha256(), nullptr) == 1;
        OPENSSL_free(der);
        if (!isHashed)
        {
            return {};
        }
        constexpr char kNibbleCharacters[] = "0123456789abcdef";
        std::string    hexText;
        hexText.reserve(static_cast<std::size_t>(digestLength) * 2U);
        for (unsigned int index = 0; index < digestLength; ++index)
        {
            hexText.push_back(kNibbleCharacters[digest[index] >> 4U]);
            hexText.push_back(kNibbleCharacters[digest[index] & 0x0FU]);
        }
        return hexText;
    }

    /**
     * @brief 从一段简单 JSON 里取字符串字段的值
     * @param body JSON 正文
     * @param fieldName 字段名
     * @return std::string 字段值；没有该字段时为空
     */
    std::string jsonStringField(const std::string &body, const std::string &fieldName)
    {
        const std::string prefix = "\"" + fieldName + "\":\"";
        const std::size_t begin  = body.find(prefix);
        if (begin == std::string::npos)
        {
            return {};
        }
        const std::size_t valueBegin = begin + prefix.size();
        const std::size_t valueEnd   = body.find('"', valueBegin);
        return valueEnd == std::string::npos ? std::string{} : body.substr(valueBegin, valueEnd - valueBegin);
    }

    /**
     * @brief 一次出站 HTTP 调用的结论
     */
    struct HttpCall
    {
        std::unique_ptr<Net::HttpClientResponse> response;   ///< 响应；失败为空
        std::chrono::milliseconds                elapsed{0}; ///< 墙钟时长
        std::string                              failure;    ///< 本端在拿到响应前失败的原因
    };

    /**
     * @brief 用实例客户端发一次 GET
     * @details 每次调用起一条独立循环：本旅程里各步互不共享连接，失败也不会互相污染
     * @param material 证书材料
     * @param url 目标地址
     * @param bearerToken 非空时带 `Authorization: Bearer`
     * @param withIdentity 是否出示客户端证书
     * @param timeout 整体时限
     * @return HttpCall 响应、时长与本端失败原因
     */
    HttpCall callHttp(const JourneyMaterial &material, const std::string &url, const std::string &bearerToken, const bool withIdentity, const std::chrono::milliseconds timeout)
    {
        HttpCall result;
        try
        {
            Core::EventLoop loop;
            Core::TlsPolicy policy;
            policy.certificateAuthorityFile = material.trustedCertificate;
            // 回环旅程必须直连：CI runner 常带 https_proxy，跟着环境走会把本机请求交给代理
            Net::HttpProxyPolicy proxyPolicy;
            proxyPolicy.consultEnvironment = false;
            Net::HttpClient client(loop, Net::HttpOutboundConnectionPool::Config{}, policy, proxyPolicy);
            if (withIdentity && !client.setClientCertificate(material.clientCertificate, material.clientKey))
            {
                result.failure = "客户端身份装不上：" + material.clientCertificate;
                return result;
            }
            Net::HttpClientRequest request;
            if (!bearerToken.empty())
            {
                request.headers.emplace_back("Authorization", "Bearer " + bearerToken);
            }
            const auto startedAt   = std::chrono::steady_clock::now();
            auto       requestBody = [&]() -> Core::Task<>
            {
                try
                {
                    result.response = co_await client.send(url, request, timeout);
                } catch (const std::exception &failure)
                {
                    result.failure = failure.what();
                }
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
                loop.stop();
                co_return;
            };
            Core::Task<> requestTask = requestBody();
            if (!requestTask.isReady())
            {
                loop.scheduler().schedule(requestTask.handle());
            }
            loop.run();
        } catch (const std::exception &failure)
        {
            result.failure = failure.what();
        }
        return result;
    }

    /**
     * @brief 一次 WebSocket 升级与回显的结论
     */
    struct WebSocketCall
    {
        bool                           isConnected{false};                                               ///< 升级是否谈成
        bool                           isEchoed{false};                                                  ///< 文本消息是否原样回来
        std::string                    failure;                                                          ///< 失败原因
        Net::WebSocketUpgradeErrorKind upgradeKind{Net::WebSocketUpgradeErrorKind::LocalRequestInvalid}; ///< 升级失败类别
    };

    /**
     * @brief 走 wss 连一次、发一条文本、等回显、按协议关闭
     * @param material 证书材料（信任库 + 客户端身份）
     * @param port 目标端口
     * @param requestTarget origin-form 请求目标（口令放在查询串里）
     * @return WebSocketCall 升级、回显与失败类别
     */
    WebSocketCall callWebSocket(const JourneyMaterial &material, const std::uint16_t port, const std::string &requestTarget)
    {
        WebSocketCall result;
        try
        {
            Core::EventLoop loop;
            Core::TlsPolicy policy;
            policy.certificateAuthorityFile = material.trustedCertificate;
            Core::TlsContext tlsContext(policy, Core::TlsContext::Role::Client);
            if (!tlsContext.loadCertificate(material.clientCertificate, material.clientKey))
            {
                result.failure = "客户端 TLS 上下文装不上身份证书：" + material.clientCertificate;
                return result;
            }
            tlsContext.enableClientPeerVerification();

            Net::WebSocketClient::Configuration configuration;
            configuration.hostName         = "127.0.0.1";
            configuration.port             = port;
            configuration.requestTarget    = requestTarget;
            configuration.clientTls        = &tlsContext;
            configuration.handshakeTimeout = kStepTimeout;
            auto sessionBody               = [&]() -> Core::Task<>
            {
                try
                {
                    auto connected = co_await Net::WebSocketClient::connect(loop, configuration);
                    if (!connected.has_value())
                    {
                        result.failure     = connected.error().message;
                        result.upgradeKind = connected.error().kind;
                        loop.stop();
                        co_return;
                    }
                    std::unique_ptr<Net::WebSocketClient> session = std::move(*connected);
                    result.isConnected                            = true;
                    if (!co_await session->sendText(kJourneyMessage))
                    {
                        result.failure = "升级成功但文本帧没能写出去";
                        loop.stop();
                        co_return;
                    }
                    auto received = co_await session->receiveFor(kStepTimeout);
                    if (!received.has_value())
                    {
                        result.failure = received.error().message;
                        loop.stop();
                        co_return;
                    }
                    result.isEchoed = received->opCode == Net::WebSocketOpCode::Text && received->payload == kJourneyMessage;
                    if (!result.isEchoed)
                    {
                        result.failure = "回显与发送不一致：操作码 " + std::to_string(static_cast<int>(received->opCode));
                    }
                    static_cast<void>(co_await session->close(Net::kWebSocketNormalClosureCode));
                } catch (const std::exception &failure)
                {
                    result.failure = failure.what();
                }
                loop.stop();
                co_return;
            };
            Core::Task<> sessionTask = sessionBody();
            if (!sessionTask.isReady())
            {
                loop.scheduler().schedule(sessionTask.handle());
            }
            loop.run();
        } catch (const std::exception &failure)
        {
            result.failure = failure.what();
        }
        return result;
    }

    /**
     * @brief 装好旅程的业务路由与两道中间件
     * @param server 目标服务器
     * @param loop 服务器所属循环（/slow 的定时器挂在这里）
     * @param state 跨处理器共享的状态
     */
    void installJourneyRoutes(Net::HttpsServer &server, Core::EventLoop &loop, const std::shared_ptr<JourneyState> &state)
    {
        Net::Router &router = server.router();

        // 认证：HTTP 用 Authorization，WebSocket 升级用查询串（出站客户端没有附加头部的入口）
        router.addMiddleware(
                [](Net::HttpRequest &request, Net::HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    bool isAuthorized = false;
                    if (request.path().starts_with("/ws"))
                    {
                        const auto parameters = request.queryParams();
                        const auto found      = parameters.find("token");
                        isAuthorized          = found != parameters.end() && found->second == kJourneyToken;
                    } else
                    {
                        const auto authorization = request.firstHeaderValue("authorization");
                        isAuthorized             = authorization.has_value() && *authorization == "Bearer " + std::string(kJourneyToken);
                    }
                    if (!isAuthorized)
                    {
                        response.setStatus(401);
                        static_cast<void>(response.setHeader("content-type", "application/json"));
                        response.setBody(R"({"error":"unauthorized"})");
                        co_return;
                    }
                    co_await next();
                });

        // 限流只作用于 /limited：其余步骤不该被这枚桶的余量影响
        router.addMiddleware(
                [state](Net::HttpRequest &request, Net::HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    if (!request.path().starts_with("/limited"))
                    {
                        co_await next();
                        co_return;
                    }
                    if (!state->rateLimitBucket->tryAcquire())
                    {
                        const auto waitMilliseconds  = state->rateLimitBucket->timeUntilTokenAvailable().count();
                        const auto retryAfterSeconds = std::max<std::int64_t>(1, (waitMilliseconds + 999) / 1000);
                        response.setStatus(429);
                        static_cast<void>(response.setHeader("content-type", "text/plain"));
                        static_cast<void>(response.setHeader("retry-after", std::to_string(retryAfterSeconds)));
                        response.setBody("Too Many Requests");
                        co_return;
                    }
                    co_await next();
                });

        router.get("/hello",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       static_cast<void>(response.setHeader("content-type", "text/plain"));
                       response.setBody("hello from journey");
                       co_return;
                   });

        router.get("/limited",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("limited-ok");
                       co_return;
                   });

        // 身份步：把服务端验过的客户端证书指纹原样交给业务
        router.get("/whoami",
                   [](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<>
                   {
                       const Core::TlsPeerIdentityPtr &identity    = request.tlsPeerIdentity();
                       const std::string               fingerprint = identity != nullptr ? identity->sha256FingerprintHex() : std::string{};
                       const bool                      isVerified  = identity != nullptr && identity->isVerified();
                       response.setStatus(200);
                       static_cast<void>(response.setHeader("content-type", "application/json"));
                       response.setBody(std::string{"{\"fingerprint\":\""} + fingerprint + "\",\"verified\":" + (isVerified ? "true" : "false") + "}");
                       co_return;
                   });

        // 错误展示步：处理器抛出，框架折成 500，内部原因不进响应体
        router.get("/boom",
                   [](Net::HttpRequest &, Net::HttpResponse &) -> Core::Task<>
                   {
                       throw std::runtime_error("journey deliberate handler failure");
                       co_return;
                   });

        // 首包超时步：处理器按住响应头，让客户端在拿到第一段响应字节之前到点
        router.get("/slow-headers",
                   [&loop, state](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                   {
                       state->slowHeadersEntered.store(true, std::memory_order_release);
                       Core::Timer holdTimer(loop);
                       co_await holdTimer.waitFor(std::chrono::milliseconds{1500});
                       response.setStatus(200);
                       response.setBody("late-headers");
                       co_return;
                   });

        // 优雅停机步：在途请求要能在 drain 期间做完
        router.get("/slow",
                   [&loop, state](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                   {
                       state->slowEntered.store(true, std::memory_order_release);
                       Core::Timer holdTimer(loop);
                       co_await holdTimer.waitFor(std::chrono::milliseconds{800});
                       response.setStatus(200);
                       response.setBody("slow-finished");
                       co_return;
                   });

        // 升级步：把收到的数据消息按原类型回显
        router.get("/ws",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [](Net::WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       const bool isSent = message->opCode == Net::WebSocketOpCode::Binary ? co_await peer.sendBinary(message->payload)
                                                                                                           : co_await peer.sendText(message->payload);
                                       if (!isSent)
                                       {
                                           co_return;
                                       }
                                   }
                                   co_return;
                               });
                       co_return;
                   });
    }

    /**
     * @brief 一台跑在自有循环线程上的 HTTPS 服务器
     */
    class JourneyServer
    {
    public:
        /**
         * @brief 造服务器、装路由、起监听
         * @param requestedPort 监听端口；0 表示由内核选一个
         * @param material 服务端证书与校验客户端证书的 CA
         * @param state 跨处理器共享的状态
         */
        JourneyServer(const std::uint16_t requestedPort, const JourneyMaterial &material, const std::shared_ptr<JourneyState> &state)
        {
            try
            {
                const std::optional<Core::InetAddress> address = Core::InetAddress::resolve("127.0.0.1", requestedPort);
                if (!address.has_value())
                {
                    m_failure = "解析回环地址失败，端口 " + std::to_string(requestedPort);
                    return;
                }
                m_server = std::make_unique<Net::HttpsServer>(m_loop, *address, material.serverCertificate, material.serverKey);
                if (!m_server->loadClientCertificateAuthority(material.clientCertificate))
                {
                    m_failure = "服务端装不上校验客户端证书的 CA：" + material.clientCertificate;
                    m_server.reset();
                    return;
                }
                m_server->setClientCertificateRequired(true);

                Net::HttpServerLimits limits;
                limits.idleTimeout  = std::chrono::seconds{10};
                limits.readTimeout  = std::chrono::seconds{10};
                limits.writeTimeout = std::chrono::seconds{10};
                m_server->setLimits(limits);
                installJourneyRoutes(*m_server, m_loop, state);

                m_startTask.emplace(driveStartTask(*m_server, m_startFailure, m_startThrew));
                m_loop.scheduler().schedule(m_startTask->handle());
            } catch (const std::exception &failure)
            {
                m_server.reset();
                m_failure = failure.what();
                return;
            }
            m_thread = std::thread([this]() { m_loop.run(); });
        }

        JourneyServer(const JourneyServer &)            = delete;
        JourneyServer &operator=(const JourneyServer &) = delete;

        ~JourneyServer()
        {
            // 顺序：先让循环线程停手并退出，再收尾服务器（close 会销毁挂起的协程帧）
            m_loop.stop();
            if (m_thread.joinable())
            {
                m_thread.join();
            }
            if (m_server != nullptr)
            {
                m_server->close();
            }
        }

        /// 装配阶段是否成功（失败原因见 failureReason）
        [[nodiscard]] bool isBuilt() const noexcept
        {
            return m_server != nullptr;
        }

        /// 装配阶段的失败原因
        [[nodiscard]] const std::string &failureReason() const noexcept
        {
            return m_failure;
        }

        /// start() 是否以异常收场
        [[nodiscard]] bool startThrew() const noexcept
        {
            return m_startThrew.load(std::memory_order_acquire);
        }

        /// start() 的异常文本
        [[nodiscard]] const std::string &startFailureReason() const noexcept
        {
            return m_startFailure;
        }

        /// 等到服务器真的进入接受循环
        [[nodiscard]] bool awaitRunning() const
        {
            return waitUntil([this]() { return m_server != nullptr && m_server->isRunning(); }, kStepTimeout);
        }

        /// 实际监听端口（端口 0 时由内核选出）
        [[nodiscard]] std::uint16_t listeningPort() const noexcept
        {
            return m_server != nullptr ? m_server->listeningPort() : 0;
        }

        /// 服务器是否还在接受连接
        [[nodiscard]] bool isRunning() const noexcept
        {
            return m_server != nullptr && m_server->isRunning();
        }

        /**
         * @brief 让服务器停止接受新连接并在时限内排空在途请求
         * @param timeout 交给 drain 的等待上限
         * @return bool drain 是否在期限内返回
         */
        bool requestGracefulDrain(const std::chrono::milliseconds timeout)
        {
            if (m_server == nullptr)
            {
                return false;
            }
            m_drainDone.store(false, std::memory_order_release);
            Core::Task<> drainTask = drainOnLoopTask(*m_server, timeout, m_drainDone);
            m_loop.scheduler().scheduleRemote(drainTask.handle());
            m_drainTasks.push_back(std::move(drainTask));
            return waitUntil([this]() { return m_drainDone.load(std::memory_order_acquire); }, timeout * 3 + kStepTimeout);
        }

    private:
        /**
         * @brief 把 start() 包一层，异常留在本对象上而不是把循环线程带崩
         */
        static Core::Task<> driveStartTask(Net::HttpsServer &server, std::string &startFailure, std::atomic<bool> &startThrew)
        {
            try
            {
                co_await server.start();
            } catch (const Base::Exception &failure)
            {
                startFailure = failure.what();
                startThrew.store(true, std::memory_order_release);
            } catch (const std::exception &failure)
            {
                startFailure = failure.what();
                startThrew.store(true, std::memory_order_release);
            }
            co_return;
        }

        /**
         * @brief 投到服务器循环上的 stop + drain
         */
        static Core::Task<> drainOnLoopTask(Net::HttpsServer &server, const std::chrono::milliseconds timeout, std::atomic<bool> &drainDone)
        {
            server.stop();
            co_await server.drain(timeout);
            drainDone.store(true, std::memory_order_release);
            co_return;
        }

        Core::EventLoop                   m_loop;              ///< 承载服务器的循环
        std::unique_ptr<Net::HttpsServer> m_server;            ///< 服务器本体
        std::optional<Core::Task<>>       m_startTask;         ///< start() 的协程帧
        std::vector<Core::Task<>>         m_drainTasks;        ///< drain 的协程帧
        std::thread                       m_thread;            ///< 跑循环的线程
        std::atomic<bool>                 m_startThrew{false}; ///< start() 是否抛过
        std::atomic<bool>                 m_drainDone{false};  ///< drain 是否已返回
        std::string                       m_startFailure;      ///< start() 的异常文本
        std::string                       m_failure;           ///< 装配阶段的失败原因
    };

    /**
     * @brief 建连步：一次带身份与口令的 HTTPS GET
     */
    StepOutcome stepConnect(const JourneyMaterial &material, const std::string &baseUrl)
    {
        const HttpCall call = callHttp(material, baseUrl + "/hello", std::string(kJourneyToken), true, kStepTimeout);
        if (call.failure.empty() && call.response != nullptr && call.response->statusCode == 200 && call.response->body == "hello from journey")
        {
            return {.isOk = true, .detail = "status=200 elapsed_ms=" + std::to_string(call.elapsed.count())};
        }
        return {.isOk   = false,
                .detail = "status=" + std::to_string(call.response != nullptr ? call.response->statusCode : 0) + " elapsed_ms=" + std::to_string(call.elapsed.count()) +
                          " reason=" + (call.failure.empty() ? "没有拿到期望的 200 响应" : call.failure)};
    }

    /**
     * @brief 升级与认证步：带口令的 wss 回显、错口令的升级被拒、错口令的 HTTP 被拒
     */
    StepOutcome stepUpgradeAndAuthentication(const JourneyMaterial &material, const std::string &baseUrl, const std::uint16_t port)
    {
        const WebSocketCall accepted     = callWebSocket(material, port, "/ws?token=" + std::string(kJourneyToken));
        const WebSocketCall rejected     = callWebSocket(material, port, "/ws?token=wrong-token");
        const HttpCall      unauthorized = callHttp(material, baseUrl + "/hello", "wrong-token", true, kStepTimeout);

        const bool isHttpRejected = unauthorized.response != nullptr && unauthorized.response->statusCode == 401;
        const bool isOk = accepted.isConnected && accepted.isEchoed && !rejected.isConnected && rejected.upgradeKind == Net::WebSocketUpgradeErrorKind::NotSwitchingProtocols &&
                          isHttpRejected;
        return {.isOk   = isOk,
                .detail = "ws_echo=" + std::string(accepted.isEchoed ? "1" : "0") + " ws_bad_token_connected=" + std::string(rejected.isConnected ? "1" : "0") +
                          " ws_bad_token_kind=" + std::to_string(static_cast<int>(rejected.upgradeKind)) +
                          " http_bad_token_status=" + std::to_string(unauthorized.response != nullptr ? unauthorized.response->statusCode : 0) +
                          (accepted.failure.empty() && rejected.failure.empty() ? std::string{} : " reason=" + accepted.failure + rejected.failure)};
    }

    /**
     * @brief 首包超时步：客户端在响应头之前到点，之后服务仍可用
     * @details 先确认处理器真的跑起来了，「到点」才不是连握手都没走完的假象
     */
    StepOutcome stepFirstPacketTimeout(const JourneyMaterial &material, const std::string &baseUrl, const std::shared_ptr<JourneyState> &state)
    {
        state->slowHeadersEntered.store(false, std::memory_order_release);
        const HttpCall timedOut         = callHttp(material, baseUrl + "/slow-headers", std::string(kJourneyToken), true, std::chrono::milliseconds{500});
        const bool     isHandlerEntered = waitUntil([&state]() { return state->slowHeadersEntered.load(std::memory_order_acquire); }, std::chrono::milliseconds{1000});
        const HttpCall afterTimeout     = callHttp(material, baseUrl + "/hello", std::string(kJourneyToken), true, kStepTimeout);

        const bool isOk = timedOut.response == nullptr && timedOut.failure.empty() && timedOut.elapsed < std::chrono::milliseconds{1200} && isHandlerEntered &&
                          afterTimeout.response != nullptr && afterTimeout.response->statusCode == 200;
        return {.isOk   = isOk,
                .detail = "timed_out=" + std::string(timedOut.response == nullptr ? "1" : "0") + " elapsed_ms=" + std::to_string(timedOut.elapsed.count()) +
                          " handler_entered=" + std::string(isHandlerEntered ? "1" : "0") +
                          " server_after_status=" + std::to_string(afterTimeout.response != nullptr ? afterTimeout.response->statusCode : 0)};
    }

    /**
     * @brief mTLS 身份步：业务读到的指纹必须等于客户端证书的现算指纹，不带身份连不上
     */
    StepOutcome stepMutualTlsIdentity(const JourneyMaterial &material, const std::string &baseUrl)
    {
        const std::string expectedFingerprint = certificateSha256(material.clientCertificate);
        const HttpCall    identified          = callHttp(material, baseUrl + "/whoami", std::string(kJourneyToken), true, kStepTimeout);
        const HttpCall    anonymous           = callHttp(material, baseUrl + "/whoami", std::string(kJourneyToken), false, std::chrono::milliseconds{4000});

        const std::string observedFingerprint = identified.response != nullptr ? jsonStringField(identified.response->body, "fingerprint") : std::string{};
        const bool        isVerified          = identified.response != nullptr && identified.response->body.find("\"verified\":true") != std::string::npos;
        const bool        isOk                = expectedFingerprint.size() == 64U && identified.response != nullptr && identified.response->statusCode == 200 &&
                                                observedFingerprint == expectedFingerprint && isVerified && anonymous.response == nullptr;
        return {.isOk   = isOk,
                .detail = "expected_prefix=" + expectedFingerprint.substr(0, 16) + " observed_prefix=" + observedFingerprint.substr(0, 16) +
                          " verified=" + std::string(isVerified ? "1" : "0") + " anonymous_connected=" + std::string(anonymous.response != nullptr ? "1" : "0")};
    }

    /**
     * @brief 错误展示步：处理器抛出折成 500，内部原因不进响应体，链路 id 仍在
     */
    StepOutcome stepErrorDisplay(const JourneyMaterial &material, const std::string &baseUrl)
    {
        const HttpCall call = callHttp(material, baseUrl + "/boom", std::string(kJourneyToken), true, kStepTimeout);
        if (call.response == nullptr)
        {
            return {.isOk = false, .detail = "没有拿到 500 响应： " + call.failure};
        }
        const std::optional<std::string_view> requestId = call.response->headerValue("x-request-id");
        const bool isOk = call.response->statusCode == 500 && call.response->body == "Internal Server Error" && call.response->body.find("deliberate") == std::string::npos &&
                          requestId.has_value() && !requestId->empty();
        return {.isOk   = isOk,
                .detail = "status=" + std::to_string(call.response->statusCode) + " body_bytes=" + std::to_string(call.response->body.size()) +
                          " request_id=" + std::string(requestId.has_value() ? "1" : "0")};
    }

    /**
     * @brief 限流步：桶容量 2，前两条放行、第三条回 429 并带 Retry-After
     */
    StepOutcome stepRateLimit(const JourneyMaterial &material, const std::string &baseUrl)
    {
        const HttpCall first  = callHttp(material, baseUrl + "/limited", std::string(kJourneyToken), true, kStepTimeout);
        const HttpCall second = callHttp(material, baseUrl + "/limited", std::string(kJourneyToken), true, kStepTimeout);
        const HttpCall third  = callHttp(material, baseUrl + "/limited", std::string(kJourneyToken), true, kStepTimeout);

        const int    firstStatus       = first.response != nullptr ? first.response->statusCode : 0;
        const int    secondStatus      = second.response != nullptr ? second.response->statusCode : 0;
        const int    thirdStatus       = third.response != nullptr ? third.response->statusCode : 0;
        const auto   retryAfter        = third.response != nullptr ? third.response->headerValue("retry-after") : std::nullopt;
        std::int64_t retryAfterSeconds = 0;
        const bool isRetryAfterValid = retryAfter.has_value() && !retryAfter->empty() &&
                                       std::from_chars(retryAfter->data(), retryAfter->data() + retryAfter->size(), retryAfterSeconds).ec == std::errc{} && retryAfterSeconds >= 1;
        return {.isOk   = firstStatus == 200 && secondStatus == 200 && thirdStatus == 429 && isRetryAfterValid,
                .detail = "statuses=" + std::to_string(firstStatus) + "," + std::to_string(secondStatus) + "," + std::to_string(thirdStatus) +
                          " retry_after=" + std::string(retryAfter.has_value() ? std::string(*retryAfter) : "absent")};
    }

    /**
     * @brief 优雅停机步：在途请求做完、drain 返回、之后不再服务
     */
    StepOutcome stepGracefulShutdown(const JourneyMaterial &material, const std::string &baseUrl, JourneyServer &server, const std::shared_ptr<JourneyState> &state)
    {
        state->slowEntered.store(false, std::memory_order_release);
        HttpCall    slowCall;
        std::thread slowThread([&slowCall, &material, &baseUrl]()
                               { slowCall = callHttp(material, baseUrl + "/slow", std::string(kJourneyToken), true, std::chrono::milliseconds{10000}); });
        const bool  isHandlerEntered = waitUntil([&state]() { return state->slowEntered.load(std::memory_order_acquire); }, kStepTimeout);
        const bool  isDrained        = server.requestGracefulDrain(std::chrono::milliseconds{5000});
        slowThread.join();
        const HttpCall afterDrain = callHttp(material, baseUrl + "/hello", std::string(kJourneyToken), true, std::chrono::milliseconds{2000});

        const bool isOk = isHandlerEntered && isDrained && slowCall.response != nullptr && slowCall.response->statusCode == 200 && slowCall.response->body == "slow-finished" &&
                          !server.isRunning() && afterDrain.response == nullptr;
        return {.isOk   = isOk,
                .detail = "handler_entered=" + std::string(isHandlerEntered ? "1" : "0") + " drained=" + std::string(isDrained ? "1" : "0") +
                          " slow_status=" + std::to_string(slowCall.response != nullptr ? slowCall.response->statusCode : 0) +
                          " still_running=" + std::string(server.isRunning() ? "1" : "0") + " after_drain_connected=" + std::string(afterDrain.response != nullptr ? "1" : "0")};
    }

    /**
     * @brief 重连步：同一端口起一台新服务，客户端重新连通
     */
    StepOutcome stepReconnect(const JourneyMaterial &material, const std::uint16_t port, const std::shared_ptr<JourneyState> &state)
    {
        JourneyServer restarted(port, material, state);
        const bool    isBuilt = restarted.isBuilt() && restarted.awaitRunning();
        if (!isBuilt)
        {
            return {.isOk   = false,
                    .detail = "同一端口 " + std::to_string(port) + " 上的新服务没能起来：" + (restarted.isBuilt() ? restarted.startFailureReason() : restarted.failureReason())};
        }
        const std::string baseUrl = "https://127.0.0.1:" + std::to_string(restarted.listeningPort());
        const HttpCall    call    = callHttp(material, baseUrl + "/hello", std::string(kJourneyToken), true, kStepTimeout);
        return {.isOk   = call.response != nullptr && call.response->statusCode == 200 && call.response->body == "hello from journey",
                .detail = "port=" + std::to_string(restarted.listeningPort()) + " status=" + std::to_string(call.response != nullptr ? call.response->statusCode : 0)};
    }

    /**
     * @brief 解析命令行
     * @param argc 实参个数
     * @param argv 实参表
     * @param material 输出：证书材料
     * @param isStrict 输出：是否把 SKIP 也当失败
     * @return bool 参数本身是否合法
     */
    bool parseArguments(const int argc, char **argv, JourneyMaterial &material, bool &isStrict)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view option(argv[index]);
            if (option == "--require-all")
            {
                isStrict = true;
                continue;
            }
            if (index + 1 >= argc)
            {
                return false;
            }
            const std::string value(argv[index + 1]);
            if (option == "--server-cert")
            {
                material.serverCertificate = value;
            } else if (option == "--server-key")
            {
                material.serverKey = value;
            } else if (option == "--client-cert")
            {
                material.clientCertificate = value;
            } else if (option == "--client-key")
            {
                material.clientKey = value;
            } else if (option == "--ca")
            {
                material.trustedCertificate = value;
            } else
            {
                return false;
            }
            ++index;
        }
        return true;
    }

    /**
     * @brief 五份材料是否都在
     */
    bool isMaterialComplete(const JourneyMaterial &material)
    {
        std::error_code error;
        return !material.serverCertificate.empty() && !material.serverKey.empty() && !material.clientCertificate.empty() && !material.clientKey.empty() &&
               !material.trustedCertificate.empty() && std::filesystem::exists(material.serverCertificate, error) && std::filesystem::exists(material.serverKey, error) &&
               std::filesystem::exists(material.clientCertificate, error) && std::filesystem::exists(material.clientKey, error) &&
               std::filesystem::exists(material.trustedCertificate, error);
    }

    /**
     * @brief 安装步：包里报的版本必须与 find_package 读到的版本一致
     */
    StepOutcome stepInstall()
    {
#if defined(JOURNEY_EXPECTED_VERSION)
        const std::string expectedVersion = JOURNEY_EXPECTED_VERSION;
        const std::string packageVersion  = ASYN_VERSION_STRING;
        return {.isOk = expectedVersion == packageVersion, .detail = "package_version=" + packageVersion + " find_package_version=" + expectedVersion};
#else
        return {.isOk = true, .detail = "package_version=" + std::string(ASYN_VERSION_STRING) + " find_package_version=未提供"};
#endif
    }
} // namespace

int main(const int argc, char **argv)
{
    JourneyMaterial material;
    bool            isStrict = false;
    if (!parseArguments(argc, argv, material, isStrict))
    {
        std::printf("JOURNEY_STEP=arguments RESULT=FAIL DETAIL=参数写法不对：可用 --server-cert/--server-key/--client-cert/--client-key/--ca/--require-all\n");
        return 2;
    }

    JourneyReport     report;
    const StepOutcome installOutcome = stepInstall();
    if (installOutcome.isOk)
    {
        report.pass("install", installOutcome.detail);
    } else
    {
        report.fail("install", installOutcome.detail);
    }

    if (!isMaterialComplete(material))
    {
        const std::string reason = "缺证书夹具：需要 --server-cert/--server-key/--client-cert/--client-key/--ca 五份都在";
        for (const char *stepName: {"connect", "upgrade_auth", "first_packet_timeout", "mtls_identity", "error_display", "rate_limit", "graceful_shutdown", "reconnect"})
        {
            // 严格档把「本可制造却缺料」当失败；默认档按计划显式跳过并写原因
            if (isStrict)
            {
                report.fail(stepName, reason);
            } else
            {
                report.skip(stepName, reason);
            }
        }
        report.skip("external_dns", "旅程不依赖公网 DNS：解析器注入与超时判据在仓库用例里覆盖");
        report.skip("real_acme_issuer", "真实 ACME 签发由 Pebble 跨实现作业覆盖，本旅程不外连机构");
        return report.summarize();
    }

    const auto state       = std::make_shared<JourneyState>();
    state->rateLimitBucket = std::make_shared<Net::TokenBucket>(0.01, 2.0);

    std::unique_ptr<JourneyServer> server = std::make_unique<JourneyServer>(0, material, state);
    if (!server->isBuilt() || !server->awaitRunning())
    {
        const std::string reason = server->isBuilt() ? server->startFailureReason() : server->failureReason();
        for (const char *stepName: {"connect", "upgrade_auth", "first_packet_timeout", "mtls_identity", "error_display", "rate_limit", "graceful_shutdown", "reconnect"})
        {
            report.fail(stepName, "服务器没能进入接受循环：" + reason);
        }
        report.skip("external_dns", "旅程不依赖公网 DNS：解析器注入与超时判据在仓库用例里覆盖");
        report.skip("real_acme_issuer", "真实 ACME 签发由 Pebble 跨实现作业覆盖，本旅程不外连机构");
        return report.summarize();
    }

    const std::uint16_t port    = server->listeningPort();
    const std::string   baseUrl = "https://127.0.0.1:" + std::to_string(port);

    const auto recordStep = [&report](const std::string_view name, const StepOutcome &outcome)
    {
        if (outcome.isOk)
        {
            report.pass(name, outcome.detail);
        } else
        {
            report.fail(name, outcome.detail);
        }
    };

    recordStep("connect", stepConnect(material, baseUrl));
    recordStep("upgrade_auth", stepUpgradeAndAuthentication(material, baseUrl, port));
    recordStep("first_packet_timeout", stepFirstPacketTimeout(material, baseUrl, state));
    recordStep("mtls_identity", stepMutualTlsIdentity(material, baseUrl));
    recordStep("error_display", stepErrorDisplay(material, baseUrl));
    recordStep("rate_limit", stepRateLimit(material, baseUrl));
    recordStep("graceful_shutdown", stepGracefulShutdown(material, baseUrl, *server, state));

    // 停机之后同端口重新起一台：先彻底收掉旧服务（含监听套接字），再让新服务占回同一个端口
    server.reset();
    recordStep("reconnect", stepReconnect(material, port, state));

    report.skip("external_dns", "旅程不依赖公网 DNS：解析器注入与超时判据在仓库用例里覆盖");
    report.skip("real_acme_issuer", "真实 ACME 签发由 Pebble 跨实现作业覆盖，本旅程不外连机构");
    return report.summarize();
}
