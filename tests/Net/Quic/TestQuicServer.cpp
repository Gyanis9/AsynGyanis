// QUIC 服务端的构造期与配置校验用例：这两类判据不需要对端，因此留在树内。
//
// 对外行为（握手、ALPN、流数据与流收尾、drain 时的 CONNECTION_CLOSE、半开连接的回收、
// 跨实例会话恢复、单来源限额）不再由链接进来的第三方 QUIC 实现裁定：那类「同一棵树里互测」
// 暴露不出双方共享的误解，还会跟着被测实现一起被改松。它们整体搬到进程外的独立实现上：
//     scripts/quic_cross_check.sh <构建目录> <带 aioquic 的 python>
// 被测端是 tests/Tools/QuicProbeServer.cpp，判据由 aioquic 的解析器给出。
// 改 QuicServer 的对外行为时，跑那条脚本才算过门禁；在本文件里加「自家客户端连自家服务端」
// 的用例等于把刚拆掉的裁判请回来。
#include "Net/Quic/QuicServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/TimerQueue.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Routing/Router.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include "CoreTestSupport.h"
#include "AssertionSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Core::TestSupport::captureCoroutineFailure;
        using AsynGyanis::Core::TestSupport::EventLoopThread;
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /// 夹具等待上限
        constexpr std::chrono::milliseconds kWaitTimeout{8000};

        /**
         * @brief 断言 QUIC 服务端构造/配置按指定类型拒绝，并交回原因
         * @tparam ExceptionType 期望的异常类型
         * @param action 构造或配置动作
         * @param expected 原因里必须出现的片段（哪一份文件、哪一项配置或哪一条前置）
         */
        template<typename ExceptionType, typename Action>
        void expectQuicRejected(Action &&action, const std::string_view expected)
        {
            const std::string reason = AsynGyanis::TestSupport::requireFailureText<ExceptionType>(action, "QUIC 服务端");
            EXPECT_NE(reason.find(expected), std::string::npos) << "原因没有点明拒绝的是哪一项：" << reason;
        }

        /// 对端等一条报文/一个结果的有界上限：回环上本该在毫秒级完成，超了就是没回来
        constexpr std::chrono::milliseconds kPeerWaitTimeout{5000};

        /// 服务器证书与私钥（与 TLS 用例共用同一份夹具）
        std::string certificatePath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_cert.pem";
        }

        std::string privateKeyPath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_key.pem";
        }

        /**
         * @brief 造一份只填了证书与私钥的配置：接手那几条用例不关心其余档位
         * @return QuicServer::Configuration 可直接交给构造的配置
         */
        QuicServer::Configuration makeServerConfiguration()
        {
            QuicServer::Configuration configuration;
            configuration.certificateFile = certificatePath();
            configuration.privateKeyFile  = privateKeyPath();
            return configuration;
        }

        /**
         * @brief 手拼一条 v1 Initial 数据报，并补齐到指定总长
         * @details 载荷是纯垃圾：这条通道只判「头部读得懂」与「UDP 负载有多大」，后续解密注定失败。
         *          包头按 §17.2.2 取：固定位与 Initial 类型、版本 1、两条 8 字节连接标识、零长 Token、
         *          Length 域（按取值自动选一或两字节档）与一字节包号。
         * @param datagramByteLength 目标数据报总字节数
         * @return std::vector<std::uint8_t> 一条可直接交给 UDP 的 Initial
         */
        std::vector<std::uint8_t> makeProbeInitial(const std::size_t datagramByteLength)
        {
            constexpr std::size_t     kProbeConnectionIdByteCount = 8;
            std::vector<std::uint8_t> packet{0xC0U, 0x00U, 0x00U, 0x00U, 0x01U, static_cast<std::uint8_t>(kProbeConnectionIdByteCount)};
            for (std::size_t index = 0; index < kProbeConnectionIdByteCount; ++index)
            {
                packet.push_back(static_cast<std::uint8_t>(0xD0U + index));
            }
            packet.push_back(static_cast<std::uint8_t>(kProbeConnectionIdByteCount));
            for (std::size_t index = 0; index < kProbeConnectionIdByteCount; ++index)
            {
                packet.push_back(static_cast<std::uint8_t>(0x50U + index));
            }
            packet.push_back(0x00U); // Token 长度

            // Length 覆盖包号与载荷；先按「没有 Length 域」估一次档位，再扣掉它自己的字节数
            const std::size_t prefixByteCount      = packet.size();
            std::size_t       lengthFieldByteCount = datagramByteLength - prefixByteCount < 64U ? 1U : 2U;
            const std::size_t lengthValue          = datagramByteLength - prefixByteCount - lengthFieldByteCount;
            if (lengthFieldByteCount == 1U)
            {
                packet.push_back(static_cast<std::uint8_t>(lengthValue));
            } else
            {
                packet.push_back(static_cast<std::uint8_t>(0x40U | ((lengthValue >> 8U) & 0x3FU)));
                packet.push_back(static_cast<std::uint8_t>(lengthValue & 0xFFU));
            }
            packet.resize(datagramByteLength, 0x00U);
            return packet;
        }

        /**
         * @brief 起一台回环上的 QUIC 服务端，把循环放到自己的线程上跑
         * @details 只为本文件的两条构造期判据服务：构造要落到一个真实的事件循环上，
         *          而端口与连接数这类成员只由循环线程读写，读它们一律走投递（跨线程直读
         *          就是与循环抢同一个成员，TSan 报过这一类）。
         */
        class RunningQuicServer
        {
        public:
            /**
             * @brief 起服务端
             * @param idleTimeout 空闲/握手超时
             * @param perIpConnectionLimiter 单来源并发上限的限额器；空表示不按来源限制
             * @param sessionTicketKeyFiles 会话票据密钥文件列表；空表示按 OpenSSL 默认
             * @param adoptedBoundSocket 接手模式：给一份别人已经绑好的数据报套接字，本夹具改用
             *        不带地址的 listen()；给空则按老写法自己绑一个端口
             */
            explicit RunningQuicServer(const std::chrono::seconds idleTimeout = std::chrono::seconds{30}, std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter = nullptr,
                                       std::vector<std::string> sessionTicketKeyFiles = {}, std::optional<Platform::DatagramSocket> adoptedBoundSocket = std::nullopt)
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile        = certificatePath();
                configuration.privateKeyFile         = privateKeyPath();
                configuration.idleTimeout            = idleTimeout;
                configuration.perIpConnectionLimiter = std::move(perIpConnectionLimiter);
                configuration.sessionTicketKeyFiles  = std::move(sessionTicketKeyFiles);

                if (adoptedBoundSocket.has_value())
                {
                    m_server = std::make_unique<QuicServer>(m_loop, configuration, std::move(*adoptedBoundSocket));
                    m_listenTask.emplace(m_server->listen());
                } else
                {
                    m_server = std::make_unique<QuicServer>(m_loop, configuration);
                    m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                }
                // 在起线程之前把自己排进所属循环的就绪队列：调度器只在循环线程上跑，
                // 这里还在创建者线程上，于是这一次排入是「归属线程内」的合法调用
                m_loop.scheduler().schedule(m_listenTask->handle());

                m_loopThread = std::thread([this] { m_loop.run(); });

                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (refreshListeningPort() == 0 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }

            ~RunningQuicServer()
            {
                m_server->stop();
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            RunningQuicServer(const RunningQuicServer &) = delete;

            RunningQuicServer &operator=(const RunningQuicServer &) = delete;

            /// 循环线程发布出来的本端端口（构造期已等到非 0；这里读的是那一份快照）
            [[nodiscard]] std::uint16_t listeningPort() const noexcept
            {
                return m_listeningPort.load(std::memory_order_acquire);
            }

        private:
            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             * @param action 待执行的动作
             */
            void runOnLoopAndWait(const std::function<void()> &action)
            {
                std::atomic<bool> isFinished{false};
                m_loop.scheduler().postRemote(
                        [&action, &isFinished]
                        {
                            action();
                            isFinished.store(true, std::memory_order_release);
                        });

                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (!isFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                EXPECT_TRUE(isFinished.load(std::memory_order_acquire)) << "投递到循环线程的动作没有在时限内完成";
            }

            /// 在循环线程上取一次本端端口，更新快照后返回
            std::uint16_t refreshListeningPort()
            {
                std::uint16_t port = 0;
                runOnLoopAndWait([this, &port] { port = m_server->listeningPort(); });
                m_listeningPort.store(port, std::memory_order_release);
                return port;
            }

            Core::EventLoop             m_loop;             ///< 服务端所属事件循环
            std::unique_ptr<QuicServer> m_server;           ///< 被测服务端
            std::optional<Core::Task<>> m_listenTask;       ///< 监听协程（Task 没有默认构造，用 optional 托管）
            std::thread                 m_loopThread;       ///< 跑循环的线程
            std::atomic<std::uint16_t>  m_listeningPort{0}; ///< 循环线程写下的本端端口快照
        };
    } // namespace

    /**
     * @brief 钉住运行时轮换入口的两条用法闸：保护没开、或新密钥短于下限
     * @details 轮换是「密钥泄露了就换一把」的唯一入口；静默成功会让运维以为换了而实际没换，
     *          短密钥则把地址验证的认证码降成可穷举的量（RFC 9000 §8.1.4）。
     */
    TEST(QuicServer, RotateAddressValidationKeyRejectsDisabledAndShortKeys)
    {
        Core::EventLoop loop;

        QuicServer::Configuration disabled      = makeServerConfiguration();
        disabled.addressValidationTokenLifetime = std::chrono::seconds{0};
        QuicServer                      disabledServer(loop, disabled);
        const std::vector<std::uint8_t> key(32U, 0xA7U);
        try
        {
            disabledServer.rotateAddressValidationKey(std::span<const std::uint8_t>(key));
            FAIL() << "没开地址验证的服务端接受了轮换";
        } catch (const Base::InvalidArgumentException &failure)
        {
            EXPECT_NE(std::string{failure.what()}.find("没开地址验证"), std::string::npos) << failure.what();
        }

        QuicServer::Configuration enabled      = makeServerConfiguration();
        enabled.addressValidationTokenLifetime = std::chrono::seconds{30};
        enabled.addressValidationKey           = key;
        QuicServer enabledServer(loop, enabled);

        const std::vector<std::uint8_t> shortKey(15U, 0x3BU);
        try
        {
            enabledServer.rotateAddressValidationKey(std::span<const std::uint8_t>(shortKey));
            FAIL() << "低于下限的新密钥被接受了";
        } catch (const Base::InvalidArgumentException &failure)
        {
            EXPECT_NE(std::string{failure.what()}.find("16"), std::string::npos) << "原因没有点明下限：" << failure.what();
        }
        EXPECT_NO_THROW(enabledServer.rotateAddressValidationKey(std::span<const std::uint8_t>(key)));
    }

    /**
     * @brief 票据密钥不合格时构造当场失败，而不是悄悄退回「每个上下文一份随机密钥」
     * @details 退回随机密钥的表现是恢复命中率归零而服务一切正常——既没有错误日志也没有失败请求，
     *          是这类配置最贵的失败形态。异常文本还要点名是哪一份文件：本条失败没有 OpenSSL
     *          错误栈可查
     */
    TEST(QuicServer, RejectsInvalidTicketKeyFilesDuringConstruction)
    {
        AsynGyanis::TestSupport::TemporaryDirectory directory("QuicTicketKeyInvalid");
        const std::string                           wrongLengthPath = (directory.path() / "wrong-length.key").string();
        EXPECT_TRUE(directory.writeBinaryFile("wrong-length.key", AsynGyanis::TestSupport::makeBytePattern(31, 64)));

        const std::vector<std::string> keyFiles{wrongLengthPath};
        try
        {
            static_cast<void>(std::make_unique<RunningQuicServer>(std::chrono::seconds{30}, nullptr, keyFiles));
            FAIL() << "长度为 64 字节的密钥文件（既不是 48 也不是 80）应当让构造失败";
        } catch (const Base::Exception &failure)
        {
            const std::string message = failure.what();
            EXPECT_TRUE(message.find("wrong-length.key") != std::string::npos) << "异常没点名是哪一份文件：" << message;
        }

        // 文件根本不存在同样要抛，且不能留下半构造的对象：上面那台已经抛在构造期，
        // 这里的断言只判「会不会抛」，端口与握手都不参与
        expectQuicRejected<Base::Exception>(
                [&]
                {
                    static_cast<void>(
                            std::make_unique<RunningQuicServer>(std::chrono::seconds{30}, nullptr, std::vector<std::string>{(directory.path() / "missing.key").string()}));
                },
                "missing.key");
    }

    /**
     * @brief 构造期证书或私钥不合规时当场抛出，且不留下一份无人认领的 TLS 上下文
     * @details SSL_CTX 是构造里第一件拿到的资源，其后四道检查任一不过都抛——那时析构函数不会跑，
     *          旧写法存在成员里的裸指针就此没人负责（实测一份 1784 字节直漏 + 连带 35 KiB 的间接量，
     *          每次构造失败漏一整份）。判据分两层：抛与不抛由这里钉，漏与不漏由容器的 LSan 门禁钉，
     *          两层缺一都挡不住「改成不抛而是吞掉」这种倒退。
     */
    TEST(QuicServer, ReleasesTlsContextWhenCertificateValidationFailsDuringConstruction)
    {
        // 构造与销毁都在本线程：这个循环没交给别的线程，本线程就是它的归属线程
        Core::EventLoop loop;

        QuicServer::Configuration missingCertificate;
        missingCertificate.certificateFile = std::string(TEST_FIXTURES_DIR) + "/no-such-cert.pem";
        missingCertificate.privateKeyFile  = privateKeyPath();
        expectQuicRejected<Base::SystemException>([&] { QuicServer server(loop, missingCertificate); }, "no-such-cert.pem");

        QuicServer::Configuration missingPrivateKey;
        missingPrivateKey.certificateFile = certificatePath();
        missingPrivateKey.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/no-such-key.pem";
        expectQuicRejected<Base::SystemException>([&] { QuicServer server(loop, missingPrivateKey); }, "no-such-key.pem");

        // 两个文件各自都在、也能各自加载，只有配对检查拦得住：这一道是构造里最后一道抛点
        QuicServer::Configuration mismatchedPair;
        mismatchedPair.certificateFile = certificatePath();
        mismatchedPair.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/test_ip_key.pem";
        expectQuicRejected<Base::SystemException>([&] { QuicServer server(loop, mismatchedPair); }, "私钥加载失败");

        // 反复构造失败也要抛，且不留残留：以上三道各来一轮，退出时 LSan 不许报任何东西
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            expectQuicRejected<Base::SystemException>([&] { QuicServer server(loop, missingCertificate); }, "证书加载失败");
        }
    }

    /**
     * @brief 钉住：applicationProtocol 配成 h3 之外的一切都在构造期被拒
     * @details 这一项过去只有「打进启动日志」一个消费方——ALPN 回调始终只认 h3。于是写 `h3-29` 的人
     *          会在日志里看到「ALPN h3-29」，而协商按 h3 发生：日志与现场各说一套，排查时第一条就被引到
     *          错的方向。既然改不动行为，就该在构造期说清楚；顺带保住「日志里那个名字就是协商用的那个」
     */
    TEST(QuicServer, RejectsApplicationProtocolOtherThanH3DuringConstruction)
    {
        Core::EventLoop loop;

        QuicServer::Configuration outdatedDraft = makeServerConfiguration();
        outdatedDraft.applicationProtocol       = "h3-29";
        expectQuicRejected<Base::InvalidArgumentException>([&] { QuicServer server(loop, outdatedDraft); }, "只服务 h3");

        // 反向那一格必须放行：判据不能顺手把唯一正确的写法也拒掉
        const QuicServer::Configuration current = makeServerConfiguration();
        EXPECT_NO_THROW(QuicServer server(loop, current));
    }

    /**
     * @brief 钉住：h3 也能被问出「这台卡在哪」——并发上限与按来源的限额对象都有读口
     * @details `connectionCount()` 一直能读，但没有分母就读不出「是不是贴着上限跑」；而 TCP 侧的
     *          `TcpServer::maximumConnections()` 与限额读口早就有，同一台机器上三条通道对同一个
     *          运维问句给出「两问答得出、一问答不出」就是漂移。限额那条还钉「交回同一份」：
     *          多台共用一份账时，读到副本等于读到一个不会动的对象。
     */
    TEST(QuicServer, ReportsTheAdmissionCapsItWasConfiguredWith)
    {
        Core::EventLoop loop;

        const std::shared_ptr<PerIpConnectionLimiter> limiter = std::make_shared<PerIpConnectionLimiter>(3);

        QuicServer::Configuration configuration = makeServerConfiguration();
        configuration.maximumConnections        = 77;
        configuration.perIpConnectionLimiter    = limiter;
        const QuicServer server(loop, configuration);

        EXPECT_EQ(server.maximumConnections(), 77U) << "并发上限读不回来，运维只能翻配置文件猜这台卡在哪";
        EXPECT_EQ(server.perIpConnectionLimiter(), limiter) << "限额要交回共用那一份，而不是副本或空";
        // 分母要进快照：`/metrics` 上只有分子时，「离上限还有多远」这类预警画不出来
        const HttpServerStats capsSnapshot = server.stats();
        EXPECT_EQ(capsSnapshot.maximumConnections, 77U) << "stats() 没带上本监听器的并发上限";
        EXPECT_EQ(capsSnapshot.maximumConnectionsPerIp, 3U) << "stats() 没带上单来源上限：抓到拒绝数也判断不出闸门装着没有";

        // 0 是显式写法「不设这道限」，读数必须原样说出 0 而不是替调用方发明一个默认值
        QuicServer::Configuration unlimited = makeServerConfiguration();
        unlimited.maximumConnections        = 0;
        const QuicServer unlimitedServer(loop, unlimited);
        EXPECT_EQ(unlimitedServer.maximumConnections(), 0U) << "0 表示不限，读口不该把它翻成某个具体数";
        EXPECT_EQ(unlimitedServer.perIpConnectionLimiter(), nullptr);
    }

    /**
     * @brief 钉住：TLS 策略真的被施加到 QUIC 这份上下文上，而不是收下就算
     * @details 三条判据挑的是「OpenSSL 会拒绝的写法」：如果本服务端只是把策略存起来却没施加，
     *          这三份配置都会构造成功，用例即红——这是「配置被静默忽略」唯一能在树内看到的形状。
     *          版本那一条不同：把最高版本压到 1.2 是 QUIC 满足不了的要求，按「拒绝而不是悄悄改回」
     *          处理，消息里要能看出是哪一项。证书夹具与 TLS 用例共用，不依赖外部服务。
     */
    TEST(QuicServer, RejectsTlsPolicyThatOpenSslOrQuicCannotHonorDuringConstruction)
    {
        Core::EventLoop loop;

        QuicServer::Configuration cappedBelowTls13;
        cappedBelowTls13.certificateFile                  = certificatePath();
        cappedBelowTls13.privateKeyFile                   = privateKeyPath();
        cappedBelowTls13.tlsPolicy.maximumProtocolVersion = Core::TlsPolicy::ProtocolVersion::Tls1_2;
        try
        {
            QuicServer server(loop, cappedBelowTls13);
            FAIL() << "QUIC 只跑 TLS 1.3，把最高版本限定在 1.2 的策略应当当场被拒";
        } catch (const Base::Exception &failure)
        {
            const std::string message = failure.what();
            EXPECT_TRUE(message.find("最高版本") != std::string::npos) << "异常没点出是版本这一项：" << message;
        }

        // 三份各自不合规的写法：套件串、曲线名、1.2 套件列表。哪一份都能被 OpenSSL 拒绝，
        // 也各自对应策略里一个字段，所以三条都红或都绿才说明这一整块被接到了上下文上
        const std::vector<std::pair<const char *, std::function<void(Core::TlsPolicy &)>>> refused{
                {"1.3 套件串", [](Core::TlsPolicy &policy) { policy.tls13CipherSuites = "TLS_NOT_A_REAL_SUITE"; }},
                {"命名曲线", [](Core::TlsPolicy &policy) { policy.supportedGroups = "not-a-curve"; }},
                {"1.2 套件串", [](Core::TlsPolicy &policy) { policy.cipherList = "!"; }},
        };
        for (const auto &[label, mutate]: refused)
        {
            SCOPED_TRACE(std::string(label));
            QuicServer::Configuration configuration;
            configuration.certificateFile = certificatePath();
            configuration.privateKeyFile  = privateKeyPath();
            mutate(configuration.tlsPolicy);
            expectQuicRejected<Base::Exception>([&] { QuicServer server(loop, configuration); }, "策略");
        }
    }

    /**
     * @brief 钉住：一份写给 HTTPS 那两侧的完整策略交给 QUIC 也能起服务，不适用的项不当场报错
     * @details 同一份 `TlsPolicy` 多半要喂给三个监听器：CA 文件、校验深度、1.2 套件串这几项在
     *          「QUIC 不要求客户端证书」这条通路上用不上，但它们是合法配置，拒掉就等于逼使用方
     *          为每个监听器各写一份策略。这一条与上一条配对，把边界钉在「合用但本通路用不上 → 收下」
     *          与「本通路满足不了 → 拒绝」两侧。
     */
    TEST(QuicServer, AcceptsAPolicyWrittenForTheOtherTlsListeners)
    {
        Core::EventLoop loop;

        QuicServer::Configuration configuration;
        configuration.certificateFile                    = certificatePath();
        configuration.privateKeyFile                     = privateKeyPath();
        configuration.tlsPolicy.cipherList               = "HIGH:!aNULL:!MD5";
        configuration.tlsPolicy.minimumProtocolVersion   = Core::TlsPolicy::ProtocolVersion::Tls1_2;
        configuration.tlsPolicy.certificateAuthorityFile = certificatePath();
        configuration.tlsPolicy.verifyDepth              = 4;
        configuration.tlsPolicy.supportedGroups          = "X25519:secp384r1";
        configuration.tlsPolicy.securityLevel            = 2;

        EXPECT_NO_THROW(QuicServer server(loop, configuration)) << "这几项在 QUIC 侧用不上，但都是合法配置，不该挡住启动";
    }

    namespace
    {
        /**
         * @brief 只把定时驱动的纯换算转发出来的探针
         * @details 不构造对象，因此不需要证书、UDP 端口与事件循环——要测的是「什么时候该醒」这条
         *          纯换算，把它从读时钟的循环里剥出来才能确定性地判。
         */
        class TickerWakePointProbe : public QuicServer
        {
        public:
            using QuicServer::kIdleTickerSleep;
            using QuicServer::nextTickerWakePoint;
        };

        /**
         * @brief 只把连接准入换算转发出来的探针（不起对象，理由同 TickerWakePointProbe）
         */
        class ConnectionAdmissionProbe : public QuicServer
        {
        public:
            using QuicServer::admitsNewConnection;
        };
    } // namespace

    /**
     * @brief 钉住：在线连接上限里的 0 是「不限」，到数才关；两条口径必须与 HTTP 侧一致
     * @details 把 0 当成「一个也不收」时症状极难自查：监听在跑、日志还会说「已达上限 0」，
     *          而配置者以为自己设的是「不设限」。`HttpServerConfig::maximumConnections` 一直是
     *          0 = 不限，同一份配置在两条服务路径上必须同一把尺。
     */
    TEST(QuicServer, TreatsZeroConnectionLimitAsUnlimitedAndClosesAtTheCap)
    {
        EXPECT_TRUE(ConnectionAdmissionProbe::admitsNewConnection(0, 0));
        EXPECT_TRUE(ConnectionAdmissionProbe::admitsNewConnection(10'000, 0)) << "0 被当成「一个也不收」时，一台监听谁都连不上";
        EXPECT_TRUE(ConnectionAdmissionProbe::admitsNewConnection(9, 10));
        EXPECT_FALSE(ConnectionAdmissionProbe::admitsNewConnection(10, 10)) << "到达上限这一条就该关，不是超过才关";
        EXPECT_FALSE(ConnectionAdmissionProbe::admitsNewConnection(11, 10));
    }

    /**
     * @brief 钉住：定时驱动下一次醒来的时刻——最早截止优先、节拍封顶、零连接退到空闲上界
     * @details 这条换算是「不再一律按固定节拍轮询」改动的全部判据，因此按纯函数直测而不是等时钟：
     *          读时钟的用例在共享机器上必然飘。零连接那一档不是洁癖——实测一台空闲监听器按 10 毫秒
     *          白轮每秒 100 次，占了整场进程外画像里四万次 epoll_wait 的绝大部分。
     *          节拍上限留着是有原因的：needsFlush 那一档补刀与 h3 请求的读时限都没有可查的截止时刻，
     *          只能靠轮询兜，所以「有连接但截止很远」必须仍按节拍到。
     */
    TEST(QuicServer, PicksTheEarliestDeadlineButCapsTheTickerAndIdlesWhenEmpty)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now{std::chrono::milliseconds{1'000'000}};
        constexpr auto          tick = std::chrono::milliseconds{10};

        // 零连接：不管传进来的截止是什么，都按空闲上界睡
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(false, Clock::time_point::max(), now, tick), now + TickerWakePointProbe::kIdleTickerSleep);
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(false, now + std::chrono::milliseconds{1}, now, tick), now + TickerWakePointProbe::kIdleTickerSleep)
                << "没有连接时，任何截止时刻都不该把节拍叫醒";

        // 有连接：早于节拍的截止按时到
        const Clock::time_point soonDeadline = now + std::chrono::milliseconds{3};
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, soonDeadline, now, tick), soonDeadline);
        // 晚于节拍的按节拍到（补刀那一档靠轮询兜）
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, now + std::chrono::seconds{5}, now, tick), now + tick);
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, Clock::time_point::max(), now, tick), now + tick) << "查不到截止时不能睡过头";
        // 已经到期：不早于 now（睡过头就等于把这条截止丢到下下拍）
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, now - std::chrono::milliseconds{1}, now, tick), now);
        // 换算成立：0 会被定时器当成解除武装，因此已到期的那一拍必须折成最近的一次唤醒
        EXPECT_EQ(Core::Detail::armedDurationFor(TickerWakePointProbe::nextTickerWakePoint(true, now - std::chrono::milliseconds{1}, now, tick), now),
                  std::chrono::milliseconds{1});

        // 节拍配成非正数＝不设上限，只按各连接自己的截止时刻睡
        const Clock::time_point farDeadline = now + std::chrono::seconds{30};
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, farDeadline, now, std::chrono::milliseconds::zero()), farDeadline);
    }

    /**
     * @brief 钉住：静态目录这类配置要有路由器可登记，没接之前当场拒而不是装作配好了
     * @details QuicServer 的路由器是外部交来的（本类不持有），这与两条 TCP 服务器自己持有路由器的
     *          形态不同。若允许在没接路由器时设静态目录，配置会落进一个永远不会被服务到的对象里——
     *          运维看到的就是「配了目录但一个文件都不出」，而这条路径上没有任何一条报错。
     *          另一侧也要钉：接上路由器之后目录不存在只降级为「未启用」并记告警，不抛——
     *          与明文/TLS 两侧同一判据（规范化失败是关闭静态服务，不是启动失败）。
     */
    TEST(QuicServer, RejectsStaticDirectoryConfigurationBeforeARouterIsAttached)
    {
        Core::EventLoop loop;

        QuicServer::Configuration configuration;
        configuration.certificateFile = certificatePath();
        configuration.privateKeyFile  = privateKeyPath();
        QuicServer server(loop, configuration);

        expectQuicRejected<Base::InvalidArgumentException>([&] { server.staticFileDir("web"); }, "必须先 setRouter()");
        expectQuicRejected<Base::InvalidArgumentException>([&] { server.setStaticFileCacheControl(std::optional<std::string>{"max-age=5"}); }, "必须先 setRouter()");
        // 目录列表开关与另外两条静态配置同一条前置：兜底路由没登记时没有可设置的配置本体
        expectQuicRejected<Base::InvalidArgumentException>([&] { server.staticDirectoryListing(true); }, "必须先 setRouter()");

        Router router;
        server.setRouter(router);
        // 不存在的目录：关静态服务并告警，读出空串；这一步不许抛
        EXPECT_NO_THROW(server.staticFileDir("definitely-not-here-asyngyanis"));
        EXPECT_TRUE(server.staticFileDir().empty()) << "规范化失败的目录应当落为「未启用」";
        EXPECT_NO_THROW(server.setStaticFileCacheControl(std::optional<std::string>{"max-age=5"}));
        EXPECT_NO_THROW(server.staticDirectoryListing(true)) << "h3 一侧没接上目录列表开关";

        // 写侧三个通道都有读口也有；此前 h3 只有写侧——运维要问「这条通道当下发的是哪条 Cache-Control」
        // 问不出，而旁边的 staticFileDir() 就有一个读口。钉住读回来的是写进去的那一份，且关掉之后跟着关
        ASSERT_TRUE(server.staticFileCacheControl().has_value()) << "刚设过的 Cache-Control 读不出来";
        EXPECT_EQ(*server.staticFileCacheControl(), "max-age=5");
        server.setStaticFileCacheControl(std::nullopt);
        EXPECT_FALSE(server.staticFileCacheControl().has_value()) << "空 optional 是「不发这条头」，读口要如实报未设置，不能报空串";
        server.setStaticFileCacheControl(std::optional<std::string>{"no-store"});
        ASSERT_TRUE(server.staticFileCacheControl().has_value());
        EXPECT_EQ(*server.staticFileCacheControl(), "no-store") << "改过一次之后读口还停在第一份上";
    }


    /**
     * @brief 限额的读源只有一处：Configuration 交进来的那份就是 getters 读到的那份，setter 换的是同一份
     * @details 这一条钉的是「两条通路」那类缺陷：本类此前只在构造那一刻读 `Configuration::serverLimits`，
     *          而会话与静态目录的映射上限各自还要读一次配置——加了 setter 之后若漏改其中一处，就会出现
     *          「setter 改了、会话仍拿旧值」或反过来。断言先验默认取自配置（不是默认档），再验换一份
     *          之后两处一起变。
     * @note 证伪：把会话那侧的读源改回 `m_configuration.serverLimits`，本条在「换一份之后」那格红。
     */
    TEST(QuicServer, LimitsAreHeldInOnePlaceAndSwapWholesale)
    {
        Core::EventLoop loop;

        QuicServer::Configuration configuration;
        configuration.certificateFile = certificatePath();
        configuration.privateKeyFile  = privateKeyPath();

        auto limits                          = std::make_shared<HttpServerLimits>();
        limits->maximumRequestsPerConnection = 7;
        limits->readTimeout                  = std::chrono::seconds{11};
        configuration.serverLimits           = limits;

        HttpParserLimits parserLimits;
        parserLimits.maximumHeaderCount = 33;
        configuration.parserLimits      = parserLimits;

        QuicServer server(loop, configuration);
        // 默认读的就是配置里那一份（不是 HttpServerLimits 的默认档）：这条若红，说明构造时接错了源
        EXPECT_EQ(server.limits().maximumRequestsPerConnection, 7U) << "限额没按 Configuration 给的那份生效";
        EXPECT_EQ(server.limits().readTimeout, std::chrono::seconds{11});
        EXPECT_EQ(server.parserLimits().maximumHeaderCount, 33U);

        HttpServerLimits replacement;
        replacement.maximumRequestsPerConnection = 2;
        replacement.idleTimeout                  = std::chrono::seconds{3};
        server.setLimits(replacement);
        EXPECT_EQ(server.limits().maximumRequestsPerConnection, 2U) << "setLimits 之后 getters 还读旧的那份";
        // 整体换代而不是逐字段合并：没写的字段回到默认档，而不是留着上一份的 11 秒
        EXPECT_EQ(server.limits().readTimeout, HttpServerLimits{}.readTimeout) << "换限额做成了改字段：在途会话会读到半新半旧的组合";

        HttpParserLimits replacementParser;
        replacementParser.maximumHeaderCount = 5;
        server.setParserLimits(replacementParser);
        EXPECT_EQ(server.parserLimits().maximumHeaderCount, 5U);
    }

    /**
     * @brief 端口由别人 bind、服务端只接手：h3 监听器在接手来的那条端口上就位
     * @details 跨进程共享一条 UDP 端口在 worker 侧就是这一形状（Windows 上多个进程各自 bind 同一端口
     *          不分摊，只能这么交）。判据落在「发布出来的端口就是交过来那一个」：接手那条路若偷偷
     *          自己再绑一次，端口必然对不上，那种服务端看着在监听、对端往原端口发的报文却一条也进不来。
     * @note 「能不能真做成一次握手」不在这里判——本文件的规矩是不把自家客户端当裁判。那一格由
     *       scripts/quic_cross_check.sh 的 --adopt-through-self-channel 档对着 aioquic 验
     */
    TEST(QuicServer, ServesThePortOfTheSocketSomeoneElseBound)
    {
        // 用例自己初始化套接字库：ctest 逐用例起进程，绑定的动作不能指望别的用例顺带做过
        const Platform::Socket::Initialization network;
        Platform::DatagramSocket               boundSocket = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(boundSocket.isValid()) << "夹具绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Platform::SocketAddress boundAddress = boundSocket.localAddress();
        const std::uint16_t           boundPort    = Core::InetAddress(boundAddress.storage, boundAddress.length).port();
        ASSERT_NE(boundPort, 0U);

        RunningQuicServer serving(std::chrono::seconds{30}, nullptr, {}, std::move(boundSocket));

        EXPECT_EQ(serving.listeningPort(), boundPort) << "接手模式下端口该来自交来的那份套接字，而不是自己再绑一个";
    }

    /**
     * @brief 小于 1200 字节的 Initial 必须在入口丢弃：不建连接、不回包（RFC 9000 §14.1）
     * @details 规范给的是 MUST discard：这一档既是「这条路径扛得住最小 MTU」的凭据，也是客户端的
     *          合规要求。入口放行它的代价不止多一条连接——地址验证开着时本端会先回一条 Retry，
     *          一个不用补齐就能拿到回包的对端从此没有动力按 §14.1 补齐。判据三面：小包一条回话
     *          都不该有、连接数为 0；同一形状补齐到 1200 之后仍要走进正常流程并拿到 Retry（旧实现
     *          两格都会回 Retry，因此第一面红、第二面绿，两侧合起来才钉住「按尺寸分流」）。
     * @note 这里刻意手拼字节而不用自家客户端：判据是「入口按 UDP 负载尺寸分流」，不是一次握手。
     *       与文件开头的约定不冲突——没有让自家实现充当裁判，报文形状直接取自 §17.2.2。
     */
    TEST(QuicServer, DiscardsSmallInitialDatagramsWithoutAnswering)
    {
        const Platform::Socket::Initialization network;
        Core::EventLoop                        loop;
        QuicServer::Configuration              configuration = makeServerConfiguration();
        // 开着地址验证：正常流程的第一站就是 Retry，回没回话因此成了「有没有走到这一步」的判据
        configuration.addressValidationTokenLifetime = std::chrono::seconds{30};
        configuration.addressValidationKey           = std::vector<std::uint8_t>(32U, 0x5AU);
        QuicServer                      server(loop, configuration);
        std::optional<Core::Task<void>> listenTask = server.listen(Core::InetAddress("127.0.0.1", 0));
        EventLoopThread                 loopThread{loop};
        loopThread.loop().scheduler().scheduleRemote(listenTask->handle());
        ASSERT_TRUE(waitForCondition([&] { return server.listeningPort() != 0U; }, kPeerWaitTimeout)) << "服务端没进入监听，后面的判据都是空的";

        const Core::InetAddress        serverAddress("127.0.0.1", server.listeningPort());
        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();

        const std::vector<std::uint8_t> smallInitial = makeProbeInitial(66U);
        ASSERT_EQ(smallInitial.size(), 66U);
        ASSERT_GT(sender.send(serverAddress.platformAddress(), smallInitial.data(), smallInitial.size()), 0);

        // 等一个明显超过回环往返的窗口：旧实现会回一条 Retry，新实现一条都不回
        std::vector<std::uint8_t> reply(1200U);
        bool                      gotReply     = false;
        const auto                pollDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
        while (std::chrono::steady_clock::now() < pollDeadline && !gotReply)
        {
            Platform::SocketAddress replyFrom{};
            gotReply = sender.receive(reply.data(), reply.size(), replyFrom) > 0;
            if (!gotReply)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        EXPECT_FALSE(gotReply) << "66 字节的 Initial 得到了回话：小于 1200 的 Initial 必须整条丢弃（RFC 9000 §14.1）";
        EXPECT_EQ(server.connectionCount(), 0U) << "丢掉的 Initial 不该留下连接状态";

        // 对照：同一形状补齐到 1200 之后，入口必须把它交给正常流程，地址验证回一条 Retry
        const std::vector<std::uint8_t> paddedInitial = makeProbeInitial(1200U);
        ASSERT_EQ(paddedInitial.size(), 1200U);
        ASSERT_GT(sender.send(serverAddress.platformAddress(), paddedInitial.data(), paddedInitial.size()), 0);

        std::size_t replyLength = 0;
        ASSERT_TRUE(waitForCondition(
                [&]
                {
                    Platform::SocketAddress replyFrom{};
                    const ssize_t           received = sender.receive(reply.data(), reply.size(), replyFrom);
                    if (received <= 0)
                    {
                        return false;
                    }
                    replyLength = static_cast<std::size_t>(received);
                    return true;
                },
                kPeerWaitTimeout))
                << "补齐到 1200 的 Initial 什么都没收到：入口把合规尺寸的报文也一起丢了";
        ASSERT_GE(replyLength, 5U) << "回包短于长头固定段";
        EXPECT_NE(reply[0] & 0x80U, 0U) << "回包不是长头";
        EXPECT_EQ((reply[0] >> 4U) & 0x03U, 3U) << "回包不是 Retry：地址验证开着，没带令牌的合规 Initial 应当进正常流程";
        EXPECT_EQ(server.connectionCount(), 0U) << "Retry 不该为这条握手留下状态";
    }

    /**
     * @brief 两种启动顺序不许混用：接手来的不许再 bind，按地址来的不许无参启动
     * @details 混用的后果都是静默的：接手来的服务端再去 bind 会把交过来的套接字闲置，对端往那个端口
     *          发的报文一条也到不了，症状与「移交没做成」一模一样；反过来无参的 listen() 落在按地址
     *          构造的对象上没有套接字可服务。两种都只可能出自调用方写错，所以当场点名而不是挑一种继续。
     * @note listen() 是惰性协程：抛出点在首次恢复时，`EXPECT_THROW(server.listen(...))` 只构造了协程帧、
     *       什么都测不到，因此两条帧都要投给循环真跑一遍再接异常
     */
    TEST(QuicServer, RejectsMixingTheAdoptedSocketWithAnAddress)
    {
        // 声明顺序就是销毁顺序的反面：循环最先声明（最后销毁），服务端在它之后、驱动帧再后，
        // 后台循环包装器最后声明（最先销毁、析构里 join）——服务端持有套接字，必须晚于循环停止才销毁
        Core::EventLoop                        loop;
        const Platform::Socket::Initialization network;
        QuicServer::Configuration              configuration = makeServerConfiguration();
        Platform::DatagramSocket               boundSocket   = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(boundSocket.isValid()) << "夹具绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();

        QuicServer                      adopting(loop, configuration, std::move(boundSocket));
        QuicServer                      binding(loop, makeServerConfiguration());
        std::optional<Core::Task<void>> adoptingWithAddress;
        std::optional<Core::Task<void>> bindingWithoutAddress;
        EventLoopThread                 loopThread{loop};

        std::string       adoptingReason;
        std::string       bindingReason;
        std::atomic<bool> isAdoptingRejected{false};
        std::atomic<bool> isBindingRejected{false};

        adoptingWithAddress.emplace(captureCoroutineFailure(adopting.listen(Core::InetAddress("127.0.0.1", 0)), adoptingReason, isAdoptingRejected));
        bindingWithoutAddress.emplace(captureCoroutineFailure(binding.listen(), bindingReason, isBindingRejected));
        loopThread.loop().scheduler().scheduleRemote(adoptingWithAddress->handle());
        loopThread.loop().scheduler().scheduleRemote(bindingWithoutAddress->handle());

        EXPECT_TRUE(waitForCondition([&] { return isAdoptingRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "接手来的服务端调带地址的 listen() 没被拒：那份套接字会被静默闲置";
        EXPECT_NE(adoptingReason.find("接手"), std::string::npos) << adoptingReason;
        EXPECT_TRUE(waitForCondition([&] { return isBindingRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "按地址构造的服务端调不带地址的 listen() 没被拒：没有可服务的套接字";
        EXPECT_NE(bindingReason.find("接手"), std::string::npos) << bindingReason;
    }

    /**
     * @brief 空对象（绑定失败或已被移走的那一份）不许当成接手来的一格：构造当场拒
     * @details 那种服务端跑起来是「一条报文也收不到的空壳」，而原因在调用方手里——自己 bind 的那份
     *          大概绑定就失败了，跨进程接手的那份没走 DatagramSocket::adopt。判据排在创建 TLS 上下文
     *          之前：构造期抛出之后析构不会跑，而上下文一旦 `release()` 交接给成员就无人认领（实测把
     *          本判据挪到那句交接之后，容器 LSan 报出 SSL_CTX_new 那一块 1784 字节的直接泄漏）
     */
    TEST(QuicServer, RejectsAnAdoptedSocketThatIsNotUsable)
    {
        Core::EventLoop           loop;
        QuicServer::Configuration configuration = makeServerConfiguration();
        expectQuicRejected<Base::InvalidArgumentException>([&] { QuicServer server(loop, configuration, Platform::DatagramSocket{}); }, "交来的套接字无效");
    }

    /**
     * @brief 已经在监听的 QUIC 服务端不许再 listen() 一次
     * @details 第二次启动会把 m_socket 换成新的一份，而收报文协程与定时器协程都还挂在旧的那份上（事件循环
     *          里的注册对象归它）：旧对象一被销毁，那个端口就再没人读。两侧实测的形态都是「端口换了、
     *          原服务不再应答」，Windows 与容器 ASan 下都没报出 use-after-free——坏的是这台服务端看起来
     *          还在服务。换端口没有原地重来的用法，新建一台即可。
     * @note 第一条要先真跑进监听（等端口发布出来）再投第二条，否则测的是「两条一起投」那种次序
     * @note 证伪（两侧实测）：摘掉两条 listen() 入口上的 requireFreshStart()，本条红在「没被拒」与
     *       「换了端口」两句上
     */
    TEST(QuicServer, RejectsASecondListenWhileAlreadyServing)
    {
        // 声明顺序照本文件其它用例：循环最先（最后销毁），服务端、驱动帧随后，后台循环包装器最后
        // （最先销毁、析构里 join）——服务端持有套接字，必须晚于循环停止才销毁
        Core::EventLoop                        loop;
        const Platform::Socket::Initialization network;
        QuicServer::Configuration              configuration = makeServerConfiguration();
        QuicServer                             server(loop, configuration);
        std::optional<Core::Task<void>>        firstDriver;
        std::optional<Core::Task<void>>        secondDriver;
        EventLoopThread                        loopThread{loop};

        firstDriver.emplace(server.listen(Core::InetAddress("127.0.0.1", 0)));
        loopThread.loop().scheduler().scheduleRemote(firstDriver->handle());
        ASSERT_TRUE(waitForCondition([&] { return server.listeningPort() != 0U; }, kPeerWaitTimeout)) << "第一条 listen() 没进入监听，后面的判据无从成立";

        const std::uint16_t servingPort = server.listeningPort();
        std::string         reason;
        std::atomic<bool>   isRejected{false};
        secondDriver.emplace(captureCoroutineFailure(server.listen(Core::InetAddress("127.0.0.1", 0)), reason, isRejected));
        loopThread.loop().scheduler().scheduleRemote(secondDriver->handle());

        EXPECT_TRUE(waitForCondition([&] { return isRejected.load(std::memory_order_acquire); }, kPeerWaitTimeout))
                << "第二次 listen() 没被拒：它会换掉正在服务的那份套接字，收报文与定时器两条协程都挂在已销毁的对象上";
        EXPECT_NE(reason.find("已经在 UDP 端口"), std::string::npos) << "文案没点名「重复启动」这个成因：" << reason;
        EXPECT_EQ(server.listeningPort(), servingPort) << "被拒的第二次启动换了端口：那次启动并非无害";
    }

    /**
     * @brief 监听协程在自己的首次派发里跑到收尾时，不许在就绪队列里留下一条已销毁帧的句柄
     * @details CI 上的形状是 h3 大正文那几条：回环把接收缓冲塞满，收报文那一趟每次 await 都立即就绪、
     *          一次都不让出，父协程因此在首次派发里就跑到了收尾——而定时器协程的句柄那时还排在队列里
     *          等着第一次派发，它所属的帧又只由父协程的局部 Task 拿着，于是先被销毁、后被 resume。
     *          TSan 的三条报告同指那一帧（resume 已释放的帧、帧内字段读到 1103 的收尾段、promise 里
     *          exception_ptr 的析构读）。
     *          本用例把这个竞态拉成确定形状：停止排在首次派发之前，收报文的 while 当场为假，父协程同样
     *          在一趟里跑到收尾。判据按「这一趟派发跑完还剩没剩没派发的句柄」问，不借 sanitizer 也红得起来。
     * @note 手泵是有意的：这条判据要的就是派发次序由用例决定，循环真跑起来会把两拍并进同一趟，形状反倒不成立
     * @note 证伪：把启动改回投调度器（`scheduler().schedule(expiryTask.handle())`）→ 红在「还剩一条」那句；
     *       只把收尾那次 co_await 摘掉而保留就地起首拍 → 本条仍绿，它钉的是「句柄归属 must 活过派发」这一条，
     *       等它退出那一条由上面第二句（监听协程一趟跑完）与容器 TSan/ASan 档的 h3 用例共同兜住
     */
    TEST(QuicServer, LeavesNoUndispatchedTickerHandleWhenListeningEndsInOnePass)
    {
        // 声明顺序照本文件其它用例：循环最先（最后销毁），socket 库、服务端、驱动帧依次随后
        Core::EventLoop                        loop;
        const Platform::Socket::Initialization network;
        QuicServer::Configuration              configuration = makeServerConfiguration();
        QuicServer                             server(loop, configuration);
        std::optional<Core::Task<void>>        listenTask;

        listenTask.emplace(server.listen(Core::InetAddress("127.0.0.1", 0)));
        loop.scheduler().schedule(listenTask->handle());
        // 停止排在首次派发之前：这一拍进去就会出来，父协程不会在任何 await 上让出
        server.stop();

        ASSERT_TRUE(loop.scheduler().runOne()) << "首次派发没跑起来：后面两句判据都是空的";
        EXPECT_TRUE(listenTask->isReady()) << "监听协程没在一趟里跑完，本用例要钉的形状没立住";
        EXPECT_FALSE(loop.scheduler().hasWork()) << "收尾后就绪队列里还剩没派发的句柄：那是定时器协程的帧，"
                                                    "而它已经随监听协程一起销毁，下一趟派发就是读已释放的内存";

        // 旧写法在这里 resume 那块已释放的帧（容器 ASan/TSan 报 heap-use-after-free，CI 三条报告都出自这一步）
        static_cast<void>(loop.scheduler().runOne());
    }

} // namespace AsynGyanis::Net
