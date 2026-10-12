// QuicClientConnection 用例：与自家服务端做完握手并定下 ALPN、名字不符的证书要被拒、
// 黑洞地址按时限收场、主机名为空在构造期就拒

#include "Net/Quic/QuicClientConnection.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/Socket.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"
#include "HttpTestSupport.h"
#include "NetTestSupport.h"
#include "QuicRfcVectors.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::waitForCondition;
        using namespace std::chrono_literals;
        using AsynGyanis::Net::TestSupport::kAppendixA2ProtectedPacketHex;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;

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

        // 两处简写要先声明再定义：它们互相引用夹具与配置构造函数
        class RunningServerPeer;
        class ConnectAttempt;
        QuicClientConnection::Configuration makeConfiguration(const std::string &hostName, const std::filesystem::path &certificateAuthorityFile,
                                                              const std::chrono::milliseconds handshakeTimeout);

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
             * @param configurationTweak 造服务端之前对配置的追加改动。双向 TLS 那两条要靠它打开
             *                           「服务端要求客户端证书」并装上校验用的 CA——配置判据全在构造期
             *                           定死，这是唯一的下手机会
             */
            RunningServerPeer(std::filesystem::path certificateFile, std::filesystem::path privateKeyFile,
                              const std::function<void(QuicServer::Configuration &)> &configurationTweak = {})
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile = std::move(certificateFile).string();
                configuration.privateKeyFile  = std::move(privateKeyFile).string();
                configuration.idleTimeout     = std::chrono::seconds{30};
                if (configurationTweak)
                {
                    configurationTweak(configuration);
                }
                m_certificateFile = configuration.certificateFile;
                m_privateKeyFile  = configuration.privateKeyFile;
                m_server          = std::make_unique<QuicServer>(m_loop, configuration);
                m_server->setRouter(m_router);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                m_loop.scheduler().schedule(m_listenTask->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
                // 端口由循环线程在内核分配后发布，跨线程只读那一个原子量（listeningPort 的约定）
                static_cast<void>(waitForCondition([this] { return m_server->listeningPort() != 0U; }, kWaitTimeout));
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

            /// 本服务端（跨线程读它的两个计数只走原子镜像：connectionCount / handshakingConnectionCount）
            [[nodiscard]] const QuicServer &server() const noexcept
            {
                return *m_server;
            }

            /// 实际绑定到的端口
            [[nodiscard]] std::uint16_t listeningPort() const noexcept
            {
                return m_server->listeningPort();
            }

            /// 本端证书文件路径（用例要把新身份覆盖到同一批路径上， reloadCertificate() 按原路径重读）
            [[nodiscard]] const std::string &certificateFile() const noexcept
            {
                return m_certificateFile;
            }

            /**
             * @brief 在服务端所属的循环线程上做一次证书热重载
             * @return true 新身份已生效
             * @details 走 postRemote 而不是直接调：h3 的连接是在循环线程上 SSL_new 出来的，
             *          重载的提交点要与它定序（本类的锁只保证快照一致，不保证顺序）
             */
            bool reloadCertificate()
            {
                std::promise<bool> finished;
                auto               outcome = finished.get_future();
                m_loop.scheduler().postRemote([this, &finished] { finished.set_value(m_server->reloadCertificate()); });
                return outcome.get();
            }


        private:
            std::string                 m_certificateFile{}; ///< 起服务端时用的证书路径（重载按它重读）
            std::string                 m_privateKeyFile{};  ///< 配套的私钥路径
            Core::EventLoop             m_loop;              ///< 服务端所属循环
            Router                      m_router;            ///< 空路由器，只为让会话建得起来
            std::unique_ptr<QuicServer> m_server{};          ///< 对面那一方
            std::optional<Core::Task<>> m_listenTask{};      ///< 监听协程
            std::thread                 m_loopThread{};      ///< 跑循环的线程
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
                m_loopThread = std::thread([this] { m_loop.run(); });
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
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
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
                const auto startedAt    = std::chrono::steady_clock::now();
                auto       client       = std::make_unique<QuicClientConnection>(m_loop, m_configuration);
                const bool isSuccessful = co_await client->connect(m_serverAddress);
                m_isSuccessful.store(isSuccessful, std::memory_order_release);
                if (isSuccessful)
                {
                    m_negotiatedApplicationProtocol = client->negotiatedApplicationProtocol();
                }
                m_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
                // 套接字要在循环线程上关：交给测试线程的帧销毁时再做就跨线程了
                client.reset();
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop                     m_loop;                            ///< 客户端所属循环
            QuicClientConnection::Configuration m_configuration;                   ///< 本次尝试用的配置
            Core::InetAddress                   m_serverAddress;                   ///< 目标地址
            std::optional<Core::Task<>>         m_task{};                          ///< 尝试协程
            std::thread                         m_loopThread{};                    ///< 跑循环的线程
            std::atomic<bool>                   m_isFinished{false};               ///< 结果已就位
            std::atomic<bool>                   m_isSuccessful{false};             ///< 握手是否完成
            std::string                         m_negotiatedApplicationProtocol{}; ///< 协商到的 ALPN
            std::chrono::milliseconds           m_elapsed{0};                      ///< 整次尝试耗时
        };

        /**
         * @brief 造一份指向回环某端口的客户端配置
         * @param hostName 主机名（同时是 SNI 与证书校验目标）
         * @param certificateAuthorityFile 信任锚
         * @param handshakeTimeout 握手时限
         * @return QuicClientConnection::Configuration 配好的配置
         */
        /**
         * @brief 把一份夹具文件原样复制到目标路径
         */
        void copyFixtureFile(const std::filesystem::path &source, const std::filesystem::path &destination)
        {
            std::error_code failure;
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, failure);
            ASSERT_FALSE(failure) << "复制 " << source.string() << " 到 " << destination.string() << " 失败：" << failure.message();
        }

        /**
         * @brief 往路径里写一段文本（用来摆「证书文件坏了」这类现场）
         */
        void writeTextFile(const std::filesystem::path &path, const std::string_view contents)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            ASSERT_TRUE(stream.good()) << "写 " << path.string() << " 没成功";
        }

        /**
         * @brief 读一份文件的全文
         */
        [[nodiscard]] std::string readWholeFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 把两份自签证书并成一份信任锚文件
         * @details 信任对两种身份都成立，才能保证「连不上」的唯一解释是**身份本身变了**，
         *          而不是新那张不在信任库里——否则这条判据是空的
         */
        void writeCombinedTrustAnchors(const std::filesystem::path &destination, const std::vector<std::filesystem::path> &certificates)
        {
            std::string combined;
            for (const std::filesystem::path &certificate: certificates)
            {
                combined += readWholeFile(certificate);
            }
            writeTextFile(destination, combined);
        }

        /**
         * @brief 与给定端口上的服务端握一次手，交回成没成
         */
        [[nodiscard]] bool handshakeSucceeded(const std::uint16_t port, const std::string &hostName, const std::filesystem::path &trustAnchors)
        {
            ConnectAttempt attempt{makeConfiguration(hostName, trustAnchors, std::chrono::milliseconds{4000}), Core::InetAddress::resolve("127.0.0.1", port).value()};
            EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次握手连结果都没交回来";
            return attempt.isSuccessful();
        }

        QuicClientConnection::Configuration makeConfiguration(const std::string &hostName, const std::filesystem::path &certificateAuthorityFile,
                                                              const std::chrono::milliseconds handshakeTimeout)
        {
            QuicClientConnection::Configuration configuration;
            configuration.hostName                           = hostName;
            configuration.applicationProtocolIdentifiers     = {std::string{"h3"}};
            configuration.tlsPolicy.certificateAuthorityFile = certificateAuthorityFile.string();
            configuration.handshakeTimeout                   = handshakeTimeout;
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

        ConnectAttempt attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, 4s), Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "出站握手既没成也没失败，挂在那里";
        EXPECT_TRUE(attempt.isSuccessful()) << "握手没完成：" << attempt.elapsed().count() << " 毫秒后收场";
        EXPECT_EQ(attempt.negotiatedApplicationProtocol(), "h3") << "握手成了但 ALPN 没谈定，出站侧不知道该按哪套协议说话";
    }

    /**
     * @brief 名额归因（B-1）：未完成握手的连接与已验证连接是两个可分的数
     * @details 两半：① 真握完一条之后，在线数 ≥1 而「未验证」归零——两个读数按验证状态分开，
     *          这就是归因；② 只发 Initial、握手谈不成的流量（RFC 9001 附录 A.2 的整包，ALPN 不是
     *          h3）不留下僵尸条目：服务端拒绝它，等待之后两数复原。两类连接共用同一份
     *          maximumConnections 是刻意的（理由见 QuicConnection.h 的清单），本用例钉的是归因那一半。
     */
    TEST(QuicServer, AttributesHandshakingConnectionsSeparatelyFromEstablishedOnes)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";
        EXPECT_EQ(server.server().connectionCount(), 0U);
        EXPECT_EQ(server.server().handshakingConnectionCount(), 0U);

        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", kIpCertificatePath)) << "基线握手没成";
        // ① 握手完成后：在线 ≥1、「未验证」归零——两个读数按状态分开。
        // 归因镜像由定时循环按拍重算，而空闲起步的第一拍要等满 kIdleTickerSleep（1 秒，见 QuicServer.h）才落地：
        // **固定睡满那一拍再加余量**，让本条的断言读的是重算之后的值。只等「条件成立」不行——镜像初值就是 0，
        // 条件在重算前恒真；正确实现下这段等待不改变结论（重算前后都是 0），把归因写错（例如拿在线数当
        // 未验证数）才会在重算后读出大于 0 的值，本条即钉住它
        ASSERT_TRUE(waitForCondition([&] { return server.server().connectionCount() >= 1U; }, kWaitTimeout)) << "完成的握手没让服务端记上在线连接";
        std::this_thread::sleep_for(std::chrono::milliseconds{1200});
        EXPECT_EQ(server.server().handshakingConnectionCount(), 0U) << "已完成的握手仍留在「未验证」名下";
        const std::size_t onlineAfterHandshake = server.server().connectionCount();

        // ② 谈不成握手的 Initial 流量：拒绝之后不留条目（ALPN 不是 h3，服务端按握手失败收口）
        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const std::vector<std::uint8_t> badHandshakeInitial = makeBytesFromHex(kAppendixA2ProtectedPacketHex);
        ASSERT_EQ(badHandshakeInitial.size(), 1200U) << "附录 A.2 整包长度不是 1200：向量用法不对";
        const Core::InetAddress serverAddress("127.0.0.1", server.listeningPort());
        ASSERT_GT(sender.send(serverAddress.platformAddress(), badHandshakeInitial.data(), badHandshakeInitial.size()), 0);

        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        EXPECT_EQ(server.server().handshakingConnectionCount(), 0U) << "谈不成握手的 Initial 在「未验证」名下有残留";
        EXPECT_EQ(server.server().connectionCount(), onlineAfterHandshake) << "坏握手流量改变了在线数：条目没被收干净";
    }

    /**
     * @brief 钉住：单来源名额拒绝的告警被时间窗压住，不是每个被拒的 Initial 一条
     * @details 被拒的连接不进别名索引，对端的握手重传会以新 Initial 反复走到这个判定点——不压重条时
     *          这是**一条报文一条日志**的远端可驱动刷屏面。与 PROXY 组同一形状：量级进那一条放行的
     *          日志（同窗压掉了多少条），重复条目才被丢掉；撤掉闸门时本用例的「恰一条」当场红。
     */
    TEST(QuicServer, ThrottlesRepeatedPerIpRejectionWarnings)
    {
        const HttpTestSupport::LogCapture logCapture; // 先挂再开服：日志走根日志器，晚一步就漏掉前面几条

        const std::shared_ptr<PerIpConnectionLimiter> limiter = std::make_shared<PerIpConnectionLimiter>(2);
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [&limiter](QuicServer::Configuration &configuration) { configuration.perIpConnectionLimiter = limiter; }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        // 先把手上的两个名额占满：两条完成握手的连接都从 127.0.0.1 来，同一份单来源账
        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", kIpCertificatePath)) << "第一条基线握手没成";
        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", kIpCertificatePath)) << "第二条基线握手没成";
        ASSERT_TRUE(waitForCondition([&] { return server.server().connectionCount() >= 2U; }, kWaitTimeout)) << "两条完成的握手没让服务端记上在线连接";

        // 名额已满：新 Initial 一律被单来源闸拒掉（附录 A.2 的整包就够——走到这层不需要能解开的握手）
        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const std::vector<std::uint8_t> rejectedInitial = makeBytesFromHex(kAppendixA2ProtectedPacketHex);
        const Core::InetAddress         serverAddress("127.0.0.1", server.listeningPort());
        for (std::size_t attempt = 0; attempt < 5U; ++attempt)
        {
            ASSERT_GT(sender.send(serverAddress.platformAddress(), rejectedInitial.data(), rejectedInitial.size()), 0);
        }

        // 等第一条放行的日志落地、再给整批一点消费时间：窗口 10 秒，五条串行发送至多一条
        ASSERT_TRUE(waitForCondition([&] { return logCapture.countContaining("的并发连接已达上限") >= 1U; }, kWaitTimeout))
                << "被拒的 Initial 一条告警都没留：闸门把这条信号整个吞了";
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        EXPECT_EQ(logCapture.countContaining("的并发连接已达上限"), 1U) << "一批同类拒绝写出了多条告警：这个调用点没接闸门";
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

        ConnectAttempt attempt{makeConfiguration("localhost", kIpCertificatePath, 4s), Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
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

        ConnectAttempt attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, kHandshakeTimeout), Core::InetAddress::resolve("192.0.2.1", 443).value()};
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
        Core::EventLoop                     loop;
        QuicClientConnection::Configuration configuration;
        configuration.applicationProtocolIdentifiers = {std::string{"h3"}};

        EXPECT_THROW(static_cast<void>(QuicClientConnection{loop, configuration}), Base::InvalidArgumentException) << "空主机名被放过了：这条连接出去不会校验对端身份";
    }

    /**
     * @brief 把服务端配成「要求客户端证书却没给信任锚」时，构造当场抛而不是留一条永远握不成的监听器
     * @details 这种配置在 TLS 里的形态不是「退化成不校验」而是**每条握手都失败**（没有可对照的根，
     *          任何客户端证书都验不过）。启动时点名比让运维从一堆握手失败里反推这一行配置便宜得多，
     *          与 HTTPS 侧 `setClientCertificateRequired()` 的拒绝同一判据。
     */
    TEST(QuicServer, RejectsClientCertificateRequirementWithoutATrustAnchor)
    {
        Core::EventLoop           loop;
        QuicServer::Configuration configuration;
        configuration.certificateFile           = kIpCertificatePath.string();
        configuration.privateKeyFile            = kIpPrivateKeyPath.string();
        configuration.requireClientCertificates = true;

        try
        {
            static_cast<void>(QuicServer{loop, configuration});
            FAIL() << "要求客户端证书却没有 CA 的配置被放过了：这台服务端每条握手都会失败";
        } catch (const Base::SystemException &failure)
        {
            // 判据取「点名了该给的那一项」：只断异常类型的话，任何一条别的构造期拒绝都能顶掉这条用例
            const std::string reason = failure.what();
            EXPECT_NE(reason.find("certificateAuthorityFile"), std::string::npos) << "原因没指出缺的是信任锚：" << reason;
        }
    }

    /**
     * @brief 服务端要求客户端证书时，出示受信任证书的客户端能握上手并谈定 h3
     * @details 夹具是自签的：一张证书既是服务端身份又是它自己的根，因此同一份文件既能当服务端的
     *          证书、又能当校验客户端证书的 CA（与 HTTPS 侧那条 mTLS 用例同一手法）。
     */
    TEST(QuicClientConnection, CompletesTheHandshakeWhenTheClientPresentsATrustedCertificate)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [](QuicServer::Configuration &configuration)
                                 {
                                     configuration.requireClientCertificates          = true;
                                     configuration.tlsPolicy.certificateAuthorityFile = kIpCertificatePath.string();
                                 }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        auto configuration                  = makeConfiguration("127.0.0.1", kIpCertificatePath, 4s);
        configuration.clientCertificateFile = kIpCertificatePath.string();
        configuration.clientPrivateKeyFile  = kIpPrivateKeyPath.string();

        ConnectAttempt attempt{configuration, Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "带客户端证书的握手既没成也没失败，挂在那里";
        EXPECT_TRUE(attempt.isSuccessful()) << "合法客户端证书没被接受：" << attempt.elapsed().count() << " 毫秒后收场";
        EXPECT_EQ(attempt.negotiatedApplicationProtocol(), "h3") << "握手成了但 ALPN 没谈定";
    }

    /**
     * @brief 客户端证书与私钥只给一项时在构造期就拒
     * @details 只给一项的后果不在启动时报「少一个参数」，而是到握手里出示证书那一刻才发现没有可签名的
     *          私钥（或反过来）——失败点离成因隔了一整条握手，最难查。
     */
    TEST(QuicClientConnection, RejectsAHalfSuppliedClientIdentityDuringConstruction)
    {
        Core::EventLoop loop;

        for (const auto &halfSupplied: {std::pair{kIpCertificatePath.string(), std::string{}}, std::pair{std::string{}, kIpPrivateKeyPath.string()}})
        {
            auto configuration                  = makeConfiguration("127.0.0.1", kIpCertificatePath, 4s);
            configuration.clientCertificateFile = halfSupplied.first;
            configuration.clientPrivateKeyFile  = halfSupplied.second;

            EXPECT_THROW(static_cast<void>(QuicClientConnection{loop, configuration}), Base::InvalidArgumentException)
                    << "证书与私钥只给了一项却被放过：证书=\"" << halfSupplied.first << "\"，私钥=\"" << halfSupplied.second << "\"";
        }
    }

    /**
     * @brief 钉住：证书热重载之后，握手看到的就是新身份，而且新身份确实在正常服务
     * @details 两种身份只差 SAN（一张 IP、一张 DNS），信任锚却同时装着两张：于是「按 IP 连不上、
     *          按 localhost 连得上」这一对读数的唯一解释是服务端出示的证书换了。撤掉
     *          reloadCertificate() 的换指针对调，或者换上去却没读新文件，本条立刻红在一侧。
     */
    TEST(QuicClientConnection, HandshakeSeesTheNewCertificateAfterTheServerReloadsIt)
    {
        AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("QuicCertificateReload");
        const std::filesystem::path                 servedCertificate = temporaryDirectory.path() / "served-cert.pem";
        const std::filesystem::path                 servedPrivateKey  = temporaryDirectory.path() / "served-key.pem";
        const std::filesystem::path                 trustAnchors      = temporaryDirectory.path() / "anchors.pem";

        // 第一份身份：只认 127.0.0.1 那张。路径必须是本用例自己的复制件——夹具是共享的，不能覆写
        copyFixtureFile(kIpCertificatePath, servedCertificate);
        copyFixtureFile(kIpPrivateKeyPath, servedPrivateKey);
        writeCombinedTrustAnchors(trustAnchors, {kIpCertificatePath, fixturePath("test_localhost_cert.pem")});

        RunningServerPeer server{servedCertificate, servedPrivateKey};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";
        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", trustAnchors)) << "换之前连不上，本条用例的基线就不成立";

        // 证书自动化的标准动作：新证书覆盖到同一条路径，然后叫服务重载
        copyFixtureFile(fixturePath("test_localhost_cert.pem"), servedCertificate);
        copyFixtureFile(fixturePath("test_localhost_key.pem"), servedPrivateKey);
        ASSERT_TRUE(server.reloadCertificate()) << "重载报了失败，可路径上明明是一张合法的证书";

        EXPECT_FALSE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", trustAnchors)) << "旧身份还在服务：热重载没换上新证书";
        EXPECT_TRUE(handshakeSucceeded(server.listeningPort(), "localhost", trustAnchors)) << "新身份连不上：换是换了，却没在正常服务";
    }

    /**
     * @brief 钉住：重载失败时旧证书原样继续服务，而不是先把线上身份拆了再报错
     */
    TEST(QuicClientConnection, KeepsServingTheOldCertificateWhenReloadFails)
    {
        AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory("QuicCertificateReloadFailure");
        const std::filesystem::path                 servedCertificate = temporaryDirectory.path() / "served-cert.pem";
        const std::filesystem::path                 servedPrivateKey  = temporaryDirectory.path() / "served-key.pem";
        const std::filesystem::path                 trustAnchors      = temporaryDirectory.path() / "anchors.pem";

        copyFixtureFile(kIpCertificatePath, servedCertificate);
        copyFixtureFile(kIpPrivateKeyPath, servedPrivateKey);
        writeCombinedTrustAnchors(trustAnchors, {kIpCertificatePath, fixturePath("test_localhost_cert.pem")});

        RunningServerPeer server{servedCertificate, servedPrivateKey};
        ASSERT_NE(server.listeningPort(), 0U);
        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", trustAnchors));

        // 新证书坏了（续期最常见的现场：下载到一半断电，磁盘上是半个文件）
        writeTextFile(servedCertificate, "这不是 PEM，只是半份被截断的证书\n");
        EXPECT_FALSE(server.reloadCertificate()) << "读不出证书却报了成功";
        EXPECT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", trustAnchors)) << "重载失败就把线上身份弄没了：旧上下文该原样继续服务";
    }
} // namespace AsynGyanis::Net
