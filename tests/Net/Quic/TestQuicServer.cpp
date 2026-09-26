// QUIC 服务端的构造期与配置校验用例：这两类判据不需要对端，因此留在树内。
//
// 对外行为（握手、ALPN、流数据与流收尾、drain 时的 CONNECTION_CLOSE、半开连接的回收、
// 跨实例会话恢复、单来源限额）**不再由链接进来的第三方 QUIC 实现裁定**：那类「同一棵树里互测」
// 暴露不出双方共享的误解，还会跟着被测实现一起被改松。它们整体搬到进程外的独立实现上：
//     scripts/quic_cross_check.sh <构建目录> <带 aioquic 的 python>
// 被测端是 tests/Tools/QuicProbeServer.cpp，判据由 aioquic 的解析器给出。
// 改 QuicServer 的对外行为时，跑那条脚本才算过门禁；在本文件里加「自家客户端连自家服务端」
// 的用例等于把刚拆掉的裁判请回来。
#include "Net/Quic/QuicServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/TimerQueue.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include "CoreTestSupport.h"

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
        /// 夹具等待上限
        constexpr std::chrono::milliseconds kWaitTimeout{8000};

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
             */
            explicit RunningQuicServer(const std::chrono::seconds idleTimeout = std::chrono::seconds{30},
                                       std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter = nullptr,
                                       std::vector<std::string> sessionTicketKeyFiles = {})
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile       = certificatePath();
                configuration.privateKeyFile        = privateKeyPath();
                configuration.idleTimeout           = idleTimeout;
                configuration.perIpConnectionLimiter = std::move(perIpConnectionLimiter);
                configuration.sessionTicketKeyFiles  = std::move(sessionTicketKeyFiles);

                m_server = std::make_unique<QuicServer>(m_loop, configuration);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                // 在起线程之前把自己排进所属循环的就绪队列：调度器只在循环线程上跑，
                // 这里还在创建者线程上，于是这一次排入是「归属线程内」的合法调用
                m_loop.scheduler().schedule(m_listenTask->handle());

                m_loopThread = std::thread([this]
                {
                    m_loop.run();
                });

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

            Core::EventLoop                 m_loop;             ///< 服务端所属事件循环
            std::unique_ptr<QuicServer>     m_server;           ///< 被测服务端
            std::optional<Core::Task<>>     m_listenTask;       ///< 监听协程（Task 没有默认构造，用 optional 托管）
            std::thread                     m_loopThread;       ///< 跑循环的线程
            std::atomic<std::uint16_t>      m_listeningPort{0}; ///< 循环线程写下的本端端口快照
        };
    } // namespace

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
        EXPECT_THROW(static_cast<void>(std::make_unique<RunningQuicServer>(
                             std::chrono::seconds{30}, nullptr, std::vector<std::string>{(directory.path() / "missing.key").string()})),
                     Base::Exception);
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
        EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);

        QuicServer::Configuration missingPrivateKey;
        missingPrivateKey.certificateFile = certificatePath();
        missingPrivateKey.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/no-such-key.pem";
        EXPECT_THROW(QuicServer server(loop, missingPrivateKey), Base::SystemException);

        // 两个文件各自都在、也能各自加载，只有配对检查拦得住：这一道是构造里最后一道抛点
        QuicServer::Configuration mismatchedPair;
        mismatchedPair.certificateFile = certificatePath();
        mismatchedPair.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/test_ip_key.pem";
        EXPECT_THROW(QuicServer server(loop, mismatchedPair), Base::SystemException);

        // 反复构造失败也要抛，且不留残留：以上三道各来一轮，退出时 LSan 不许报任何东西
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);
        }
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
        cappedBelowTls13.certificateFile                 = certificatePath();
        cappedBelowTls13.privateKeyFile                  = privateKeyPath();
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
            {"1.3 套件串", [](Core::TlsPolicy &policy)
             {
                 policy.tls13CipherSuites = "TLS_NOT_A_REAL_SUITE";
             }},
            {"命名曲线", [](Core::TlsPolicy &policy)
             {
                 policy.supportedGroups = "not-a-curve";
             }},
            {"1.2 套件串", [](Core::TlsPolicy &policy)
             {
                 policy.cipherList = "!";
             }},
        };
        for (const auto &[label, mutate]: refused)
        {
            QuicServer::Configuration configuration;
            configuration.certificateFile = certificatePath();
            configuration.privateKeyFile  = privateKeyPath();
            mutate(configuration.tlsPolicy);
            EXPECT_THROW(QuicServer server(loop, configuration), Base::Exception) << "这一项 OpenSSL 认不出来却起了服务：" << label;
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
        configuration.certificateFile            = certificatePath();
        configuration.privateKeyFile             = privateKeyPath();
        configuration.tlsPolicy.cipherList       = "HIGH:!aNULL:!MD5";
        configuration.tlsPolicy.minimumProtocolVersion = Core::TlsPolicy::ProtocolVersion::Tls1_2;
        configuration.tlsPolicy.certificateAuthorityFile = certificatePath();
        configuration.tlsPolicy.verifyDepth      = 4;
        configuration.tlsPolicy.supportedGroups  = "X25519:secp384r1";
        configuration.tlsPolicy.securityLevel    = 2;

        EXPECT_NO_THROW(QuicServer server(loop, configuration))
                << "这几项在 QUIC 侧用不上，但都是合法配置，不该挡住启动";
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
    } // namespace

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
        constexpr auto tick = std::chrono::milliseconds{10};

        // 零连接：不管传进来的截止是什么，都按空闲上界睡
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(false, Clock::time_point::max(), now, tick),
                  now + TickerWakePointProbe::kIdleTickerSleep);
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(false, now + std::chrono::milliseconds{1}, now, tick),
                  now + TickerWakePointProbe::kIdleTickerSleep) << "没有连接时，任何截止时刻都不该把节拍叫醒";

        // 有连接：早于节拍的截止按时到
        const Clock::time_point soonDeadline = now + std::chrono::milliseconds{3};
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, soonDeadline, now, tick), soonDeadline);
        // 晚于节拍的按节拍到（补刀那一档靠轮询兜）
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, now + std::chrono::seconds{5}, now, tick), now + tick);
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, Clock::time_point::max(), now, tick), now + tick)
                << "查不到截止时不能睡过头";
        // 已经到期：不早于 now（睡过头就等于把这条截止丢到下下拍）
        EXPECT_EQ(TickerWakePointProbe::nextTickerWakePoint(true, now - std::chrono::milliseconds{1}, now, tick), now);
        // 换算成立：0 会被定时器当成解除武装，因此已到期的那一拍必须折成最近的一次唤醒
        EXPECT_EQ(Core::detail::armedDurationFor(
                          TickerWakePointProbe::nextTickerWakePoint(true, now - std::chrono::milliseconds{1}, now, tick), now),
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

        EXPECT_THROW(server.staticFileDir("web"), Base::InvalidArgumentException);
        EXPECT_THROW(server.setStaticFileCacheControl(std::optional<std::string>{"max-age=5"}), Base::InvalidArgumentException);

        Router router;
        server.setRouter(router);
        // 不存在的目录：关静态服务并告警，读出空串；这一步不许抛
        EXPECT_NO_THROW(server.staticFileDir("definitely-not-here-asyngyanis"));
        EXPECT_TRUE(server.staticFileDir().empty()) << "规范化失败的目录应当落为「未启用」";
        EXPECT_NO_THROW(server.setStaticFileCacheControl(std::optional<std::string>{"max-age=5"}));
    }

} // namespace AsynGyanis::Net
