// QuicClientConnection 用例：与自家服务端做完握手并定下 ALPN、名字不符的证书要被拒、
// 黑洞地址按时限收场、主机名为空在构造期就拒

#include "Net/Quic/QuicClientConnection.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/Socket.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::waitForCondition;
        using namespace std::chrono_literals;

        /// 一般等待上限：回环上的 QUIC 握手是毫秒级的，超时即判失败
        constexpr std::chrono::milliseconds kWaitTimeout{8000};

        /**
         * @brief 取仓库内的证书夹具路径
         * @param fileName 文件名
         * @return std::filesystem::path 完整路径
         */
        std::filesystem::path fixturePath(const std::string_view fileName)
        {
            return std::filesystem::path(TEST_FIXTURES_DIR) / fileName;
        }

        /// 回环 IP 专用的证书对：CN=127.0.0.1 且 SAN=IP:127.0.0.1，出站侧要它才验得过
        const std::filesystem::path kIpCertificatePath = fixturePath("test_ip_cert.pem");
        const std::filesystem::path kIpPrivateKeyPath  = fixturePath("test_ip_key.pem");

        /**
         * @brief 起一个真在服务端：给本文件当对面那一方
         * @details 只挂一个空 Router：本文件的判据全在传输层与 TLS 层，不做任何 HTTP/3 请求，
         *          因此不需要业务路由，但必须有 Router（`QuicServer` 的会话是按路由器建的）
         */
        class RunningServerPeer
        {
        public:
            /**
             * @brief 按给定证书起服务端并等它进入监听
             * @param certificateFile 服务端证书
             * @param privateKeyFile 配套私钥
             */
            RunningServerPeer(std::filesystem::path certificateFile, std::filesystem::path privateKeyFile)
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile = std::move(certificateFile).string();
                configuration.privateKeyFile  = std::move(privateKeyFile).string();
                configuration.idleTimeout     = std::chrono::seconds{30};
                m_server = std::make_unique<QuicServer>(m_loop, configuration);
                m_server->setRouter(m_router);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                m_loop.scheduler().schedule(m_listenTask->handle());
                m_loopThread = std::thread([this]
                {
                    m_loop.run();
                });
                // 端口由循环线程在内核分配后发布，跨线程只读那一个原子量（listeningPort 的约定）
                static_cast<void>(waitForCondition(
                        [this]
                        {
                            return m_server->listeningPort() != 0U;
                        },
                        kWaitTimeout));
            }

            ~RunningServerPeer()
            {
                m_server->stop();
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            RunningServerPeer(const RunningServerPeer &) = delete;

            RunningServerPeer &operator=(const RunningServerPeer &) = delete;

            /// 实际绑定到的端口
            [[nodiscard]] std::uint16_t listeningPort() const noexcept
            {
                return m_server->listeningPort();
            }

        private:
            Core::EventLoop             m_loop;          ///< 服务端所属循环
            Router                      m_router;        ///< 空路由器，只为让会话建得起来
            std::unique_ptr<QuicServer> m_server{};      ///< 对面那一方
            std::optional<Core::Task<>> m_listenTask{};  ///< 监听协程
            std::thread                 m_loopThread{};  ///< 跑循环的线程
        };

        /**
         * @brief 在客户端自己的循环线程上跑一次 connect()，把结果交回测试线程
         * @details 协程必须在循环线程上启动与收尾（事件循环的线程契约），因此结果、协商到的 ALPN
         *          与耗时都由循环线程写、测试线程在 `awaitFinished()` 之后读
         */
        class ConnectAttempt
        {
        public:
            /**
             * @brief 排好一次出站连接尝试并起循环线程
             * @param configuration 客户端配置
             * @param serverAddress 目标地址
             */
            ConnectAttempt(QuicClientConnection::Configuration configuration, const Core::InetAddress &serverAddress) :
                m_configuration(std::move(configuration)), m_serverAddress(serverAddress)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this]
                {
                    m_loop.run();
                });
            }

            ~ConnectAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            ConnectAttempt(const ConnectAttempt &) = delete;

            ConnectAttempt &operator=(const ConnectAttempt &) = delete;

            /// 等这次尝试出结果
            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition(
                        [this]
                        {
                            return m_isFinished.load(std::memory_order_acquire);
                        },
                        timeout);
            }

            [[nodiscard]] bool isSuccessful() const noexcept
            {
                return m_isSuccessful.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::string negotiatedApplicationProtocol() const
            {
                return m_negotiatedApplicationProtocol;
            }

            [[nodiscard]] std::chrono::milliseconds elapsed() const noexcept
            {
                return m_elapsed;
            }

        private:
            /// 在循环线程上跑完整次尝试
            Core::Task<> run()
            {
                const auto startedAt = std::chrono::steady_clock::now();
                auto client = std::make_unique<QuicClientConnection>(m_loop, m_configuration);
                const bool isSuccessful = co_await client->connect(m_serverAddress);
                m_isSuccessful.store(isSuccessful, std::memory_order_release);
                if (isSuccessful)
                {
                    m_negotiatedApplicationProtocol = client->negotiatedApplicationProtocol();
                }
                m_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startedAt);
                // 套接字要在循环线程上关：交给测试线程的帧销毁时再做就跨线程了
                client.reset();
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop                       m_loop;                            ///< 客户端所属循环
            QuicClientConnection::Configuration   m_configuration;                   ///< 本次尝试用的配置
            Core::InetAddress                     m_serverAddress;                   ///< 目标地址
            std::optional<Core::Task<>>           m_task{};                          ///< 尝试协程
            std::thread                           m_loopThread{};                    ///< 跑循环的线程
            std::atomic<bool>                     m_isFinished{false};               ///< 结果已就位
            std::atomic<bool>                     m_isSuccessful{false};             ///< 握手是否完成
            std::string                           m_negotiatedApplicationProtocol{}; ///< 协商到的 ALPN
            std::chrono::milliseconds             m_elapsed{0};                      ///< 整次尝试耗时
        };

        /**
         * @brief 造一份指向回环某端口的客户端配置
         * @param hostName 主机名（同时是 SNI 与证书校验目标）
         * @param certificateAuthorityFile 信任锚
         * @param handshakeTimeout 握手时限
         * @return QuicClientConnection::Configuration 配好的配置
         */
        QuicClientConnection::Configuration makeConfiguration(const std::string &hostName,
                                                              const std::filesystem::path &certificateAuthorityFile,
                                                              const std::chrono::milliseconds handshakeTimeout)
        {
            QuicClientConnection::Configuration configuration;
            configuration.hostName                  = hostName;
            configuration.applicationProtocolIdentifiers = {std::string{"h3"}};
            configuration.tlsPolicy.certificateAuthorityFile = certificateAuthorityFile.string();
            configuration.handshakeTimeout          = handshakeTimeout;
            return configuration;
        }
    } // namespace

    /**
     * @brief 出站侧能把握手做到完成，并与服务端谈定 h3
     * @details 这条是「角色化之后两型真的对称」的正面判据：Initial 密钥方向、传输参数里该不该出现
     *          ODCID、握手确认的时刻、以及 §7.2 的「目的标识要换成服务端自报的那个」全部要成立，
     *          否则对面根本不会把这条连接当成一条完成的握手。撤掉客户端一侧的密钥方向翻转，
     *          本条第一个 Initial 就解不开（对端会静默丢弃），时限到点收场。
     */
    TEST(QuicClientConnection, CompletesTheHandshakeAndNegotiatesH3WithOurOwnServer)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        ConnectAttempt attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, 4s),
                               Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "出站握手既没成也没失败，挂在那里";
        EXPECT_TRUE(attempt.isSuccessful()) << "握手没完成：" << attempt.elapsed().count() << " 毫秒后收场";
        EXPECT_EQ(attempt.negotiatedApplicationProtocol(), "h3") << "握手成了但 ALPN 没谈定，出站侧不知道该按哪套协议说话";
    }

    /**
     * @brief 证书链可信、名字却对不上时，握手必须失败
     * @details 用「同一份信任锚 + 另一个主机名」这一组，是为了让失败原因只能是主机名校验：
     *          换一张不认识的 CA 就同时把链的校验也打掉了，那条红了说明不了名字这一环。
     *          去掉 `SSL_set1_host`，本条会绿——那正是它要拦的缺陷。
     */
    TEST(QuicClientConnection, RejectsATrustedCertificateWhoseNameDoesNotMatch)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath};
        ASSERT_NE(server.listeningPort(), 0U);

        ConnectAttempt attempt{makeConfiguration("localhost", kIpCertificatePath, 4s),
                               Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "名字不符的握手既没被拒也没收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "对端证书的名字与目标不符却完成了握手：身份校验形同虚设";
    }

    /**
     * @brief 对端完全不说话时，按握手时限收场而不是吊死
     * @details 目标选 RFC 5737 的文档地址段：那里没有服务、也通常不会回 ICMP 端口不可达，
     *          于是唯一可能的结束方式就是看门狗掐断。断言带上下界——只判「结果是假」会让
     *          「立刻失败」（比如套接字建不起来）也蒙混过关，那与超时是两回事。
     */
    TEST(QuicClientConnection, GivesUpWithinTheHandshakeDeadlineWhenNothingAnswers)
    {
        constexpr std::chrono::milliseconds kHandshakeTimeout{800};

        ConnectAttempt attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, kHandshakeTimeout),
                               Core::InetAddress::resolve("192.0.2.1", 443).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "没有看门狗掐断：这条尝试挂住了";
        EXPECT_FALSE(attempt.isSuccessful());
        EXPECT_GE(attempt.elapsed(), kHandshakeTimeout) << "比时限还早就收场：不是被握手时限掐断的，看门狗没生效";
        EXPECT_LT(attempt.elapsed(), kHandshakeTimeout * 8) << "拖到空闲超时才回：握手时限没有掐住这条路";
    }

    /**
     * @brief 主机名为空在构造期就拒，不留到碰网络之后
     * @details SNI 与证书校验目标都取自它，空值意味着「拿一条不校验身份的握手出去」；
     *          而这类配置错误的正确暴露时刻是调用方把配置交进来的那一句，不是某个协程里
     */
    TEST(QuicClientConnection, RejectsAnEmptyHostNameDuringConstruction)
    {
        Core::EventLoop loop;
        QuicClientConnection::Configuration configuration;
        configuration.applicationProtocolIdentifiers = {std::string{"h3"}};

        EXPECT_THROW(static_cast<void>(QuicClientConnection{loop, configuration}), Base::InvalidArgumentException)
                << "空主机名被放过了：这条连接出去不会校验对端身份";
    }

} // namespace AsynGyanis::Net
