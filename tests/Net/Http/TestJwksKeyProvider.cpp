// JwksKeyProvider 的用例：本地起一个「最小 JWKS 对端」（后台线程、回固定 JSON），主线程跑事件循环。
// 覆盖场景：
// - FetchesKeysAndVerifiesSignedToken（首次拉取 → 缓存可见 → 真令牌验过）
// - UnknownKidRequestsEarlierRefresh（未知 kid 拒 → 请求提前刷新 → 换键集后轮上）
// - MalformedKeyFieldsAreSkippedInsteadOfKillingTheRefreshLoop（坏字段只跳过那条键，轮换照常）
// - UnreachableEndpointReportsFailureAndRefusesBeforeFirstFetch（拉不到：lastError 有因、verify 拒）
// - StartRejectsNonPositiveRefreshRequestSlice（非正分片会让轮换忙等，启动即抛）
// - NumericKidTypesDoNotThrowFromVerify（JWKS 与令牌头的 kid 都是数字：读取口不抛，按无 kid 处理）
// - DestroyingProviderWithSuspendedCoroutineIsSafe（挂起中选择析构——ASan 下无 UAF 即判据）

#include "Net/Http/Auth/JwksKeyProvider.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/EventLoop/EventLoop.h"

#include "ExpectedAssertions.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Core/EventLoop/Timer.h"

