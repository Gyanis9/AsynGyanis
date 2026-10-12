// Redis 链路 TLS 的端到端用例：本文件自带一台「TLS 终止器」——裸 OpenSSL 服务端在 loopback 上
// 接受密文、把明文拼给真 Redis（容器里的 redis 本身不带 TLS，所以终止器就是那条缺失的服务端）。
// 判据落在「命令真的穿过了 TLS 往返」与三条校验去向（VerifyIdentity 正/负、VerifyCa 负、Required 正）
// 以及 Preferred 回落到明文这一档上。门控：ASYN_REDIS_TEST_PASSWORD 未设置整组 GTEST_SKIP。
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisConnection.h"

#include "DatabaseTestSupport.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace AsynGyanis::Database
{
    namespace
    {
#ifdef DATABASE_HAS_REDIS
        constexpr bool kRedisDriverCompiled = true;
#else
        constexpr bool kRedisDriverCompiled = false;
#endif

        /// 证书夹具：ip 证书带 IP:127.0.0.1 的 SAN（VerifyIdentity 正向要它），localhost 证书不同源（当「错误 CA」用）
        const std::filesystem::path kIpCertificatePath        = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem";
        const std::filesystem::path kIpPrivateKeyPath         = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem";
        const std::filesystem::path kLocalhostCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_localhost_cert.pem";

        /// 成功断言类用例允许的夹具重建次数（夹具抖动约千分之几，见 VerifyIdentity 正路用例的注释）
        constexpr int kFixtureAttemptCount = 3;

        using TestSupport::readEnvironmentPortOrDefault;
        using TestSupport::readEnvironmentTextOrDefault;

        /// 把整段字节送出；对端收口时放弃（SIGPIPE 由 Socket::Initialization 在进程级忽略）
        bool sendAll(const int socket, const std::string_view bytes)
        {
            std::size_t sentByteCount = 0;
            while (sentByteCount < bytes.size())
            {
                const ssize_t justSent = ::send(socket, bytes.data() + sentByteCount, static_cast<int>(bytes.size() - sentByteCount), 0);
                if (justSent <= 0)
                {
                    return false;
                }
                sentByteCount += static_cast<std::size_t>(justSent);
            }
            return true;
        }

        /// 给描述符装上收发时限：终止器是测试夹具，任何一步卡住都不该把用例拖到天荒地老
        void applySocketTimeouts(const int socket)
        {
            timeval timeoutValue{3, 0};
            static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutValue), sizeof(timeoutValue)));
            static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutValue), sizeof(timeoutValue)));
        }

        /**
         * @brief TLS 终止器：accept 密文连接 → 服务端握手 → 与真 Redis 之间双向拼字节
         * @details 只服务本文件的用例（串行连接，逐个处理）；握手失败按「本连接作废」处置并计数，
         *          负向用例正是靠「客户端拒了证书 → 终止器这边也握不成」这条链收场
         */
        class TlsTerminatingRedisProxy
        {
        public:
            TlsTerminatingRedisProxy(const std::filesystem::path &certificatePath, const std::filesystem::path &privateKeyPath, std::string upstreamHost,
                                     const std::uint16_t upstreamPort) :
                m_certificatePath(certificatePath), m_privateKeyPath(privateKeyPath), m_upstreamHost(std::move(upstreamHost)), m_upstreamPort(upstreamPort)
            {
            }

            ~TlsTerminatingRedisProxy()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            TlsTerminatingRedisProxy(const TlsTerminatingRedisProxy &)            = delete;
            TlsTerminatingRedisProxy &operator=(const TlsTerminatingRedisProxy &) = delete;

            bool start()
            {
                m_worker            = std::thread([this] { run(); });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                while (m_port.load(std::memory_order_acquire) == 0U && !m_isFailed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return m_port.load(std::memory_order_acquire) != 0U;
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::size_t acceptedConnectionCount() const noexcept
            {
                return m_acceptedCount.load(std::memory_order_acquire);
            }

            /// 服务端侧握手失败的连接数（负向用例之外若不为 0，说明有握手在服务端也失败了，供排障）
            [[nodiscard]] std::size_t handshakeFailureCount() const noexcept
            {
                return m_handshakeFailureCount.load(std::memory_order_acquire);
            }

            /// 服务端侧第一笔握手失败的原因（排障用；无失败时为空）
            [[nodiscard]] std::string lastHandshakeError() const
            {
                const std::lock_guard guard(m_diagnosticMutex);
                return m_lastHandshakeError;
            }

        private:
            void run()
            {
                const Platform::Socket::Initialization network;
                const int                              listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(listener))
                {
                    m_isFailed.store(true, std::memory_order_release);
                    return;
                }
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = 0;
                if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    m_isFailed.store(true, std::memory_order_release);
                    return;
                }
                sockaddr_in bound{};
                socklen_t   boundLength = static_cast<socklen_t>(sizeof(bound));
                if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    m_isFailed.store(true, std::memory_order_release);
                    return;
                }

                SSL_CTX *const serverContext = SSL_CTX_new(TLS_server_method());
                if (serverContext == nullptr || SSL_CTX_use_certificate_file(serverContext, m_certificatePath.string().c_str(), SSL_FILETYPE_PEM) != 1 ||
                    SSL_CTX_use_PrivateKey_file(serverContext, m_privateKeyPath.string().c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(serverContext) != 1)
                {
                    if (serverContext != nullptr)
                    {
                        SSL_CTX_free(serverContext);
                    }
                    Platform::FileDescriptor::close(listener);
                    m_isFailed.store(true, std::memory_order_release);
                    return;
                }

                m_port.store(ntohs(bound.sin_port), std::memory_order_release);
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
                    sockaddr_in peer{};
                    socklen_t   peerLength = static_cast<socklen_t>(sizeof(peer));
                    const int   client     = static_cast<int>(::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peerLength));
                    if (!Platform::FileDescriptor::isValid(client))
                    {
                        continue;
                    }
                    m_acceptedCount.fetch_add(1, std::memory_order_acq_rel);
                    serveOneConnection(serverContext, client);
                    Platform::FileDescriptor::close(client);
                }
                SSL_CTX_free(serverContext);
                Platform::FileDescriptor::close(listener);
            }

            /// 一条客户连接：服务端握手 → 与上游 Redis 双向拼接（任一侧收口即收场）
            void serveOneConnection(SSL_CTX *const serverContext, const int clientSocket)
            {
                applySocketTimeouts(clientSocket);
                SSL *const ssl = SSL_new(serverContext);
                if (ssl == nullptr || ::SSL_set_fd(ssl, clientSocket) != 1)
                {
                    if (ssl != nullptr)
                    {
                        SSL_free(ssl);
                    }
                    return;
                }
                if (SSL_accept(ssl) != 1)
                {
                    // 握手失败（负向用例里客户端拒了证书就是这条）：本连接作废，原因记账供排障
                    m_handshakeFailureCount.fetch_add(1, std::memory_order_acq_rel);
                    char reason[256]{};
                    ERR_error_string_n(ERR_get_error(), reason, sizeof(reason));
                    int       socketError       = 0;
                    socklen_t socketErrorLength = static_cast<socklen_t>(sizeof(socketError));
                    static_cast<void>(::getsockopt(clientSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socketError), &socketErrorLength));
                    char      probeByte   = 0;
                    const int probeResult = ::recv(clientSocket, &probeByte, 1, MSG_PEEK);
                    const int probeErrno  = probeResult < 0 ? errno : 0;
                    {
                        const std::lock_guard guard(m_diagnosticMutex);
                        if (m_lastHandshakeError.empty())
                        {
                            m_lastHandshakeError = std::string(reason[0] == '\0' ? "错误队列为空" : reason) + "；SO_ERROR=" + std::to_string(socketError) +
                                                   "；peek=" + std::to_string(probeResult) + "(" + std::to_string(probeErrno) + ")";
                        }
                    }
                    SSL_free(ssl);
                    return;
                }

                const int upstream = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(upstream))
                {
                    static_cast<void>(SSL_shutdown(ssl));
                    SSL_free(ssl);
                    return;
                }
                applySocketTimeouts(upstream);
                sockaddr_in upstreamAddress{};
                upstreamAddress.sin_family = AF_INET;
                upstreamAddress.sin_port   = htons(m_upstreamPort);
                // inet_pton 而不是 inet_addr：后者在 MSVC 上被 C4996 判弃用，且把 255.255.255.255 也当失败值
                if (::inet_pton(AF_INET, m_upstreamHost.c_str(), &upstreamAddress.sin_addr) != 1 ||
                    ::connect(upstream, reinterpret_cast<const sockaddr *>(&upstreamAddress), sizeof(upstreamAddress)) != 0)
                {
                    Platform::FileDescriptor::close(upstream);
                    static_cast<void>(SSL_shutdown(ssl));
                    SSL_free(ssl);
                    return;
                }

                constexpr int kPumpBufferBytes = 16384;
                bool          isDone           = false;
                while (!isDone && !m_isStopping.load(std::memory_order_acquire))
                {
                    // TLS 侧可读性 = 「原始 fd 可读」或「SSL 缓冲里还有没取走的明文」两者之一；
                    // 只有真跑过 select 才能信 FD_ISSET——旁路 select 时两个判断都必须是假
                    bool isClientReadable   = SSL_pending(ssl) > 0;
                    bool isUpstreamReadable = false;
                    if (!isClientReadable)
                    {
                        fd_set readSet{};
                        FD_ZERO(&readSet);
                        FD_SET(clientSocket, &readSet);
                        FD_SET(upstream, &readSet);
                        timeval pollInterval{0, 50 * 1000};
                        if (::select(std::max(clientSocket, upstream) + 1, &readSet, nullptr, nullptr, &pollInterval) > 0)
                        {
                            isClientReadable   = FD_ISSET(clientSocket, &readSet) != 0;
                            isUpstreamReadable = FD_ISSET(upstream, &readSet) != 0;
                        }
                    }

                    // 客户端 → Redis：密文进来，明文交给上游
                    if (isClientReadable)
                    {
                        char      plainBytes[kPumpBufferBytes];
                        const int receivedBytes = SSL_read(ssl, plainBytes, static_cast<int>(sizeof(plainBytes)));
                        const int sslError      = receivedBytes <= 0 ? SSL_get_error(ssl, receivedBytes) : SSL_ERROR_NONE;
                        if (receivedBytes > 0)
                        {
                            if (!sendAll(upstream, std::string_view(plainBytes, static_cast<std::size_t>(receivedBytes))))
                            {
                                isDone = true;
                            }
                        } else if (sslError == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // 收发时限到点但连接健康：不是收口，下一轮 select 继续等
                        } else if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE)
                        {
                            isDone = true;
                        }
                    }

                    // Redis → 客户端：明文回来，加密送回
                    if (!isDone && isUpstreamReadable)
                    {
                        char          plainBytes[kPumpBufferBytes];
                        const ssize_t receivedBytes = ::recv(upstream, plainBytes, static_cast<int>(sizeof(plainBytes)), 0);
                        if (receivedBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // 同客户端侧：时限到点不算收口
                        } else if (receivedBytes <= 0)
                        {
                            isDone = true;
                        } else if (SSL_write(ssl, plainBytes, static_cast<int>(receivedBytes)) <= 0)
                        {
                            isDone = true;
                        }
                    }
                }

                static_cast<void>(SSL_shutdown(ssl));
                SSL_free(ssl);
                Platform::FileDescriptor::close(upstream);
            }

            std::filesystem::path      m_certificatePath;
            std::filesystem::path      m_privateKeyPath;
            std::string                m_upstreamHost;
            std::uint16_t              m_upstreamPort{0};
            std::thread                m_worker;
            std::atomic<bool>          m_isStopping{false};
            std::atomic<bool>          m_isFailed{false};
            std::atomic<std::uint16_t> m_port{0};
            std::atomic<std::size_t>   m_acceptedCount{0};
            std::atomic<std::size_t>   m_handshakeFailureCount{0};
            mutable std::mutex         m_diagnosticMutex;    ///< 保护下面的诊断文本
            std::string                m_lastHandshakeError; ///< 服务端侧第一笔握手失败原因
        };

        /// 门控与配置装载：与 TestRedisIntegration.cpp 同一套环境变量口径
        [[nodiscard]] std::string readGatedPassword(std::string &skipReason)
        {
            if (!kRedisDriverCompiled)
            {
                skipReason = "当前构建未编译 Redis 驱动（未定义 DATABASE_HAS_REDIS），跳过真机 TLS 用例";
                return {};
            }
            const std::string password = TestSupport::readEnvironmentVariableText("ASYN_REDIS_TEST_PASSWORD");
            if (password.empty())
            {
                skipReason = "未设置环境变量 ASYN_REDIS_TEST_PASSWORD，跳过真实 Redis TLS 用例";
                return {};
            }
            return password;
        }

        /// 连到真 Redis 的配置（作终止器的上游目标；也供 Preferred 回落用例直连）
        [[nodiscard]] ConnectionConfig upstreamConfigFromEnvironment(const std::string &password)
        {
            ConnectionConfig configuration = ConnectionConfig::redisDefault();
            configuration.host             = readEnvironmentTextOrDefault("ASYN_REDIS_TEST_HOST", "127.0.0.1");
            configuration.port             = readEnvironmentPortOrDefault("ASYN_REDIS_TEST_PORT", 6379);
            configuration.password         = password;
            return configuration;
        }
    } // namespace

    /**
     * @brief VerifyIdentity 正向：CA 与主机名都对得上时，命令真的从隧道里往返
     */
    TEST(RedisTls, VerifyIdentityRoundTripsCommandsThroughTheTlsTerminator)
    {
        std::string       skipReason;
        const std::string password = readGatedPassword(skipReason);
        if (!skipReason.empty())
        {
            GTEST_SKIP() << skipReason;
        }
        const ConnectionConfig upstream = upstreamConfigFromEnvironment(password);

        // 终止器夹具在 Windows 上以约千分之几的概率出现「握手中途双方同时看到 EOF（错误队列为空、SO_ERROR=0）」
        // ——已定性为夹具侧丢连接（产品侧对 EOF 的处置是正确行为），根因未明。为不把夹具抖动混进门禁，
        // 成功断言类用例允许把夹具整个重建一次再断言；两次都握不上才算失败，诊断一并带出
        bool        isSucceeded = false;
        std::string failureDetail;
        std::size_t acceptedConnections = 0;
        for (int attempt = 0; attempt < kFixtureAttemptCount && !isSucceeded; ++attempt)
        {
            TlsTerminatingRedisProxy proxy(kIpCertificatePath, kIpPrivateKeyPath, upstream.host, upstream.port);
            ASSERT_TRUE(proxy.start()) << "TLS 终止器没起来";

            ConnectionConfig configuration             = upstream;
            configuration.host                         = "127.0.0.1"; // 终止器监听在 loopback，证书 SAN 也是它
            configuration.port                         = proxy.port();
            configuration.tls.mode                     = DatabaseTlsPolicy::Mode::VerifyIdentity;
            configuration.tls.certificateAuthorityFile = kIpCertificatePath.string();

            RedisConnection connection(configuration);
            if (!connection.connect())
            {
                failureDetail = connection.lastError() + " | 终止器：握手失败数=" + std::to_string(proxy.handshakeFailureCount()) + " 服务端原因=" + proxy.lastHandshakeError();
                continue;
            }

            // 一条写一条读：任何一路没走通（或压根没走 TLS），终止器的拼接都交不出正确回复
            const std::string key = "asyn:tls:roundtrip";
            ASSERT_NE(connection.executeCommand({"SET", key, "through-tls"}), nullptr) << connection.lastError();
            const std::unique_ptr<DatabaseResult> getResult = connection.executeCommand({"GET", key});
            ASSERT_NE(getResult, nullptr) << connection.lastError();
            ASSERT_TRUE(getResult->next());
            EXPECT_EQ(TestSupport::asText(getResult->getValue(std::size_t{0})), std::optional<std::string>("through-tls"));
            acceptedConnections = proxy.acceptedConnectionCount();
            isSucceeded         = true;
            static_cast<void>(connection.executeCommand({"DEL", key}));
        }
        ASSERT_TRUE(isSucceeded) << "夹具两次重建都没握上手：" << failureDetail;
        EXPECT_EQ(acceptedConnections, 1U) << "命令没有经过终止器";
    }

    /**
     * @brief VerifyIdentity 负向：CA 对但主机名对不上（localhost 证书 + 127.0.0.1 目标）必须拒
     */
    TEST(RedisTls, VerifyIdentityRejectsHostnameMismatch)
    {
        std::string       skipReason;
        const std::string password = readGatedPassword(skipReason);
        if (!skipReason.empty())
        {
            GTEST_SKIP() << skipReason;
        }
        const ConnectionConfig upstream = upstreamConfigFromEnvironment(password);

        TlsTerminatingRedisProxy proxy(kLocalhostCertificatePath, std::filesystem::path(TEST_FIXTURES_DIR) / "test_localhost_key.pem", upstream.host, upstream.port);
        ASSERT_TRUE(proxy.start()) << "TLS 终止器没起来";

        ConnectionConfig configuration             = upstream;
        configuration.host                         = "127.0.0.1";
        configuration.port                         = proxy.port();
        configuration.tls.mode                     = DatabaseTlsPolicy::Mode::VerifyIdentity;
        configuration.tls.certificateAuthorityFile = kLocalhostCertificatePath.string();

        RedisConnection connection(configuration);
        EXPECT_FALSE(connection.connect()) << "主机名对不上的证书竟然连上了";
        EXPECT_NE(connection.lastError().find("TLS"), std::string::npos) << connection.lastError();
    }

    /**
     * @brief VerifyCa 负向：链根本不在信任锚里（拿另一份自签证书当 CA）必须拒
     */
    TEST(RedisTls, VerifyCaRejectsAnUntrustedCertificate)
    {
        std::string       skipReason;
        const std::string password = readGatedPassword(skipReason);
        if (!skipReason.empty())
        {
            GTEST_SKIP() << skipReason;
        }
        const ConnectionConfig upstream = upstreamConfigFromEnvironment(password);

        TlsTerminatingRedisProxy proxy(kIpCertificatePath, kIpPrivateKeyPath, upstream.host, upstream.port);
        ASSERT_TRUE(proxy.start()) << "TLS 终止器没起来";

        ConnectionConfig configuration             = upstream;
        configuration.host                         = "127.0.0.1";
        configuration.port                         = proxy.port();
        configuration.tls.mode                     = DatabaseTlsPolicy::Mode::VerifyCa;
        configuration.tls.certificateAuthorityFile = kLocalhostCertificatePath.string(); // 与服务端证书不同源

        RedisConnection connection(configuration);
        EXPECT_FALSE(connection.connect()) << "错误 CA 之下连接竟然建成了";
        EXPECT_NE(connection.lastError().find("TLS"), std::string::npos) << connection.lastError();
    }

    /**
     * @brief Required 正向：只要求加密、不校链（自签服务端也接受），命令照样往返
     */
    TEST(RedisTls, RequiredEncryptsWithoutChainVerification)
    {
        std::string       skipReason;
        const std::string password = readGatedPassword(skipReason);
        if (!skipReason.empty())
        {
            GTEST_SKIP() << skipReason;
        }
        const ConnectionConfig upstream = upstreamConfigFromEnvironment(password);

        // 与 VerifyIdentity 正路同一套「夹具可重建一次」的处置（缘由见那条用例的注释）
        bool        isSucceeded = false;
        std::string failureDetail;
        std::size_t acceptedConnections = 0;
        for (int attempt = 0; attempt < kFixtureAttemptCount && !isSucceeded; ++attempt)
        {
            TlsTerminatingRedisProxy proxy(kIpCertificatePath, kIpPrivateKeyPath, upstream.host, upstream.port);
            ASSERT_TRUE(proxy.start()) << "TLS 终止器没起来";

            ConnectionConfig configuration = upstream;
            configuration.host             = "127.0.0.1";
            configuration.port             = proxy.port();
            configuration.tls.mode         = DatabaseTlsPolicy::Mode::Required; // 不给 CA

            RedisConnection connection(configuration);
            if (!connection.connect())
            {
                failureDetail = connection.lastError() + " | 终止器：握手失败数=" + std::to_string(proxy.handshakeFailureCount()) + " 服务端原因=" + proxy.lastHandshakeError();
                continue;
            }
            ASSERT_NE(connection.executeCommand({"PING"}), nullptr) << connection.lastError();
            acceptedConnections = proxy.acceptedConnectionCount();
            isSucceeded         = true;
        }
        ASSERT_TRUE(isSucceeded) << "夹具两次重建都没握上手：" << failureDetail;
        EXPECT_EQ(acceptedConnections, 1U) << "PING 没有经过终止器";
    }

    /**
     * @brief Preferred 回落：对端不带 TLS 时握手必失败，按档位语义弃掉重拨、以明文建成并可用
     */
    TEST(RedisTls, PreferredFallsBackToPlainAgainstAServerWithoutTls)
    {
        std::string       skipReason;
        const std::string password = readGatedPassword(skipReason);
        if (!skipReason.empty())
        {
            GTEST_SKIP() << skipReason;
        }

        // 直连真 Redis（容器里的 redis 不带 TLS）：ClientHello 会被它当协议错误对待、握手必失败
        ConnectionConfig configuration = upstreamConfigFromEnvironment(password);
        configuration.tls.mode         = DatabaseTlsPolicy::Mode::Preferred;

        RedisConnection connection(configuration);
        ASSERT_TRUE(connection.connect()) << connection.lastError();
        ASSERT_NE(connection.executeCommand({"PING"}), nullptr) << connection.lastError();
    }
} // namespace AsynGyanis::Database
