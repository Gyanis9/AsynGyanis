// 消费者旅程：仓库外的工程只经 find_package 使用公开 API，把「装出来的包能干什么」串成一条链。
// 每一步打印一行机读结果；环境造不出来的步骤显式 SKIP 并写原因，不静默算通过。

#include "AsynGyanisFeatures.h"

#include "Base/Config/ConfigManager.h"
#include "Base/Exception/Exception.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/FileSink.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Process/GracefulShutdown.h"
#include "Core/Process/UpgradeChannel.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPeerIdentity.h"
#include "Core/Tls/TlsPolicy.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Sqlite/SqliteConnection.h"
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
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

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
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
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

    /// worker 子进程的内部参数：由多进程网关步骤自己拼，普通旅程参数里不出现
    constexpr std::string_view kWorkerChildFlag   = "--worker-child";
    constexpr std::string_view kWorkerPortOption  = "--port";
    constexpr std::string_view kWorkerIndexOption = "--worker-index";

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
     * @brief 把一步的结论写进机读报告
     * @param report 报告收集器
     * @param name 步骤名
     * @param outcome 步骤结论
     */
    void recordOutcome(JourneyReport &report, const std::string_view name, const StepOutcome &outcome)
    {
        if (outcome.isOk)
        {
            report.pass(name, outcome.detail);
        } else
        {
            report.fail(name, outcome.detail);
        }
    }

    /**
     * @brief 旅程临时目录：构造即建，析构即清
     */
    class JourneyTemporaryDirectory
    {
    public:
        JourneyTemporaryDirectory()
        {
            std::error_code             error;
            const std::filesystem::path base = std::filesystem::temp_directory_path(error);
            if (error)
            {
                m_failureReason = "临时目录不可用：" + error.message();
                return;
            }
            m_path = base / ("asyn-journey-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(m_path, error);
            if (error)
            {
                m_failureReason = "临时目录建不出来：" + error.message();
                m_path.clear();
            }
        }

        ~JourneyTemporaryDirectory()
        {
            if (!m_path.empty())
            {
                std::error_code error;
                std::filesystem::remove_all(m_path, error);
            }
        }

        JourneyTemporaryDirectory(const JourneyTemporaryDirectory &)            = delete;
        JourneyTemporaryDirectory &operator=(const JourneyTemporaryDirectory &) = delete;

        [[nodiscard]] bool isReady() const noexcept
        {
            return m_failureReason.empty();
        }

        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

        [[nodiscard]] const std::string &failureReason() const noexcept
        {
            return m_failureReason;
        }

    private:
        std::filesystem::path m_path;
        std::string           m_failureReason;
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
     * @brief 用实例客户端发一次明文 HTTP GET（多进程网关自检用，不碰证书材料）
     * @param url 目标地址
     * @param timeout 整体时限
     * @return HttpCall 响应、时长与本端失败原因
     */
    HttpCall callPlainHttp(const std::string &url, const std::chrono::milliseconds timeout)
    {
        HttpCall result;
        try
        {
            Core::EventLoop      loop;
            Net::HttpProxyPolicy proxyPolicy;
            // 与 callHttp 同一条理由：CI 的代理环境变量会把回环请求交给代理
            proxyPolicy.consultEnvironment = false;
            Net::HttpClient        client(loop, Net::HttpOutboundConnectionPool::Config{}, Core::TlsPolicy{}, proxyPolicy);
            Net::HttpClientRequest request;
            const auto             startedAt   = std::chrono::steady_clock::now();
            auto                   requestBody = [&]() -> Core::Task<>
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
     * @brief 从一条单值查询里读整数
     * @param connection 已连接的数据库连接
     * @param sql 只返回一行一列的语句
     * @param failure 输出失败原因
     * @return std::optional<std::int64_t> 读到的整数；失败为空
     */
    std::optional<std::int64_t> readSingleInteger(Database::DatabaseConnection &connection, const std::string_view sql, std::string &failure)
    {
        const std::unique_ptr<Database::DatabaseResult> result = connection.execute(sql);
        if (result == nullptr)
        {
            failure = connection.lastError();
            return std::nullopt;
        }
        if (!result->next())
        {
            failure = "查询没有返回任何行";
            return std::nullopt;
        }
        const Database::DatabaseValue value = result->getValue(0);
        if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
        {
            return *number;
        }
        failure = "COUNT(*) 没有交出整型";
        return std::nullopt;
    }

    /**
     * @brief 业务事务步：回滚不落库、提交落一行
     * @param workspace 旅程临时目录
     * @return StepOutcome 结论与读数
     */
    StepOutcome stepDatabaseTransaction(const std::filesystem::path &workspace)
    {
        if (workspace.empty())
        {
            return {.isOk = false, .detail = "临时目录不可用"};
        }
        const std::filesystem::path databasePath = workspace / "journey.sqlite";
        Database::SqliteConnection  connection(Database::ConnectionConfig::sqliteDefault(databasePath.string()));
        if (!connection.connect())
        {
            return {.isOk = false, .detail = "SQLite 建连失败：" + connection.lastError()};
        }
        if (connection.execute("CREATE TABLE journey_tx (id INTEGER PRIMARY KEY, value TEXT NOT NULL)") == nullptr)
        {
            return {.isOk = false, .detail = "建表失败：" + connection.lastError()};
        }
        if (!connection.beginTransaction() || connection.execute("INSERT INTO journey_tx (id, value) VALUES (1, 'rolled-back')") == nullptr || !connection.rollback())
        {
            return {.isOk = false, .detail = "回滚事务失败：" + connection.lastError()};
        }
        std::string failure;
        const auto  rollbackCount = readSingleInteger(connection, "SELECT COUNT(*) FROM journey_tx", failure);
        if (!rollbackCount.has_value() || *rollbackCount != 0)
        {
            return {.isOk = false, .detail = "回滚后计数不是 0：" + failure};
        }
        if (!connection.beginTransaction() || connection.execute("INSERT INTO journey_tx (id, value) VALUES (2, 'committed')") == nullptr || !connection.commit())
        {
            return {.isOk = false, .detail = "提交事务失败：" + connection.lastError()};
        }
        const auto commitCount = readSingleInteger(connection, "SELECT COUNT(*) FROM journey_tx", failure);
        if (!commitCount.has_value() || *commitCount != 1)
        {
            return {.isOk = false, .detail = "提交后计数不是 1：" + failure};
        }
        return {.isOk = true, .detail = "rolled_back=0 committed=1 path=" + databasePath.string()};
    }

    /**
     * @brief 配置装载步：从临时 YAML 读到整数与字符串
     * @param workspace 旅程临时目录
     * @return StepOutcome 结论与读数
     */
    StepOutcome stepConfigLoad(const std::filesystem::path &workspace)
    {
        if (workspace.empty())
        {
            return {.isOk = false, .detail = "临时目录不可用"};
        }
        const std::filesystem::path configPath = workspace / "journey.yaml";
        {
            std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
            output << "journey:\n  value: 41\n  name: consumer\n";
            if (!output)
            {
                return {.isOk = false, .detail = "配置文件写不出来：" + configPath.string()};
            }
        }
        auto &manager = Base::ConfigManager::instance();
        manager.clear();
        const Base::ConfigLoadResult loaded = manager.loadFiles({configPath});
        const int                    value  = manager.getInt("journey.value", 0);
        const std::string            name   = manager.getString("journey.name");
        manager.clear();
        if (!loaded.success || value != 41 || name != "consumer")
        {
            return {.isOk = false, .detail = "loaded=" + std::to_string(loaded.success) + " value=" + std::to_string(value) + " name=" + name};
        }
        return {.isOk = true, .detail = "value=41 name=consumer files=" + std::to_string(loaded.loadedFiles.size())};
    }

    /**
     * @brief 日志 Sink 步：一条 Info 必须落进临时文件
     * @param workspace 旅程临时目录
     * @return StepOutcome 结论与读数
     */
    StepOutcome stepLogSink(const std::filesystem::path &workspace)
    {
        if (workspace.empty())
        {
            return {.isOk = false, .detail = "临时目录不可用"};
        }
        const std::filesystem::path logPath = workspace / "journey.log";
        {
            Base::Logger logger("journey.logger");
            logger.setLevel(Base::LogLevel::Trace);
            logger.addSink(std::make_shared<Base::FileSink>(logPath, true));
            logger.log(Base::LogLevel::Info, "journey-log-line");
            logger.flush();
        }
        std::ifstream     input(logPath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (text.find("journey-log-line") == std::string::npos)
        {
            return {.isOk = false, .detail = "日志没有落盘：" + logPath.string() + " bytes=" + std::to_string(text.size())};
        }
        return {.isOk = true, .detail = "bytes=" + std::to_string(text.size()) + " path=" + logPath.string()};
    }

    /**
     * @brief 把 argv[0] 落成绝对路径：worker 由 supervisor 另起进程，相对路径会按它自己的 CWD 解析
     * @param argv0 本进程的 argv[0]
     * @return std::string 绝对路径；解析失败时原样交回
     */
    std::string resolveExecutablePath(const char *argv0)
    {
        const std::string original = argv0 == nullptr ? std::string{} : std::string(argv0);
        if (original.empty())
        {
            return original;
        }
        std::error_code             error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(original, error);
        return error ? original : absolutePath.string();
    }

    /**
     * @brief 在 worker 的循环上驱动服务器，把启动异常留在本地而不是带崩循环线程
     * @param server 待启动的服务器
     * @param loop 服务器所属循环（失败时用来停它）
     * @param startFailed 启动是否抛过（输出）
     * @param failure 异常文本（输出）
     * @return Core::Task<> 服务器跑完或启动失败后完成
     */
    Core::Task<> driveWorkerServer(Net::HttpServer &server, Core::EventLoop &loop, std::atomic<bool> &startFailed, std::string &failure)
    {
        try
        {
            co_await server.start();
        } catch (const std::exception &error)
        {
            failure = error.what();
            startFailed.store(true, std::memory_order_release);
            loop.stop();
        }
        co_return;
    }

    /**
     * @brief worker 子进程：在指定/接手的监听套接字上跑一台最小 HTTP 网关
     * @details master 由 WorkerSupervisor 起两个这样的进程：POSIX 各自 bind 同一端口（SO_REUSEPORT），
     *          Windows 接手 master 交来的监听描述符。停机信号由 GracefulShutdown 接住，在循环线程上
     *          停服务器。子进程不打印任何 JOURNEY_STEP 行，避免污染 master 的机读报告。
     * @param argc 参数个数（master 拼出来的内部参数）
     * @param argv 参数数组
     * @return int 进程退出码；0 表示按停机请求正常收尾
     */
    int runWorkerChild(const int argc, char **argv)
    {
        std::uint16_t port           = 0;
        std::string   workerIndex    = "-";
        std::string   handoffAddress = {};
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view option = argv[index];
            if (index + 1 >= argc)
            {
                continue;
            }
            if (option == kWorkerPortOption)
            {
                const std::string_view value        = argv[index + 1];
                std::uint32_t          parsedPort   = 0;
                const auto [endPointer, parseError] = std::from_chars(value.data(), value.data() + value.size(), parsedPort);
                if (parseError == std::errc{} && endPointer == value.data() + value.size() && parsedPort <= 65535U)
                {
                    port = static_cast<std::uint16_t>(parsedPort);
                }
            } else if (option == kWorkerIndexOption)
            {
                workerIndex = argv[index + 1];
            } else if (option == Core::kHandedOverListenerArgument)
            {
                handoffAddress = argv[index + 1];
            }
        }

        try
        {
            Core::EventLoop                  loop;
            std::unique_ptr<Net::HttpServer> server;
            if (!handoffAddress.empty())
            {
                std::expected<int, Core::UpgradeHandoffError> adopted = Core::adoptHandedOverListener(handoffAddress, kStepTimeout);
                if (!adopted.has_value())
                {
                    std::fprintf(stderr, "worker 接手监听套接字失败：%s\n", adopted.error().message.c_str());
                    return 4;
                }
                // 所有权随构造交给服务器，收口时由它关闭
                server = std::make_unique<Net::HttpServer>(loop, *adopted);
            } else
            {
                if (port == 0)
                {
                    std::fprintf(stderr, "worker 既没有接手监听套接字，也没有拿到端口\n");
                    return 3;
                }
                server = std::make_unique<Net::HttpServer>(loop, Core::InetAddress::localhost(port));
            }

            Core::GracefulShutdown shutdown(loop);
            shutdown.onShutdown(
                    [&server, &loop]
                    {
                        server->stop();
                        loop.stop();
                    });
            server->router().get("/worker-health",
                                 [workerIndex](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                                 {
                                     response.setStatus(200);
                                     response.setBody("worker index=" + workerIndex + " pid=" + std::to_string(Platform::ProcessInfo::currentProcessId()));
                                     co_return;
                                 });

            std::atomic<bool> startFailed{false};
            std::string       startFailure;
            Core::Task<>      startTask = driveWorkerServer(*server, loop, startFailed, startFailure);
            loop.scheduler().schedule(startTask.handle());
            loop.run();
            if (startFailed.load(std::memory_order_acquire))
            {
                std::fprintf(stderr, "worker 服务器启动失败：%s\n", startFailure.c_str());
                return 6;
            }
            return 0;
        } catch (const std::exception &failure)
        {
            std::fprintf(stderr, "worker 启动失败：%s\n", failure.what());
            return 5;
        }
    }

    /**
     * @brief 多进程网关一步的读数
     */
    struct WorkerGatewayEvidence
    {
        bool        isGatewayAnswered{false}; ///< 端口上确实有 worker 应答了 200
        int         statusCode{0};            ///< 网关返回的状态码
        std::string body;                     ///< 网关返回的正文（带 worker 序号与进程号）
        std::string failure;                  ///< 最近一次自检失败的原因
    };

    /**
     * @brief 多进程网关一步：起两个 worker 共享同一端口，验证应答后请求停止并确认端口关闭
     * @details 两个平台的分摊形状由 WorkerSupervisor 自己选：POSIX 各 worker 自己 bind 同一端口
     *          （SO_REUSEPORT），Windows 由本进程建好监听套接字后逐个移交。端口先由内核分配再传给
     *          worker，不写死端口。自检发生在「整池进程都活着」的回调里，随后直接请求停止——
     *          run() 必须在单线程进程里 fork，因此不能在它旁边另起一条观察线程。
     * @param executablePath 本可执行文件的绝对路径（worker 就是同一个二进制的 --worker-child 模式）
     * @return StepOutcome 结论与读数（网关状态/正文、停止结果、停止后端口是否拒绝连接）
     */
    StepOutcome stepWorkerStartStop(const std::string &executablePath)
    {
        Core::EventLoop   listenerLoop;
        Core::AsyncSocket listener = Core::AsyncSocket::create(listenerLoop);
        if (!listener.bind(Core::InetAddress::localhost(0)) || !listener.listen(128))
        {
            return {.isOk = false, .detail = "worker 网关的监听套接字建不起来"};
        }
        const std::uint16_t port = listener.localAddress().port();
        if (port == 0)
        {
            return {.isOk = false, .detail = "worker 网关读不出分配到的端口"};
        }
#if ASYN_PLATFORM_WIN32
        // 移交档位要的是这枚已 bind + listen 的描述符；所有权仍在本进程，收尾时由本进程关闭
        const int handedOverDescriptor = listener.fileDescriptor();
#else
        // POSIX 上 worker 各自 bind 同一端口：探针只用来让内核分配端口，关上之后才轮到 worker
        listener.close();
#endif

        Core::WorkerSupervisor::Configuration configuration;
        configuration.executablePath      = executablePath;
        configuration.workerArguments     = {std::string(kWorkerChildFlag), std::string(kWorkerPortOption), std::to_string(port)};
        configuration.workerIndexArgument = std::string(kWorkerIndexOption);
        configuration.workerCount         = 2;
        configuration.pollInterval        = std::chrono::milliseconds{50};
        configuration.shutdownTimeout     = std::chrono::seconds{5};
#if ASYN_PLATFORM_WIN32
        configuration.handoff = Core::WorkerSupervisor::Handoff{handedOverDescriptor, std::chrono::seconds{5}};
#endif

        const std::string     healthUrl = "http://127.0.0.1:" + std::to_string(port) + "/worker-health";
        WorkerGatewayEvidence evidence;
        auto                  stopHook    = std::make_shared<std::function<void()>>();
        configuration.onAllWorkersRunning = [&evidence, healthUrl, stopHook]()
        {
            // 整池进程活着不等于端口已在应答：两者之间隔着 bind/listen 与首轮 accept，按有界轮询等
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline)
            {
                const HttpCall call = callPlainHttp(healthUrl, std::chrono::milliseconds{500});
                if (call.response != nullptr && call.response->statusCode == 200 && !call.response->body.empty())
                {
                    evidence.isGatewayAnswered = true;
                    evidence.statusCode        = call.response->statusCode;
                    evidence.body              = call.response->body;
                    evidence.failure.clear();
                    break;
                }
                evidence.failure = call.failure.empty() ? "网关没有返回 200" : call.failure;
                std::this_thread::sleep_for(kPollInterval);
            }
            if (*stopHook)
            {
                (*stopHook)();
            }
        };

        bool isStoppedByRequest = false;
        try
        {
            Core::WorkerSupervisor supervisor(std::move(configuration));
            *stopHook          = [&supervisor] { supervisor.requestStop(); };
            isStoppedByRequest = supervisor.run();
        } catch (const std::exception &failure)
        {
#if ASYN_PLATFORM_WIN32
            listener.close();
#endif
            return {.isOk = false, .detail = "worker 编排失败：" + std::string(failure.what())};
        }
#if ASYN_PLATFORM_WIN32
        // 先放掉本进程这枚监听引用：同一内核对象的另一个句柄还开着时，「停止后端口拒绝连接」永远是假
        listener.close();
#endif
        const HttpCall afterStop    = callPlainHttp(healthUrl, std::chrono::milliseconds{500});
        const bool     isPortClosed = afterStop.response == nullptr;
        const bool     isOk         = evidence.isGatewayAnswered && isStoppedByRequest && isPortClosed;
        return {.isOk   = isOk,
                .detail = "workers=2 gateway_status=" + std::to_string(evidence.statusCode) + " body=" + evidence.body + " stopped=" + (isStoppedByRequest ? "1" : "0") +
                          " port_closed=" + (isPortClosed ? "1" : "0") + (evidence.failure.empty() ? std::string{} : " reason=" + evidence.failure)};
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
    // worker 分支必须排在参数校验之前：master 传给 worker 的只有内部参数，没有证书材料
    for (int index = 1; index < argc; ++index)
    {
        if (std::string_view(argv[index]) == kWorkerChildFlag)
        {
            return runWorkerChild(argc, argv);
        }
    }

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

    // 三条非网络步骤不依赖证书材料：缺料时也要真跑，才能证明包里带出了 Database/Base
    JourneyTemporaryDirectory workspace;
    recordOutcome(report, "database_transaction", workspace.isReady() ? stepDatabaseTransaction(workspace.path()) : StepOutcome{false, workspace.failureReason()});
    recordOutcome(report, "config_load", workspace.isReady() ? stepConfigLoad(workspace.path()) : StepOutcome{false, workspace.failureReason()});
    recordOutcome(report, "log_sink", workspace.isReady() ? stepLogSink(workspace.path()) : StepOutcome{false, workspace.failureReason()});
    recordOutcome(report, "worker_start_stop", stepWorkerStartStop(resolveExecutablePath(argc > 0 ? argv[0] : nullptr)));

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

    recordOutcome(report, "connect", stepConnect(material, baseUrl));
    recordOutcome(report, "upgrade_auth", stepUpgradeAndAuthentication(material, baseUrl, port));
    recordOutcome(report, "first_packet_timeout", stepFirstPacketTimeout(material, baseUrl, state));
    recordOutcome(report, "mtls_identity", stepMutualTlsIdentity(material, baseUrl));
    recordOutcome(report, "error_display", stepErrorDisplay(material, baseUrl));
    recordOutcome(report, "rate_limit", stepRateLimit(material, baseUrl));
    recordOutcome(report, "graceful_shutdown", stepGracefulShutdown(material, baseUrl, *server, state));

    // 停机之后同端口重新起一台：先彻底收掉旧服务（含监听套接字），再让新服务占回同一个端口
    server.reset();
    recordOutcome(report, "reconnect", stepReconnect(material, port, state));

    report.skip("external_dns", "旅程不依赖公网 DNS：解析器注入与超时判据在仓库用例里覆盖");
    report.skip("real_acme_issuer", "真实 ACME 签发由 Pebble 跨实现作业覆盖，本旅程不外连机构");
    return report.summarize();
}
