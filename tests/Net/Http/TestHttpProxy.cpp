// HttpProxyPolicy 测试：出站代理策略的纯函数面（URL 解析 / NO_PROXY 匹配 / 环境合成 / 凭据打码）
// 与 e2e 面（绝对形式、NO_PROXY 直连、CONNECT 隧道与复用、拒因）。期望值凡涉及 Basic 值的都用
// python base64 现算后硬编码（独立于被测实现的口径）。
#include "Net/Http/Client/HttpProxyPolicy.h"

#include "CommonTestSupport.h"
#include "ExpectedAssertions.h"
#include "HttpTestSupport.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsContext.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Server/HttpsServer.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 进程环境变量的临时改写：析构恢复原值（含「原本没有这条」）
        class ScopedEnvironmentVariable
        {
        public:
            ScopedEnvironmentVariable(std::string name, const std::optional<std::string> &value) : m_name(std::move(name))
            {
                m_previous = Platform::ProcessInfo::environmentVariable(m_name);
                apply(value);
            }

            ~ScopedEnvironmentVariable()
            {
                apply(m_previous);
            }

            ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)            = delete;
            ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

        private:
            void apply(const std::optional<std::string> &value)
            {
#if ASYN_PLATFORM_WIN32
                // 与 ProcessInfo::environmentVariable() 的读侧同一条 CRT 通道（_dupenv_s ↔ _putenv_s）
                static_cast<void>(::_putenv_s(m_name.c_str(), value.has_value() ? value->c_str() : ""));
#else
                if (value.has_value())
                {
                    static_cast<void>(::setenv(m_name.c_str(), value->c_str(), 1));
                } else
                {
                    static_cast<void>(::unsetenv(m_name.c_str()));
                }
#endif
            }

            std::string                m_name;
            std::optional<std::string> m_previous;
        };
    } // namespace

    /**
     * @brief 代理地址解析的三类形状：带 scheme、裸 host:port、裸 host（缺省 80）
     */
    TEST(HttpProxyPolicy, ParsesHttpUrlsAndBareAuthorities)
    {
        const auto full = parseProxyUrl("http://proxy.example.com:3128");
        ASSERT_HAS_VALUE(full) << full.error();
        EXPECT_EQ(full->host, "proxy.example.com");
        EXPECT_EQ(full->port, 3128U);
        EXPECT_TRUE(full->authorizationHeader.empty()) << "没有凭据段却拼出了认证头";

        const auto bare = parseProxyUrl("proxy.internal:8080");
        ASSERT_HAS_VALUE(bare) << bare.error();
        EXPECT_EQ(bare->host, "proxy.internal");
        EXPECT_EQ(bare->port, 8080U);

        const auto defaultPort = parseProxyUrl("proxy.internal");
        ASSERT_HAS_VALUE(defaultPort) << defaultPort.error();
        EXPECT_EQ(defaultPort->host, "proxy.internal");
        EXPECT_EQ(defaultPort->port, 80U) << "裸主机名没有落到 http 的默认端口";

        const auto v6 = parseProxyUrl("http://[::1]:3128");
        ASSERT_HAS_VALUE(v6) << v6.error();
        EXPECT_EQ(v6->host, "::1") << "IPv6 方括号没有剥掉";
        EXPECT_EQ(v6->port, 3128U);
    }

    /**
     * @brief https:// 代理：TLS 连到代理自身的标志位与 443 缺省端口
     * @details 代理地址的 scheme 决定「怎么连代理」，与目标 scheme 无关：https:// 代理的
     *          isTls 必须为真（建立侧据此先做外层 TLS 握手），显式端口与凭据段照旧生效。
     *          证伪：把 scheme 判定退回「只认 http」，三条 isTls 断言全红。
     */
    TEST(HttpProxyPolicy, AcceptsHttpsProxySchemeWithTlsFlagAndPort443)
    {
        const auto defaultPort = parseProxyUrl("https://proxy.example");
        ASSERT_HAS_VALUE(defaultPort) << defaultPort.error();
        EXPECT_TRUE(defaultPort->isTls) << "https:// 代理没有置 TLS 标志";
        EXPECT_EQ(defaultPort->port, 443U) << "https:// 代理没有落到 443 缺省端口";

        const auto explicitPort = parseProxyUrl("https://proxy.example:8443");
        ASSERT_HAS_VALUE(explicitPort) << explicitPort.error();
        EXPECT_TRUE(explicitPort->isTls);
        EXPECT_EQ(explicitPort->port, 8443U);

        const auto uppercase = parseProxyUrl("HTTPS://Proxy.Example");
        ASSERT_HAS_VALUE(uppercase) << uppercase.error();
        EXPECT_TRUE(uppercase->isTls) << "scheme 比较区分了大小写";

        const auto withCredentials = parseProxyUrl("https://alice:s3cret@proxy.example:8443");
        ASSERT_HAS_VALUE(withCredentials) << withCredentials.error();
        EXPECT_TRUE(withCredentials->isTls);
        EXPECT_EQ(withCredentials->authorizationHeader, "Basic YWxpY2U6czNjcmV0");

        // 明文代理仍是默认档：没有 https:// 前缀时按 80 且不置 TLS 位
        const auto plain = parseProxyUrl("http://proxy.example:3128");
        ASSERT_HAS_VALUE(plain) << plain.error();
        EXPECT_FALSE(plain->isTls) << "http:// 代理被误置 TLS 标志";
    }

    /**
     * @brief 不支持的形状一律拒绝并点名（不静默变形）：socks5 代理、空主机、越界端口、空值
     */
    TEST(HttpProxyPolicy, RejectsUnsupportedOrMalformedProxyUrls)
    {
        const auto socksProxy = parseProxyUrl("socks5://proxy.example:1080");
        ASSERT_FALSE(socksProxy.has_value()) << "socks5 代理必须拒绝（本层只认 http/https 代理）";
        EXPECT_NE(socksProxy.error().find("socks5"), std::string::npos) << socksProxy.error();

        const auto emptyHost = parseProxyUrl("http://:3128");
        ASSERT_FALSE(emptyHost.has_value()) << "空主机被放行";

        const auto badPort = parseProxyUrl("proxy.example:70000");
        ASSERT_FALSE(badPort.has_value()) << "越界端口被放行";

        const auto empty = parseProxyUrl("");
        ASSERT_FALSE(empty.has_value()) << "空代理地址被放行";
    }

    /**
     * @brief 凭据段折成 Basic 头：原文、%XX 转义、非法转义拒绝
     */
    TEST(HttpProxyPolicy, TurnsUserInfoIntoBasicAuthorizationHeader)
    {
        const auto route = parseProxyUrl("http://alice:s3cret@proxy.example.com:3128");
        ASSERT_HAS_VALUE(route) << route.error();
        EXPECT_EQ(route->host, "proxy.example.com") << "凭据段没有从主机名里剥掉";
        EXPECT_EQ(route->port, 3128U);
        EXPECT_EQ(route->authorizationHeader, "Basic YWxpY2U6czNjcmV0");

        const auto escaped = parseProxyUrl("http://alice:p%40ss@proxy.example.com:3128");
        ASSERT_HAS_VALUE(escaped) << escaped.error();
        EXPECT_EQ(escaped->authorizationHeader, "Basic YWxpY2U6cEBzcw==") << "%XX 没有解码：p%40ss 应当解回 p@ss";

        const auto badEscape = parseProxyUrl("http://alice:p%ZZ@proxy.example.com:3128");
        ASSERT_FALSE(badEscape.has_value()) << "非法 %XX 被静默放过";
    }

    /**
     * @brief 豁免表：通配、后缀（含子域）、点边界、端口限定、大小写、空表
     */
    TEST(HttpProxyPolicy, NoProxyMatchingCoversWildcardSuffixAndPort)
    {
        EXPECT_TRUE(matchesNoProxy("anything.example", 80, "*"));
        EXPECT_TRUE(matchesNoProxy("example.com", 80, "example.com"));
        EXPECT_TRUE(matchesNoProxy("api.example.com", 443, "example.com"));
        EXPECT_TRUE(matchesNoProxy("api.example.com", 80, ".example.com")) << "前导点写法应当等效于裸后缀";
        EXPECT_TRUE(matchesNoProxy("example.com", 80, ".example.com")) << "前导点写法还应当命中域本身（curl 语义）";
        EXPECT_FALSE(matchesNoProxy("notexample.com", 80, "example.com")) << "后缀匹配越过了点边界";
        EXPECT_FALSE(matchesNoProxy("example.com", 80, "other.com, api.internal"));
        EXPECT_TRUE(matchesNoProxy("api.internal", 80, "other.com, api.internal"));
        EXPECT_TRUE(matchesNoProxy("example.com", 8080, "example.com:8080"));
        EXPECT_FALSE(matchesNoProxy("example.com", 80, "example.com:8080")) << "带端口的条目没有按端口限制";
        EXPECT_TRUE(matchesNoProxy("API.Example.COM", 80, "example.com")) << "主机比较区分了大小写";
        EXPECT_FALSE(matchesNoProxy("example.com", 80, "")) << "空豁免表被当成全豁免";
    }

    /**
     * @brief 环境合成：空字段取环境、显式值优先、consultEnvironment=false 整体不读
     */
    TEST(HttpProxyPolicy, EnvironmentFillsEmptyFieldsAndExplicitValuesWin)
    {
        const ScopedEnvironmentVariable httpLower{"http_proxy", std::string("http://env-http:3128")};
        const ScopedEnvironmentVariable httpsLower{"https_proxy", std::string("http://env-https:3129")};
        const ScopedEnvironmentVariable noLower{"no_proxy", std::string("env.example")};

        const HttpProxyPolicy frozen = applyProxyEnvironment(HttpProxyPolicy{});
        EXPECT_EQ(frozen.httpProxy, "http://env-http:3128");
        EXPECT_EQ(frozen.httpsProxy, "http://env-https:3129");
        EXPECT_EQ(frozen.noProxy, "env.example");

        HttpProxyPolicy explicitPolicy;
        explicitPolicy.httpProxy    = "http://explicit:1111";
        const HttpProxyPolicy mixed = applyProxyEnvironment(explicitPolicy);
        EXPECT_EQ(mixed.httpProxy, "http://explicit:1111") << "显式值被环境覆盖了";
        EXPECT_EQ(mixed.httpsProxy, "http://env-https:3129") << "另一支的空字段没有取到环境";

        HttpProxyPolicy optOut;
        optOut.consultEnvironment       = false;
        const HttpProxyPolicy untouched = applyProxyEnvironment(optOut);
        EXPECT_TRUE(untouched.httpProxy.empty()) << "consultEnvironment=false 仍读了环境";
        EXPECT_TRUE(untouched.httpsProxy.empty());
        EXPECT_TRUE(untouched.noProxy.empty());
    }

    /**
     * @brief 大写变量同样认（小写缺失时回落到大写）
     */
    TEST(HttpProxyPolicy, UppercaseEnvironmentNamesAreRecognized)
    {
        const ScopedEnvironmentVariable httpLower{"http_proxy", std::nullopt};
        const ScopedEnvironmentVariable httpUpper{"HTTP_PROXY", std::string("http://upper:2222")};

        const HttpProxyPolicy frozen = applyProxyEnvironment(HttpProxyPolicy{});
        EXPECT_EQ(frozen.httpProxy, "http://upper:2222") << "大写 HTTP_PROXY 没有被识别";
    }

    /**
     * @brief 每请求解析：按 scheme 选支、豁免表短路、坏地址报错（而非静默直连）、未配置直连
     */
    TEST(HttpProxyPolicy, ResolvePicksSchemeSpecificProxyAndHonorsNoProxy)
    {
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpProxy          = "http://plain-proxy:3128";
        policy.httpsProxy         = "http://tls-proxy:3129";
        policy.noProxy            = "direct.example";

        const auto plain = resolveOutboundProxy(policy, "http", "origin.example", 80);
        ASSERT_HAS_VALUE(plain) << plain.error();
        ASSERT_TRUE(plain->has_value()) << "http 目标没有选中 http_proxy 那一支";
        EXPECT_EQ(plain->value().host, "plain-proxy");
        EXPECT_EQ(plain->value().port, 3128U);

        const auto secure = resolveOutboundProxy(policy, "https", "origin.example", 443);
        ASSERT_HAS_VALUE(secure) << secure.error();
        ASSERT_TRUE(secure->has_value()) << "https 目标没有选中 https_proxy 那一支";
        EXPECT_EQ(secure->value().host, "tls-proxy");
        EXPECT_EQ(secure->value().port, 3129U);

        const auto exempt = resolveOutboundProxy(policy, "http", "direct.example", 80);
        ASSERT_HAS_VALUE(exempt) << exempt.error();
        EXPECT_FALSE(exempt->has_value()) << "豁免表命中的目标仍在走代理";

        HttpProxyPolicy broken;
        broken.consultEnvironment = false;
        broken.httpProxy          = "socks5://bad.example:1080";
        const auto refusal        = resolveOutboundProxy(broken, "http", "origin.example", 80);
        ASSERT_FALSE(refusal.has_value()) << "坏代理地址被静默放行（应当拒绝请求）";
        EXPECT_NE(refusal.error().find("socks5"), std::string::npos) << refusal.error();

        HttpProxyPolicy none;
        none.consultEnvironment = false;
        const auto direct       = resolveOutboundProxy(none, "http", "origin.example", 80);
        ASSERT_HAS_VALUE(direct) << direct.error();
        EXPECT_FALSE(direct->has_value()) << "没有配置代理时没有直连";

        const auto otherScheme = resolveOutboundProxy(policy, "ftp", "origin.example", 21);
        ASSERT_HAS_VALUE(otherScheme) << otherScheme.error();
        EXPECT_FALSE(otherScheme->has_value()) << "非 http(s) 的 scheme 不该套代理";
    }

    /**
     * @brief 打码：凭据段换成 ***（带 scheme 与裸权威两种形状），无凭据原样
     */
    TEST(HttpProxyPolicy, RedactsCredentialsInEchoedProxyText)
    {
        EXPECT_EQ(redactProxyCredentials("http://alice:s3cret@proxy.example:3128"), "http://***@proxy.example:3128");
        EXPECT_EQ(redactProxyCredentials("alice:pass@proxy.example:3128"), "***@proxy.example:3128");
        EXPECT_EQ(redactProxyCredentials("http://proxy.example:3128"), "http://proxy.example:3128");
        EXPECT_EQ(redactProxyCredentials(""), "");
    }

    // ============================================================================
    // e2e 面：脚本化 TCP 对端（当代理或当起点）+ 真 TLS 起点，判据落在线上字节与响应上
    // ============================================================================
    namespace
    {
        using AsynGyanis::TestSupport::waitForCondition;

        /// 证书夹具：与 TestHttpsServer 同一套自签材料（含 127.0.0.1 的 SAN）
        const std::filesystem::path kProxyFixtureCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";
        const std::filesystem::path kProxyFixturePrivateKeyPath  = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";
        /// SAN 只有 IP:127.0.0.1 的那张夹具证书：TLS 代理夹具监听在回环 IP 上，客户端按 IP 字面量校验
        const std::filesystem::path kLoopbackFixtureCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem";
        const std::filesystem::path kLoopbackFixturePrivateKeyPath  = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem";
        constexpr auto              kProxyWaitTimeout               = std::chrono::milliseconds{10000};
        constexpr auto              kProxyScriptTimeout             = std::chrono::seconds{5};

        /**
         * @brief 把多张自签夹具证书拼成一个 PEM 信任库（TLS-in-TLS 用例：外层验代理、内层验起点）
         * @details 两层 TLS 各出示一张不同的自签证书，而客户端只有一份 TlsPolicy——把两张都写进
         *          同一个 CA 文件即可。文件落在系统临时目录，析构即删。
         */
        class CombinedCertificateAuthorityFile
        {
        public:
            CombinedCertificateAuthorityFile(const std::filesystem::path &firstCertificate, const std::filesystem::path &secondCertificate)
            {
                std::string combinedText;
                for (const std::filesystem::path *source: {&firstCertificate, &secondCertificate})
                {
                    std::ifstream input(*source, std::ios::binary);
                    combinedText += std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                    combinedText += '\n';
                }
                m_path = std::filesystem::temp_directory_path() / ("asyngyanis-proxy-ca-" + std::to_string(std::random_device{}()) + ".pem");
                std::ofstream output(m_path, std::ios::binary);
                output << combinedText;
            }

            ~CombinedCertificateAuthorityFile()
            {
                std::error_code failure;
                std::filesystem::remove(m_path, failure);
            }

            CombinedCertificateAuthorityFile(const CombinedCertificateAuthorityFile &)            = delete;
            CombinedCertificateAuthorityFile &operator=(const CombinedCertificateAuthorityFile &) = delete;

            [[nodiscard]] const std::filesystem::path &path() const noexcept
            {
                return m_path;
            }

        private:
            std::filesystem::path m_path;
        };

        /// 把整段字节送出去；对端收口时放弃（SIGPIPE 由 Socket::Initialization 在进程级忽略）
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

        /**
         * @brief 剧本拿着的那条已接受连接：读请求头区、回文、拼接转发
         */
        class ScriptedPeerConnection
        {
        public:
            ScriptedPeerConnection(const int socket, std::string &headSink, std::mutex &sinkMutex, const std::atomic<bool> &isStopping) noexcept :
                m_socket(socket), m_headSink(headSink), m_sinkMutex(sinkMutex), m_isStopping(isStopping)
            {
            }

            /// 读到头区结束（CRLFCRLF，含）；超时或对端收口则把已读到的部分交出来，并把全部读数记进台账
            std::string readRequestHead()
            {
                std::string buffer;
                const auto  deadline = std::chrono::steady_clock::now() + kProxyScriptTimeout;
                while (buffer.find("\r\n\r\n") == std::string::npos && !m_isStopping.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(m_socket, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(m_socket + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }
                    char          chunk[4096];
                    const ssize_t received = ::recv(m_socket, chunk, static_cast<int>(sizeof(chunk)), 0);
                    if (received <= 0)
                    {
                        break;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(received));
                }
                {
                    const std::lock_guard guard(m_sinkMutex);
                    m_headSink += buffer;
                }
                return buffer;
            }

            bool sendText(const std::string_view text)
            {
                return sendAll(m_socket, text);
            }

            /// 把本连接与 loopback:targetPort 拼起来双向转发，直到任一侧收口或收到收摊标志
            void spliceToLoopback(const std::uint16_t targetPort)
            {
                const int upstream = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(upstream))
                {
                    return;
                }
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(targetPort);
                if (::connect(upstream, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
                {
                    Platform::FileDescriptor::close(upstream);
                    return;
                }

                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(m_socket, &readSet);
                    FD_SET(upstream, &readSet);
                    const int highest = (m_socket > upstream ? m_socket : upstream) + 1;
                    timeval   pollInterval{0, 50 * 1000};
                    if (::select(highest, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }
                    bool isDone = false;
                    for (const int readable: {m_socket, upstream})
                    {
                        if (FD_ISSET(readable, &readSet) == 0)
                        {
                            continue;
                        }
                        char          chunk[16384];
                        const ssize_t received = ::recv(readable, chunk, static_cast<int>(sizeof(chunk)), 0);
                        if (received <= 0 || !sendAll(readable == m_socket ? upstream : m_socket, std::string_view(chunk, static_cast<std::size_t>(received))))
                        {
                            isDone = true;
                            break;
                        }
                    }
                    if (isDone)
                    {
                        break;
                    }
                }
                Platform::FileDescriptor::close(upstream);
            }

        private:
            int                      m_socket;     ///< 已接受的连接
            std::string             &m_headSink;   ///< 请求头区读数落点（跨线程经 m_sinkMutex）
            std::mutex              &m_sinkMutex;  ///< 保护落点
            const std::atomic<bool> &m_isStopping; ///< 收摊标志
        };

        /**
         * @brief 只在 127.0.0.1 随机端口上说话的脚本化 TCP 对端：accept 一条跑一条剧本，剧本自己收口
         */
        class ScriptedTcpPeer
        {
        public:
            using PeerScript = std::function<void(ScriptedPeerConnection &)>;

            explicit ScriptedTcpPeer(PeerScript script) : m_script(std::move(script))
            {
            }

            ScriptedTcpPeer(const ScriptedTcpPeer &)            = delete;
            ScriptedTcpPeer &operator=(const ScriptedTcpPeer &) = delete;

            ~ScriptedTcpPeer()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            /// 起工作线程并等它把端口报回来
            bool start()
            {
                m_worker = std::thread([this] { run(); });
                return waitFor([this] { return m_port.load(std::memory_order_acquire) != 0U; });
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::size_t acceptedConnectionCount() const noexcept
            {
                return m_acceptedCount.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::string recordedHeadBytes() const
            {
                const std::lock_guard guard(m_sinkMutex);
                return m_headBytes;
            }

        private:
            static bool waitFor(const std::function<bool()> &condition)
            {
                const auto deadline = std::chrono::steady_clock::now() + kProxyWaitTimeout;
                while (!condition() && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return condition();
            }

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
                    ScriptedPeerConnection connection(client, m_headBytes, m_sinkMutex, m_isStopping);
                    m_script(connection);
                    Platform::FileDescriptor::close(client);
                }
                Platform::FileDescriptor::close(listener);
            }

            PeerScript                 m_script;
            std::thread                m_worker;
            std::atomic<bool>          m_isStopping{false};
            std::atomic<std::uint16_t> m_port{0};
            std::atomic<std::size_t>   m_acceptedCount{0};
            mutable std::mutex         m_sinkMutex;
            std::string                m_headBytes;
        };

        /**
         * @brief ScriptedTlsPeer 交到剧本手里的一条已握手连接：读头 / 写响应都经 SSL
         */
        class ScriptedTlsPeerConnection
        {
        public:
            ScriptedTlsPeerConnection(SSL *ssl, const int socket, std::string &headSink, std::mutex &sinkMutex, const std::atomic<bool> &isStopping) :
                m_ssl(ssl), m_socket(socket), m_headSink(headSink), m_sinkMutex(sinkMutex), m_isStopping(isStopping)
            {
            }

            ScriptedTlsPeerConnection(const ScriptedTlsPeerConnection &)            = delete;
            ScriptedTlsPeerConnection &operator=(const ScriptedTlsPeerConnection &) = delete;

            /// 读到头区结束（CRLFCRLF，含）；超时或对端收口交出已读部分，并把全部读数记进台账
            std::string readRequestHead()
            {
                std::string buffer;
                const auto  deadline = std::chrono::steady_clock::now() + kProxyScriptTimeout;
                while (buffer.find("\r\n\r\n") == std::string::npos && !m_isStopping.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(m_socket, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(m_socket + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }
                    char      chunk[4096];
                    const int received = SSL_read(m_ssl, chunk, static_cast<int>(sizeof(chunk)));
                    if (received <= 0)
                    {
                        if (const int error = SSL_get_error(m_ssl, received); error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                        {
                            continue;
                        }
                        break;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(received));
                }
                {
                    const std::lock_guard guard(m_sinkMutex);
                    m_headSink += buffer;
                }
                return buffer;
            }

            /// 经 TLS 把整段字节写出去；对端收口时放弃
            bool sendText(const std::string_view text)
            {
                return sslWriteAll(text.data(), text.size());
            }

            /// 把 TLS 会话与 loopback:targetPort 拼起来双向转发（TLS-in-TLS 用例里代理的行为）
            /// @details 逐段转发解密后的字节：TLS 侧收到什么就原样送往上游、上游回什么就经 SSL_write 送回。
            ///          阻塞模型（worker 线程专用），select 50ms 轮询给收摊标志留出口
            void spliceToLoopback(const std::uint16_t targetPort)
            {
                const int upstream = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(upstream))
                {
                    return;
                }
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(targetPort);
                if (::connect(upstream, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
                {
                    Platform::FileDescriptor::close(upstream);
                    return;
                }

                bool upstreamOpen = true;
                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(upstream, &readSet);
                    FD_SET(m_socket, &readSet);
                    const int highest = (upstream > m_socket ? upstream : m_socket) + 1;
                    timeval   pollInterval{0, 50 * 1000};
                    if (::select(highest, &readSet, nullptr, nullptr, &pollInterval) < 0)
                    {
                        break;
                    }

                    if (upstreamOpen && FD_ISSET(upstream, &readSet))
                    {
                        char          buffer[8192];
                        const ssize_t received = ::recv(upstream, buffer, sizeof(buffer), 0);
                        if (received <= 0)
                        {
                            upstreamOpen = false;
                        } else if (!sslWriteAll(buffer, static_cast<std::size_t>(received)))
                        {
                            break;
                        }
                    }

                    // TLS 侧可读两层含义：网络上有记录，或解密缓冲里还留着上次没读走的明文
                    if (FD_ISSET(m_socket, &readSet) || SSL_pending(m_ssl) > 0)
                    {
                        char      buffer[8192];
                        const int received = SSL_read(m_ssl, buffer, static_cast<int>(sizeof(buffer)));
                        if (received <= 0)
                        {
                            if (const int error = SSL_get_error(m_ssl, received); error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                            {
                                continue;
                            }
                            break;
                        }
                        if (!upstreamOpen || !sendAll(upstream, std::string_view(buffer, static_cast<std::size_t>(received))))
                        {
                            break;
                        }
                    }
                }
                Platform::FileDescriptor::close(upstream);
            }

        private:
            /// 循环写全一段字节（SSL_write 允许部分写）；失败返回 false
            bool sslWriteAll(const char *bytes, const std::size_t byteCount)
            {
                std::size_t written = 0;
                while (written < byteCount)
                {
                    const int result = SSL_write(m_ssl, bytes + written, static_cast<int>(byteCount - written));
                    if (result <= 0)
                    {
                        const int error = SSL_get_error(m_ssl, result);
                        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                        {
                            continue;
                        }
                        return false;
                    }
                    written += static_cast<std::size_t>(result);
                }
                return true;
            }

        private:
            SSL                     *m_ssl;        ///< 已握手的服务端会话（不拥有）
            int                      m_socket;     ///< 底层描述符（只用于 select 前置等待）
            std::string             &m_headSink;   ///< 请求头区读数落点（跨线程经 m_sinkMutex）
            std::mutex              &m_sinkMutex;  ///< 保护落点
            const std::atomic<bool> &m_isStopping; ///< 收摊标志
        };

        /**
         * @brief 脚本化 TLS 对端（https 代理夹具）：accept 一条、先做服务端 TLS 握手、再跑一条剧本
         * @details 与 ScriptedTcpPeer 同骨架，多出的三件事：每连接先用夹具证书完成一次服务端握手、
         *          记录本次协商出的 ALPN（外层到代理的 TLS 不含 ALPN，用例据此钉住「外层恒走
         *          HTTP/1.1 语义」）、剧本收发一律经 SSL（fd 只用于 select 前置等待）。
         *          证书用 SAN=IP:127.0.0.1 的那张：客户端按回环 IP 字面量校验主机名。
         */
        class ScriptedTlsPeer
        {
        public:
            using TlsPeerScript = std::function<void(SSL *ssl, class ScriptedTlsPeerConnection &connection)>;

            explicit ScriptedTlsPeer() = default;

            ScriptedTlsPeer(const ScriptedTlsPeer &)            = delete;
            ScriptedTlsPeer &operator=(const ScriptedTlsPeer &) = delete;

            ~ScriptedTlsPeer()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            /// 指定证书以后台 TLS 服务端身份起监听（剧本在 start 之前装好）
            bool start(TlsPeerScript script, const std::filesystem::path &certificatePath = kLoopbackFixtureCertificatePath,
                       const std::filesystem::path &privateKeyPath = kLoopbackFixturePrivateKeyPath)
            {
                m_script = std::move(script);
                Core::TlsPolicy policy;
                m_context = std::make_unique<Core::TlsContext>(policy, Core::TlsContext::Role::Server);
                if (!m_context->loadCertificate(certificatePath.string(), privateKeyPath.string()))
                {
                    return false;
                }
                m_worker = std::thread([this] { run(); });
                return waitFor([this] { return m_port.load(std::memory_order_acquire) != 0U; });
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::size_t acceptedConnectionCount() const noexcept
            {
                return m_acceptedCount.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::size_t completedHandshakeCount() const noexcept
            {
                return m_handshakeCount.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::string recordedHeadBytes() const
            {
                const std::lock_guard guard(m_sinkMutex);
                return m_headBytes;
            }

            /// 最近一条连接上协商出的 ALPN；外层到代理的 TLS 不声明 ALPN，因此这里应为空串
            [[nodiscard]] std::string recordedNegotiatedAlpn() const
            {
                const std::lock_guard guard(m_sinkMutex);
                return m_negotiatedAlpn;
            }

        private:
            static bool waitFor(const std::function<bool()> &condition)
            {
                const auto deadline = std::chrono::steady_clock::now() + kProxyWaitTimeout;
                while (!condition() && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return condition();
            }

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
                    serveOneConnection(client);
                    Platform::FileDescriptor::close(client);
                }
                Platform::FileDescriptor::close(listener);
            }

            void serveOneConnection(const int client)
            {
                std::unique_ptr<SSL, void (*)(SSL *)> ssl(m_context->createSSL(client), [](SSL *session) { SSL_free(session); });
                if (ssl == nullptr || SSL_accept(ssl.get()) != 1)
                {
                    return;
                }
                m_handshakeCount.fetch_add(1, std::memory_order_acq_rel);

                const unsigned char *negotiated      = nullptr;
                unsigned int         negotiatedBytes = 0;
                SSL_get0_alpn_selected(ssl.get(), &negotiated, &negotiatedBytes);
                {
                    const std::lock_guard guard(m_sinkMutex);
                    m_negotiatedAlpn.assign(negotiated != nullptr ? reinterpret_cast<const char *>(negotiated) : "", negotiatedBytes);
                }

                ScriptedTlsPeerConnection connection(ssl.get(), client, m_headBytes, m_sinkMutex, m_isStopping);
                m_script(ssl.get(), connection);
                SSL_shutdown(ssl.get());
            }

            TlsPeerScript                     m_script;
            std::unique_ptr<Core::TlsContext> m_context;
            std::thread                       m_worker;
            std::atomic<bool>                 m_isStopping{false};
            std::atomic<std::uint16_t>        m_port{0};
            std::atomic<std::size_t>          m_acceptedCount{0};
            std::atomic<std::size_t>          m_handshakeCount{0};
            mutable std::mutex                m_sinkMutex;
            std::string                       m_headBytes;
            std::string                       m_negotiatedAlpn;
        };

        /**
         * @brief 把监听描述符透出成测试访问器的 HttpsServer（模板夹具的 listeningPort 需要它）
         */
        class AddressableHttpsServer final : public HttpsServer
        {
        public:
            using HttpsServer::HttpsServer;

            [[nodiscard]] int listenDescriptor() const
            {
                return m_acceptor.fileDescriptor();
            }

            [[nodiscard]] std::size_t activeConnectionCount() const
            {
                return m_connectionManager.activeCount();
            }
        };

        /**
         * @brief 跑起一台真 TLS 起点（自签夹具证书）的夹具；起点上有基类注册的 /hello → served-hello
         */
        class RunningTlsOriginFixture final : public HttpTestSupport::RunningServerFixture<AddressableHttpsServer>
        {
        public:
            RunningTlsOriginFixture() :
                RunningServerFixture<AddressableHttpsServer>(HttpServerLimits{}, std::chrono::milliseconds{100}, HttpParserLimits{},
                                                             [](Core::EventLoop &serverLoop)
                                                             {
                                                                 return AddressableHttpsServer(serverLoop, Core::InetAddress::localhost(0), kProxyFixtureCertificatePath.string(),
                                                                                               kProxyFixturePrivateKeyPath.string());
                                                             })
            {
                startServer();
            }
        };

        /// 一次实例路径请求的结局：成功留正文，失败留原因
        struct RequestOutcome
        {
            std::string body;
            std::string reason;
        };

        Core::Task<void> clientGetTask(Core::EventLoop &loop, HttpClient &client, std::string url, RequestOutcome &outcome)
        {
            try
            {
                if (std::unique_ptr<HttpClientResponse> response = co_await client.get(url); response != nullptr)
                {
                    outcome.body = response->body;
                } else
                {
                    outcome.reason = "客户端返回空响应（原因见日志）";
                }
            } catch (const std::exception &failure)
            {
                outcome.reason = failure.what();
            }
            loop.stop();
        }

        Core::Task<void> clientTwoGetsTask(Core::EventLoop &loop, HttpClient &client, std::string url, RequestOutcome &first, RequestOutcome &second)
        {
            try
            {
                if (std::unique_ptr<HttpClientResponse> response = co_await client.get(url); response != nullptr)
                {
                    first.body = response->body;
                } else
                {
                    first.reason = "客户端返回空响应（原因见日志）";
                }
                if (std::unique_ptr<HttpClientResponse> response = co_await client.get(url); response != nullptr)
                {
                    second.body = response->body;
                } else
                {
                    second.reason = "客户端返回空响应（原因见日志）";
                }
            } catch (const std::exception &failure)
            {
                second.reason = failure.what();
            }
            loop.stop();
        }

        /// 静态路径（每次调用现读环境）的结局：成功/失败 + 正文或原因
        struct StaticSendResult
        {
            bool        succeeded{false};
            std::string body;
            std::string reason;
        };

        Core::Task<void> staticSendTask(Core::EventLoop &loop, std::string url, StaticSendResult &result)
        {
            try
            {
                std::expected<HttpClientResponse, std::string> sent = co_await HttpClient::send(loop, url, HttpClientRequest{});
                if (sent.has_value())
                {
                    result.succeeded = true;
                    result.body      = sent->body;
                } else
                {
                    result.reason = sent.error();
                }
            } catch (const std::exception &failure)
            {
                result.reason = failure.what();
            }
            loop.stop();
        }

        StaticSendResult runStaticSend(std::string url)
        {
            Core::EventLoop  loop;
            StaticSendResult result;
            Core::Task<void> task = staticSendTask(loop, std::move(url), result);
            if (!task.isReady())
            {
                loop.scheduler().schedule(task.handle());
            }
            loop.run();
            return result;
        }
    } // namespace

    /**
     * @brief 明文请求经代理：请求行必须是绝对形式（RFC 9112 §3.2.2），并带上代理认证头
     */
    TEST(HttpProxyE2E, PlainRequestGoesThroughProxyInAbsoluteForm)
    {
        ScriptedTcpPeer proxy(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nvia-prx"));
                });
        ASSERT_TRUE(proxy.start()) << "代理对端没起来";

        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpProxy          = "http://alice:s3cret@127.0.0.1:" + std::to_string(proxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, Core::TlsPolicy{}, policy);

        RequestOutcome   outcome;
        Core::Task<void> task = clientGetTask(loop, client, "http://example.test/echo?x=1", outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(outcome.reason.empty()) << outcome.reason;
        EXPECT_EQ(outcome.body, "via-prx") << "响应没有从代理那条路回来";
        const std::string head = proxy.recordedHeadBytes();
        EXPECT_NE(head.find("GET http://example.test/echo?x=1 HTTP/1.1"), std::string::npos) << "请求行不是绝对形式：" << head;
        EXPECT_NE(head.find("Host: example.test"), std::string::npos) << head;
        EXPECT_NE(head.find("Proxy-Authorization: Basic YWxpY2U6czNjcmV0"), std::string::npos) << "代理认证头没带上或值不对：" << head;
    }

    /**
     * @brief http 目标经 https 代理：先与代理做 TLS 握手（按代理主机名校验），绝对形式请求走 TLS
     * @details 代理地址的 scheme 决定「怎么连代理」：https:// 代理要先 TLS 再说话，请求形状不变
     *          （绝对形式 + Proxy-Authorization，RFC 9112 §3.2.2）。外层 TLS 刻意不声明 ALPN——
     *          到代理的连接恒按 HTTP/1.1 语义（CONNECT 与绝对形式都是 h1 的），代理侧读数为空
     *          是这条取舍的线上判据。
     *          证伪：让建立侧忽略 isTls 按明文连代理，代理侧握手计数停在 0、请求头是裸字节，全红。
     */
    TEST(HttpProxyE2E, HttpOverTlsProxyHandshakesTlsFirstAndStaysHttp11)
    {
        ScriptedTlsPeer proxy;
        ASSERT_TRUE(proxy.start(
                [](SSL *, ScriptedTlsPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nvia-tls"));
                }))
                << "TLS 代理对端没起来";

        Core::TlsPolicy tlsPolicy;
        tlsPolicy.certificateAuthorityFile = kLoopbackFixtureCertificatePath.string();
        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpProxy          = "https://alice:s3cret@127.0.0.1:" + std::to_string(proxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, tlsPolicy, policy);

        RequestOutcome   outcome;
        Core::Task<void> task = clientGetTask(loop, client, "http://example.test/echo?x=1", outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(outcome.reason.empty()) << outcome.reason;
        EXPECT_EQ(outcome.body, "via-tls") << "响应没有从 TLS 代理那条路回来";
        EXPECT_EQ(proxy.completedHandshakeCount(), 1U) << "与代理之间没有完成 TLS 握手";
        EXPECT_TRUE(proxy.recordedNegotiatedAlpn().empty()) << "外层到代理的 TLS 声明了 ALPN，协商出「" << proxy.recordedNegotiatedAlpn() << "」";
        const std::string head = proxy.recordedHeadBytes();
        EXPECT_NE(head.find("GET http://example.test/echo?x=1 HTTP/1.1"), std::string::npos) << "TLS 里的请求行不是绝对形式：" << head;
        EXPECT_NE(head.find("Proxy-Authorization: Basic YWxpY2U6czNjcmV0"), std::string::npos) << "代理认证头没带上或值不对：" << head;
    }

    /**
     * @brief https 代理的证书校验按代理主机名走：链已受信而名字不符同样拒
     * @details 客户端信任库注入的正是代理出示那张证书的签发者（自签），链校验因此会过——
     *          挡下它必须靠主机名校验（对端 127.0.0.1 vs CN=asyngyanis-test）。实例路径的失败原因
     *          走 ERROR 日志（get() 的契约），这里钉的是「连上了代理、TLS 没成」这一形状：
     *          接受计数为 1 排除「根本没连上」，握手计数为 0 说明败在 TLS 层。
     *          证伪：给外层 TLS 摘掉 SSL_set1_host/set1_ip_asc 那一段，握手被放行，全红。
     */
    TEST(HttpProxyE2E, TlsProxyWithMismatchedHostnameIsRejected)
    {
        ScriptedTlsPeer mismatchProxy;
        // 故意用 CN=asyngyanis-test 的那张：客户端按 IP 127.0.0.1 校验，必须验不过
        ASSERT_TRUE(mismatchProxy.start(
                [](SSL *, ScriptedTlsPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
                },
                kProxyFixtureCertificatePath, kProxyFixturePrivateKeyPath))
                << "TLS 代理对端没起来";

        Core::TlsPolicy tlsPolicy;
        // 信任库收下那张证书的签发者（自签即它自己）：链校验会过，只剩主机名一关挡着
        tlsPolicy.certificateAuthorityFile = kProxyFixtureCertificatePath.string();
        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpProxy          = "https://127.0.0.1:" + std::to_string(mismatchProxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, tlsPolicy, policy);

        RequestOutcome   outcome;
        Core::Task<void> task = clientGetTask(loop, client, "http://example.test/hello", outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_FALSE(outcome.reason.empty()) << "证书不符的代理握手被放行，请求成功了";
        EXPECT_EQ(mismatchProxy.acceptedConnectionCount(), 1U) << "请求没连到代理，失败原因不在本用例要考的那一格";
        EXPECT_EQ(mismatchProxy.completedHandshakeCount(), 0U) << "客户端没通过校验，代理侧却完成了握手";
    }

    /**
     * @brief 静态入口经 https 代理遇到不受信的代理证书：失败原因点名「代理」这一层
     * @details 静态入口没有注入 TLS 策略的口，用的是进程级信任库（惰性创建并缓存，
     *          见 HttpsServer 那批静态用例的同一约定）：本用例按约定在首个静态 HTTPS 请求
     *          之前把信任库设成 SAN=IP:127.0.0.1 的夹具证书，代理出示另一张自签（CN=asyngyanis-test）
     *          因此不受信。失败要点到「与代理的 TLS 握手」：排查时这正是「代理证书有问题」
     *          与「起点证书有问题」的分界。
     */
    TEST(HttpProxyE2E, UntrustedTlsProxyFailsWithProxyNamedReason)
    {
        ScriptedTlsPeer untrustedProxy;
        // 代理出示 CN=asyngyanis-test 的那张：与下面设的信任库（IP 那张）不是同一份签发者
        ASSERT_TRUE(untrustedProxy.start(
                [](SSL *, ScriptedTlsPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
                },
                kProxyFixtureCertificatePath, kProxyFixturePrivateKeyPath))
                << "TLS 代理对端没起来";

        // 进程级信任库的既有约定：必须是进程内首个静态 HTTPS 请求之前设好；取 IP 那张夹具证书
        const ScopedEnvironmentVariable trustedFile{"SSL_CERT_FILE", kLoopbackFixtureCertificatePath.string()};
        const ScopedEnvironmentVariable httpProxy{"http_proxy", std::string("https://127.0.0.1:") + std::to_string(untrustedProxy.port())};
        const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

        const StaticSendResult result = runStaticSend("http://example.test/hello");
        EXPECT_FALSE(result.succeeded) << "不受信的代理证书被放行了";
        EXPECT_NE(result.reason.find("TLS"), std::string::npos) << result.reason;
        EXPECT_NE(result.reason.find("代理"), std::string::npos) << "失败原因没有点到「代理」这一层：" << result.reason;
        EXPECT_EQ(untrustedProxy.completedHandshakeCount(), 0U) << "客户端没通过校验，代理侧却完成了握手";
    }

    /**
     * @brief https 目标经 https 代理：TLS-in-TLS 全链（外层到代理、内层到起点），第二条复用同一条隧道
     * @details 拔号目标是代理（外层 TLS 按代理主机名 127.0.0.1 校验、出示 IP 那张夹具证书），
     *          CONNECT 在外层 TLS 里发，内层 TLS 按目标主机名 asyngyanis-test 校验（出示 CN 那张）。
     *          客户端只有一份 TlsPolicy，因此信任库是把两张自签证书拼在一个文件里的临时 PEM。
     *          代理侧的桥接逐段转发解密字节——两条 TLS 会话各自独立，泵（内层）只要把字节原样搬运。
     *          证伪：拆掉内层泵的任何一半（不把 SSL 的输出排出、或不把外层字节灌进去），握手当场卡死。
     */
    TEST(HttpProxyE2E, TlsTargetThroughTlsProxyCompletesTlsInTls)
    {
        RunningTlsOriginFixture origin;
        ASSERT_TRUE(origin.awaitRunning(kProxyWaitTimeout)) << "TLS 起点没起来";

        ScriptedTlsPeer tlsProxy;
        ASSERT_TRUE(tlsProxy.start(
                [&origin](SSL *, ScriptedTlsPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 Connection Established\r\n\r\n"));
                    connection.spliceToLoopback(origin.listeningPort());
                }))
                << "TLS 代理对端没起来";

        const CombinedCertificateAuthorityFile trustStore(kLoopbackFixtureCertificatePath, kProxyFixtureCertificatePath);
        Core::TlsPolicy                        tlsPolicy;
        tlsPolicy.certificateAuthorityFile = trustStore.path().string();
        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpsProxy         = "https://127.0.0.1:" + std::to_string(tlsProxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, tlsPolicy, policy);

        const std::string url = "https://asyngyanis-test:" + std::to_string(origin.listeningPort()) + "/hello";
        RequestOutcome    first;
        RequestOutcome    second;
        Core::Task<void>  task = clientTwoGetsTask(loop, client, url, first, second);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(first.reason.empty()) << first.reason;
        EXPECT_TRUE(second.reason.empty()) << second.reason;

        EXPECT_EQ(first.body, "served-hello") << "TLS-in-TLS 第一条没到达起点";
        EXPECT_EQ(second.body, "served-hello") << "TLS-in-TLS 第二条没到达起点";
        EXPECT_EQ(tlsProxy.completedHandshakeCount(), 1U) << "与代理的外层 TLS 没完成";
        EXPECT_EQ(tlsProxy.acceptedConnectionCount(), 1U) << "两条请求没有复用同一条外层隧道";
        EXPECT_TRUE(tlsProxy.recordedNegotiatedAlpn().empty()) << "外层到代理的 TLS 声明了 ALPN，协商出「" << tlsProxy.recordedNegotiatedAlpn() << "」";
        const std::string head = tlsProxy.recordedHeadBytes();
        EXPECT_NE(head.find("CONNECT asyngyanis-test:" + std::to_string(origin.listeningPort()) + " HTTP/1.1"), std::string::npos)
                << "CONNECT 不是 authority-form 或没发进外层 TLS：" << head;
    }

    /**
     * @brief TLS-in-TLS 的内层照常声明 ALPN：谈出 h2 时多路复用经两层 TLS 走通
     * @details 内层 TLS 的字节流由泵搬运，而 h2 的读写是并发协程——这条用例是泵「方向分离」
     *          设计的实战检验（写侧排空与读侧灌入互不挡道）。起点夹具的 ALPN 偏好是 h2，
     *          因此谈出 h2 之后请求必须真的按 h2 说话并拿到响应。
     */
    TEST(HttpProxyE2E, TlsInTlsInnerAlpnNegotiatesHttp2AndRequestsFlowThrough)
    {
        RunningTlsOriginFixture origin;
        ASSERT_TRUE(origin.awaitRunning(kProxyWaitTimeout)) << "TLS 起点没起来";

        ScriptedTlsPeer tlsProxy;
        ASSERT_TRUE(tlsProxy.start(
                [&origin](SSL *, ScriptedTlsPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 Connection Established\r\n\r\n"));
                    connection.spliceToLoopback(origin.listeningPort());
                }))
                << "TLS 代理对端没起来";

        const CombinedCertificateAuthorityFile trustStore(kLoopbackFixtureCertificatePath, kProxyFixtureCertificatePath);
        Core::TlsPolicy                        tlsPolicy;
        tlsPolicy.certificateAuthorityFile = trustStore.path().string();
        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpsProxy         = "https://127.0.0.1:" + std::to_string(tlsProxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, tlsPolicy, policy);

        const std::string url = "https://asyngyanis-test:" + std::to_string(origin.listeningPort()) + "/hello";
        RequestOutcome    outcome;
        Core::Task<void>  task = clientGetTask(loop, client, url, outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(outcome.reason.empty()) << outcome.reason;
        EXPECT_EQ(outcome.body, "served-hello") << "内层 ALPN 谈出 h2 之后请求没走通";
        EXPECT_EQ(client.idleHttp2ConnectionCount(), 1U) << "内层没谈出 h2（或没留下 h2 连接）——ALPN 没有被照常转发";
    }

    /**
     * @brief NO_PROXY 命中：直连起点（origin-form），代理一个连接都不该收到
     */
    TEST(HttpProxyE2E, NoProxyEntryBypassesTheProxyForOriginFormRequests)
    {
        ScriptedTcpPeer origin(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\ndirect"));
                });
        ASSERT_TRUE(origin.start()) << "起点对端没起来";

        ScriptedTcpPeer proxy(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"));
                });
        ASSERT_TRUE(proxy.start()) << "代理对端没起来";

        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpProxy          = "http://127.0.0.1:" + std::to_string(proxy.port());
        policy.noProxy            = "127.0.0.1";
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, Core::TlsPolicy{}, policy);

        RequestOutcome   outcome;
        Core::Task<void> task = clientGetTask(loop, client, "http://127.0.0.1:" + std::to_string(origin.port()) + "/hello", outcome);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(outcome.reason.empty()) << outcome.reason;
        EXPECT_EQ(outcome.body, "direct") << "豁免表命中的请求没有直连";
        EXPECT_EQ(proxy.acceptedConnectionCount(), 0U) << "豁免表命中的请求仍然连了代理";
        const std::string head = origin.recordedHeadBytes();
        EXPECT_NE(head.find("GET /hello HTTP/1.1"), std::string::npos) << "直连请求的请求行不是 origin-form：" << head;
        EXPECT_EQ(head.find("http://"), std::string::npos) << "直连请求被写成了绝对形式：" << head;
    }

    /**
     * @brief 环境驱动：静态入口每次调用现读 http_proxy，明文请求经代理（绝对形式）
     */
    TEST(HttpProxyE2E, EnvironmentDrivenProxyAppliesToStaticRequests)
    {
        ScriptedTcpPeer proxy(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nenv-via!"));
                });
        ASSERT_TRUE(proxy.start()) << "代理对端没起来";

        const ScopedEnvironmentVariable httpProxy{"http_proxy", std::string("http://127.0.0.1:") + std::to_string(proxy.port())};
        const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

        const StaticSendResult result = runStaticSend("http://env-target.test/hello");
        ASSERT_TRUE(result.succeeded) << result.reason;
        EXPECT_EQ(result.body, "env-via!") << "环境里的代理没有被静态入口采用";
        const std::string head = proxy.recordedHeadBytes();
        EXPECT_NE(head.find("GET http://env-target.test/hello HTTP/1.1"), std::string::npos) << head;
    }

    /**
     * @brief https 经 CONNECT 隧道到真 TLS 起点；第二条请求复用同一条隧道（代理侧连接计数钉住）
     */
    TEST(HttpProxyE2E, ConnectTunnelCarriesHttpsAndThePoolReusesTheTunnel)
    {
        RunningTlsOriginFixture origin;
        ASSERT_TRUE(origin.awaitRunning(kProxyWaitTimeout)) << "TLS 起点没起来";

        ScriptedTcpPeer proxy(
                [&origin](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 Connection Established\r\n\r\n"));
                    connection.spliceToLoopback(origin.listeningPort());
                });
        ASSERT_TRUE(proxy.start()) << "代理对端没起来";

        Core::TlsPolicy tlsPolicy;
        tlsPolicy.certificateAuthorityFile = kProxyFixtureCertificatePath.string();
        Core::EventLoop loop;
        HttpProxyPolicy policy;
        policy.consultEnvironment = false;
        policy.httpsProxy         = "http://alice:s3cret@127.0.0.1:" + std::to_string(proxy.port());
        HttpClient client(loop, HttpOutboundConnectionPool::Config{}, tlsPolicy, policy);

        const std::string url = "https://asyngyanis-test:" + std::to_string(origin.listeningPort()) + "/hello";
        RequestOutcome    first;
        RequestOutcome    second;
        Core::Task<void>  task = clientTwoGetsTask(loop, client, url, first, second);
        if (!task.isReady())
        {
            loop.scheduler().schedule(task.handle());
        }
        loop.run();

        EXPECT_TRUE(first.reason.empty()) << first.reason;
        EXPECT_TRUE(second.reason.empty()) << second.reason;
        EXPECT_EQ(first.body, "served-hello") << "第一条 https 请求没有经隧道到达起点";
        EXPECT_EQ(second.body, "served-hello") << "第二条 https 请求没有经隧道到达起点";
        EXPECT_EQ(proxy.acceptedConnectionCount(), 1U) << "两条请求没有复用同一条隧道";
        const std::string head = proxy.recordedHeadBytes();
        EXPECT_NE(head.find("CONNECT asyngyanis-test:" + std::to_string(origin.listeningPort()) + " HTTP/1.1"), std::string::npos) << "CONNECT 不是 authority-form：" << head;
        EXPECT_NE(head.find("Proxy-Authorization: Basic YWxpY2U6czNjcmV0"), std::string::npos) << "CONNECT 没带代理认证头：" << head;
    }

    /**
     * @brief CONNECT 被拒：失败原因点名代理给的状态码，407 的措辞指向凭据写法
     */
    TEST(HttpProxyE2E, ConnectRejectionSurfacesTheProxyStatus)
    {
        {
            ScriptedTcpPeer reject(
                    [](ScriptedPeerConnection &connection)
                    {
                        static_cast<void>(connection.readRequestHead());
                        static_cast<void>(connection.sendText("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n"));
                    });
            ASSERT_TRUE(reject.start()) << "代理对端没起来";
            const ScopedEnvironmentVariable httpsProxy{"https_proxy", std::string("http://127.0.0.1:") + std::to_string(reject.port())};
            const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

            const StaticSendResult result = runStaticSend("https://origin.test/hello");
            EXPECT_FALSE(result.succeeded) << "407 之后请求仍然成功了";
            EXPECT_NE(result.reason.find("407"), std::string::npos) << result.reason;
            EXPECT_NE(result.reason.find("身份验证"), std::string::npos) << "407 的措辞没有指向凭据写法：" << result.reason;
            EXPECT_EQ(reject.acceptedConnectionCount(), 1U);
        }
        {
            ScriptedTcpPeer reject(
                    [](ScriptedPeerConnection &connection)
                    {
                        static_cast<void>(connection.readRequestHead());
                        static_cast<void>(connection.sendText("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"));
                    });
            ASSERT_TRUE(reject.start()) << "代理对端没起来";
            const ScopedEnvironmentVariable httpsProxy{"https_proxy", std::string("http://127.0.0.1:") + std::to_string(reject.port())};
            const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

            const StaticSendResult result = runStaticSend("https://origin.test/hello");
            EXPECT_FALSE(result.succeeded) << "502 之后请求仍然成功了";
            EXPECT_NE(result.reason.find("502"), std::string::npos) << result.reason;
        }
    }

    /**
     * @brief 代理读了 CONNECT 直接收线：拒因说清是代理先关（而不是「握手失败」一类含糊词）
     */
    TEST(HttpProxyE2E, ProxyClosingBeforeTunnelResponseFailsCleanly)
    {
        ScriptedTcpPeer closer(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    // 读完 CONNECT 就收摊：剧本返回后夹具把这条连接关掉
                });
        ASSERT_TRUE(closer.start()) << "代理对端没起来";

        const ScopedEnvironmentVariable httpsProxy{"https_proxy", std::string("http://127.0.0.1:") + std::to_string(closer.port())};
        const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

        const StaticSendResult result = runStaticSend("https://origin.test/hello");
        EXPECT_FALSE(result.succeeded) << "代理早关之后请求仍然成功了";
        EXPECT_NE(result.reason.find("代理"), std::string::npos) << result.reason;
        EXPECT_NE(result.reason.find("关闭"), std::string::npos) << "拒因没有说清是代理先关：" << result.reason;
    }

    /**
     * @brief 坏代理配置（socks5 代理）拒绝请求而不是静默直连：起点一个连接都不该收到
     */
    TEST(HttpProxyE2E, UnsupportedProxySchemeFailsInsteadOfFallingBackToDirect)
    {
        ScriptedTcpPeer origin(
                [](ScriptedPeerConnection &connection)
                {
                    static_cast<void>(connection.readRequestHead());
                    static_cast<void>(connection.sendText("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\ndirect"));
                });
        ASSERT_TRUE(origin.start()) << "起点对端没起来";

        const ScopedEnvironmentVariable httpProxy{"http_proxy", std::string("socks5://127.0.0.1:1")};
        const ScopedEnvironmentVariable noProxy{"no_proxy", std::nullopt};

        const StaticSendResult result = runStaticSend("http://127.0.0.1:" + std::to_string(origin.port()) + "/hello");
        EXPECT_FALSE(result.succeeded) << "坏代理配置被静默绕过、请求直连成功了";
        EXPECT_NE(result.reason.find("socks5"), std::string::npos) << result.reason;
        EXPECT_EQ(origin.acceptedConnectionCount(), 0U) << "坏代理配置下仍然发生了直连";
    }

    /**
     * @brief 钉住豁免表本身的匹配规则：条目形态、点边界后缀、端口参与、畸形条目不牵连整张表
     * @details 期望值取自 HttpProxyPolicy.h 写明的契约（与 curl 的 NO_PROXY 同语义），不是照着实现的
     *          分支凑出来的。既有用例只在端到端那一层走过 `*` 一条，函数本体从没被执行过——而这一层
     *          判错的后果是「本该走代理的流量直连出去」或「该豁免的内网地址被送去代理」，两头都静默。
     */
    TEST(HttpProxyNoProxy, MatchesByEntryFormDotBoundaryAndPort)
    {
        EXPECT_TRUE(matchesNoProxy("anywhere.invalid", 443, "*")) << "* 应当豁免一切目标";

        // 裸域名命中自己与子域；后缀只是「长得像」的不算
        EXPECT_TRUE(matchesNoProxy("example.com", 80, "example.com"));
        EXPECT_TRUE(matchesNoProxy("api.example.com", 80, "example.com"));
        EXPECT_TRUE(matchesNoProxy("deep.api.example.com", 80, ".example.com")) << "前导点等价于裸域名，子域要命中";
        EXPECT_FALSE(matchesNoProxy("notexample.com", 80, "example.com")) << "点边界被当成子串包含：外部的同名后缀会被误豁免";

        // 主机名比较不区分大小写，两个方向都要
        EXPECT_TRUE(matchesNoProxy("API.Example.COM", 80, "api.example.com"));
        EXPECT_TRUE(matchesNoProxy("api.example.com", 80, "API.EXAMPLE.COM"));

        // 带端口的条目：端口参与匹配，不符就不豁免
        EXPECT_TRUE(matchesNoProxy("intranet.local", 8443, "intranet.local:8443"));
        EXPECT_FALSE(matchesNoProxy("intranet.local", 9999, "intranet.local:8443")) << "端口不符却命中，会把该走代理的流量直连出去";

        // 端口文本越界或不是数字：宁可判不命中，也不回绕成一个能凑上的值
        EXPECT_FALSE(matchesNoProxy("intranet.local", 80, "intranet.local:99999")) << "超出 65535 的端口文本被折成了能凑上的值";
        EXPECT_FALSE(matchesNoProxy("intranet.local", 80, "intranet.local:http")) << "非数字的端口文本被静默当成 0";

        // IPv6 条目用方括号，host 参数本身不带括号
        EXPECT_TRUE(matchesNoProxy("::1", 8080, "[::1]:8080"));
        EXPECT_FALSE(matchesNoProxy("::1", 80, "[::1]:8080"));

        // 空条目与两侧空白跳过：表里只要有正常条目命中就算命中
        EXPECT_TRUE(matchesNoProxy("a.local", 80, " , a.local , ")) << "条目两侧的空白或空条目把整张表带坏了";

        // 缺收尾方括号的畸形条目只让那一条不命中，后面的正常条目照常有效
        EXPECT_TRUE(matchesNoProxy("b.local", 80, "[b.local, b.local")) << "一条畸形条目牵连了整张表";
        EXPECT_FALSE(matchesNoProxy("c.local", 80, "[b.local")) << "整张表都是畸形条目时不该豁免任何东西";

        // 空表谁都不豁免
        EXPECT_FALSE(matchesNoProxy("example.com", 80, "")) << "空豁免表被当成了通配";
    }
} // namespace AsynGyanis::Net