#include <openssl/core_names.h>
#include <openssl/bn.h>
#include <openssl/evp.h>

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
        using TimePoint = std::chrono::system_clock::time_point;

        constexpr std::int64_t kNowSeconds = 1791547200; // 2026-10-12 12:00:00 UTC

        /// 现场生成一枚 RSA 密钥对（与 JwtVerifier 用例同一形状：不拿写死的密钥冒充）
        [[nodiscard]] std::shared_ptr<EVP_PKEY> generateRsaKeyPair()
        {
            EVP_PKEY_CTX *keygenContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (keygenContext == nullptr || EVP_PKEY_keygen_init(keygenContext) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }
            int        keyBits        = 2048;
            OSSL_PARAM keygenParams[] = {OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_RSA_BITS, &keyBits), OSSL_PARAM_END};
            if (EVP_PKEY_CTX_set_params(keygenContext, keygenParams) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }
            EVP_PKEY *generatedKey = nullptr;
            if (EVP_PKEY_keygen(keygenContext, &generatedKey) != 1)
            {
                EVP_PKEY_CTX_free(keygenContext);
                return nullptr;
            }
            EVP_PKEY_CTX_free(keygenContext);
            return std::shared_ptr<EVP_PKEY>(generatedKey, EVP_PKEY_free);
        }

        /// 导出密钥的 n/e（base64url）——JWKS 里两个字段的现成材料
        [[nodiscard]] std::pair<std::string, std::string> exportModulusExponent(EVP_PKEY *key)
        {
            BIGNUM *modulus  = nullptr;
            BIGNUM *exponent = nullptr;
            if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &modulus) != 1 || EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &exponent) != 1)
            {
                BN_free(modulus);
                BN_free(exponent);
                return {};
            }
            std::string modulusBytes(static_cast<std::size_t>(BN_num_bytes(modulus)), '\0');
            std::string exponentBytes(static_cast<std::size_t>(BN_num_bytes(exponent)), '\0');
            BN_bn2bin(modulus, reinterpret_cast<unsigned char *>(modulusBytes.data()));
            BN_bn2bin(exponent, reinterpret_cast<unsigned char *>(exponentBytes.data()));
            BN_free(modulus);
            BN_free(exponent);
            return {base64UrlEncode(modulusBytes), base64UrlEncode(exponentBytes)};
        }

        /// 用私钥对给定的头/载荷 JSON 文本签出一枚完整令牌
        [[nodiscard]] std::string makeTokenFromJson(const std::string &headerJson, const std::string &payloadJson, EVP_PKEY *privateKey)
        {
            const std::string signingInput = base64UrlEncode(headerJson) + "." + base64UrlEncode(payloadJson);

            std::string signature;
            {
                EVP_MD_CTX *digestContext = EVP_MD_CTX_new();
                if (digestContext != nullptr && EVP_DigestSignInit(digestContext, nullptr, EVP_sha256(), nullptr, privateKey) == 1 &&
                    EVP_DigestSignUpdate(digestContext, signingInput.data(), signingInput.size()) == 1)
                {
                    std::size_t signatureLength = 0;
                    if (EVP_DigestSignFinal(digestContext, nullptr, &signatureLength) == 1)
                    {
                        signature.resize(signatureLength);
                        if (EVP_DigestSignFinal(digestContext, reinterpret_cast<unsigned char *>(signature.data()), &signatureLength) != 1)
                        {
                            signature.clear();
                        } else
                        {
                            signature.resize(signatureLength);
                        }
                    }
                }
                EVP_MD_CTX_free(digestContext);
            }
            return signingInput + "." + base64UrlEncode(signature);
        }

        /// 拼一枚真令牌（头里带 kid）
        [[nodiscard]] std::string makeToken(const std::string &kid, EVP_PKEY *privateKey, const std::int64_t expiration)
        {
            const std::string headerJson  = R"({"alg":"RS256","typ":"JWT","kid":")" + kid + R"("})";
            const std::string payloadJson = R"({"iss":"https://issuer.example","aud":"https://api.example","exp":)" + std::to_string(expiration) + "}";
            return makeTokenFromJson(headerJson, payloadJson, privateKey);
        }

        /// 一份含单个 RSA 公钥的 JWKS 文档
        [[nodiscard]] std::string jwksDocument(const std::string &kid, const std::pair<std::string, std::string> &modulusExponent)
        {
            return R"({"keys":[{"kty":"RSA","kid":")" + kid + R"(","use":"sig","alg":"RS256","n":")" + modulusExponent.first + R"(","e":")" + modulusExponent.second + R"("}]})";
        }

        /// 同一份文档，但 kid 写成数字（类型不符的读取面）
        [[nodiscard]] std::string jwksDocumentWithNumericKid(const std::pair<std::string, std::string> &modulusExponent)
        {
            return R"({"keys":[{"kty":"RSA","kid":123,"use":"sig","alg":"RS256","n":")" + modulusExponent.first + R"(","e":")" + modulusExponent.second + R"("}]})";
        }

        /**
         * @brief 最小 JWKS 对端：127.0.0.1 随机端口，accept 一条回一条固定 JSON（body 可换），走到推进收口
         * @details 只在后台线程碰套接字；用例在主线程跑事件循环。响应带 Connection: close，客户端读到
         *          EOF 即成一次完整读取
         */
        class MiniJwksServer
        {
        public:
            MiniJwksServer() = default;

            MiniJwksServer(const MiniJwksServer &)            = delete;
            MiniJwksServer &operator=(const MiniJwksServer &) = delete;

            ~MiniJwksServer()
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

            void setBody(std::string body)
            {
                const std::lock_guard guard(m_bodyMutex);
                m_body = std::move(body);
            }

        private:
            [[nodiscard]] std::string body() const
            {
                const std::lock_guard guard(m_bodyMutex);
                return m_body;
            }

            void run()
            {
                // Socket 初始化（Windows 上就是 WSAStartup，顺带忽略 SIGPIPE）：没有它
                // ::socket 一次都建不出来，对端端口永远报不出来
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
                    timeval pollInterval{};
                    pollInterval.tv_usec = 50 * 1000;
                    if (::select(listener + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue;
                    }

                    const int connection = static_cast<int>(::accept(listener, nullptr, nullptr));
                    if (connection < 0)
                    {
                        continue;
                    }

                    // 读掉请求头（到空行为止）：判据是「拿到请求」而不是「读全」，读不完也不阻塞接受
                    std::string requestBytes;
                    char        chunk[1024];
                    while (requestBytes.find("\r\n\r\n") == std::string::npos)
                    {
                        const int received = ::recv(connection, chunk, static_cast<int>(sizeof(chunk)), 0);
                        if (received <= 0)
                        {
                            break;
                        }
                        requestBytes.append(chunk, static_cast<std::size_t>(received));
                    }

                    const std::string currentBody = body();
                    const std::string response    = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(currentBody.size()) +
                                                    "\r\nConnection: close\r\n\r\n" + currentBody;
                    std::size_t       sentTotal   = 0;
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
                    m_requests.fetch_add(1, std::memory_order_acq_rel);
                }
                Platform::FileDescriptor::close(listener);
            }

            std::thread                m_worker;
            std::atomic<std::uint16_t> m_port{0};
            std::atomic<std::uint64_t> m_requests{0};
            std::atomic<bool>          m_isStopping{false};
            mutable std::mutex         m_bodyMutex;
            std::string                m_body;
        };

        /// 在循环上等到条件成立（或超时），随后停掉循环——用例的主控协程
        Core::Task<void> waitUntilAndStop(Core::EventLoop &loop, const std::function<bool()> &condition, bool &satisfied)
        {
            Core::Timer timer(loop);
            const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                {
                    satisfied = true;
                    break;
                }
                co_await timer.waitFor(std::chrono::milliseconds{20});
            }
            loop.stop();
        }

        /// 只在循环上等待指定时长后停掉（析构用例用）
        Core::Task<void> waitThenStop(Core::EventLoop &loop, const std::chrono::milliseconds duration)
        {
            Core::Timer timer(loop);
            co_await timer.waitFor(duration);
            loop.stop();
        }

        /// 起主控协程并跑到它自己停循环：Task 必须活到 run() 之后——临时对象取 handle 会当场析构帧
        void runControlTask(Core::EventLoop &loop, Core::Task<void> control)
        {
            if (!control.isReady())
            {
                loop.scheduler().schedule(control.handle());
            }
            loop.run();
        }

        /// 轮换场景的全部阶段写在一条协程里，一次 run() 跑到底——
        /// 中途 stop 再次 run 会让后台循环的定时等待不再被重唤醒（实测刷新永不发生）
        Core::Task<void> rotationScenario(Core::EventLoop &loop, const std::shared_ptr<JwksKeyProvider> &provider, MiniJwksServer &server,
                                          const std::pair<std::string, std::string> &modulusExponent, const std::string &rotatedToken)
        {
            Core::Timer timer(loop);
            const auto  nowTimePoint = TimePoint{std::chrono::seconds{kNowSeconds}};

            // 阶段 1：等首拉完成
            const auto phaseOneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (provider->cachedKeyCount() == 0U && std::chrono::steady_clock::now() < phaseOneDeadline)
            {
                co_await timer.waitFor(std::chrono::milliseconds{20});
            }
            EXPECT_GT(provider->cachedKeyCount(), 0U) << provider->lastError();

            // 阶段 2：头里是 k2、而当前键集只有 k1：必须拒，且原因点名 kid
            const std::optional<std::string> rejected = provider->verify(rotatedToken, JwtClaimExpectations{}, nowTimePoint);
            EXPECT_HAS_VALUE(rejected) << "未知 kid 的令牌竟被放行";
            if (rejected.has_value())
            {
                EXPECT_NE(rejected->find("kid"), std::string::npos) << *rejected;
            }

            // 阶段 3：签发方补上 k2（同一把公钥换个名字）：提前刷新请求应让它在分片粒度内被拉进来
            server.setBody(jwksDocument("k2", modulusExponent));
            const auto phaseThreeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (provider->verify(rotatedToken, JwtClaimExpectations{}, nowTimePoint).has_value() && std::chrono::steady_clock::now() < phaseThreeDeadline)
            {
                co_await timer.waitFor(std::chrono::milliseconds{20});
            }
            EXPECT_FALSE(provider->verify(rotatedToken, JwtClaimExpectations{}, nowTimePoint).has_value())
                    << "未知 kid 的提前刷新没有在期限内跟上，lastError=" << provider->lastError();
            loop.stop();
        }

        /// 坏字段场景：文档里同时有「n 被写成数字」的坏键与完好的键；阶段 2 再验证轮换仍然工作
        Core::Task<void> malformedKeyScenario(Core::EventLoop &loop, const std::shared_ptr<JwksKeyProvider> &provider, MiniJwksServer &server,
                                              const std::pair<std::string, std::string> &modulusExponent, const std::string &rotatedToken)
        {
            Core::Timer timer(loop);
            const auto  nowTimePoint = TimePoint{std::chrono::seconds{kNowSeconds}};

            // 阶段 1：坏键跳过、好键入缓存。旧行为是读坏键的 n 时抛类型异常——后台拉取协程当场死掉
            // （分离投递的任务只留一行日志），这里就永远等不到 0 以外的键数
            const auto phaseOneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (provider->cachedKeyCount() == 0U && std::chrono::steady_clock::now() < phaseOneDeadline)
            {
                co_await timer.waitFor(std::chrono::milliseconds{20});
            }
            EXPECT_EQ(provider->cachedKeyCount(), 1U) << "一份含坏字段的文档里，完好的键应照常入缓存；lastError=" << provider->lastError();

            // 阶段 2：轮换仍要工作——坏文档没有把后台循环打死（未知 kid 触发提前刷新，换键集后验过）
            server.setBody(jwksDocument("k2", modulusExponent));
            const auto phaseTwoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (provider->verify(rotatedToken, JwtClaimExpectations{}, nowTimePoint).has_value() && std::chrono::steady_clock::now() < phaseTwoDeadline)
            {
                co_await timer.waitFor(std::chrono::milliseconds{20});
            }
            EXPECT_FALSE(provider->verify(rotatedToken, JwtClaimExpectations{}, nowTimePoint).has_value())
                    << "未知 kid 的提前刷新没有在期限内跟上——拉取循环可能已被坏字段打死，lastError=" << provider->lastError();
            loop.stop();
        }
    } // namespace

    TEST(JwksKeyProviderTest, FetchesKeysAndVerifiesSignedToken)
    {
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);
        const auto modulusExponent = exportModulusExponent(signingKey.get());

        MiniJwksServer server;
        server.setBody(jwksDocument("k1", modulusExponent));
        ASSERT_TRUE(server.start());

        Core::EventLoop loop;
        auto            provider = JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl             = "http://127.0.0.1:" + std::to_string(server.port()) + "/jwks",
                                                                                       .refreshInterval     = std::chrono::seconds{60},
                                                                                       .refreshRequestSlice = std::chrono::milliseconds{50}});

        bool cacheReady = false;
        runControlTask(loop, waitUntilAndStop(loop, [&provider] { return provider->cachedKeyCount() > 0; }, cacheReady));

        ASSERT_TRUE(cacheReady) << "首次拉取没有在期限内完成，lastError=" << provider->lastError();
        EXPECT_EQ(provider->cachedKeyCount(), 1U);

        const std::string    token = makeToken("k1", signingKey.get(), kNowSeconds + 3600);
        JwtClaimExpectations expectations;
        expectations.issuer                      = "https://issuer.example";
        expectations.audience                    = "https://api.example";
        const std::optional<std::string> outcome = provider->verify(token, expectations, TimePoint{std::chrono::seconds{kNowSeconds}});
        EXPECT_FALSE(outcome.has_value()) << *outcome;
    }

    TEST(JwksKeyProviderTest, UnknownKidRequestsEarlierRefresh)
    {
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);
        const auto modulusExponent = exportModulusExponent(signingKey.get());

        MiniJwksServer server;
        server.setBody(jwksDocument("k1", modulusExponent));
        ASSERT_TRUE(server.start());

        Core::EventLoop loop;
        auto            provider = JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl             = "http://127.0.0.1:" + std::to_string(server.port()) + "/jwks",
                                                                                       .refreshInterval     = std::chrono::seconds{60},
                                                                                       .refreshRequestSlice = std::chrono::milliseconds{50}});

        const std::string rotatedToken = makeToken("k2", signingKey.get(), kNowSeconds + 3600);
        runControlTask(loop, rotationScenario(loop, provider, server, modulusExponent, rotatedToken));
    }

    TEST(JwksKeyProviderTest, MalformedKeyFieldsAreSkippedInsteadOfKillingTheRefreshLoop)
    {
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);
        const auto modulusExponent = exportModulusExponent(signingKey.get());

        MiniJwksServer server;
        // 两条键：第一条的 n 是数字（类型不符），第二条完好。文档整体仍是合法 JSON——
        // 单条坏记录不该把整次轮换判失败，更不该让读字段的异常把拉取循环带走
        server.setBody(R"({"keys":[{"kty":"RSA","kid":"broken","n":42,"e":"AQAB"},{"kty":"RSA","kid":"k1","use":"sig","alg":"RS256","n":")" + modulusExponent.first + R"(","e":")" +
                       modulusExponent.second + R"("}]})");
        ASSERT_TRUE(server.start());

        Core::EventLoop loop;
        auto            provider = JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl             = "http://127.0.0.1:" + std::to_string(server.port()) + "/jwks",
                                                                                       .refreshInterval     = std::chrono::seconds{60},
                                                                                       .refreshRequestSlice = std::chrono::milliseconds{50}});

        const std::string rotatedToken = makeToken("k2", signingKey.get(), kNowSeconds + 3600);
        runControlTask(loop, malformedKeyScenario(loop, provider, server, modulusExponent, rotatedToken));
    }

    TEST(JwksKeyProviderTest, UnreachableEndpointReportsFailureAndRefusesBeforeFirstFetch)
    {
        Core::EventLoop loop;
        // 127.0.0.1:1 上不会有人监听：连接被拒是稳定形状，不需要对端配合
        auto provider = JwksKeyProvider::start(
                loop,
                JwksKeyProviderOptions{.jwksUrl = "http://127.0.0.1:1/jwks", .refreshInterval = std::chrono::seconds{60}, .refreshRequestSlice = std::chrono::milliseconds{50}});

        bool failureReported = false;
        runControlTask(loop, waitUntilAndStop(loop, [&provider] { return !provider->lastError().empty(); }, failureReported));

        ASSERT_TRUE(failureReported) << "拉取失败没有留下原因";
        EXPECT_EQ(provider->cachedKeyCount(), 0U);

        // 用一枚形状完整的真令牌：假令牌会在「形状校验」层先被拒，走不到本用例要钉的那条判据
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);
        const std::string                token   = makeToken("k1", signingKey.get(), kNowSeconds + 3600);
        const std::optional<std::string> outcome = provider->verify(token, JwtClaimExpectations{}, TimePoint{std::chrono::seconds{kNowSeconds}});
        ASSERT_HAS_VALUE(outcome);
        EXPECT_NE(outcome->find("尚未成功拉取"), std::string::npos) << *outcome;
    }

    TEST(JwksKeyProviderTest, StartRejectsNonPositiveRefreshRequestSlice)
    {
        // 只起循环、不起对端：非正分片在 start 的参数校验里就该被拦下——放进去会让
        // refreshLoop 的分片等待立即返回、waited 不前进，把轮换变成忙等
        Core::EventLoop loop;
        const auto      startWithZeroSlice = [&loop]
        { return JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl = "http://127.0.0.1:1/jwks", .refreshRequestSlice = std::chrono::milliseconds{0}}); };
        EXPECT_THROW(static_cast<void>(startWithZeroSlice()), Base::InvalidArgumentException);
    }

    TEST(JwksKeyProviderTest, NumericKidTypesDoNotThrowFromVerify)
    {
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);
        const auto modulusExponent = exportModulusExponent(signingKey.get());

        MiniJwksServer server;
        // 两边 kid 都写成数字：读取口不抛异常是判据。旧实现里 header.value("kid", ...) 在类型不符时
        // 会抛 type_error，穿出以 optional 为契约的 verify 口
        server.setBody(jwksDocumentWithNumericKid(modulusExponent));
        ASSERT_TRUE(server.start());

        Core::EventLoop loop;
        auto            provider = JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl             = "http://127.0.0.1:" + std::to_string(server.port()) + "/jwks",
                                                                                       .refreshInterval     = std::chrono::seconds{60},
                                                                                       .refreshRequestSlice = std::chrono::milliseconds{50}});

        bool cacheReady = false;
        runControlTask(loop, waitUntilAndStop(loop, [&provider] { return provider->cachedKeyCount() > 0; }, cacheReady));
        ASSERT_TRUE(cacheReady) << "首次拉取没有在期限内完成，lastError=" << provider->lastError();

        const std::string token =
                makeTokenFromJson(R"({"alg":"RS256","typ":"JWT","kid":123})",
                                  R"({"iss":"https://issuer.example","aud":"https://api.example","exp":)" + std::to_string(kNowSeconds + 3600) + "}", signingKey.get());

        // 数字 kid 按「没带 kid」处理：遍历键集，签名对得上即通过——关键是这枚令牌不得让 verify 抛异常
        std::optional<std::string> outcome;
        EXPECT_NO_THROW(outcome = provider->verify(token, JwtClaimExpectations{}, TimePoint{std::chrono::seconds{kNowSeconds}}));
        EXPECT_FALSE(outcome.has_value()) << *outcome;
    }

    TEST(JwksKeyProviderTest, DestroyingProviderWithSuspendedCoroutineIsSafe)
    {
        const std::shared_ptr<EVP_PKEY> signingKey = generateRsaKeyPair();
        ASSERT_NE(signingKey, nullptr);

        MiniJwksServer server;
        server.setBody(jwksDocument("k1", exportModulusExponent(signingKey.get())));
        ASSERT_TRUE(server.start());

        Core::EventLoop loop;
        {
            auto provider = JwksKeyProvider::start(loop, JwksKeyProviderOptions{.jwksUrl             = "http://127.0.0.1:" + std::to_string(server.port()) + "/jwks",
                                                                                .refreshInterval     = std::chrono::seconds{60},
                                                                                .refreshRequestSlice = std::chrono::milliseconds{50}});

            // 先跑一小段让首投被消费（帧落到定时/IO 等待上），再析构。直接在首投还排在队列里时
            // 析构是禁止用法：投出去的句柄搞不回来，帧先没了而队列里还留着它——下一次 run() 取到
            // 就是悬垂句柄（实测 ASan 当场报 UAF；供给器的头文件 @warning 写明这条）
            bool firstTickConsumed = false;
            runControlTask(loop, waitUntilAndStop(loop, [&provider] { return provider->cachedKeyCount() > 0; }, firstTickConsumed));
            ASSERT_TRUE(firstTickConsumed) << provider->lastError();
            // 此刻帧挂起在周期等待上：析构应安全（挂起中的帧可销毁、等待器随帧撤销）
        }

        // 循环上再跑一小会儿：若析构留下了悬垂的等待器/回调，这里会以 UAF 或崩溃现形
        runControlTask(loop, waitThenStop(loop, std::chrono::milliseconds{150}));
        SUCCEED();
    }

} // namespace AsynGyanis::Net
