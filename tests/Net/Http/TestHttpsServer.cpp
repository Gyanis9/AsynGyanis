// TestHttpsServer.cpp —— HTTPS 侧端到端覆盖（真实 TLS 回环 + 仓库自签证书）：
//   一. 请求服务与统计：一条 HTTPS 请求得到 200 与正确正文，totalRequestCount 与 2xx 计数各 +1；
//   二. request-id：响应带形态合法的 x-request-id，客户端自带的合法值被原样回显；
//   三. 空闲超时：短 idleTimeout + 短清扫节拍下，TLS 连接被清扫协程收口并计入 timeoutClosedCount；
//   四. 前缀隔离：HTTP 与 HTTPS 各自的 request-id 前缀不相等。
// 夹具在本文件内自建（TestHttpsServer + TlsLoopbackClient），回环端口由内核分配，用例之间不共用端口。

#include "Net/Http/HttpsServer.h"

#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpServerStats.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include "HttpTestSupport.h"

#include "CoreTestSupport.h"
#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 仓库内预生成的自签测试证书（CN=asyngyanis-test，有效期至 2036）
        const std::filesystem::path kTestCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

        /// 出站客户端用例专用的证书夹具：CN=localhost 且 SAN=DNS:localhost（生成命令见用例说明）。
        /// 与 test_cert.pem 的差别就在名字——那张只认 CN=asyngyanis-test，任何用 IP/localhost 连它的
        /// 客户端都该被主机名校验拒掉，因此它做不了「名字对得上」的正面用例
        const std::filesystem::path kLocalhostCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_localhost_cert.pem";

        /// 仓库内预生成的配套私钥
        const std::filesystem::path kTestKeyPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";

        /// 与上面那张 localhost 证书配套的私钥
        const std::filesystem::path kLocalhostKeyPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_localhost_key.pem";

        /// 客户端正面用例的证书夹具：CN=127.0.0.1 且 SAN=IP:127.0.0.1（生成命令见用例说明）。
        /// 正面用例直接连回环 IP，不经过域名解析——省掉「localhost 解析成 ::1 而服务端只监听 IPv4」
        /// 这类与本用例无关的干扰
        const std::filesystem::path kLoopbackCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem";

        /// 与上面那张回环 IP 证书配套的私钥
        const std::filesystem::path kLoopbackKeyPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem";

        /// request-id 前缀的长度（十六进制服务器序号）
        constexpr std::size_t kRequestIdPrefixLength = 4;

        /// request-id 响应头的行内前缀（响应头名前带 CRLF，避免误命中正文里的同名文本）
        constexpr std::string_view kRequestIdHeaderLinePrefix = "\r\nx-request-id: ";

        /// SSL_CTX 释放器：断言提前返回时客户端上下文也不会泄漏
        struct SslContextDeleter
        {
            void operator()(SSL_CTX *context) const noexcept
            {
                SSL_CTX_free(context);
            }
        };

        /// SSL 对象释放器
        struct SslDeleter
        {
            void operator()(SSL *ssl) const noexcept
            {
                SSL_free(ssl);
            }
        };

        /**
         * @brief 观测性/超时用例共用的限额：超时三项都设得很长
         * @details 只有空闲超时用例会覆写 idleTimeout；其余用例不该被清扫在中途收口连接。
         * @return HttpServerLimits 关掉超时保护的配置
         */
        HttpServerLimits makeLongTimeoutLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout  = std::chrono::seconds{10};
            limits.readTimeout  = std::chrono::seconds{10};
            limits.writeTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 判断请求标识是否符合自动生成格式（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::looksLikeGeneratedRequestId;

        /**
         * @brief 统计文本里指定子串出现的次数（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::countTextOccurrences;

        /**
         * @brief 取响应文本里第 occurrence 个 x-request-id 头的值
         * @param responseText 已收到的全部字节
         * @param occurrence 第几个（从 0 开始）
         * @return std::string 头部值；不存在时返回空串
         */
        std::string requestIdHeaderAt(const std::string &responseText, const std::size_t occurrence)
        {
            std::size_t foundPosition = responseText.find(kRequestIdHeaderLinePrefix);
            for (std::size_t skippedCount = 0; foundPosition != std::string::npos; ++skippedCount)
            {
                if (skippedCount == occurrence)
                {
                    const std::size_t valueStart = foundPosition + kRequestIdHeaderLinePrefix.size();
                    const std::size_t lineEnd    = responseText.find("\r\n", valueStart);
                    if (lineEnd == std::string::npos)
                    {
                        return {};
                    }
                    return responseText.substr(valueStart, lineEnd - valueStart);
                }
                foundPosition = responseText.find(kRequestIdHeaderLinePrefix, foundPosition + kRequestIdHeaderLinePrefix.size());
            }
            return {};
        }

        /**
         * @brief 一条到 127.0.0.1 指定端口的 TLS 客户端
         *
         * @details 客户端刻意不用框架的 TlsSocket/AsyncSocket：用例只需要「握手、发字节、看对端什么时候断」，
         *          再引入一套事件循环无助于验证服务端行为。套接字保持非阻塞，握手与读写都用
         *          「SSL_xxx 返回 WANT_* 就轮询重试」的方式推进，因此任何一步都不会把测试线程挂死。
         * @note 客户端不校验服务端证书：仓库夹具是自签证书，用例要验证的是加密通道与协议行为本身。
         */
        class TlsLoopbackClient
        {
        public:
            /**
             * @brief 连接并完成 TLS 握手
             * @param port 服务端监听端口
             */
            explicit TlsLoopbackClient(const std::uint16_t port)
            {
                m_context.reset(SSL_CTX_new(TLS_client_method()));
                if (m_context == nullptr)
                {
                    return;
                }

                m_descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(m_descriptor))
                {
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(port);
                if (::connect(m_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
                {
                    closeNow();
                    return;
                }

                // 转非阻塞：握手与读写靠轮询推进，用例不会因为对端不响应而永久挂起
                Platform::FileDescriptor::setNonBlocking(m_descriptor);

                m_ssl.reset(SSL_new(m_context.get()));
                if (m_ssl == nullptr || SSL_set_fd(m_ssl.get(), m_descriptor) != 1)
                {
                    m_ssl.reset();
                    closeNow();
                    return;
                }

                m_handshakeDone = performHandshake(kWaitTimeout);
            }

            ~TlsLoopbackClient()
            {
                closeNow();
            }

            TlsLoopbackClient(const TlsLoopbackClient &) = delete;

            TlsLoopbackClient &operator=(const TlsLoopbackClient &) = delete;

            /// TLS 握手是否已完成
            [[nodiscard]] bool isHandshakeComplete() const noexcept
            {
                return m_handshakeDone;
            }

            /// 本端 TCP 端口：服务端在会话里看到的对端端口就是它
            [[nodiscard]] std::uint16_t localPort() const
            {
                return queryBoundPort(m_descriptor);
            }

            /**
             * @brief 把整段字节写出去
             * @param payload 待发字节
             * @param timeout 写入等待上限
             * @return true 全部字节已交给 OpenSSL
             */
            bool sendText(const std::string_view payload, const std::chrono::milliseconds timeout) const
            {
                if (!m_handshakeDone)
                {
                    return false;
                }

                const auto  deadline      = std::chrono::steady_clock::now() + timeout;
                std::size_t writtenLength = 0;

                while (writtenLength < payload.size())
                {
                    const int writeLength = SSL_write(m_ssl.get(), payload.data() + writtenLength, static_cast<int>(payload.size() - writtenLength));
                    if (writeLength > 0)
                    {
                        writtenLength += static_cast<std::size_t>(writeLength);
                        continue;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), writeLength);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }

            /**
             * @brief 读一次，读到的字节追加到 accumulated
             * @param accumulated 输入输出：累计读到的字节
             * @return ReadOutcome 本次读取的结论；无数据可读时为 Idle，稍后重试即可
             */
            ReadOutcome readOnce(std::string &accumulated) const
            {
                if (!m_handshakeDone)
                {
                    return ReadOutcome::Broken;
                }

                std::array<char, kClientChunkLength> chunkStorage{};
                const int                            readLength = SSL_read(m_ssl.get(), chunkStorage.data(), static_cast<int>(chunkStorage.size()));
                if (readLength > 0)
                {
                    accumulated.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    return ReadOutcome::Data;
                }

                const int errorCode = SSL_get_error(m_ssl.get(), readLength);
                if (errorCode == SSL_ERROR_ZERO_RETURN)
                {
                    return ReadOutcome::PeerClosed;
                }
                if (errorCode == SSL_ERROR_WANT_READ || errorCode == SSL_ERROR_WANT_WRITE)
                {
                    return ReadOutcome::Idle;
                }
                // 对端未发 close_notify 就断开时是 SYSCALL + 0：同样按「已关闭」而不是「出错」处理
                if (errorCode == SSL_ERROR_SYSCALL && readLength == 0)
                {
                    return ReadOutcome::PeerClosed;
                }
                return ReadOutcome::Broken;
            }

            /**
             * @brief 轮询读直到累计出现指定次数的标记文本
             * @param accumulated 输入输出：累计读到的字节
             * @param expectedText 标记文本
             * @param expectedCount 期望出现次数
             * @param timeout 等待上限
             * @return true 在时限内凑齐
             */
            bool waitForTextOccurrences(std::string &accumulated, const std::string_view expectedText, const std::size_t expectedCount,
                                        const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (countTextOccurrences(accumulated, expectedText) < expectedCount)
                {
                    const ReadOutcome outcome = readOnce(accumulated);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return countTextOccurrences(accumulated, expectedText) >= expectedCount;
            }

            /**
             * @brief 轮询读直到观察到对端关闭或连接出错
             * @param accumulated 输入输出：累计读到的字节
             * @param timeout 等待上限
             * @return true 在时限内观察到连接已断
             */
            bool waitForClosure(std::string &accumulated, const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const ReadOutcome outcome = readOnce(accumulated);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        return true;
                    }
                    if (outcome == ReadOutcome::Data)
                    {
                        continue;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            /// 关闭本端
            void closeNow() noexcept
            {
                // 先尽力发出 close_notify 再释放 SSL 对象：让服务端看到的是正常关闭
                if (m_ssl != nullptr)
                {
                    SSL_shutdown(m_ssl.get());
                    m_ssl.reset();
                }
                Platform::FileDescriptor::close(m_descriptor);
                m_descriptor = Platform::FileDescriptor::kInvalid;
            }

        private:
            /**
             * @brief 在时限内推进 SSL_connect 直到完成
             * @param timeout 握手等待上限
             * @return true 握手完成
             */
            bool performHandshake(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const int result = SSL_connect(m_ssl.get());
                    if (result == 1)
                    {
                        return true;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), result);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            std::unique_ptr<SSL_CTX, SslContextDeleter> m_context;                                        ///< 客户端 TLS 上下文
            std::unique_ptr<SSL, SslDeleter>            m_ssl;                                            ///< 客户端 SSL 对象（关联 m_descriptor）
            int                                         m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 底层 TCP 描述符
            bool                                        m_handshakeDone{false};                           ///< TLS 握手是否已完成
            Platform::Socket::Initialization            m_socketInitialization;                           ///< 保证 Winsock 在本对象存活期间保持初始化
        };

        /**
         * @brief 把基类的监听描述符与活跃连接数透出成只读访问器的测试服务器
         * @details 端口由内核分配，只能从监听描述符反查；活跃连接数是「会话是否真的退出」的判据。
         */
        class TestHttpsServer final : public HttpsServer
        {
        public:
            using HttpsServer::HttpsServer;

            /// 监听描述符
            [[nodiscard]] int listenDescriptor() const
            {
                return m_acceptor.fileDescriptor();
            }

            /// 当前挂在连接管理器上的活跃连接数
            [[nodiscard]] std::size_t activeConnectionCount() const
            {
                return m_connectionManager.activeCount();
            }

            /// 当前挂在连接管理器上的活跃连接（取快照，供用例按基类接口观察会话）
            [[nodiscard]] std::vector<std::shared_ptr<Core::Connection>> activeConnections() const
            {
                return m_connectionManager.snapshot();
            }
        };

        /**
         * @brief 跑起一台真实 HttpsServer 的夹具（骨架见 HttpTestSupport 的 RunningServerFixture）
         */
        class RunningHttpsServerFixture final : public RunningServerFixture<TestHttpsServer>
        {
        public:
            /**
             * @brief 构造并启动服务器
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             * @param registerRoutes 可选的附加路由注册动作，在投递 start() 之前执行
             * @param parserLimits 可选的解析器资源上限，在投递 start() 之前落定，只影响此后新建的会话
             * @param configureServer 可选的服务器配置动作（例如打开指标端点），同样在 start() 之前执行
             * @param certificatePath 服务器证书路径，默认用仓库自签夹具 test_cert.pem
             * @param privateKeyPath 服务器私钥路径，默认用仓库自签夹具 test_key.pem
             */
            RunningHttpsServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval, const RouteRegistrar &registerRoutes = {},
                                      const HttpParserLimits &parserLimits = HttpParserLimits{}, const std::function<void(HttpsServer &)> &configureServer = {},
                                      const std::filesystem::path &certificatePath = kTestCertificatePath, const std::filesystem::path &privateKeyPath = kTestKeyPath) :
                RunningServerFixture<TestHttpsServer>(limits, sweepInterval, parserLimits, [&certificatePath, &privateKeyPath](Core::EventLoop &serverLoop)
                                                      { return TestHttpsServer(serverLoop, Core::InetAddress::localhost(0), certificatePath.string(), privateKeyPath.string()); })
            {
                if (registerRoutes)
                {
                    registerRoutes(server().router(), loop());
                }

                // 端点这类「必须开机前落定」的配置同样在投递 start() 之前做
                if (configureServer)
                {
                    configureServer(server());
                }

                startServer();
            }
        };
    } // namespace

    /**
     * @brief 钉住：一条 HTTPS 请求得到 200 与正确正文，统计里的请求条数与状态码类计数各 +1
     */

    /**
     * @brief 用出站客户端请求一次 HTTPS 地址（自建循环，跑完即停）
     * @param url 目标地址
     * @return std::unique_ptr<HttpClientResponse> 响应；失败（含证书校验不过）返回空
     */
    std::unique_ptr<HttpClientResponse> doHttpsGet(const std::string &url)
    {
        Core::EventLoop                     loop;
        std::unique_ptr<HttpClientResponse> result;
        // 惰性协程的帧记住的是闭包对象的地址：闭包必须先落到具名变量上再调用
        auto requestBody = [&loop, &result, &url]() -> Core::Task<>
        {
            result = co_await HttpClient::get(loop, url);
            loop.stop();
        };
        Core::Task<> request = requestBody();
        if (!request.isReady())
        {
            loop.scheduler().schedule(request.handle());
        }
        loop.run();
        return result;
    }

    /**
     * @brief 用出站客户端向 HTTPS 地址 POST 一段正文（自建循环，跑完即停）
     * @param url 目标地址
     * @param contentType 正文媒体类型
     * @param body 正文
     * @return std::unique_ptr<HttpClientResponse> 响应；失败（含证书校验不过）返回空
     */
    std::unique_ptr<HttpClientResponse> doHttpsPost(const std::string &url, const std::string &contentType, const std::string &body)
    {
        Core::EventLoop                     loop;
        std::unique_ptr<HttpClientResponse> result;
        auto                                requestBody = [&loop, &result, &url, &contentType, &body]() -> Core::Task<>
        {
            result = co_await HttpClient::post(loop, url, contentType, body);
            loop.stop();
        };
        Core::Task<> request = requestBody();
        if (!request.isReady())
        {
            loop.scheduler().schedule(request.handle());
        }
        loop.run();
        return result;
    }

    /**
     * @brief 一次「带着自己 TLS 上下文」的出站请求的结论
     */
    struct ClientTlsAttemptOutcome
    {
        std::unique_ptr<HttpClientResponse> response;             ///< 响应；握手或请求失败时为空
        bool                                identityLoaded{true}; ///< 客户端身份是否装载成功；未要求身份时恒为 true
    };

    /**
     * @brief 用给定的出站 TLS 策略（可带客户端身份）GET 一次：自建客户端与循环，跑完即停
     * @details 与 doHttpsGet 的差别只在客户端是哪一档：静态那一支没有承载策略的地方，用的是进程级
     *          默认上下文；出站策略（信任库、握手段位）与客户端身份都挂在**实例**上，只有走实例入口
     *          才看得到。循环刻意每次新建而不是让调用方复用：EventLoop 的停止请求是粘性的，第二次
     *          run() 会立刻返回，那时候的「空响应」就成了与 TLS 无关的假证据
     * @param url 目标地址
     * @param policy 出站 TLS 策略与信任库
     * @param clientCertificateFile 客户端身份证书；空串表示本端不带身份
     * @param clientKeyFile 客户端身份私钥
     * @param requestTimeout 整条请求的时限
     * @param poolConfig 池的参数；要验「响应正文上限」这一项就从这里传
     * @return ClientTlsAttemptOutcome 响应与身份装载结论
     */
    ClientTlsAttemptOutcome getWithClientTls(const std::string &url, const Core::TlsPolicy &policy, const std::string &clientCertificateFile, const std::string &clientKeyFile,
                                             const std::chrono::milliseconds requestTimeout, const HttpOutboundConnectionPool::Config &poolConfig = {})
    {
        Core::EventLoop         loop;
        HttpClient              client(loop, poolConfig, policy);
        ClientTlsAttemptOutcome outcome;
        if (!clientCertificateFile.empty())
        {
            outcome.identityLoaded = client.setClientCertificate(clientCertificateFile, clientKeyFile);
            if (!outcome.identityLoaded)
            {
                return outcome;
            }
        }
        // 惰性协程的帧记住的是闭包对象的地址：闭包必须先落到具名变量上再调用
        auto requestBody = [&loop, &client, &outcome, &url, requestTimeout]() -> Core::Task<>
        {
            outcome.response = co_await client.get(url, requestTimeout);
            loop.stop();
        };
        Core::Task<> request = requestBody();
        if (!request.isReady())
        {
            loop.scheduler().schedule(request.handle());
        }
        loop.run();
        return outcome;
    }

    /// 一个带池的客户端走完一串请求、中途再收一次口之后的结论
    struct PooledHttpsRunOutcome
    {
        std::vector<int>         statusCodes;            ///< 每条请求的状态码；0 表示这条失败了
        std::vector<std::string> reasonPhrases;          ///< 与 statusCodes 对齐的原因短语（h2 没有这一项）
        std::size_t              heldConnectionCount{0}; ///< 三条请求跑完时客户端池里留着的 h2 连接条数
        std::uint64_t            activeWhileHeld{0};     ///< 收口之前服务端在册的连接数
        std::uint64_t            activeAfterClose{0};    ///< closeIdleConnections 之后服务端在册的连接数
    };

    /**
     * @brief 用同一个带池的客户端依次 GET，然后「收口 → 等服务端察觉 → 再发一条」
     * @details 客户端、循环与服务端都活在调用方栈上：客户端一析构在册数就自己归零，那时候量到的 0
     *          证明不了是 closeIdleConnections() 的功劳。收口这一步必须在同一条循环上做——缓存的
     *          h2 连接归属那条循环，换一条循环再用它就是跨线程驱动别人的协程帧。
     * @param loop 客户端事件循环
     * @param client 被测客户端（持有池）
     * @param server 被测服务端（只读它的统计）
     * @param urls 依次发出的地址
     * @param settleTime 每段观测之前留给服务端的收口时间
     * @param outcome 就地收集结论
     */
    Core::Task<void> runPooledGets(Core::EventLoop &loop, HttpClient &client, TestHttpsServer &server, const std::vector<std::string> &urls,
                                   const std::chrono::milliseconds settleTime, PooledHttpsRunOutcome &outcome)
    {
        for (const std::string &url: urls)
        {
            const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
            outcome.statusCodes.push_back(response ? response->statusCode : 0);
            outcome.reasonPhrases.push_back(response ? response->reasonPhrase : std::string{});
        }
        outcome.heldConnectionCount = client.idleHttp2ConnectionCount();
        Core::Timer heldTimer(loop);
        co_await heldTimer.waitFor(settleTime);
        outcome.activeWhileHeld = server.stats().activeConnectionCount;

        client.closeIdleConnections();
        Core::Timer afterCloseTimer(loop);
        co_await afterCloseTimer.waitFor(settleTime);
        outcome.activeAfterClose = server.stats().activeConnectionCount;

        // 收口之后再来一条：池里没了可复用的就必须重开一条，这条 200 证明作废的连接没被交出去
        const std::unique_ptr<HttpClientResponse> reopened = co_await client.get(urls.front());
        outcome.statusCodes.push_back(reopened ? reopened->statusCode : 0);
        outcome.reasonPhrases.push_back(reopened ? reopened->reasonPhrase : std::string{});
        loop.stop();
        co_return;
    }

    /// 一条池化连接同时供两条并发请求时的结论
    struct SharedConnectionRunOutcome
    {
        int         warmupStatusCode{0};       ///< 暖场那条（当时池里还没东西可复用）的状态码
        std::string warmupReasonPhrase;        ///< 暖场那条的原因短语，h2 没有这一项
        int         firstStatusCode{0};        ///< 第一条并发请求的状态码；0 表示这条失败了
        int         secondStatusCode{0};       ///< 第二条并发请求的状态码；0 表示这条失败了
        std::string firstReasonPhrase;         ///< 第一条的原因短语
        std::string secondReasonPhrase;        ///< 第二条的原因短语
        std::size_t heldConnectionCount{0};    ///< 采样那一刻池里留着的 h2 连接条数
        std::size_t multiplexedStreamCount{0}; ///< 采样那一刻最忙一条连接上同时在途的流数
    };

    /**
     * @brief 发一条 GET，把状态码与原因短语写回调用方给的格子
     * @details 输出走引用而不是返回值：这条协程要和兄弟协程并发跑在同一个循环上，驱动协程不能
     *          直接 co_await 它（一个 Task 只能被读一次），只能事后读这些格子。
     * @param client 被测客户端（持有池）
     * @param url 请求地址
     * @param statusCode 输出：状态码；失败为 0
     * @param reasonPhrase 输出：原因短语，h2 没有这一项
     * @param finishedRequestCount 输出：已收口的条数，驱动协程据此判断何时可以停
     */
    Core::Task<void> runOnePooledGet(HttpClient &client, const std::string &url, int &statusCode, std::string &reasonPhrase, std::size_t &finishedRequestCount)
    {
        const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
        statusCode                                         = response ? response->statusCode : 0;
        reasonPhrase                                       = response ? response->reasonPhrase : std::string{};
        ++finishedRequestCount;
        co_return;
    }

    /**
     * @brief 每 1 毫秒让出一次循环，直到谓词为真或时限用完
     * @details 有界：上限用完既不抛也不挂，让调用方把「没等到」报成断言失败。用在客户端循环里，
     *          是因为这几处的时机只能由循环自己推进（测试线程一等就会把协程卡死）。
     * @param loop 承载等待的事件循环
     * @param isSatisfied 每轮询问一次的谓词
     * @param timeout 等待上限
     */
    Core::Task<void> waitUntil(Core::EventLoop &loop, const std::function<bool()> &isSatisfied, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!isSatisfied() && std::chrono::steady_clock::now() < deadline)
        {
            Core::Timer pollTimer(loop);
            co_await pollTimer.waitFor(std::chrono::milliseconds{1});
        }
        co_return;
    }

    /**
     * @brief 先暖场一条让池里留下连接，再并发发两条，并在两条都还在途时采样复用度
     * @details 采样点读的是客户端自己的计数（池里的连接条数与最忙一条上的在途流数），因此不需要
     *          「睡一段时间再看」——慢路由把那两条请求按在途中，采样协程每 1 毫秒让出一次循环，
     *          时机由计数本身决定。暖场那一条是必需的：冷池里两条并发请求各要开一条连接，
     *          那时候复用度为 1 才是对的行为。
     * @param loop 客户端事件循环
     * @param client 被测客户端（持有池）
     * @param warmUrl 暖场地址（快路由）
     * @param slowUrl 并发地址（慢路由）
     * @param outcome 就地收集结论
     */
    Core::Task<void> runSharedConnectionGets(Core::EventLoop &loop, HttpClient &client, const std::string &warmUrl, const std::string &slowUrl, SharedConnectionRunOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> warmup = co_await client.get(warmUrl);
        outcome.warmupStatusCode                         = warmup ? warmup->statusCode : 0;
        outcome.warmupReasonPhrase                       = warmup ? warmup->reasonPhrase : std::string{};

        std::size_t      finishedRequestCount = 0;
        Core::Task<void> first                = runOnePooledGet(client, slowUrl, outcome.firstStatusCode, outcome.firstReasonPhrase, finishedRequestCount);
        Core::Task<void> second               = runOnePooledGet(client, slowUrl, outcome.secondStatusCode, outcome.secondReasonPhrase, finishedRequestCount);
        if (!first.isReady())
        {
            loop.scheduler().schedule(first.handle());
        }
        if (!second.isReady())
        {
            loop.scheduler().schedule(second.handle());
        }

        // 两次有界的轮询：时机由计数本身决定，不靠睡
        co_await waitUntil(loop, [&client] { return client.http2MaximumInFlightStreamCount() >= 2U; }, std::chrono::seconds{2});
        outcome.heldConnectionCount    = client.idleHttp2ConnectionCount();
        outcome.multiplexedStreamCount = client.http2MaximumInFlightStreamCount();

        co_await waitUntil(loop, [&finishedRequestCount] { return finishedRequestCount >= 2U; }, std::chrono::seconds{4});
        // 采样之后必须把两条在飞的请求各自收到口再退出。留着挂起的协程随本帧一起销毁，等于把
        // 已经排进就绪队列的唤醒留在悬空帧上——下一次 runAll() 就是一次读后释放
        // （计数没到 2 也要等：请求自带时限，它一定会以失败收口，失败由 outcome 的断言去抓）
        if (!first.isReady())
        {
            co_await first;
        }
        if (!second.isReady())
        {
            co_await second;
        }
        loop.stop();
        co_return;
    }

    /// 「同一条池化 h2 连接上顺序跑一批请求」的结论
    struct SustainedRunOutcome
    {
        std::size_t successCount{0};        ///< 状态码与正文都对上的条数
        std::size_t mismatchCount{0};       ///< 正文与本轮序号对不上的条数（串流时会跳起来）
        std::size_t heldConnectionCount{0}; ///< 跑完之后池里留着的 h2 连接条数
        std::size_t inFlightAfterRun{0};    ///< 跑完之后最忙一条连接上还在途的流数
        std::string firstError;             ///< 第一条不合的说明
    };

    /**
     * @brief 在同一条池化 h2 连接上顺序发 requestCount 条 GET，每条的正文都得是它自己那个序号
     * @details 一条一条问是要把「复用」这一支跑长：头块的 HPACK 动态表、按流的发送窗口、在途流表
     *          这些连接级状态，两条请求时可能恰好错不开，跑两百条就会错开。序号是服务端递增出来的，
     *          因此「答串了」与「少了/多了字节」都能从正文本身看出来，而不是只看状态码 200。
     *          跑完再读池里的连接条数与在途流数：前者钉住「这一批确实走在复用上」，后者钉住
     *          「收口的流都从在途表里摘走了」——留在表里的话，容器的 LSan 会连着这份表一起报。
     * @param loop 承载这批请求的事件循环（跑完由本协程叫停）
     * @param client 被测客户端（持有池）
     * @param url 请求地址
     * @param requestCount 发几条
     * @param outcome 就地收集结论
     */
    Core::Task<void> runSustainedPooledGets(Core::EventLoop &loop, HttpClient &client, const std::string &url, const std::size_t requestCount, SustainedRunOutcome &outcome)
    {
        for (std::size_t index = 0U; index < requestCount; ++index)
        {
            const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
            const std::string                         expected = "tick-" + std::to_string(index);
            if (!response || response->statusCode != 200)
            {
                if (outcome.firstError.empty())
                {
                    outcome.firstError = "第 " + std::to_string(index) + " 条没拿到 200";
                }
                continue;
            }
            if (response->body != expected)
            {
                ++outcome.mismatchCount;
                if (outcome.firstError.empty())
                {
                    outcome.firstError = "第 " + std::to_string(index) + " 条的正文是 " + response->body;
                }
                continue;
            }
            ++outcome.successCount;
        }
        outcome.heldConnectionCount = client.idleHttp2ConnectionCount();
        outcome.inFlightAfterRun    = client.http2MaximumInFlightStreamCount();
        loop.stop();
        co_return;
    }

    /// 冷池并发一把的观测量
    struct ColdBurstOutcome
    {
        std::atomic<std::size_t> finishedCount{0};       ///< 已经收口的条数
        std::atomic<std::size_t> successCount{0};        ///< 拿到 200 的条数
        std::size_t              heldConnectionCount{0}; ///< 最后一条收口时池里留着的 h2 连接条数
        std::size_t              activeWhileHeld{0};     ///< 请求都还在途时服务器侧在册的连接数
    };

    /**
     * @brief 在请求都还挂在服务器里的时候，从服务器一侧读「同时有多少条连接」
     * @details 判据必须在请求还没答完的时候取：跑完之后池按端点只留一格，五条握手会被折叠成
     *          一格，那时再数就分不出「一条连接服务五条请求」与「五条连接各服务一条然后剩一条」。
     *          服务器侧的在册连接数没有这个问题——会话是谁建的它就数谁。
     * @note 读数的回调**按值收**：协程的参数住在帧里，首次恢复之后才读，收引用的话
     *       调用点那个临时 `std::function` 早就出了作用域（ASan 报 stack-use-after-scope 抓到过）。
     */
    Core::Task<void> sampleActiveConnections(Core::EventLoop &loop, const std::function<std::size_t()> reader, std::size_t *const destination)
    {
        Core::Timer timer(loop);
        co_await timer.waitFor(std::chrono::milliseconds{150});
        *destination = reader();
        co_return;
    }

    /**
     * @brief 冷池上并发发出的一条 GET；最后一条收口时记下池里的连接条数并叫停循环
     * @details 「池里剩几条连接」只有在全都跑完之后才有意义，因此把读数放在最后一条的收尾里做。
     *          收口计数用原子量：这几路协程各自在自己的挂起/恢复上来写同一个 outcome。
     */
    Core::Task<void> fetchOneOnColdPool(Core::EventLoop &loop, HttpClient &client, const std::string &url, const std::size_t expectedCount, ColdBurstOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
        if (response != nullptr && response->statusCode == 200)
        {
            outcome.successCount.fetch_add(1U, std::memory_order_relaxed);
        }
        if (outcome.finishedCount.fetch_add(1U, std::memory_order_acq_rel) + 1U == expectedCount)
        {
            outcome.heldConnectionCount = client.idleHttp2ConnectionCount();
            loop.stop();
        }
        co_return;
    }

    /// 建连必败那一路的观测量：几条并发有没有各自收场
    struct RefusalBurstOutcome
    {
        std::atomic<std::size_t> finishedCount{0}; ///< 已经回来的条数（有人挂住就数不满）
        std::atomic<std::size_t> refusedCount{0};  ///< 拿回空响应的条数
    };

    /**
     * @brief 冷池上并发发出的一条 GET，预期拿回空响应；最后一条收口时叫停循环
     * @param loop 客户端的事件循环
     * @param client 被测客户端（用它的池）
     * @param url 必败的目标（连一个刚关掉的监听端口）
     * @param timeout 本次请求的时限
     * @param expectedCount 一共几条，用于认出「最后一条」
     * @param outcome 输出：收口与拒绝的条数
     */
    Core::Task<void> fetchOneUntilRefused(Core::EventLoop &loop, HttpClient &client, const std::string &url, const std::chrono::milliseconds timeout,
                                          const std::size_t expectedCount, RefusalBurstOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> response = co_await client.get(url, timeout);
        if (response == nullptr)
        {
            outcome.refusedCount.fetch_add(1U, std::memory_order_relaxed);
        }
        if (outcome.finishedCount.fetch_add(1U, std::memory_order_acq_rel) + 1U == expectedCount)
        {
            loop.stop();
        }
        co_return;
    }

    /**
     * @brief 到点就叫停循环的看门狗：把「有人永远回不来」从挂死的用例变成一条红断言
     * @details 没有它时，一次漏掉的结算会让 loop.run() 转到天荒地老（等建连的那一步本身不设时限，
     *          靠领导者结算唤醒）。这里给一个远超正常收场耗时的宽限窗，到点直接 stop。
     * @param loop 要叫停的循环
     * @param grace 宽限时长
     */
    Core::Task<void> stopLoopAfterGrace(Core::EventLoop &loop, const std::chrono::milliseconds grace)
    {
        Core::Timer timer(loop);
        co_await timer.waitFor(grace);
        loop.stop();
        co_return;
    }

    /**
     * @brief 注册一条「进门即计数、按住 400 毫秒再回正文」的 GET 路由（路径 /slow）
     * @details 400 毫秒是给客户端侧留的观察窗口：慢到够在客户端循环里采样到「请求仍在途」，
     *          又远不到默认的请求时限。等待挂在服务器的循环上，不占线程。
     * @param router 被测服务器的路由器
     * @param serverLoop 服务器的事件循环（定时器挂在它上面）
     * @param entryCount 可选的输出：处理器每被进一次加一，空指针表示不计数
     */
    void registerHoldingRoute(Router &router, Core::EventLoop &serverLoop, std::atomic<std::size_t> *const entryCount)
    {
        // 同一条法登记 GET 与 POST 两个方法：一侧的既有用例按 GET 问它，另一侧要拿 POST 测
        // 「非幂等请求不许重发」——按方法分路由是路由器的常态，不是一件事的两种写法
        const auto handler = [&serverLoop, entryCount](HttpRequest &, HttpResponse &response) -> Core::Task<>
        {
            if (entryCount != nullptr)
            {
                ++(*entryCount);
            }
            Core::Timer processingTimer(serverLoop);
            co_await processingTimer.waitFor(std::chrono::milliseconds{400});
            response.setBody("served-slow");
            co_return;
        };
        router.get("/slow", handler);
        router.post("/slow", handler);
    }

    /// 「请求还在途就收口空闲连接」的结论
    struct CloseWhileBusyRunOutcome
    {
        int         warmupStatusCode{0};   ///< 暖场那条的状态码：它把连接放进池里，是后面的前提
        int         statusCode{0};         ///< 在途那条请求最终的状态码
        std::string reasonPhrase;          ///< 在途那条的原因短语，h2 没有这一项
        std::size_t inFlightWhenClosed{0}; ///< 收口那一刻最忙一条连接上的在途流数
        std::size_t heldBeforeClose{0};    ///< 收口之前池里留着的 h2 连接条数
        std::size_t heldAfterClose{0};     ///< 收口之后池里留着的 h2 连接条数
    };

    /**
     * @brief 暖场一条、再起一条慢请求，等它上了线就调用 closeIdleConnections()
     * @details 收口点刻意落在「请求在途」这段时间里：这条入口的契约是只收空闲的，正被人用的那条
     *          不能掐。判据用「慢路由被进了几次」而不是「请求是否 200」——把在途请求掐断之后，
     *          调用方仍可能走一遍重连重来，状态码照样是 200，只有重复进入会露出那次重发。
     * @param loop 客户端事件循环
     * @param client 被测客户端（持有池）
     * @param warmUrl 暖场地址（快路由）
     * @param slowUrl 在途地址（慢路由）
     * @param outcome 就地收集结论
     */
    Core::Task<void> runCloseWhileBusy(Core::EventLoop &loop, HttpClient &client, const std::string &warmUrl, const std::string &slowUrl, CloseWhileBusyRunOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> warmup = co_await client.get(warmUrl);
        outcome.warmupStatusCode                         = warmup ? warmup->statusCode : 0;

        std::size_t      finishedRequestCount = 0;
        Core::Task<void> busy                 = runOnePooledGet(client, slowUrl, outcome.statusCode, outcome.reasonPhrase, finishedRequestCount);
        if (!busy.isReady())
        {
            loop.scheduler().schedule(busy.handle());
        }

        co_await waitUntil(loop, [&client] { return client.http2MaximumInFlightStreamCount() >= 1U; }, std::chrono::seconds{2});
        outcome.inFlightWhenClosed = client.http2MaximumInFlightStreamCount();
        outcome.heldBeforeClose    = client.idleHttp2ConnectionCount();
        client.closeIdleConnections();
        outcome.heldAfterClose = client.idleHttp2ConnectionCount();

        co_await waitUntil(loop, [&finishedRequestCount] { return finishedRequestCount >= 1U; }, std::chrono::seconds{4});
        loop.stop();
        co_return;
    }

    /// 只发一条被测请求的结论（前面先走一条暖场请求把连接放进池里）
    struct SingleRunOutcome
    {
        int                                 warmupStatusCode{0}; ///< 暖场那条的状态码
        std::unique_ptr<HttpClientResponse> response;            ///< 被测那条的响应；空表示失败
    };

    /// 「问一句 host 回显给你」那条请求的结论
    struct HostEchoRunOutcome
    {
        int         statusCode{0}; ///< 状态码
        std::string echoedHost;    ///< 服务器侧看到的 host / :authority 原文
    };

    /**
     * @brief 发一条 GET，把服务器回显的权威主机带回来
     * @param loop 客户端事件循环
     * @param client 被测客户端
     * @param url 请求地址
     * @param outcome 就地收集结论
     */
    Core::Task<void> runHostEcho(Core::EventLoop &loop, HttpClient &client, const std::string &url, HostEchoRunOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
        if (response != nullptr)
        {
            outcome.statusCode = response->statusCode;
            outcome.echoedHost = response->body;
        }
        loop.stop();
        co_return;
    }

    /**
     * @brief 请求已整个发出、却没等到回音时，出站侧不许重来一次
     * @details h2 一条连接上跑几条流，一条请求被时限掐掉会把同一条通路上的兄弟一起带走。这时
     *          「有没有收到过字节」这一位判不出该不该重发：POST 已整个交上通路、对端只是还没答完，
     *          重发就是把非幂等请求做两遍。判据用路由自己的进入次数——它数的就是「这个请求被交付了几次」。
     * @param loop 客户端事件循环
     * @param client 被测客户端（持有池）
     * @param warmUrl 暖场地址（把连接放进池里，后面那条才走「复用」这一支）
     * @param slowUrl 慢地址：本端会在对端答完之前放弃
     * @param outcome 就地收集结论
     */
    Core::Task<void> runNoReplayAfterSend(Core::EventLoop &loop, HttpClient &client, const std::string &warmUrl, const std::string &slowUrl, SingleRunOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> warmup = co_await client.get(warmUrl);
        outcome.warmupStatusCode                         = warmup ? warmup->statusCode : 0;

        outcome.response = co_await client.post(slowUrl, "text/plain", "payload-once", std::chrono::milliseconds{150});
        // 给「悄悄重发的那一遍」留足落地时间：它要重做 TLS 握手，比正常路径更慢
        Core::Timer settleTimer(loop);
        co_await settleTimer.waitFor(std::chrono::milliseconds{900});
        loop.stop();
        co_return;
    }

    /// 池里那条 h2 连接被对端在空闲期收掉之后，再发一条请求的结论
    struct IdleGraceRunOutcome
    {
        std::size_t heldAfterFirst{0};   ///< 第一条之后池里留着的 h2 连接条数
        int         firstStatusCode{0};  ///< 第一条的状态码；0 表示失败
        int         secondStatusCode{0}; ///< 宽限期之后再发一条的状态码；0 表示失败
    };

    /**
     * @brief 在同一条循环、同一个客户端上跑「GET 暖场 → 等服务端收掉空闲连接 → 再一条请求」
     * @details 宽限期里本端不探测那条连接，也不泵它——留着的正是一份「看起来健康、其实已经被对端
     *          收了」的存货，这就是 keep-alive 的固有竞态在 h2 上的形状。两段必须在同一条协程里
     *          跑完：换一条循环就等于换一个池。
     * @details 第二条之后再留一段落地时间：如果本端悄悄重发了，那一遍要重做 DNS 与 TLS 握手，
     *          比正常路径慢，不等够就会漏看服务端的进入次数。
     * @param loop 客户端事件循环
     * @param client 被测客户端（持有池）
     * @param warmUrl 暖场地址：它把连接放进池里，是后面那段的前提
     * @param secondUrl 宽限期之后再发一条的地址
     * @param isSecondPost true 第二条走 POST（带正文），否则走 GET
     * @param idleGrace 留给服务端收口空闲连接的宽限期
     * @param secondTimeout 第二条请求的整体时限
     * @param outcome 就地收集结论
     */
    Core::Task<void> runWarmThenSecondAfterIdleGrace(Core::EventLoop &loop, HttpClient &client, const std::string &warmUrl, const std::string &secondUrl, const bool isSecondPost,
                                                     const std::chrono::milliseconds idleGrace, const std::chrono::milliseconds secondTimeout, IdleGraceRunOutcome &outcome)
    {
        const std::unique_ptr<HttpClientResponse> first = co_await client.get(warmUrl);
        outcome.firstStatusCode                         = first ? first->statusCode : 0;
        outcome.heldAfterFirst                          = client.idleHttp2ConnectionCount();
        Core::Timer graceTimer(loop);
        co_await graceTimer.waitFor(idleGrace);
        std::unique_ptr<HttpClientResponse> second;
        if (isSecondPost)
        {
            second = co_await client.post(secondUrl, "text/plain", "payload-once", secondTimeout);
        } else
        {
            second = co_await client.get(secondUrl, secondTimeout);
        }
        outcome.secondStatusCode = second ? second->statusCode : 0;
        Core::Timer settleTimer(loop);
        co_await settleTimer.waitFor(std::chrono::milliseconds{400});
        loop.stop();
        co_return;
    }

    /**
     * @brief 临时把 SSL_CERT_FILE 指向某张证书，让出站客户端信任它
     * @details 客户端只认系统 CA 库（SSL_CTX_set_default_verify_paths），而仓库夹具是自签的；
     *          OpenSSL 解析默认路径时认 SSL_CERT_FILE 这个环境变量，于是用例借此把夹具证书
     *          当成受信根。**必须在进程内第一次 HTTPS 客户端请求之前设好**——SSL_CTX 是那时
     *          惰性创建并缓存的。析构还原原值（没设过就清掉）
     */
    class ScopedTrustedCertificateFile
    {
    public:
        explicit ScopedTrustedCertificateFile(const std::filesystem::path &certificatePath)
        {
            m_previousValue = readEnvironment("SSL_CERT_FILE");
#if ASYN_PLATFORM_WIN32
            static_cast<void>(::_putenv_s("SSL_CERT_FILE", certificatePath.string().c_str()));
#else
            static_cast<void>(::setenv("SSL_CERT_FILE", certificatePath.string().c_str(), 1));
#endif
        }

        ~ScopedTrustedCertificateFile()
        {
#if ASYN_PLATFORM_WIN32
            static_cast<void>(::_putenv_s("SSL_CERT_FILE", m_previousValue.c_str()));
#else
            if (m_previousValue.empty())
            {
                static_cast<void>(::unsetenv("SSL_CERT_FILE"));
            } else
            {
                static_cast<void>(::setenv("SSL_CERT_FILE", m_previousValue.c_str(), 1));
            }
#endif
        }

        ScopedTrustedCertificateFile(const ScopedTrustedCertificateFile &) = delete;

        ScopedTrustedCertificateFile &operator=(const ScopedTrustedCertificateFile &) = delete;

    private:
        /**
         * @brief 读一个环境变量并拷贝成 std::string
         * @details MSVC 在 /W4 下把 std::getenv 判为弃用（C4996），Windows 侧改用 _dupenv_s；
         *          两种实现都立即拷贝，调用方不保留指向环境块的指针
         * @param variableName 环境变量名
         * @return std::string 取值；未设置时为空串
         */
        [[nodiscard]] static std::string readEnvironment(const char *variableName)
        {
#if ASYN_PLATFORM_WIN32
            char  *rawValue      = nullptr;
            size_t valueCapacity = 0;
            if (::_dupenv_s(&rawValue, &valueCapacity, variableName) != 0 || rawValue == nullptr)
            {
                return {};
            }
            std::string variableValue(rawValue);
            std::free(rawValue);
            return variableValue;
#else
            const char *rawValue = std::getenv(variableName);
            return rawValue != nullptr ? std::string(rawValue) : std::string{};
#endif
        }

        std::string m_previousValue; ///< 之前的值；原本没设过时为空串
    };

    TEST(HttpsServer, ServesRequestAndCountsStats)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout)) << "HTTPS 请求未得到完整响应";

        EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << "响应状态行不是 200：「" << receivedText << "」";

        // 响应发出与计数落账之间隔着一次协程恢复，因此按条件轮询而不是立刻断言
        ASSERT_TRUE(waitForCondition([&fixture] { return fixture.server().stats().totalRequestCount >= 1; }, kWaitTimeout)) << "统计未在时限内记下这条 HTTPS 请求";

        const HttpServerStats stats = fixture.server().stats();
        EXPECT_EQ(stats.totalRequestCount, 1u) << "累计请求条数不符（不含解析失败）";
        EXPECT_EQ(stats.status2xxCount, 1u) << "200 没有计入 2xx";
        EXPECT_EQ(stats.status1xxCount, 0u) << "1xx 计数应为 0";
        EXPECT_EQ(stats.status3xxCount, 0u) << "3xx 计数应为 0";
        EXPECT_EQ(stats.status4xxCount, 0u) << "4xx 计数应为 0";
        EXPECT_EQ(stats.status5xxCount, 0u) << "5xx 计数应为 0";
        EXPECT_EQ(stats.badRequestCount, 0u) << "正常报文被记成了协议错误";

        // 时序纪律：先等客户端读到关闭，再断言会话收口
        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "HTTPS 会话收口后未从连接管理器摘除";
        EXPECT_EQ(fixture.server().stats().activeConnectionCount, 0u) << "连接已关闭，活跃连接数没有回落";
    }

    /**
     * @brief HTTPS 侧也能导出 /metrics 与 /healthz：抓一次端点就能看到 TLS 路径上的计数
     * @details 采集本来就在做（Http2Session 一直向本服务器的采集端计数），缺的只是把读数暴露出来；
     *          顺带钉住「端点自身那条请求也计入请求数」——它就是一条普通路由
     */
    TEST(HttpsServer, ExposesMetricsAndHealthEndpoints)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{},
                                          [](HttpsServer &server)
                                          {
                                              server.enableMetricsEndpoint();
                                              server.enableHealthEndpoint();
                                          });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        TlsLoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout)) << "HTTPS 请求未得到响应";

        // 健康检查端点：固定正文
        std::string healthText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /healthz HTTP/1.1"), kWaitTimeout)) << "健康检查请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(healthText, kHealthCheckResponseBody, 1, kWaitTimeout)) << "健康检查端点没有回固定正文：「" << healthText << "」";

        // 指标端点：值行里应当有**三条**请求——先前那条 /hello、/healthz，以及本条 /metrics 自己
        // （计数发生在派发业务之前，因此它自己也算在内）。等的是值行而不是指标名：
        // HELP/TYPE 行先到，只等名字会在取到值之前就返回
        std::string metricsText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /metrics HTTP/1.1"), kWaitTimeout)) << "指标请求未能写入";
        EXPECT_TRUE(client.waitForTextOccurrences(metricsText, "asyn_http_requests_total 3", 1, kWaitTimeout))
                << "抓取这一刻应当已记下三条请求（/hello、/healthz 与 /metrics 自己）：「" << metricsText << "」";
        // 状态码类在**第二次抓取**里核对：响应计数发生在「响应已排入待发字节」之后、渲染那一刻之前，
        // 因此第一次抓取的正文里还没有它自己那条。第二次抓取时前三条响应都已落账
        std::string secondMetricsText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /metrics HTTP/1.1"), kWaitTimeout)) << "第二次指标请求未能写入";
        EXPECT_TRUE(client.waitForTextOccurrences(secondMetricsText, "asyn_http_responses_total{status_class=\"2xx\"} 3", 1, kWaitTimeout))
                << "成功响应的状态码类没有计入：「" << secondMetricsText << "」";
    }

    /**
     * @brief 客户端校验主机名：证书名字对得上就正常握手并拿到响应
     * @details 夹具证书 test_ip_cert.pem 是仓库预生成的自签证书，SAN 只有 IP:127.0.0.1。
     *          用例把它同时当「服务端身份」与「受信根」，链校验必然通过——能过就只说明客户端走的是
     *          IP 那一路校验（X509_VERIFY_PARAM_set1_ip_asc），这正是要钉住的。
     */
    TEST(HttpsServer, ClientAcceptsCertificateMatchingTheRequestedHost)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string                         url      = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";
        const std::unique_ptr<HttpClientResponse> response = doHttpsGet(url);
        ASSERT_NE(response, nullptr) << "证书名字与请求的主机名一致，握手却被拒";
        EXPECT_EQ(response->statusCode, 200);
        EXPECT_NE(response->body.find("served-hello"), std::string::npos) << "正文：" << response->body;
    }

    /**
     * @brief 钉住：ALPN 选到 h2 时，出站客户端改按 HTTP/2 说话，方法与正文都照样带过去
     * @details 服务端的 ALPN 偏好里 h2 排在 http/1.1 之前，这条 TLS 连接协商出的结果必然是 h2。客户端
     *          若不认这个结果、照旧写请求行，握手之后就是帧格式错乱的失败。两条判据分开看：200 与回显
     *          一致说明这趟真走通了；**原因短语为空**说明状态是从 :status 解出来的——HTTP/1.1 的答一定
     *          带 "OK"（本服务端的状态行照常写原因短语），所以这一条能把「走了 h2」与「退回 h1」分开。
     */
    TEST(HttpsServer, OutboundClientSpeaksHttp2WhenAlpnSelectsIt)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100},
                [](Router &router, Core::EventLoop &)
                {
                    router.post("/h2echo",
                                [](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                                {
                                    response.setBody(request.body());
                                    co_return;
                                });
                },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        const std::string host = "https://127.0.0.1:" + std::to_string(fixture.listeningPort());

        const std::unique_ptr<HttpClientResponse> got = doHttpsGet(host + "/hello");
        ASSERT_NE(got, nullptr) << "协商出 h2 之后这趟请求没走通：客户端多半仍按 HTTP/1.1 写字节";
        EXPECT_EQ(got->statusCode, 200);
        EXPECT_NE(got->body.find("served-hello"), std::string::npos) << "正文：" << got->body;
        EXPECT_TRUE(got->reasonPhrase.empty()) << "拿到了原因短语「" << got->reasonPhrase << "」：这趟走的是 HTTP/1.1，ALPN 结果没被采纳";

        const std::string                         payload = "hello over h2";
        const std::unique_ptr<HttpClientResponse> posted  = doHttpsPost(host + "/h2echo", "text/plain", payload);
        ASSERT_NE(posted, nullptr) << "带正文的 h2 出站请求失败";
        EXPECT_EQ(posted->statusCode, 200);
        EXPECT_EQ(posted->body, payload) << "方法或正文在换乘 h2 时丢了";
    }

    /**
     * @brief 钉住：带池的客户端把 h2 连接留在池里复用，收口时真的把它关掉
     * @details 三条请求只该付一次 TLS 握手与一次 h2 前奏。四条判据一起看，缺一条都定不了位：客户端侧
     *          留着一根（不缓存的话这里必是 0）、服务端侧在册也是这一根、closeIdleConnections() 之后
     *          服务端侧归零（本端若只丢指针就不会归零）、最后再发一条仍拿到 200（作废的连接没被错交出
     *          去）。原因短语全空顺带钉住这四条走的都是 h2 而不是退回 HTTP/1.1。
     */
    TEST(HttpsServer, PooledClientHoldsAndReleasesItsHttp2Connection)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";
        // urls 必须是具名对象：驱动协程按引用拿着它，跨过 co_await 之后还要读
        const std::vector<std::string> urls{url, url, url};
        Core::EventLoop                loop;
        HttpClient                     client(loop);
        PooledHttpsRunOutcome          outcome;
        auto                           work = runPooledGets(loop, client, fixture.server(), urls, std::chrono::milliseconds{200}, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_EQ(outcome.statusCodes.size(), 4U);
        for (const int statusCode: outcome.statusCodes)
        {
            EXPECT_EQ(statusCode, 200);
        }
        for (const std::string &reasonPhrase: outcome.reasonPhrases)
        {
            EXPECT_TRUE(reasonPhrase.empty()) << "有原因短语＝那条其实是 HTTP/1.1 的答，ALPN 结果没被采纳";
        }
        EXPECT_EQ(outcome.heldConnectionCount, 1U) << "三条请求之后池里没留着 h2 连接：每条都在重做握手";
        EXPECT_EQ(outcome.activeWhileHeld, 1U) << "服务端侧的连接数与客户端对不上：复用没成立";
        EXPECT_EQ(outcome.activeAfterClose, 0U) << "closeIdleConnections 之后服务端还认着这条连接：本端只是丢了指针";
    }

    /**
     * @brief 钉住：池里已有一条 h2 连接时，两条并发请求共用这一条而不是各开一条
     * @details 判据是采样那一刻「池里一条连接、最忙那条上两条在途流」：一台主机只留一条连接，这个组合
     *          只能解释为两条请求共用了同一条通路。若取用时把连接从池里摘走（那是 h1 的租用语义），
     *          第二条就问不到货、只能重做一遍 TLS 握手，采样会停在「一条连接、一条流」——而那正是
     *          本条要拦住的退化。两条都以 200 收口，顺带钉住复用没把并发请求做丢；原因短语全空钉住
     *          这三条走的都是 h2。
     */
    TEST(HttpsServer, PooledClientMultiplexesConcurrentRequestsOnOneConnection)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, [](Router &router, Core::EventLoop &serverLoop) { registerHoldingRoute(router, serverLoop, nullptr); },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string hostPrefix = "https://127.0.0.1:" + std::to_string(fixture.listeningPort());
        // 两个地址必须是具名对象：驱动协程按引用拿着它们，跨过 co_await 之后还要读
        const std::string          warmUrl = hostPrefix + "/hello";
        const std::string          slowUrl = hostPrefix + "/slow";
        Core::EventLoop            loop;
        HttpClient                 client(loop);
        SharedConnectionRunOutcome outcome;
        auto                       work = runSharedConnectionGets(loop, client, warmUrl, slowUrl, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();
        EXPECT_EQ(outcome.warmupStatusCode, 200) << "暖场那条没成功：后面的采样就没有前提";
        EXPECT_TRUE(outcome.warmupReasonPhrase.empty()) << "暖场那条走的是 HTTP/1.1：ALPN 没选到 h2，复用无从谈起";
        EXPECT_EQ(outcome.heldConnectionCount, 1U) << "两条并发请求在途时池里不是一条连接：复用没成立";
        EXPECT_EQ(outcome.multiplexedStreamCount, 2U) << "最忙那条连接上只有一条在途流：第二条请求另开了一条连接";
        EXPECT_EQ(outcome.firstStatusCode, 200);
        EXPECT_EQ(outcome.secondStatusCode, 200) << "两条并发请求有条没收口：复用把请求做丢了";
        EXPECT_TRUE(outcome.firstReasonPhrase.empty());
        EXPECT_TRUE(outcome.secondReasonPhrase.empty());
    }

    /**
     * @brief 钉住：同一条池化 h2 连接上顺序跑两百条请求，每条都拿回自己那份正文
     * @details 连接级状态（HPACK 动态表、按流发送窗口、在途流表）在两条请求时可能恰好错不开，跑长一
     *          批才错得开。正文由服务端按进门次序编号，因此「答串到别的流上」「字节多一截少一截」都会
     *          从正文本身露出来，而不是被一个 200 盖过去。另外两条读数把复用与收尾各钉一遍：跑完之后
     *          池里仍是一条连接（这一批确实走的是复用），在途流为零（收口的流都从表里摘走了——留着
     *          的话容器的 LSan 会连着这份表一起报出来）。
     */
    TEST(HttpsServer, PooledClientServesSustainedTrafficOnOneHttp2Connection)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        std::atomic<std::size_t>  tickCounter{0U};
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100},
                [&tickCounter](Router &router, Core::EventLoop &)
                {
                    const auto handler = [&tickCounter](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        const std::size_t ordinal = tickCounter.fetch_add(1U);
                        response.setBody("tick-" + std::to_string(ordinal));
                        co_return;
                    };
                    router.get("/tick", handler);
                },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        // 地址必须是具名对象：驱动协程按引用拿着它，跨过每一次 co_await 之后还要读
        const std::string     url           = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/tick";
        constexpr std::size_t kRequestCount = 200U;
        Core::EventLoop       loop;
        HttpClient            client(loop);
        SustainedRunOutcome   outcome;
        auto                  work = runSustainedPooledGets(loop, client, url, kRequestCount, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_TRUE(outcome.firstError.empty()) << "第一条不合的：" << outcome.firstError;
        EXPECT_EQ(outcome.successCount, kRequestCount) << "状态码与正文都对上的条数不足";
        EXPECT_EQ(outcome.mismatchCount, 0U) << "有请求拿回了别人的正文：复用把响应串到别的流上了";
        EXPECT_EQ(outcome.heldConnectionCount, 1U) << "这一批没走复用在同一条连接上";
        EXPECT_EQ(outcome.inFlightAfterRun, 0U) << "收口之后在途流没清零：流表在长连接上越积越多";
    }

    /**
     * @brief 冷池上同时进来的请求要共用一次 h2 握手，而不是每条各握手一遍
     * @details 顺序复用早就成立（上一条），但池子空的时候同时进来的请求各自去建连：TCP、TLS、ALPN、
     *          h2 前奏重复 N 遍——而 HTTP/2 的立身之本就是一条连接上多路复用，后到的请求应该等第一条
     *          握手完成再用它。判据取「请求都还挂在服务器里时，服务器在册几条连接」（采样点为什么必须
     *          在请求完成之前，见 sampleActiveConnections）；池里的条数只作旁证。
     */
    TEST(HttpsServer, CoalescesConcurrentColdStartsOntoOneHttp2Connection)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, [](Router &router, Core::EventLoop &serverLoop) { registerHoldingRoute(router, serverLoop, nullptr); },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string     url                 = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/slow";
        constexpr std::size_t kConcurrentRequests = 5U;

        Core::EventLoop               loop;
        HttpClient                    client(loop);
        ColdBurstOutcome              outcome;
        std::vector<Core::Task<void>> workers;
        workers.reserve(kConcurrentRequests + 1U);
        workers.push_back(sampleActiveConnections(loop, [&fixture] { return fixture.server().activeConnectionCount(); }, &outcome.activeWhileHeld));
        for (std::size_t index = 0U; index < kConcurrentRequests; ++index)
        {
            workers.push_back(fetchOneOnColdPool(loop, client, url, kConcurrentRequests, outcome));
        }
        for (Core::Task<void> &worker: workers)
        {
            if (!worker.isReady())
            {
                loop.scheduler().schedule(worker.handle());
            }
        }
        loop.run();

        EXPECT_EQ(outcome.successCount.load(std::memory_order_acquire), kConcurrentRequests) << "有请求没拿到 200";
        EXPECT_EQ(outcome.activeWhileHeld, 1U) << "冷池上 " << kConcurrentRequests << " 条并发在服务器上开了 " << outcome.activeWhileHeld
                                               << " 条连接：握手没被合并，重复付了这么多遍 TCP+TLS+前奏";
        EXPECT_EQ(outcome.heldConnectionCount, 1U) << "跑完之后池里不止一条：这一项单独看没有鉴别力，见采样点的说明";
    }

    /**
     * @brief 钉住：领导者建连失败时，等它的人要各自收场，而不是永远等一次不会来的结算
     * @details 合并握手把等待挂在「建连资格」上：领导者每条出口都得归还资格，漏一处就把这个端点的
     *          出站请求**永久**堵死——等建连那一步自己不设时限，只有 settle 叫得醒它，比 h2 帧那种
     *          「退化」的漏还严重一档。连一个刚关掉的监听端口是确定复现「建连必败」的写法（比挑一个
     *          自认为空闲的端口可靠）。判据取「每条都自己回来了」：漏结算时那几条会挂在等待里，
     *          循环只由看门狗叫停，收口计数数不满。
     */
    TEST(HttpsServer, FailingEstablishmentHandsTheEndpointBackToItsWaiters)
    {
        std::uint16_t closedPort = 0U;
        {
            RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{50});
            ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";
            closedPort = fixture.listeningPort();
            ASSERT_NE(closedPort, 0U);
        }

        // 刻意走明文：客户端的 SSL_CTX 是进程级缓存，在这里建一次会把后跑的 TLS 用例的受信配置
        // 挡在外面（NamesTheStageThatFailed 同一条顾虑）
        const std::string     url                 = "http://127.0.0.1:" + std::to_string(closedPort) + "/";
        constexpr std::size_t kConcurrentRequests = 3U;

        Core::EventLoop               loop;
        HttpClient                    client(loop);
        RefusalBurstOutcome           outcome;
        std::vector<Core::Task<void>> workers;
        workers.reserve(kConcurrentRequests + 1U);
        workers.push_back(stopLoopAfterGrace(loop, std::chrono::milliseconds{6000}));
        for (std::size_t index = 0U; index < kConcurrentRequests; ++index)
        {
            workers.push_back(fetchOneUntilRefused(loop, client, url, std::chrono::milliseconds{1000}, kConcurrentRequests, outcome));
        }
        for (Core::Task<void> &worker: workers)
        {
            if (!worker.isReady())
            {
                loop.scheduler().schedule(worker.handle());
            }
        }
        loop.run();

        const std::size_t finished = outcome.finishedCount.load(std::memory_order_acquire);
        EXPECT_EQ(finished, kConcurrentRequests) << "只有 " << finished << " 条收场，其余的挂在「等同一端点的建连」里没回来：建连失败那一条出口"
                                                 << "没归还建连资格，这个端点往后的出站请求会一直等下去";
        EXPECT_EQ(outcome.refusedCount.load(std::memory_order_acquire), kConcurrentRequests) << "连的是个没人听的端口，却有请求拿回了响应";
    }

    /**
     * @brief 钉住：合并只针对「能共享的那一次握手」，明文 HTTP/1.1 的并发冷启动各建各的、全都成事
     * @details h1 的连接一次只租给一个请求，领导者没有把结论交进池给别人复用的那一步，所以它在走交换
     *          之前就得把资格还回去。漏还时等待者既拿不到可复用的连接、也叫不醒自己，只能挂到各自的
     *          三十秒时限上——判据取「三条全拿到 200」，另配一条看门狗把「挂住」折成红断言而不是跑到天荒地老。
     */
    TEST(HttpsServer, ConcurrentColdStartsOnHttp1EachBuildTheirOwnConnection)
    {
        RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/tick",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                        {
                                                            response.setBody("ok");
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";

        const std::string     url                 = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/tick";
        constexpr std::size_t kConcurrentRequests = 3U;

        Core::EventLoop               loop;
        HttpClient                    client(loop);
        ColdBurstOutcome              outcome;
        std::vector<Core::Task<void>> workers;
        workers.reserve(kConcurrentRequests + 1U);
        workers.push_back(stopLoopAfterGrace(loop, std::chrono::milliseconds{6000}));
        for (std::size_t index = 0U; index < kConcurrentRequests; ++index)
        {
            workers.push_back(fetchOneOnColdPool(loop, client, url, kConcurrentRequests, outcome));
        }
        for (Core::Task<void> &worker: workers)
        {
            if (!worker.isReady())
            {
                loop.scheduler().schedule(worker.handle());
            }
        }
        loop.run();

        EXPECT_EQ(outcome.successCount.load(std::memory_order_acquire), kConcurrentRequests)
                << "并发 " << kConcurrentRequests << " 条明文请求只成了 " << outcome.successCount.load(std::memory_order_acquire)
                << " 条：h1 这一侧的建连资格没在走交换之前归还，等它的人醒不过来";
    }

    /**
     * @brief 钉住：closeIdleConnections() 只收空闲的那部分，正被人用的 h2 连接不受影响
     * @details 主判据是「慢路由只被进了一次」：把在途的那条也关掉时，客户端会重连再发一遍，状态码
     *          仍是 200，只有重复进入能露出那次悄悄的重发。另外三条把前提与放手一侧钉住：收口那一刻
     *          请求确实在途（否则这条什么都没测到）、收口之前池里确实有一条、收口之后池里确实空了。
     */
    TEST(HttpsServer, CloseIdleConnectionsLeavesInFlightHttp2RequestAlone)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        // 计数器先声明、夹具后声明：处理器在服务器的循环线程上摸它，夹具销毁时那条线程已经 join 完
        std::atomic<std::size_t>  slowHandlerEntryCount{0};
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, [&slowHandlerEntryCount](Router &router, Core::EventLoop &serverLoop)
                { registerHoldingRoute(router, serverLoop, &slowHandlerEntryCount); }, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string        hostPrefix = "https://127.0.0.1:" + std::to_string(fixture.listeningPort());
        const std::string        warmUrl    = hostPrefix + "/hello";
        const std::string        slowUrl    = hostPrefix + "/slow";
        Core::EventLoop          loop;
        HttpClient               client(loop);
        CloseWhileBusyRunOutcome outcome;
        auto                     work = runCloseWhileBusy(loop, client, warmUrl, slowUrl, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(outcome.warmupStatusCode, 200) << "暖场那条没成功：池里就没有等着收口的连接";
        EXPECT_EQ(outcome.inFlightWhenClosed, 1U) << "收口那一刻那条请求并不在途：这条用例什么都没测到";
        EXPECT_EQ(outcome.heldBeforeClose, 1U) << "收口之前池里不是一条连接：前提没成立";
        EXPECT_EQ(outcome.heldAfterClose, 0U) << "closeIdleConnections 之后池里还留着那条：本端没放手";
        EXPECT_EQ(outcome.statusCode, 200) << "在途的那条请求被收口打断了";
        EXPECT_EQ(slowHandlerEntryCount.load(std::memory_order_acquire), 1U) << "慢路由被进了两次：在途请求被掐断之后又悄悄重发了一遍";
    }

    /**
     * @brief 钉住外部契约：处理器还没答完而本端按时限放弃时，交出失败且这条请求只被执行一次
     * @details 复用连接时「一个字节没回来」通常意味着对端在我们手里空闲期间把连接收了，那种可以重来；
     *          但请求已经整个写上通路、只是对端答得慢（这里让处理器按住 400 毫秒，本端 150 毫秒就放弃），
     *          同一位看着一样，重发就把非幂等请求做两遍。判据取路由的进入次数：它数的就是被交付了几次。
     * @details 本条**不是**重发闸门的证伪用例：把闸门整段撤掉本条照样绿——挡住第二次执行的是整体时限
     *          用尽之后那段预算判定（重开连接前先算剩余预算，已经没剩的了）。闸门本身要由
     *          `DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection`（h2）与
     *          `HttpOutboundConnectionPool` 里那条同名前缀的用例（h1）来钉：那两条失败得早、预算还剩
     *          一大截，撤掉闸门立刻就能看到第二次进入。
     */
    TEST(HttpsServer, PooledClientDoesNotReplayASentRequestThatTimedOut)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        std::atomic<std::size_t>  slowHandlerEntryCount{0};
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, [&slowHandlerEntryCount](Router &router, Core::EventLoop &serverLoop)
                { registerHoldingRoute(router, serverLoop, &slowHandlerEntryCount); }, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        const std::string hostPrefix = "https://127.0.0.1:" + std::to_string(fixture.listeningPort());
        const std::string warmUrl    = hostPrefix + "/hello";
        const std::string slowUrl    = hostPrefix + "/slow";
        Core::EventLoop   loop;
        HttpClient        client(loop);
        SingleRunOutcome  outcome;
        auto              work = runNoReplayAfterSend(loop, client, warmUrl, slowUrl, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(outcome.warmupStatusCode, 200) << "暖场那条没成功：被测那条走的就不是复用这一支";
        EXPECT_EQ(outcome.response, nullptr) << "处理器还没答完，本端按时限放弃了，这里不该有一个响应";
        EXPECT_EQ(slowHandlerEntryCount.load(std::memory_order_acquire), 1U) << "请求被交付了两次：已发出的非幂等请求不该因为「没收到回音」重来一次";
    }

    /**
     * @brief 钉住：池里那条 h2 连接被服务端在空闲期收掉之后，下一条请求换一条新的重来一次
     * @details 与 HTTP/1.1 的 `RecoversWhenPooledConnectionWasClosedByPeer` 是同一条竞态在两条通路上的
     *          形状：客户端只能「用了才知道」那条已经死了。h2 这一侧此前不恢复——写进一条对端已收的
     *          套接字在本地是**成功**的，于是那条请求被记成「已整个发出」，按当时那条一刀切的规则直接
     *          交出失败。判据只有第二条拿到 200：只看「没报错」不行，得看它真的又服务了一次。
     * @details 允许重来的依据是方法幂等（RFC 9112 §9.3.2 的自动重试许可只覆盖幂等方法）：GET 做两遍
     *          与做一遍对服务器状态的影响相同，而 POST 不能——那一条由
     *          `PooledClientDoesNotReplayASentRequestThatTimedOut` 钉住。
     */
    TEST(HttpsServer, PooledClientRecoversWhenTheHttp2ConnectionWasClosedByThePeer)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        // 空闲时限压到 150 毫秒、清扫节拍 25 毫秒：第二条请求之前那条一定已经被服务端收掉
        HttpServerLimits limits = makeLongTimeoutLimits();
        limits.idleTimeout      = std::chrono::milliseconds{150};
        RunningHttpsServerFixture fixture(limits, std::chrono::milliseconds{25}, {}, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string   url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";
        Core::EventLoop     loop;
        HttpClient          client(loop);
        IdleGraceRunOutcome outcome;
        auto                work = runWarmThenSecondAfterIdleGrace(loop, client, url, url, false, std::chrono::milliseconds{600}, std::chrono::milliseconds{3000}, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(outcome.firstStatusCode, 200) << "第一条就没成：池里根本没有等着被用的存货";
        EXPECT_EQ(outcome.heldAfterFirst, 1U) << "第一条之后池里不是一条 h2 连接：第二条测的就不是「复用一条已死的」";
        EXPECT_EQ(outcome.secondStatusCode, 200) << "对端收掉空闲连接时该重开一条再来一次，而不是把这条竞态透给调用方";
    }

    /**
     * @brief 钉住 HTTP/2 这一侧的重发闸门：请求已整个写上通路时，非幂等方法不重来一次
     * @details 与 HTTP/1.1 那条 `DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection`
     *          是同一个场景的两条通路：连接被服务端在空闲期收掉，本端写它时本地是成功的，于是分不清
     *          「对端没见过这条请求」与「对端已经收下」。判据取服务端的进入次数为 **0**——闸门一撤，
     *          那条 POST 就会被换一条新连接重交一遍（拿到 200 也算泄漏了一次执行）。
     * @details 幂等方法在同样情形下允许重来一次，那是 `PooledClientRecoversWhenTheHttp2ConnectionWas
     *          ClosedByThePeer` 的恢复路径；两条合起来才是完整的口径（RFC 9112 §9.3.2）。
     */
    TEST(HttpsServer, DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        std::atomic<std::size_t> slowHandlerEntryCount{0};
        HttpServerLimits         limits = makeLongTimeoutLimits();
        limits.idleTimeout              = std::chrono::milliseconds{150};
        RunningHttpsServerFixture fixture(
                limits, std::chrono::milliseconds{25}, [&slowHandlerEntryCount](Router &router, Core::EventLoop &serverLoop)
                { registerHoldingRoute(router, serverLoop, &slowHandlerEntryCount); }, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        const std::string hostPrefix = "https://127.0.0.1:" + std::to_string(fixture.listeningPort());
        // 两个地址必须是具名对象：驱动协程按引用拿着它们，跨过 co_await 之后还要读
        const std::string   warmUrl = hostPrefix + "/hello";
        const std::string   slowUrl = hostPrefix + "/slow";
        Core::EventLoop     loop;
        HttpClient          client(loop);
        IdleGraceRunOutcome outcome;
        auto                work = runWarmThenSecondAfterIdleGrace(loop, client, warmUrl, slowUrl, true, std::chrono::milliseconds{600}, std::chrono::milliseconds{3000}, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(outcome.firstStatusCode, 200) << "暖场那条没成功：池里没有可复用的连接，被测那条走的就不是复用这一支";
        EXPECT_EQ(outcome.heldAfterFirst, 1U) << "暖场之后池里不是一条 h2 连接：前提没成立";
        EXPECT_EQ(outcome.secondStatusCode, 0) << "对端收了这条连接，本端分不清请求有没有被接手，不该给出一个成功";
        EXPECT_EQ(slowHandlerEntryCount.load(std::memory_order_acquire), 0U) << "服务端收到了那条 POST：已整个写出的非幂等请求被换一条连接重发了一遍";
    }

    /**
     * @brief 钉住 :authority 的写法：端口不等于协议默认值时必须带在权威主机里
     * @details 连到哪个端口由 URL 决定，但按 Host/:authority 分站点的对端只看这一串字符：漏掉端口
     *          就等于把 8443 上的服务当成默认站点那个（RFC 9110 §7.2 与 RFC 9113 §8.3.1 同一条法）。
     *          两条协议走的是同一个拼装函数，所以这里回显的 host 同时钉住了 h1 的 Host 与 h2 的 :authority。
     */
    TEST(HttpsServer, OutboundClientPutsThePortIntoAuthority)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100},
                [](Router &router, Core::EventLoop &)
                {
                    router.get("/host",
                               [](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                               {
                                   response.setBody(std::string(request.getHeader("host").value_or(std::string{})));
                                   co_return;
                               });
                },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        const std::string  url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/host";
        Core::EventLoop    loop;
        HttpClient         client(loop);
        HostEchoRunOutcome outcome;
        auto               work = runHostEcho(loop, client, url, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_EQ(outcome.statusCode, 200) << "出站请求没走通";
        EXPECT_EQ(outcome.echoedHost, "127.0.0.1:" + std::to_string(fixture.listeningPort())) << "回显的权威主机没带端口：对端按 Host 分站点时会认错";
    }

    /**
     * @brief 客户端校验主机名：证书名字对不上就必须拒绝，不能只看「链是受信的」
     * @details 同一张受信证书（SAN 只有 DNS:localhost）用 IP 去连：链校验必然通过（它就在受信根里），
     *          唯一能拦住这次握手的只有**主机名校验**。去掉客户端侧的 SSL_set1_host /
     *          X509_VERIFY_PARAM_set1_ip_asc 后本条会变绿——那正是被修掉的那个缺陷（CWE-297）
     */
    TEST(HttpsServer, ClientRejectsCertificateForAnotherHost)
    {
        ASSERT_TRUE(std::filesystem::exists(kLocalhostCertificatePath)) << "缺少客户端用例的证书夹具：" << kLocalhostCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLocalhostCertificatePath);

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{}, {}, kLocalhostCertificatePath, kLocalhostKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        // 受信证书 + 对不上的名字（IP 不在 SAN 里）：必须失败
        const std::string url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";
        EXPECT_EQ(doHttpsGet(url), nullptr) << "证书与请求的主机名不匹配，握手却成功了：主机名校验没生效";
    }

    /**
     * @brief 钉住双向 TLS：带合法客户端身份的出站请求得到 200，不带身份的连不上
     * @details 两端各配一半都测不出这条链路——服务端只装 CA 不要求证书，或客户端只装身份而服务端不要求，
     *          两条都会绿。三条判据合起来才把「谁能连我」钉死：
     *          ①带身份的那条真通了（策略里的 CA 被采纳、身份真的出示，否则这里就是空）；
     *          ②不带身份的那条拿不到响应；
     *          ③服务端始终只数到一条请求——这条与②互补，把「本端超时导致的空」与「对端在握手里就拒了」
     *            分开：mTLS 没生效时②会拿到 200、③会数到 2。
     * @details 信任库这里显式走 TlsPolicy.certificateAuthorityFile，不再借 SSL_CERT_FILE 环境变量：
     *          出站侧接管信任库的入口就是那一项，顺带钉住它真的被采纳。夹具是自签的，一张证书既是
     *          服务端身份又是它自己的根，两端复用同一份文件。
     */
    TEST(HttpsServer, MutualTlsAcceptsClientWithCertificateAndRejectsWithout)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();

        bool                      clientAuthorityLoaded{false};
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{},
                [&clientAuthorityLoaded](HttpsServer &server)
                {
                    clientAuthorityLoaded = server.loadClientCertificateAuthority(kLoopbackCertificatePath.string());
                    server.setClientCertificateRequired(true);
                },
                kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(clientAuthorityLoaded) << "服务端装不上校验客户端证书的 CA：后面两条判据都是空的";
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::string url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/hello";

        Core::TlsPolicy policy;
        policy.certificateAuthorityFile = kLoopbackCertificatePath.string();

        const ClientTlsAttemptOutcome granted = getWithClientTls(url, policy, kLoopbackCertificatePath.string(), kLoopbackKeyPath.string(), std::chrono::milliseconds{8000});
        // 夹具默认档会协商出 h2，正文仍是同一份 served-hello：这里验的是握手能不能成，不是走哪条协议
        ASSERT_TRUE(granted.identityLoaded) << "客户端身份装不上：正面那条没有前提";
        ASSERT_NE(granted.response, nullptr) << "带合法客户端证书的握手没走通：身份没出示，或服务端 CA 不认这张证书";
        EXPECT_EQ(granted.response->statusCode, 200);
        EXPECT_NE(granted.response->body.find("served-hello"), std::string::npos) << "正文：" << granted.response->body;

        const ClientTlsAttemptOutcome anonymous = getWithClientTls(url, policy, {}, {}, std::chrono::milliseconds{4000});
        EXPECT_EQ(anonymous.response, nullptr) << "服务端要求客户端证书，不带身份的客户端却连上了：mTLS 没生效";
        EXPECT_EQ(fixture.server().stats().totalRequestCount, 1U) << "服务端数到了第二条：那条其实被握手续了进去";
    }

    /**
     * @brief 钉住：响应正文上限这一项对协商出的 HTTP/2 同样生效
     * @details 上限只有一个入口（池的 Config），两条通路各自去取：HTTP/1.1 交给响应解析器，HTTP/2
     *          交给连接配置。少接一处，同一个调用方在明文上拦得住的响应到 h2 上就照收——那正是这一轮
     *          要修的不对称（h2 之前根本没有这道闸）。判据取「越界的收不到、放开上限的收满」两头：
     *          只验前一头的话，「上限被写死成 0」这种实现也能过。
     */
    TEST(HttpsServer, EnforcesTheConfiguredResponseBodyLimitOverHttp2)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();

        constexpr std::size_t     kBodyByteCount = 16384U;
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100},
                [](Router &router, Core::EventLoop &)
                {
                    router.get("/big",
                               [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                               {
                                   response.setBody(std::string(kBodyByteCount, 'x'));
                                   co_return;
                               });
                },
                HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        Core::TlsPolicy policy;
        policy.certificateAuthorityFile = kLoopbackCertificatePath.string();
        const std::string url           = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/big";

        HttpOutboundConnectionPool::Config tightConfig;
        tightConfig.maximumResponseBodyBytes = 1024U;
        const ClientTlsAttemptOutcome tight  = getWithClientTls(url, policy, {}, {}, std::chrono::milliseconds{5000}, tightConfig);
        EXPECT_EQ(tight.response, nullptr) << "越界的那条被当成功收了：h2 这一侧没接上上限";

        HttpOutboundConnectionPool::Config openConfig;
        openConfig.maximumResponseBodyBytes = 0U; ///< 0 表示不限
        const ClientTlsAttemptOutcome open  = getWithClientTls(url, policy, {}, {}, std::chrono::milliseconds{5000}, openConfig);
        ASSERT_NE(open.response, nullptr) << "填 0 就该放开上限：取大文件是正当用法";
        EXPECT_EQ(open.response->statusCode, 200);
        EXPECT_EQ(open.response->body.size(), kBodyByteCount) << "正文长度不对：" << open.response->body.size();
    }

    /**
     * @brief 钉住：流式上传在协商出 HTTP/2 的通路上一段一段地发，服务端按段收齐
     * @details 与 `HttpOutboundConnectionPool.UploadsStreamedBodyAsChunkedRequest` 是一对：同一个请求
     *          字段（bodySource），两条承载各自决定线上形状（chunked 分块 vs 分帧 DATA）。判据沿用
     *          那一条的握手：客户端生产第 k 段之前先等服务端交付完第 k-1 批，所以「整份攒成一坨」
     *          当场露。h2 这一侧多钉一件事：HEADERS 不许带 END_STREAM——带了就等于宣告「没有正文」，
     *          服务端只会看到 0 批，回显因此是 echoed=0 而不是 echoed=3。
     */
    TEST(HttpsServer, UploadsStreamedBodyAsDataFrameSequence)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少客户端用例的证书夹具：" << kLoopbackCertificatePath.string();
        const ScopedTrustedCertificateFile trustedCertificate(kLoopbackCertificatePath);

        std::atomic<std::size_t>  serverBatchCount{0};
        std::mutex                receivedGuard;
        std::string               receivedText;
        RunningHttpsServerFixture fixture(
                makeLongTimeoutLimits(), std::chrono::milliseconds{100}, [&serverBatchCount, &receivedGuard, &receivedText](Router &router, Core::EventLoop &)
                { registerStreamingEchoRoute(router, &serverBatchCount, &receivedGuard, &receivedText); }, HttpParserLimits{}, {}, kLoopbackCertificatePath, kLoopbackKeyPath);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        const std::string                              url = "https://127.0.0.1:" + std::to_string(fixture.listeningPort()) + std::string{kStreamEchoRoutePath};
        Core::EventLoop                                loop;
        std::expected<HttpClientResponse, std::string> outcome{std::unexpect, "还没跑"};
        auto                                           drive = [&loop, &url, &outcome, &serverBatchCount]() -> Core::Task<>
        {
            HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "text/plain";
            request.bodySource  = makeStreamEchoChunkSource(loop, serverBatchCount);
            outcome             = co_await HttpClient::send(loop, url, request, std::chrono::milliseconds{8000});
            loop.stop();
        };
        // 闭包先落到具名对象上再调用：协程帧记住的是闭包地址，临时量在语句结束就析构，
        // 恢复时读的就是死对象（容器里的 ASan 报 stack-use-after-scope）
        auto work = drive();
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_TRUE(outcome.has_value()) << "流式上传没走通：" << outcome.error();
        EXPECT_EQ(outcome->statusCode, 200);
        EXPECT_TRUE(outcome->reasonPhrase.empty()) << "有原因短语＝那条其实是 HTTP/1.1 的答，h2 这一支没被走到";
        EXPECT_EQ(outcome->body, "echoed=3") << "服务端按批交付的次数不对：" << outcome->body;
        EXPECT_EQ(serverBatchCount.load(std::memory_order_acquire), kStreamEchoChunkCount) << "正文不是一段一段到服务端的";
        const std::lock_guard<std::mutex> guard(receivedGuard);
        EXPECT_EQ(receivedText, kStreamEchoExpectedText) << "拼回的正文：「" << receivedText << "」";
    }

    /**
     * @brief 钉住：HTTPS 响应带形态合法的 x-request-id，且客户端自带的合法取值被原样回显
     * @details 两条请求走同一条 keep-alive TLS 连接：第一条不带 id 验证生成面，第二条带合法 id 验证采信面。
     */
    TEST(HttpsServer, GeneratesAndEchoesRequestId)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 第一条：不带 x-request-id，服务器应当生成一个形态合法的标识
        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "第 1 条 HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout)) << "第 1 条 HTTPS 请求未得到完整响应";

        const std::string generatedRequestId = requestIdHeaderAt(receivedText, 0);
        EXPECT_TRUE(looksLikeGeneratedRequestId(generatedRequestId))
                << "HTTPS 响应的 request-id 形态不符合约定（应为 4 位十六进制前缀 + '-' + 16 位十六进制序号）：「" << generatedRequestId << "」";

        // 第二条：带一个合法取值，服务器应当原样回显
        const std::string clientRequestId = "trace-id-from-client-42";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1", {"x-request-id: " + clientRequestId}), kWaitTimeout)) << "第 2 条 HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 2, kWaitTimeout)) << "第 2 条 HTTPS 请求未得到完整响应";

        EXPECT_EQ(requestIdHeaderAt(receivedText, 1), clientRequestId) << "客户端自带的合法 request-id 没有被原样回显";
    }

    /**
     * @brief 钉住：TLS 连接建立后一个字节都不发，服务端按 idleTimeout 收口并计入 timeoutClosedCount
     *
     * @details 与 HTTP 侧的 IdleKeepAliveConnectionIsClosedAfterIdleTimeout 同一构造：短 idleTimeout
     *          配短清扫节拍，读/写超时故意设得很长，证明收口只可能来自空闲容忍度。容忍度之内先做一次
     *          反向对照，排除「连上就被关」这种假通过。
     */
    TEST(HttpsServer, ClosesIdleConnectionAndCountsTimeout)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{300};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpsServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 会话报出的对端地址必须来自真实描述符：清扫协程关闭超时连接前要靠它写日志，
        // 取不到就会让整轮清扫中断（这正是本条用例钉住的失败路径）。
        // 取快照与读地址都投到循环线程上做：本条用例把空闲容忍度压到 300ms，清扫协程随时可能
        // 把这条连接收掉，从测试线程直接读会话就是与循环抢同一个描述符
        std::size_t activeConnectionCount = 0;
        std::string reportedRemoteAddress;
        fixture.runOnLoopAndWait(
                [&fixture, &activeConnectionCount, &reportedRemoteAddress]
                {
                    const std::vector<std::shared_ptr<Core::Connection>> activeConnections = fixture.server().activeConnections();
                    activeConnectionCount                                                  = activeConnections.size();
                    if (!activeConnections.empty())
                    {
                        reportedRemoteAddress = activeConnections.front()->remoteAddress();
                    }
                });
        ASSERT_EQ(activeConnectionCount, 1u) << "握手已完成，连接却不在连接管理器里";
        EXPECT_EQ(reportedRemoteAddress, "127.0.0.1:" + std::to_string(client.localPort())) << "HTTPS 会话没有报出真实对端地址：它去问了那条不持有描述符的占位套接字";

        // 反向对照：空闲容忍度之内不该被提前收口（否则下面的断言可能只是「连上就被关」）
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{100})) << "TLS 连接在空闲容忍度之内就被关闭：说明截止时间被设成了立即到期";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "空闲 TLS 连接未被清扫协程收口：上界 kWaitTimeout（idleTimeout 300ms + 清扫节拍 30ms）。"
                                                                          "此刻仍挂在连接管理器上的连接数 "
                                                                       << fixture.server().activeConnectionCount();
        EXPECT_TRUE(receivedText.empty()) << "服务端在空闲连接上发了不该发的字节";

        // 上报与关闭必须同时发生：只关掉描述符而没有计入超时计数，说明清扫协程在收口链路上中途退出
        EXPECT_TRUE(waitForCondition([&fixture] { return fixture.server().stats().timeoutClosedCount >= 1; }, kWaitTimeout)) << "被超时收口的连接没有计入 timeoutClosedCount";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    /**
     * @brief 钉住：HTTP 与 HTTPS 的 request-id 前缀不相等（同一进程内两边各用各的原子序号，避免 id 撞车）
     * @details 两侧各起一台真实服务器、各发一条请求，从响应头的 x-request-id 里取前缀比较。
     */
    TEST(HttpsServer, RequestIdPrefixDiffersFromHttpServer)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        // ---- HTTP 侧 ----
        RunningHttpServerFixture httpFixture(makeLongTimeoutLimits(), std::chrono::milliseconds{50});
        ASSERT_TRUE(httpFixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t httpPort = httpFixture.listeningPort();
        ASSERT_NE(httpPort, 0);

        LoopbackClient httpClient(httpPort);
        ASSERT_TRUE(httpClient.isValid()) << "HTTP 回环连接失败";

        std::string httpReceivedText;
        ASSERT_TRUE(httpClient.sendText(helloRequestText(), kWaitTimeout)) << "HTTP 请求未能写入";
        ASSERT_TRUE(httpClient.waitForText(httpReceivedText, "served-hello", kWaitTimeout)) << "HTTP 请求未得到完整响应";

        const std::string httpRequestId = requestIdHeaderAt(httpReceivedText, 0);
        ASSERT_TRUE(looksLikeGeneratedRequestId(httpRequestId)) << "HTTP 侧 request-id 形态不符：「" << httpRequestId << "」";

        // ---- HTTPS 侧 ----
        RunningHttpsServerFixture httpsFixture(makeLongTimeoutLimits(), std::chrono::milliseconds{50});
        ASSERT_TRUE(httpsFixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t httpsPort = httpsFixture.listeningPort();
        ASSERT_NE(httpsPort, 0);

        TlsLoopbackClient httpsClient(httpsPort);
        ASSERT_TRUE(httpsClient.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string httpsReceivedText;
        ASSERT_TRUE(httpsClient.sendText(helloRequestText(), kWaitTimeout)) << "HTTPS 请求未能写入";
        ASSERT_TRUE(httpsClient.waitForTextOccurrences(httpsReceivedText, "served-hello", 1, kWaitTimeout)) << "HTTPS 请求未得到完整响应";

        const std::string httpsRequestId = requestIdHeaderAt(httpsReceivedText, 0);
        ASSERT_TRUE(looksLikeGeneratedRequestId(httpsRequestId)) << "HTTPS 侧 request-id 形态不符：「" << httpsRequestId << "」";

        EXPECT_NE(httpRequestId.substr(0, kRequestIdPrefixLength), httpsRequestId.substr(0, kRequestIdPrefixLength))
                << "HTTP 与 HTTPS 用了同一个 request-id 前缀：「" << httpRequestId << "」与「" << httpsRequestId << "」";
    }

    /**
     * @brief 钉住：HTTPS 侧的解析上限与 HTTP 侧对称——头部值超限回 431 而不是 400，未超限的请求照常被服务
     */
    TEST(HttpsServer, Answers431WhenParserHeaderLimitIsSmall)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpParserLimits parserLimits;
        parserLimits.maximumHeaderFieldValueLength = 32;

        // 连接级限额保持缺省（超时都很长），本用例只观察解析上限
        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_EQ(fixture.server().parserLimits().maximumHeaderFieldValueLength, 32u) << "落定的解析上限与传入值不符";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        // 越界的那一条：单个头部值 64 字节 > 32，必须回 431（形态合法、体量越界），不能退化成 400
        {
            TlsLoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
            const std::string request = makeRequestText("GET /hello HTTP/1.1", {"x-blob: " + std::string(64, 'a')});
            ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "越界 HTTPS 请求未能写入";

            std::string receivedText;
            ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "Request Header Fields Too Large", 1, kWaitTimeout)) << "越界头部未在时限内被判 431：上界 kWaitTimeout";
            EXPECT_NE(receivedText.find("HTTP/1.1 431"), std::string::npos) << receivedText;
            EXPECT_EQ(receivedText.find("HTTP/1.1 400"), std::string::npos) << "越界被当成了协议级非法：" << receivedText;
            EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "回完 431 没有收口";
        }

        // 同一台服务器上的下一条连接：未越界的请求照样拿到 200
        {
            TlsLoopbackClient normalClient(listeningPort);
            ASSERT_TRUE(normalClient.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
            ASSERT_TRUE(normalClient.sendText(helloRequestText(), kWaitTimeout)) << "正常 HTTPS 请求未能写入";

            std::string receivedText;
            ASSERT_TRUE(normalClient.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout)) << "同一台服务器上未超限的请求未被正常服务：" << receivedText;
            EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << receivedText;
        }
    }

    /**
     * @brief 钉住：静态文件目录这项能力在 HTTPS 上同样成立，与明文共用一份实现
     * @details 这一条测的是「TLS 侧接上了那个配置本体」，不是静态服务的全部语义——条件请求、Range、
     *          映射缓存那一整套由 TestHttpStaticFileTransfer 在明文侧钉住，两边跑的是同一个
     *          StaticFileService 与同一个处理函数。此前这项能力只长在明文服务器上：明文能配的静态目录，
     *          换到 TLS 端口上就没有入口，而 443 才是静态资源的常态落点。
     *          Cache-Control 与「目录外不存在」的 404 一并钉住：前者证明配置真的到了响应里，
     *          后者证明兜底路由接的是本服务器的路由器而不是明文那台。
     */
    TEST(HttpsServer, ServesStaticFilesFromAConfiguredDirectory)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        AsynGyanis::TestSupport::TemporaryDirectory site("HttpsStaticSite");
        const std::string                           fileContent = "static-over-tls";
        ASSERT_TRUE(site.writeBinaryFile("hello.txt", fileContent));
        // 目录交给服务器之前先转成 UTF-8 文本：配置面收的就是这个刻度（Windows 上 path::string()
        // 给的是本地代码页的字节）
        const std::string siteDirectory = Platform::FileSystem::utf8FromPath(site.path());

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, HttpParserLimits{},
                                          [&siteDirectory](HttpsServer &server)
                                          {
                                              server.staticFileDir(siteDirectory);
                                              server.setStaticFileCacheControl(std::optional<std::string>{"max-age=7"});
                                          });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";

        // 读回来的是规范化后的绝对路径：两边做同一道规范化再逐字比，比出来的相等才是「配置落在本服务器上」
        // 这条断言，而不是分隔符/大小写差异下的侥幸
        EXPECT_EQ(Platform::FileSystem::utf8FromPath(std::filesystem::weakly_canonical(site.path())), fixture.server().staticFileDir());

        TlsLoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string staticText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello.txt HTTP/1.1"), kWaitTimeout)) << "静态文件请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(staticText, fileContent, 1, kWaitTimeout)) << "HTTPS 上的静态文件没有回正文：「" << staticText << "」";
        EXPECT_NE(staticText.find("HTTP/1.1 200"), std::string::npos) << staticText;
        EXPECT_NE(staticText.find("content-type: text/plain"), std::string::npos) << staticText;
        EXPECT_NE(staticText.find("cache-control: max-age=7"), std::string::npos) << "Cache-Control 没跟着静态响应出去：" << staticText;

        // 兜底路由之外、也确实不存在的文件：静态服务自己回 404（不是「路由没登记所以走了别的分支」）
        std::string missingText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /no-such-file.txt HTTP/1.1"), kWaitTimeout)) << "缺失文件的请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(missingText, "404", 1, kWaitTimeout)) << "不存在的静态文件应当回 404：「" << missingText << "」";
    }
} // namespace AsynGyanis::Net
