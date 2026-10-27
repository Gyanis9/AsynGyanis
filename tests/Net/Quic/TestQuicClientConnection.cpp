// QuicClientConnection 用例：与自家服务端做完握手并定下 ALPN、名字不符的证书要被拒、
// 黑洞地址按时限收场、主机名为空在构造期就拒

#include "Net/Quic/QuicClientConnection.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Routing/Router.h"
#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"
#include "Net/Quic/QuicAddressValidationToken.h"
#include "Net/Quic/QuicPacketBuilder.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/Socket.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"
#include "HttpTestSupport.h"
#include "NetTestSupport.h"
#include "QuicRfcVectors.h"

#include "CommonTestSupport.h"
#include "ExpectedAssertions.h"
#include "AssertionSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;

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
             * @param holdOpenAfterHandshake true 表示握手交出结果后把这条连接一直挂着，直到调用方
             *        `releaseHold()`；默认拿到结果就关
             * @details 挂着是为了让「名额已满」由用例构造出来：默认的尝试在交出结果时就把客户端关了，
             *          服务端那一条随之被收干净，占着名额的判据于是变成调度运气
             */
            ConnectAttempt(QuicClientConnection::Configuration configuration, const Core::InetAddress &serverAddress, const bool holdOpenAfterHandshake = false) :
                m_configuration(std::move(configuration)), m_serverAddress(serverAddress), m_isHeld(holdOpenAfterHandshake)
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

            /**
             * @brief 等客户端自己判出已收口（空闲超时或对端收口）
             * @param timeout 等待上限
             * @return true 时限内 `isClosed()` 已为真
             * @note 读数来自 run() 在挂起循环里镜像的那一格；没传 `holdOpenAfterHandshake` 的尝试
             *       不会更新它，调用方只应对挂着的那一档用本方法
             */
            [[nodiscard]] bool waitUntilClientClosed(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isClientClosed.load(std::memory_order_acquire); }, timeout);
            }

            /**
             * @brief 交还这条连接占着的名额，让它在自己的循环线程上关闭
             * @details 只在构造时传了 `holdOpenAfterHandshake` 的尝试有意义；没挂着的调用它是空操作
             */
            void releaseHold() noexcept
            {
                m_isHeld.store(false, std::memory_order_release);
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
                // 结果先发布，再进入可能的挂起：调用方等的是结果，不是「连接已关」
                m_isFinished.store(true, std::memory_order_release);
                // 挂着这段时间只为占住名额。每 50 毫秒醒一次：既问放行没有，也把连接自身的收口状态
                // 镜像给测试线程（空闲超时的判据由 waitUntilClientClosed 读它）。循环停了这些等待
                // 不会自己触发（Timer 的约定），而析构里的 stop 就是没人 release 时那条连接的最后出口
                Core::Timer ticker{m_loop};
                while (m_isHeld.load(std::memory_order_acquire))
                {
                    m_isClientClosed.store(client->isClosed(), std::memory_order_relaxed);
                    co_await ticker.waitFor(std::chrono::milliseconds{50});
                }
                // 套接字要在循环线程上关：交给测试线程的帧销毁时再做就跨线程了
                client.reset();
                co_return;
            }

            Core::EventLoop                     m_loop;                            ///< 客户端所属循环
            QuicClientConnection::Configuration m_configuration;                   ///< 本次尝试用的配置
            Core::InetAddress                   m_serverAddress;                   ///< 目标地址
            std::optional<Core::Task<>>         m_task{};                          ///< 尝试协程
            std::thread                         m_loopThread{};                    ///< 跑循环的线程
            std::atomic<bool>                   m_isFinished{false};               ///< 结果已就位
            std::atomic<bool>                   m_isSuccessful{false};             ///< 握手是否完成
            std::atomic<bool>                   m_isHeld{false};                   ///< 结果交出后仍把这条连接挂着（占名额用），由 releaseHold 结束
            std::atomic<bool>                   m_isClientClosed{false};           ///< 挂起循环里镜像的 client->isClosed()
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
         * @details 信任对两种身份都成立，才能保证「连不上」的唯一解释是身份本身变了，
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

        /**
         * @brief 握一次手并把它挂着，交回持有者——按来源限额的名额要靠活连接占住
         * @details 返回的是持有者而不是布尔：调用方要的是「这条连接在结果交出之后仍然活着」这段时期，
         *          用完自己 releaseHold()。握手成没成由调用方按服务端在线数判，那才是名额是否真被占住的读数
         * @param port 服务端端口
         * @return std::unique_ptr<ConnectAttempt> 挂着的握手持有者
         */
        [[nodiscard]] std::unique_ptr<ConnectAttempt> holdHandshakeOpen(const std::uint16_t port)
        {
            auto attempt = std::make_unique<ConnectAttempt>(makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{4000}),
                                                            Core::InetAddress::resolve("127.0.0.1", port).value(), true);
            static_cast<void>(attempt->awaitFinished(kWaitTimeout));
            return attempt;
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
        /**
         * @brief 手拼一条补齐到 1200 字节的客户端 Initial
         * @details 与最小可解析形状的区别只在长度：小于 §14.1 门槛的 Initial 会被服务端入口整条丢掉；
         *          需要走进正常流程的夹具用本函数。Length 域按两字节档写，载荷是纯垃圾——判据只到
         *          「头部读得懂、令牌验得过」为止。
         * @param destinationConnectionId 目的标识（本端在此识别连接）
         * @param sourceConnectionId 本端自报的源标识
         * @param token 令牌字段，空即「还没验过地址」
         * @return std::vector<std::uint8_t> 一条总长恰为 1200 字节的 Initial
         */
        std::vector<std::uint8_t> makePaddedClientInitial(const std::vector<std::uint8_t> &destinationConnectionId, const std::vector<std::uint8_t> &sourceConnectionId,
                                                          const std::vector<std::uint8_t> &token)
        {
            std::string packet{static_cast<char>(0xC0U), static_cast<char>(0x00U), static_cast<char>(0x00U), static_cast<char>(0x00U), static_cast<char>(0x01U)};
            packet.push_back(static_cast<char>(destinationConnectionId.size()));
            packet.append(reinterpret_cast<const char *>(destinationConnectionId.data()), destinationConnectionId.size());
            packet.push_back(static_cast<char>(sourceConnectionId.size()));
            packet.append(reinterpret_cast<const char *>(sourceConnectionId.data()), sourceConnectionId.size());
            packet.push_back(static_cast<char>(token.size()));
            packet.append(reinterpret_cast<const char *>(token.data()), token.size());

            // Length 覆盖包号与载荷。前缀约二十余字节，取值一定落在两字节档（64..16383）
            constexpr std::size_t kTargetDatagramByteLength = 1200;
            constexpr std::size_t kLengthFieldByteCount     = 2;
            const std::size_t     lengthValue               = kTargetDatagramByteLength - packet.size() - kLengthFieldByteCount;
            EXPECT_GE(lengthValue, 64U) << "令牌太大，Length 落不进两字节档；这份夹具需要另选编码";
            packet.push_back(static_cast<char>(0x40U | ((lengthValue >> 8U) & 0x3FU)));
            packet.push_back(static_cast<char>(lengthValue & 0xFFU));
            packet.resize(kTargetDatagramByteLength, '\0');
            EXPECT_EQ(packet.size(), kTargetDatagramByteLength);
            return {packet.begin(), packet.end()};
        }

        /**
         * @brief 把报文头里的一条连接标识/令牌视图拷成可比对的容器
         * @param bytes 指向数据报的视图
         * @return std::vector<std::uint8_t> 视图内容的副本
         */
        std::vector<std::uint8_t> copyOf(const std::span<const std::uint8_t> bytes)
        {
            return {bytes.begin(), bytes.end()};
        }

        /**
         * @brief 读一个进程级 QUIC 计数
         * @param name 指标全名
         * @return std::uint64_t 当前读数；没有这条登记时为 0
         * @details 服务端在构造时登记、析构时注销，用例在它活着的时候读到的就是这台服务端的计数
         */
        std::uint64_t processMetricValue(const std::string_view name)
        {
            for (const Core::ProcessMetricSample &sample: Core::ProcessMetricsRegistry::samples())
            {
                if (sample.name == name)
                {
                    return sample.value;
                }
            }
            return 0;
        }

        /**
         * @brief 在一个裸 UDP 套接字上等一条报文
         * @param socket 收包套接字
         * @param timeout 等待上限
         * @param from 出参：来源地址
         * @return std::optional<std::vector<std::uint8_t>> 收到的字节；超时为空
         */
        std::optional<std::vector<std::uint8_t>> receiveDatagram(const Platform::DatagramSocket &socket, const std::chrono::milliseconds timeout, Platform::SocketAddress &from)
        {
            std::vector<std::uint8_t> buffer(Platform::DatagramSocket::kMaximumDatagramBytes);
            std::size_t               byteCount = 0;
            const bool                received  = waitForCondition(
                    [&]
                    {
                        const ssize_t length = socket.receive(buffer.data(), buffer.size(), from);
                        if (length <= 0)
                        {
                            return false;
                        }
                        byteCount = static_cast<std::size_t>(length);
                        return true;
                    },
                    timeout);
            if (!received)
            {
                return std::nullopt;
            }
            buffer.resize(byteCount);
            return buffer;
        }

        /**
         * @brief 解一条客户端长头报文
         * @param datagram 数据报
         * @param header 出参：解出的明文头
         * @return true 解出了长头
         */
        bool decodeLongHeader(const std::span<const std::uint8_t> datagram, QuicPacketHeader &header)
        {
            const auto decoded = decodeQuicPacketHeader(datagram, QuicConnection::kSourceConnectionIdLength);
            if (!decoded.has_value() || !decoded->isLongHeader)
            {
                return false;
            }
            header = *decoded;
            return true;
        }

        /**
         * @brief 造一条目的标识由调用方指定的 Retry
         * @param clientInitial 解出来的客户端 Initial，只取其中的原始目的标识拼完整性伪包
         * @param destinationConnectionId 线上目的标识；正常应为客户端 Initial 的源标识
         * @param retrySourceConnectionId 服务端新挑的源标识
         * @param token 令牌
         * @param isTagValid true＝完整性标签正确；false＝翻转标签最后一字节
         * @return std::vector<std::uint8_t> 完整的 Retry 报文
         */
        std::vector<std::uint8_t> makeRetryPacket(const QuicPacketHeader &clientInitial, const std::span<const std::uint8_t> destinationConnectionId,
                                                  const std::vector<std::uint8_t> &retrySourceConnectionId, const std::vector<std::uint8_t> &token, const bool isTagValid = true)
        {
            const QuicRetryPacket retry{
                    .originalDestinationConnectionId = clientInitial.destinationConnectionId,
                    .destinationConnectionId         = destinationConnectionId,
                    .sourceConnectionId              = std::span<const std::uint8_t>(retrySourceConnectionId),
                    .token                           = std::span<const std::uint8_t>(token),
                    .version                         = kQuicVersion1,
            };
            std::string packet;
            appendQuicRetryPacket(packet, retry);
            std::vector<std::uint8_t> bytes = toUnsignedBytes(packet);
            if (!isTagValid)
            {
                bytes.back() ^= 0x01U;
            }
            return bytes;
        }

        /**
         * @brief 给一条客户端 Initial 造响应它的 Retry
         * @param clientInitial 解出来的客户端 Initial
         * @param retrySourceConnectionId 服务端新挑的源标识
         * @param token 令牌
         * @param isTagValid true＝完整性标签正确；false＝翻转标签最后一字节
         * @return std::vector<std::uint8_t> 完整的 Retry 报文
         */
        std::vector<std::uint8_t> makeRetryForClientInitial(const QuicPacketHeader &clientInitial, const std::vector<std::uint8_t> &retrySourceConnectionId,
                                                            const std::vector<std::uint8_t> &token, const bool isTagValid = true)
        {
            return makeRetryPacket(clientInitial, clientInitial.sourceConnectionId, retrySourceConnectionId, token, isTagValid);
        }

        /**
         * @brief 等一条带非空令牌的客户端 Initial
         * @details 不带令牌的包是客户端在收到 Retry 之前发出的旧 Initial 或其分片，跳过；
         *          返回第一条带令牌的整包字节，调用方按目的标识与令牌判它认了哪一条 Retry。
         * @param socket 裸 UDP 桩
         * @param timeout 等待上限
         * @return std::optional<std::vector<std::uint8_t>> 整包字节；窗口内没等到为空
         */
        std::optional<std::vector<std::uint8_t>> receiveInitialWithToken(const Platform::DatagramSocket &socket, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto                               remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                Platform::SocketAddress                  from{};
                std::optional<std::vector<std::uint8_t>> candidate = receiveDatagram(socket, remaining, from);
                if (!candidate.has_value())
                {
                    return std::nullopt;
                }
                QuicPacketHeader header;
                if (decodeLongHeader(*candidate, header) && !header.token.empty())
                {
                    return candidate;
                }
            }
            return std::nullopt;
        }

        /**
         * @brief 手工拼一条源标识等于原始目的标识的 Retry
         * @details §17.2.5.1 的 MUST NOT，现有组包器拒发这一形态；为验客户端自己的那条比较，
         *          这里按线上字段表拼到令牌末尾，再用 RFC 9001 §5.8 的固定密钥补一个合法标签。
         * @param clientInitial 客户端第一个 Initial，取它的源标识当线上目的标识
         * @param originalDestinationConnectionId 原始目的标识，既当源标识也进完整性伪包
         * @param token 令牌
         * @return std::vector<std::uint8_t> 完整报文
         */
        std::vector<std::uint8_t> makeRetryWithSourceEqualOriginalDestination(const QuicPacketHeader             &clientInitial,
                                                                              const std::span<const std::uint8_t> originalDestinationConnectionId,
                                                                              const std::vector<std::uint8_t>    &token)
        {
            auto        appendBytes = [](std::string &bytes, const std::span<const std::uint8_t> data) { bytes.append(reinterpret_cast<const char *>(data.data()), data.size()); };
            std::string packet;
            packet.push_back(static_cast<char>(0xFFU));
            packet.push_back(0);
            packet.push_back(0);
            packet.push_back(0);
            packet.push_back(1);
            packet.push_back(static_cast<char>(clientInitial.sourceConnectionId.size()));
            appendBytes(packet, clientInitial.sourceConnectionId);
            packet.push_back(static_cast<char>(originalDestinationConnectionId.size()));
            appendBytes(packet, originalDestinationConnectionId);
            appendBytes(packet, std::span<const std::uint8_t>(token));

            // RFC 9001 §5.8 为版本 1 定死的密钥与 IV
            constexpr std::array<std::uint8_t, 16> keyBytes{0xBE, 0x0C, 0x69, 0x0B, 0x9F, 0x66, 0x57, 0x5A, 0x1D, 0x76, 0x6B, 0x54, 0xE3, 0x68, 0xC8, 0x4E};
            constexpr std::array<std::uint8_t, 12> vectorBytes{0x46, 0x15, 0x99, 0xD3, 0x5D, 0x63, 0x2B, 0xF2, 0x23, 0x98, 0x25, 0xBB};
            QuicPacketKeys                         keys{};
            keys.cipherSuite = QuicCipherSuite::Aes128Gcm;
            std::ranges::copy(keyBytes, keys.encryptionKey.begin());
            std::ranges::copy(vectorBytes, keys.initializationVector.begin());

            std::string pseudoPacket;
            pseudoPacket.push_back(static_cast<char>(originalDestinationConnectionId.size()));
            appendBytes(pseudoPacket, originalDestinationConnectionId);
            pseudoPacket += packet;
            appendQuicProtectedPayload(packet, keys, 0, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pseudoPacket.data()), pseudoPacket.size()), {});
            return toUnsignedBytes(packet);
        }

        /**
         * @brief 造一条只带 PING 的服务端 Initial
         * @details 用客户端第一个 Initial 的目的标识推服务端→客户端的 Initial 密钥，两条标识按
         *          服务端方向填：目的为客户端自报的源标识，源为本端这条新标识。
         * @param clientInitial 解出来的客户端 Initial
         * @param serverConnectionId 服务端这条 Initial 的源标识
         * @param packetNumber 包号（一字节档）
         * @return std::vector<std::uint8_t> 完整数据报
         */
        std::vector<std::uint8_t> makeServerInitialDatagram(const QuicPacketHeader &clientInitial, const std::vector<std::uint8_t> &serverConnectionId,
                                                            const std::uint64_t packetNumber)
        {
            std::string frames;
            appendQuicFrame(frames, QuicFrame{QuicPingFrame{}});
            const std::vector<std::uint8_t> frameBytes = toUnsignedBytes(frames);
            QuicOutboundPacket              packet;
            packet.isLongHeader            = true;
            packet.longPacketType          = QuicLongPacketType::Initial;
            packet.destinationConnectionId = clientInitial.sourceConnectionId;
            packet.sourceConnectionId      = std::span<const std::uint8_t>(serverConnectionId);
            packet.packetNumber            = packetNumber;
            packet.packetNumberByteCount   = 1;
            packet.frames                  = std::span<const std::uint8_t>(frameBytes);
            const QuicPacketKeys keys      = deriveQuicInitialPacketKeys(clientInitial.destinationConnectionId, QuicPacketDirection::ServerToClient);
            std::string          datagram;
            appendQuicPacket(datagram, packet, keys);
            return toUnsignedBytes(datagram);
        }

        /**
         * @brief 按客户端 Initial 密钥去掉头部保护，读出完整包号
         * @param datagram 客户端发出的数据报
         * @param keyConnectionId 推导 Initial 密钥用的目的标识（首包＝原始目的标识，Retry 后＝Retry 源标识）
         * @return std::optional<std::uint64_t> 包号；任一步解不开为空
         */
        std::optional<std::uint64_t> clientInitialPacketNumber(const std::span<const std::uint8_t> datagram, const std::span<const std::uint8_t> keyConnectionId)
        {
            const auto decoded = decodeQuicPacketHeader(datagram, QuicConnection::kSourceConnectionIdLength);
            if (!decoded.has_value())
            {
                return std::nullopt;
            }
            const auto sample = extractQuicHeaderProtectionSample(datagram, *decoded);
            if (!sample.has_value())
            {
                return std::nullopt;
            }
            const QuicPacketKeys           keys = deriveQuicInitialPacketKeys(keyConnectionId, QuicPacketDirection::ClientToServer);
            const QuicHeaderProtectionMask mask = generateQuicHeaderProtectionMask(keys, *sample);
            std::vector<std::uint8_t>      mutablePacket(datagram.begin(), datagram.end());
            QuicPacketHeader               header            = *decoded;
            const auto                     unmaskedFirstByte = removeQuicHeaderProtection(mutablePacket, header, mask);
            if (!unmaskedFirstByte.has_value() || !refreshQuicPacketHeader(header, *unmaskedFirstByte, mutablePacket).has_value())
            {
                return std::nullopt;
            }
            return header.packetNumber;
        }

        /// 地址验证用例的固定密钥：服务端与用例里的核验器共用同一把，才能就地判「回包里那张令牌验得过」
        const std::vector<std::uint8_t> kAddressValidationTestKey(32U, 0x5AU);
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
     * @brief 客户端自己落实空闲超时：没有任何入站报文也必须收口
     * @details 服务端空闲时限留 30 秒（不指望它替本端收口），客户端配 300 毫秒，握手后两边都不再发
     *          东西。此时 `pumpOnce()` 挂在收包上永远等不到报文：若客户端没有自己的到期驱动，
     *          `isClosed()` 就永远为假，本条在两秒后会因 `waitUntilClientClosed` 超时而失败。
     *          连接一直挂着（holdOpenAfterHandshake=true），判据读的是连接自身的状态，而不是持有者
     *          提前把它关掉了；收口之后再交还名额。
     */
    TEST(QuicClientConnection, ClosesAnIdleConnectionWithoutInboundTraffic)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        auto configuration        = makeConfiguration("127.0.0.1", kIpCertificatePath, 4s);
        configuration.idleTimeout = std::chrono::milliseconds{300};

        ConnectAttempt attempt{configuration, Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value(), true};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "握手既没成也没失败，挂在那里";
        ASSERT_TRUE(attempt.isSuccessful()) << "握手没完成，空闲超时无从谈起：" << attempt.elapsed().count() << " 毫秒后收场";

        EXPECT_TRUE(attempt.waitUntilClientClosed(2s)) << "握手后没有任何入站报文时客户端一直没判空闲超时：客户端侧的到期驱动缺失，PTO 与空闲超时都不会到点";

        attempt.releaseHold();
    }

    /**
     * @brief 名额归因：未完成握手的连接与已验证连接是两个可分的数
     * @details 两半：① 真握完一条之后，在线数 ≥1 而「未验证」归零——两个读数按验证状态分开，
     *          这就是归因；② 只发 Initial、握手谈不成的流量（RFC 9001 附录 A.2 的整包，ALPN 不是
     *          h3）不留下僵尸条目：服务端拒绝它，等待之后两数复原。两类连接共用同一份
     *          maximumConnections 是刻意的（理由见 QuicConnection.h 的清单），本用例钉的是归因那一半。
     *          ③ 半程握手（Initial 发出后不再有任何后续）让「未验证」出现真正大于 0 的时刻——
     *          把重算块整段删掉，本用例必须在③第一个断言上转红。
     */
    TEST(QuicServer, AttributesHandshakingConnectionsSeparatelyFromEstablishedOnes)
    {
        // 空闲时限收到 3 秒：③里半程握手的连接靠它收口，收口之后两数回零也靠它兑现
        // 地址验证显式关掉：本用例测的是握手状态归因，不测地址验证——默认开着时半程 Initial 只会换回
        // 一条 Retry、连接根本不进表，③那一格无从成立
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [](QuicServer::Configuration &configuration)
                                 {
                                     configuration.idleTimeout                    = std::chrono::seconds{3};
                                     configuration.addressValidationTokenLifetime = std::chrono::seconds{0};
                                 }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";
        EXPECT_EQ(server.server().connectionCount(), 0U);
        EXPECT_EQ(server.server().handshakingConnectionCount(), 0U);

        ASSERT_TRUE(handshakeSucceeded(server.listeningPort(), "127.0.0.1", kIpCertificatePath)) << "基线握手没成";
        // ① 握手完成后：在线 ≥1、「未验证」归零——两个读数按状态分开。
        // 归因镜像由定时循环按拍重算，而空闲起步的第一拍要等满 kIdleTickerSleep（1 秒，见 QuicServer.h）才落地：
        // 固定睡满那一拍再加余量，让本条的断言读的是重算之后的值。只等「条件成立」不行——镜像初值就是 0，
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

        // ③ 半程握手：连接进表、挂在「未验证」名下——归因镜像到底有没有在算，只有这一刻看得见。
        //    半程客户端 = 只把手泵到 Initial 发出就停：服务端收下 ClientHello、在等对端后续，而这一位
        //    永远不来。此前本用例没有任何读数必须大于 0 的时刻，把重算块整段删掉也全绿
        {
            Core::EventLoop      halfClientLoop;
            QuicClientConnection halfClient(halfClientLoop, makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{4000}));
            Core::Task<bool>     halfAttempt = halfClient.connect(Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value());
            halfAttempt.handle().resume(); // 只推进到 Initial 发出；此后不再泵这具循环，握手上不去

            ASSERT_TRUE(waitForCondition([&] { return server.server().handshakingConnectionCount() >= 1U; }, kWaitTimeout))
                    << "半程握手的连接没被算进「未验证」：归因镜像没在按拍重算";
            EXPECT_LE(server.server().handshakingConnectionCount(), server.server().connectionCount()) << "「未验证」超过了在线总数";
            EXPECT_GE(server.server().connectionCount(), onlineAfterHandshake + 1U) << "半程握手没有让在线数上涨";
        } // 半程客户端退场：套接字关了，服务端那条连接等空闲时限收口

        // 空闲时限过后两数都要回零，且「未验证」的归零不得滞后于在线数。判据分两层：在线数照常等到零；
        // 「未验证」则给 100ms 宽限（空闲拍长 1s 的十分之一）后当场核对——重算排在摘除之后，
        // 最后一条离表当拍就归零。重算排在拍首（先于摘除）的实现在这 100ms 里读到的是上一拍的陈旧值 1
        // （要等下一拍、最坏 1s 后才清），本断言因此转红；「等到条件成立为止」的写法会白给它一秒追上来的机会
        ASSERT_TRUE(waitForCondition([&] { return server.server().connectionCount() == 0U; }, kWaitTimeout)) << "半程连接没有被空闲时限收干净";
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        EXPECT_EQ(server.server().handshakingConnectionCount(), 0U) << "在线数已归零 100ms 后「未验证」仍带着陈旧值：归因重算排在摘除之前，最后一条离表的那一拍没跟上";
    }

    /**
     * @brief 钉住：单来源名额拒绝的告警被时间窗压住，不是每个被拒的 Initial 一条
     * @details 被拒的连接不进别名索引，对端的握手重传会以新 Initial 反复走到这个判定点——不压重条时
     *          这是一条报文一条日志的远端可驱动刷屏面。与 PROXY 组同一形状：量级进那一条放行的
     *          日志（同窗压掉了多少条），重复条目才被丢掉；撤掉闸门时本用例的「恰一条」当场红。
     */
    TEST(QuicServer, ThrottlesRepeatedPerIpRejectionWarnings)
    {
        const HttpTestSupport::LogCapture logCapture; // 先挂再开服：日志走根日志器，晚一步就漏掉前面几条

        const std::shared_ptr<PerIpConnectionLimiter> limiter = std::make_shared<PerIpConnectionLimiter>(2);
        // 地址验证显式关掉：本用例测单来源名额拒绝的告警节流，不测地址验证——默认开着时被拒的
        // Initial 先换回一条 Retry，根本走不到名额判定，告警自然不会出现
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [&limiter](QuicServer::Configuration &configuration)
                                 {
                                     configuration.perIpConnectionLimiter         = limiter;
                                     configuration.addressValidationTokenLifetime = std::chrono::seconds{0};
                                 }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        // 先把手上的两个名额占满：两条完成握手的连接都从 127.0.0.1 来，同一份单来源账。
        // 这两条要一直挂着到发完被拒的报文为止：结果一交出就把客户端关掉的话，服务端那两条随之被收干净，
        // 「名额已满」就变成调度运气——本机空载、或五条报文挤在同一次派发里时恰好还满，满载与 CI 并行时
        // 就整批走到空名额上（一条告警都不留），实测 Windows 与本机各中过一次
        std::vector<std::unique_ptr<ConnectAttempt>> heldHandshakes;
        heldHandshakes.push_back(holdHandshakeOpen(server.listeningPort()));
        heldHandshakes.push_back(holdHandshakeOpen(server.listeningPort()));
        ASSERT_TRUE(waitForCondition([&] { return server.server().connectionCount() >= 2U; }, kWaitTimeout)) << "两条挂着的握手没让服务端记上在线连接：名额没被真占住";

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
        // 名额用完就交还：让这两条在自己的循环线程上关掉，而不是留到持有者析构时由测试线程动手
        for (const auto &held: heldHandshakes)
        {
            held->releaseHold();
        }
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

        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([&] { static_cast<void>(QuicClientConnection{loop, configuration}); }, "主机名为空", "空主机名");
    }

    /**
     * @brief 把服务端配成「要求客户端证书却没给信任锚」时，构造当场抛而不是留一条永远握不成的监听器
     * @details 这种配置在 TLS 里的形态不是「退化成不校验」而是每条握手都失败（没有可对照的根，
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

            AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([&] { static_cast<void>(QuicClientConnection{loop, configuration}); }, "必须同时给或同时不给",
                                                                                   "半套客户端身份：证书=\"" + halfSupplied.first + "\"，私钥=\"" + halfSupplied.second + "\"");
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

    /**
     * @brief 钉住「版本不合就礼貌地拒」：未知版本的长头会收到一条版本协商包，而不是被静默丢掉
     * @details 自家客户端只会发 v1，够不着这条判据，因此用裸 UDP 手拼一条别的版本：字段按 RFC 9000
     *          §17.2.1 那张图摆、也按那张图读回，全程不碰被测的编解码（两侧同源就查不出「都理解错」）。
     *          判据三件：回的是版本协商包（版本字段恒 0）、两条标识按规范互换（回包 DCID＝我发的 SCID、
     *          回包 SCID＝我发的 DCID）、末尾列着本端支持的 0x00000001。
     *          反向那一半（v1 报文不该收到这种东西）在这里只算观察：无应答时它是空的，真正钉住它的是
     *          编解码层那条 OnlyReadsVersionNegotiationRequestsForUnsupportedVersions。
     */
    TEST(QuicServer, AnswersUnsupportedVersionWithVersionNegotiationPacket)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress serverAddress("127.0.0.1", server.listeningPort());

        // 一条「本端不支持的版本」的长头：版本填 0x00000002（将来登记的下一版），DCID 8 字节、SCID 16 字节
        std::vector<std::uint8_t> unknownVersionProbe{0xC0, 0x00, 0x00, 0x00, 0x02};
        unknownVersionProbe.push_back(0x08);
        for (std::uint8_t index = 0; index < 8; ++index)
        {
            unknownVersionProbe.push_back(static_cast<std::uint8_t>(0xD0U + index));
        }
        unknownVersionProbe.push_back(0x10);
        for (std::uint8_t index = 0; index < 16; ++index)
        {
            unknownVersionProbe.push_back(static_cast<std::uint8_t>(0x50U + index));
        }
        ASSERT_GT(sender.send(serverAddress.platformAddress(), unknownVersionProbe.data(), unknownVersionProbe.size()), 0);

        // 非阻塞套接字按拍轮询：等的是「有没有回话」这件事，不是某个绝对时长
        std::vector<std::uint8_t> reply(1200);
        std::size_t               replyLength = 0;
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
                kWaitTimeout))
                << "版本不合的报文什么都没收到：静默丢等于让对端干等到它自己的超时，公网端口上这就是「在监听却不应答」";

        const std::size_t minimumPacketLength = 5U + 1U + 8U + 1U + 16U + 4U;
        ASSERT_GE(replyLength, minimumPacketLength) << "回包短于版本协商包应有的最小形状";
        EXPECT_NE(reply[0] & 0x80U, 0U) << "回包不是长头";
        const std::uint32_t replyVersion = static_cast<std::uint32_t>((static_cast<std::uint32_t>(reply[1]) << 24U) | (static_cast<std::uint32_t>(reply[2]) << 16U) |
                                                                      (static_cast<std::uint32_t>(reply[3]) << 8U) | static_cast<std::uint32_t>(reply[4]));
        EXPECT_EQ(replyVersion, 0U) << "版本协商包的版本字段必须恒 0（RFC 9000 §17.2.1）";

        // 两条标识的互换：回包的目的标识是我发出去的源标识，回包的源标识是我发出去的目的标识
        std::size_t offset = 5;
        EXPECT_EQ(reply[offset], 0x10U) << "回包的目的标识长度该等于我发出的源标识长度";
        ++offset;
        for (std::uint8_t index = 0; index < 16; ++index)
        {
            ASSERT_LT(offset, replyLength);
            EXPECT_EQ(reply[offset], static_cast<std::uint8_t>(0x50U + index)) << "回包目的标识的第 " << static_cast<unsigned int>(index) << " 字节没照抄我发的源标识";
            ++offset;
        }
        EXPECT_EQ(reply[offset], 0x08U) << "回包的源标识长度该等于我发出的目的标识长度";
        ++offset;
        for (std::uint8_t index = 0; index < 8; ++index)
        {
            ASSERT_LT(offset, replyLength);
            EXPECT_EQ(reply[offset], static_cast<std::uint8_t>(0xD0U + index)) << "回包源标识的第 " << static_cast<unsigned int>(index) << " 字节没照抄我发的目的标识";
            ++offset;
        }

        // 末尾是一列 32 位支持的版本：本实现只支持 v1，因此恰好一条、且就是 0x00000001
        ASSERT_EQ(replyLength - offset, 4U) << "支持的版本列表该只有一条 v1（多一条就是本端根本没支持那版本）";
        const std::uint32_t advertisedVersion =
                static_cast<std::uint32_t>((static_cast<std::uint32_t>(reply[offset]) << 24U) | (static_cast<std::uint32_t>(reply[offset + 1U]) << 16U) |
                                           (static_cast<std::uint32_t>(reply[offset + 2U]) << 8U) | static_cast<std::uint32_t>(reply[offset + 3U]));
        EXPECT_EQ(advertisedVersion, 0x00000001U) << "列表里列的不是 v1，客户端按列表去连就连到别处了";
    }

    /**
     * @brief 钉住地址验证：不带令牌的第一包 Initial 先吃一条 Retry，把令牌带回来才建连接
     * @details 这一格要判的是公网 UDP 端口上的成本账：一条 Initial 就能让本端建一份 TLS 会话、
     *          一张流表与一套拥塞状态，而发的人不必是自己。判据分三段，每段各钉一种失败形状：
     *          ① 没令牌→回 Retry 且不建任何状态（少了这条，Retry 就成了白加一轮握手）；
     *          ② 带真令牌回来→不再回 Retry（§8.1.2 禁止第二轮）且这条连接被受理；受理这一步读服务端写的
     *          那条日志而不是在线数——这一包的载荷注定解不开，本端会把它连同连接一起收口，在线数只在
     *          同一批里存在一瞬，拿它判就是赌调度；
     *          ③ 令牌被改过一个比特→不回 Retry 也不建连接（§8.1.2：此时只能拒或放行），且这一格必须
     *          出声——运维靠那条记录分辨「洪水带着自造的令牌」与「密钥不合」。
     *          回包按 RFC 9000 §17.2.5 那张图手工解析，不用自家解码器——两侧同源就查不出「都理解错」；
     *          两格标识的换位（线上目的标识＝本端自报的源标识，线上源标识＝本端新挑的那一条）也在这一格
     *          逐字节钉住，那是对端认领这条 Retry 的唯一凭据。
     */
    TEST(QuicServer, DemandsAddressValidationBeforeAcceptingANewConnection)
    {
        const HttpTestSupport::LogCapture logCapture; // 服务端按档位出声，读它才知道被拒的是哪一格
        constexpr std::uint8_t            kProbeConnectionIdByte = 0xD0U;
        /// Retry 末尾的完整性标签长度（RFC 9001 §5.8 定死 128 位）。本用例手工解析回包，因此按规范
        /// 的字面值取，不引实现侧的同名常量——同源就把「都算错」这种情况钉不出来
        constexpr std::size_t kRetryIntegrityTagByteLength = 16U;

        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [](QuicServer::Configuration &configuration)
                                 {
                                     configuration.addressValidationTokenLifetime = std::chrono::seconds{30};
                                     configuration.addressValidationKey           = std::vector<std::uint8_t>(32, 0x5AU);
                                 }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress serverAddress("127.0.0.1", server.listeningPort());

        const std::vector<std::uint8_t> clientDestinationConnectionId(8U, kProbeConnectionIdByte);
        // 客户端自报的源标识取「逐字节递增」而不是全同：这样既能把「本端把目的与源两格填反」查出来，
        // 也能把「抄回去但顺序错了」查出来。§17.2.5.1 要求 Retry 的线上目的标识原样回显这一格
        std::vector<std::uint8_t> clientSourceConnectionId;
        for (std::uint8_t index = 0; index < 8U; ++index)
        {
            clientSourceConnectionId.push_back(static_cast<std::uint8_t>(0x50U + index));
        }
        const std::vector<std::uint8_t> firstInitial = makePaddedClientInitial(clientDestinationConnectionId, clientSourceConnectionId, {});

        // ① 不带令牌
        ASSERT_GT(sender.send(serverAddress.platformAddress(), firstInitial.data(), firstInitial.size()), 0);
        std::vector<std::uint8_t> retryPacket(1200U);
        std::size_t               retryLength = 0;
        ASSERT_TRUE(waitForCondition(
                [&]
                {
                    Platform::SocketAddress retryFrom{};
                    const ssize_t           received = sender.receive(retryPacket.data(), retryPacket.size(), retryFrom);
                    if (received <= 0)
                    {
                        return false;
                    }
                    retryLength = static_cast<std::size_t>(received);
                    return true;
                },
                kWaitTimeout))
                << "没带令牌的 Initial 什么都没收到：本端直接开始握手，等于把成本账留给对端";

        ASSERT_GE(retryLength, 5U + 1U + clientSourceConnectionId.size() + 1U + QuicConnection::kSourceConnectionIdLength + 1U + kRetryIntegrityTagByteLength)
                << "回包短于一条 Retry 应有的最小形状（§17.2.5 的字段表：版本 4 字节加长头固定段 1 字节，两条标识各带一个长度字节，令牌至少 1 字节，末尾 16 字节标签）";
        EXPECT_NE(retryPacket[0] & 0x80U, 0U) << "Retry 不是长头";
        EXPECT_EQ((retryPacket[0] >> 4U) & 0x03U, 3U) << "长头类型位不是 3（Retry）：对端按自己认得的形态解这条";
        EXPECT_EQ(static_cast<std::uint32_t>((static_cast<std::uint32_t>(retryPacket[1]) << 24U) | (static_cast<std::uint32_t>(retryPacket[2]) << 16U) |
                                             (static_cast<std::uint32_t>(retryPacket[3]) << 8U) | static_cast<std::uint32_t>(retryPacket[4])),
                  0x00000001U)
                << "Retry 里的版本要照抄收到那一包";
        std::size_t offset = 5U;
        EXPECT_EQ(retryPacket[offset], static_cast<std::uint8_t>(clientSourceConnectionId.size())) << "回包的目的标识长度该等于我发出的源标识长度";
        ++offset;
        for (const std::uint8_t sourceConnectionIdByte: clientSourceConnectionId)
        {
            ASSERT_LT(offset, retryLength) << "回包在目的标识中途结束：这条 Retry 对端解不完";
            EXPECT_EQ(retryPacket[offset], sourceConnectionIdByte) << "线上目的标识没有原样回显本端自报的源标识（§17.2.5.1）：对端按那一格认领回包，填错就把它当无主报文丢掉";
            ++offset;
        }
        EXPECT_EQ(retryPacket[offset], static_cast<std::uint8_t>(QuicConnection::kSourceConnectionIdLength))
                << "回包的源标识长度必须等于本端短头定长：短头线上没有长度字段，换了长度后续报文就再也路由不回来";
        ++offset;
        std::vector<std::uint8_t> retrySourceConnectionId(retryPacket.begin() + static_cast<std::ptrdiff_t>(offset),
                                                          retryPacket.begin() + static_cast<std::ptrdiff_t>(offset + QuicConnection::kSourceConnectionIdLength));
        offset += retrySourceConnectionId.size();
        const std::size_t tokenByteCount = retryLength - offset - kRetryIntegrityTagByteLength;
        ASSERT_GT(tokenByteCount, 0U) << "Retry 带了零长令牌：客户端必须丢这种报文（§17.2.5.2），等于这项保护没开";
        std::vector<std::uint8_t> addressValidationToken(retryPacket.begin() + static_cast<std::ptrdiff_t>(offset),
                                                         retryPacket.begin() + static_cast<std::ptrdiff_t>(offset + tokenByteCount));
        EXPECT_EQ(server.server().connectionCount(), 0U) << "回了一条 Retry 却已经把连接建起来了：这一项保护的成本没省下来";

        // 这张令牌要能被「同一把配置密钥」验过——否则后面那步「带令牌重来」根本不该放行。放在这里判一次，
        // 失败时点名的是令牌本身而不是「连接没建起来」那条远端后果
        const QuicAddressValidationTokenCodec verifier(std::span<const std::uint8_t>(kAddressValidationTestKey), std::chrono::seconds{30});
        const auto locallyVerified = verifier.verify(std::span<const std::uint8_t>(addressValidationToken), sender.localAddress(), std::chrono::system_clock::now());
        ASSERT_HAS_VALUE(locallyVerified) << "回包里那张令牌自家验不过，状态是 " << static_cast<int>(locallyVerified.error());
        EXPECT_EQ(locallyVerified->originalDestinationConnectionId.size(), 8U) << "令牌恢复的原始目的标识长度不对";

        // ② 带着这张令牌重来，目的标识换成本端给出去的那一条
        const std::vector<std::uint8_t> retriedInitial = makePaddedClientInitial(retrySourceConnectionId, clientSourceConnectionId, addressValidationToken);
        ASSERT_GT(sender.send(serverAddress.platformAddress(), retriedInitial.data(), retriedInitial.size()), 0);
        // 这一包的载荷是故意的垃圾：本端去掉头部保护后看到保留位非 0，按 §17.3.1 把这条连接判成收口，
        // 它在表里因此只活到同一批的回收那一拍。按在线数判等于赌调度，改读受理那一刻写的日志——accept
        // 成功就无条件写一条，随后的入表与在线数 +1 在同一条直线上，没有别的出口
        EXPECT_GE(logCapture.countContainingWithin("已接受一条连接", kWaitTimeout), 1U) << "令牌验过了却没建连接：这条握手再也走不下去（受理这一步只看令牌，不看载荷）";
        EXPECT_EQ(logCapture.countContaining("地址验证没通过"), 0U) << "带着合法令牌的这一包被判成验不过：§8.1.2 只许可拒伪造的令牌，不许可把真令牌也拒掉";
        // 这一格的回话不止一种：载荷是垃圾，本端受理之后按 §17.3.1 收口，会有一条收口报文回来。
        // 因此判的不是「有没有回话」而是「回来的是不是第二条 Retry」——§8.1.2 明写带了本端签的令牌
        // 就只有拒或放行两条出口，再回一条 Retry 就是把对端按回第一轮
        std::vector<std::uint8_t> secondReply(1200U);
        bool                      gotSecondRetry = false;
        const auto                drainDeadline  = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
        while (std::chrono::steady_clock::now() < drainDeadline && !gotSecondRetry)
        {
            Platform::SocketAddress secondFrom{};
            const ssize_t           received = sender.receive(secondReply.data(), secondReply.size(), secondFrom);
            if (received <= 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            gotSecondRetry = (secondReply[0] & 0x80U) != 0U && ((secondReply[0] >> 4U) & 0x03U) == 3U;
        }
        EXPECT_FALSE(gotSecondRetry) << "对端已经把令牌带回来了，却又收到一条 Retry：§8.1.2 明写这一轮之后只能拒或放行";

        // ③ 令牌被改过一个比特：不回 Retry，也不建连接
        const std::size_t onlineBeforeTamper = server.server().connectionCount();
        // 受理这条记号是单调的：上一段刚受理过一条，这一段的判据因此是「计数不涨」而不是「表是空的」
        const std::size_t         acceptedBeforeTamper = logCapture.countContaining("已接受一条连接");
        std::vector<std::uint8_t> forgedToken          = addressValidationToken;
        // 改的是令牌正文里「原始目的标识」的第一字节（种类 1 + 签发秒 4 + 该标识长度 1，正文从第 6 字节起）。
        // 挑这一格是因为只有正文的 HMAC 拦得住它：种类没动、长度字段没动、地址认证码没动、签发秒也没动，
        // 于是形状检查与地址检查一律照过——换到别的字节上，红就落在「形状不合」或「地址不合」上，
        // 这一格判的就不再是「伪造」
        forgedToken[6] ^= 0x01U;
        const std::vector<std::uint8_t> tamperedInitial =
                makePaddedClientInitial(std::vector<std::uint8_t>(QuicConnection::kSourceConnectionIdLength, 0xB0U), clientSourceConnectionId, forgedToken);
        ASSERT_GT(sender.send(serverAddress.platformAddress(), tamperedInitial.data(), tamperedInitial.size()), 0);
        // 这一格的丢弃必须出声：只有它和计数一起在场，运维才看得出被拦下的这条是「自造的令牌」还是
        // 「别处的令牌搬了来」。那条记录写在服务端处理这条数据报的那一刻，可能晚于本端的 send 返回，
        // 所以按等待读而不是当场读
        EXPECT_GE(logCapture.countContainingWithin("地址验证没通过", kWaitTimeout), 1U) << "验失败的令牌被静默丢弃：这一格与「没收到包」在读数上看不出区别";
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_EQ(server.server().connectionCount(), onlineBeforeTamper) << "改了比特的令牌被当成验过：这道 HMAC 没在拦伪造";
        EXPECT_EQ(logCapture.countContaining("已接受一条连接"), acceptedBeforeTamper) << "改了比特的令牌被当成验过：这道 HMAC 没在拦伪造";
    }
    /**
     * @brief §10.3：目的标识不属于任何在册连接的短头报文换回一条无状态重置（丢状态的最后一招）
     * @details 判据三条，每条钉一种失败形状：
     *          ① 形状：短头（首字节高两位 0b01，§10.3 图 10）、末 16 字节是按那个目的标识派生的
     *             令牌——期望值用 Python 的 hmac 现算（独立实现），拿本仓的派生函数自己给自己出题，
     *             算错了也看不出来；
     *          ② 尺寸：比触发它的报文小一字节（§10.3.3 的 MUST，循环里尺寸一路衰减，本端因此不必维护
     *             防循环的状态），且不小于 21 字节（图 10 的下限）；22 字节的触发报文恰好换回 21 字节的
     *             重置，这一格把下限钉住；
     *          ③ 反面：长头（令牌那时还没发给对端，§10.3 明写静静丢掉同样有效）与 ≤21 字节的触发
     *             报文都不该换回任何东西——少了这两条，一个「见谁打谁」的回包器也能过前两条
     */
    TEST(QuicServer, AnswersUnknownShortHeaderDatagramsWithAStatelessReset)
    {
        constexpr std::uint8_t          kProbeConnectionIdFirstByte = 0xE0U;
        const std::vector<std::uint8_t> resetKey(32, 0x5BU); // 固定密钥：令牌因此是可预算的常量
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [&](QuicServer::Configuration &configuration) { configuration.statelessResetKey = resetKey; }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";

        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress serverAddress("127.0.0.1", server.listeningPort());

        // 一条 100 字节的短头报文：18 字节的「谁都不认识」目的标识 + 其余内容
        std::vector<std::uint8_t> probe(100, 0x11U);
        probe.front() = 0x40U;
        for (std::size_t index = 0; index < 18; ++index)
        {
            probe[1 + index] = static_cast<std::uint8_t>(kProbeConnectionIdFirstByte + index);
        }
        ASSERT_GT(sender.send(serverAddress.platformAddress(), probe.data(), probe.size()), 0);

        std::vector<std::uint8_t> reply(1500);
        std::size_t               replyLength = 0;
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
                kWaitTimeout))
                << "无主的短头报文没换回任何东西：对端认不出「连接没了」，只能等它自己的超时";

        EXPECT_EQ(replyLength, probe.size() - 1U) << "重置该比触发它的报文小一字节（§10.3.3 的 MUST 防循环）";
        EXPECT_EQ(reply[0] & 0xC0U, 0x40U) << "重置的首字节高两位该是 0b01（§10.3 图 10：短头 + 固定位）";
        // 令牌 = HMAC-SHA256(0x5b×32, 那条目的标识) 截前 16 字节；
        // python3 -c "import hmac,hashlib;print(hmac.new(bytes([0x5b])*32, bytes(0xE0+i for i in range(18)), hashlib.sha256).hexdigest()[:32])"
        const std::vector<std::uint8_t> expectedToken = makeBytesFromHex("3df37528f89c86db584f6cee0ff44c4d");
        ASSERT_GE(replyLength, expectedToken.size());
        EXPECT_TRUE(std::ranges::equal(std::span<const std::uint8_t>(reply.data() + replyLength - expectedToken.size(), expectedToken.size()),
                                       std::span<const std::uint8_t>(expectedToken)))
                << "末 16 字节不是「HMAC(静态密钥, 报文里的目的标识)」——对端照它比对，认不出来等于白发（§10.3.2）";

        // 反面一：22 字节的触发报文恰好换回 21 字节（图 10 的下限），再短一档就不该有回话
        std::vector<std::uint8_t> floorProbe(22, 0x22U);
        floorProbe.front() = 0x40U;
        ASSERT_GT(sender.send(serverAddress.platformAddress(), floorProbe.data(), floorProbe.size()), 0);
        std::size_t floorReplyLength = 0;
        ASSERT_TRUE(waitForCondition(
                [&]
                {
                    Platform::SocketAddress replyFrom{};
                    const ssize_t           received = sender.receive(reply.data(), reply.size(), replyFrom);
                    if (received <= 0)
                    {
                        return false;
                    }
                    floorReplyLength = static_cast<std::size_t>(received);
                    return true;
                },
                kWaitTimeout));
        EXPECT_EQ(floorReplyLength, 21U) << "22 字节的触发报文该换回 21 字节的重置（§10.3 图 10 的下限）";

        std::vector<std::uint8_t> tooSmallProbe(21, 0x33U);
        tooSmallProbe.front() = 0x40U;
        ASSERT_GT(sender.send(serverAddress.platformAddress(), tooSmallProbe.data(), tooSmallProbe.size()), 0);
        EXPECT_FALSE(waitForCondition(
                [&]
                {
                    Platform::SocketAddress replyFrom{};
                    return sender.receive(reply.data(), reply.size(), replyFrom) > 0;
                },
                std::chrono::milliseconds{300}))
                << "装不下 21 字节重置的触发报文也回了话：§10.3.3 的「比触发报文小」与图 10 的下限必须同时满足，宁可不发";

        // 反面二：长头（Handshake 型）的无主报文不该换回任何东西（§10.3：那时的令牌还没发给对端）
        std::vector<std::uint8_t> longHeaderProbe{0xA1U, 0x00U, 0x00U, 0x00U, 0x01U};
        longHeaderProbe.push_back(18U);
        for (std::size_t index = 0; index < 18; ++index)
        {
            longHeaderProbe.push_back(static_cast<std::uint8_t>(kProbeConnectionIdFirstByte + index));
        }
        longHeaderProbe.push_back(8U);
        for (std::uint8_t index = 0; index < 8; ++index)
        {
            longHeaderProbe.push_back(static_cast<std::uint8_t>(0x10U + index));
        }
        longHeaderProbe.push_back(0x02); // Length：包号 1 字节 + 载荷 1 字节
        longHeaderProbe.push_back(0x00); // 包号
        longHeaderProbe.push_back(0x00); // 载荷
        ASSERT_GT(sender.send(serverAddress.platformAddress(), longHeaderProbe.data(), longHeaderProbe.size()), 0);
        EXPECT_FALSE(waitForCondition(
                [&]
                {
                    Platform::SocketAddress replyFrom{};
                    return sender.receive(reply.data(), reply.size(), replyFrom) > 0;
                },
                std::chrono::milliseconds{300}))
                << "长头也换回了无状态重置：§10.3 只把重置留给「连接已经没了」那一档，长头那时的令牌还没发给对端";
    }

    /**
     * @brief 服务端要求地址验证时，客户端能接住 Retry 并完成握手
     * @details 服务端显式打开地址验证（令牌有效期 10 秒、固定密钥），于是每个新连接的第一条 Initial
     *          都会先吃一条 Retry。旧实现的客户端把 Retry 当 Malformed 丢掉，只能等握手时限；
     *          这里同时读服务端的 Retry 计数，确认这条用例真的走在那条路径上，而不是服务端没开验证。
     */
    TEST(QuicClientConnection, CompletesTheHandshakeWhenTheServerDemandsARetry)
    {
        RunningServerPeer server{kIpCertificatePath, kIpPrivateKeyPath, [](QuicServer::Configuration &configuration)
                                 {
                                     configuration.addressValidationTokenLifetime = std::chrono::seconds{10};
                                     configuration.addressValidationKey           = std::vector<std::uint8_t>(32U, 0x5AU);
                                 }};
        ASSERT_NE(server.listeningPort(), 0U) << "对面的服务端没起来，后面的判据都是空的";
        const std::uint64_t retriesBefore = processMetricValue("asyn_quic_retries_sent_total");

        ConnectAttempt attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, 4s), Core::InetAddress::resolve("127.0.0.1", server.listeningPort()).value()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "出站握手既没成也没失败，挂在那里";
        EXPECT_TRUE(attempt.isSuccessful()) << "服务端开了地址验证，客户端没能接住 Retry：" << attempt.elapsed().count() << " 毫秒后收场";
        EXPECT_EQ(attempt.negotiatedApplicationProtocol(), "h3") << "握手成了但 ALPN 没谈定";
        EXPECT_GT(processMetricValue("asyn_quic_retries_sent_total"), retriesBefore) << "服务端一条 Retry 都没发：这条用例判的就不是 Retry 那条路径";
    }

    /**
     * @brief 收到合法 Retry 后按新目的标识重推 Initial 密钥，并把同一份 ClientHello 带令牌重发
     * @details 用裸 UDP 桩代替服务端：拿出客户端第一个 Initial 的两条标识造一条标签正确的 Retry，
     *          再读回客户端紧接着发出的那一包。判据五件——目的标识换成 Retry 的源标识、自己的源标识
     *          不变、令牌原样带上、整条数据报仍是 1200 字节（令牌长度计入了补齐）、包号没有重置。
     *          旧实现在这里收不到第二包，只能等握手时限。
     */
    TEST(QuicClientConnection, ResendsTheClientHelloWithTheRetryToken)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));
        ASSERT_EQ(firstHeader.longPacketType, QuicLongPacketType::Initial);
        ASSERT_TRUE(firstHeader.token.empty()) << "第一个 Initial 不该带令牌";

        const std::vector<std::uint8_t> retrySourceConnectionId(QuicConnection::kSourceConnectionIdLength, 0x7BU);
        const std::vector<std::uint8_t> retryToken(32U, 0x6CU);
        const std::vector<std::uint8_t> retryPacket = makeRetryForClientInitial(firstHeader, retrySourceConnectionId, retryToken);
        // 先证明这条桩报文本身是一条验得过的 Retry，失败时才好把「桩造错了」与「客户端没处理」分开
        const auto selfCheck = decodeQuicRetryPacket(retryPacket, firstHeader.destinationConnectionId);
        ASSERT_HAS_VALUE(selfCheck) << selfCheck.error().message;
        ASSERT_TRUE(verifyQuicRetryPacketIntegrity(*selfCheck, retryPacket)) << "桩造的 Retry 自己都验不过";
        ASSERT_GT(stub.send(clientAddress, retryPacket.data(), retryPacket.size()), 0);

        Platform::SocketAddress                  retriedFrom{};
        std::optional<std::vector<std::uint8_t>> retriedDatagram{};
        QuicPacketHeader                         retriedHeader{};
        // Retry 之前客户端可能已经发了不止一条 Initial（ClientHello 会被预算切开），记下其中最大的包号，
        // 用来判「重发没有重置包号」（RFC 9000 §17.2.5.3）
        std::uint64_t largestPreRetryPacketNumber = clientInitialPacketNumber(*firstDatagram, firstHeader.destinationConnectionId).value_or(0);
        const auto    retryDeadline               = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < retryDeadline)
        {
            const auto                                     remaining = std::chrono::duration_cast<std::chrono::milliseconds>(retryDeadline - std::chrono::steady_clock::now());
            const std::optional<std::vector<std::uint8_t>> candidate = receiveDatagram(stub, remaining, retriedFrom);
            if (!candidate.has_value())
            {
                break;
            }
            QuicPacketHeader candidateHeader;
            if (!decodeLongHeader(*candidate, candidateHeader))
            {
                continue;
            }
            if (candidateHeader.token.empty())
            {
                if (const auto oldPacketNumber = clientInitialPacketNumber(*candidate, firstHeader.destinationConnectionId); oldPacketNumber.has_value())
                {
                    largestPreRetryPacketNumber = std::max(largestPreRetryPacketNumber, *oldPacketNumber);
                }
                continue;
            }
            retriedDatagram = candidate;
            // 报文头里的视图指向 candidate 的缓冲，拷贝之后要按拷贝重新解一遍，否则循环结束即悬空
            ASSERT_TRUE(decodeLongHeader(*retriedDatagram, retriedHeader));
            break;
        }
        ASSERT_TRUE(retriedDatagram.has_value()) << "客户端收到合法 Retry 后没有重发 Initial：旧实现把它当 Malformed 丢掉，只能等握手时限";
        EXPECT_EQ(retriedDatagram->size(), 1200U) << "带令牌的客户端 Initial 仍要补到 1200 字节（RFC 9000 §14.1）：令牌长度没计入包长就会超出";
        EXPECT_EQ(copyOf(retriedHeader.destinationConnectionId), retrySourceConnectionId) << "目的标识没换成 Retry 的源标识（RFC 9000 §17.2.5.2）";
        EXPECT_EQ(copyOf(retriedHeader.sourceConnectionId), copyOf(firstHeader.sourceConnectionId)) << "客户端不得改动自己的源标识（同节）";
        EXPECT_EQ(copyOf(retriedHeader.token), retryToken) << "重发的 Initial 没带 Retry 给的令牌";
        const std::optional<std::uint64_t> retriedPacketNumber = clientInitialPacketNumber(*retriedDatagram, retrySourceConnectionId);
        ASSERT_TRUE(retriedPacketNumber.has_value()) << "按 Retry 的源标识推 Initial 密钥后解不开重发的那一包";
        EXPECT_GT(*retriedPacketNumber, largestPreRetryPacketNumber) << "重发把包号重置了（RFC 9000 §17.2.5.3）";

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "桩不是真服务端，这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩什么都没答，握手不该成功";
    }

    /**
     * @brief 大令牌把 Length 域压进 1 字节档时，客户端 Initial 仍不小于 1200 字节
     * @details 令牌约 1 KB 时，Length 域覆盖的「包号 + 密文」不到 64 字节，线上只占 1 字节。
     *          按 2 字节预留固定开销再补 PADDING 会少补 1 字节，数据报只剩 1199，低于 §14.1 的
     *          下限。判据三件：数据报不小于 1200、令牌原样带上、Length 域等于包号起点之后的
     *          全部字节（解出来的包号与载荷长度自洽）。
     */
    TEST(QuicClientConnection, PadsALargeRetryTokenInitialToAtLeast1200Bytes)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));
        ASSERT_EQ(firstHeader.longPacketType, QuicLongPacketType::Initial);
        ASSERT_TRUE(firstHeader.token.empty()) << "第一个 Initial 不该带令牌";

        const std::vector<std::uint8_t> retrySourceConnectionId(8U, 0x7BU);
        const std::vector<std::uint8_t> retryToken(1120U, 0x6CU);
        const std::vector<std::uint8_t> retryPacket = makeRetryForClientInitial(firstHeader, retrySourceConnectionId, retryToken);
        // 先证明这条桩报文本身是一条验得过的 Retry，失败时才好把「桩造错了」与「客户端没处理」分开
        const auto selfCheck = decodeQuicRetryPacket(retryPacket, firstHeader.destinationConnectionId);
        ASSERT_HAS_VALUE(selfCheck) << selfCheck.error().message;
        ASSERT_TRUE(verifyQuicRetryPacketIntegrity(*selfCheck, retryPacket)) << "桩造的 Retry 自己都验不过";
        ASSERT_GT(stub.send(clientAddress, retryPacket.data(), retryPacket.size()), 0);

        std::optional<std::vector<std::uint8_t>> retriedDatagram{};
        QuicPacketHeader                         retriedHeader{};
        const auto                               retryDeadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < retryDeadline)
        {
            const auto                                     remaining = std::chrono::duration_cast<std::chrono::milliseconds>(retryDeadline - std::chrono::steady_clock::now());
            const std::optional<std::vector<std::uint8_t>> candidate = receiveDatagram(stub, remaining, clientAddress);
            if (!candidate.has_value())
            {
                break;
            }
            QuicPacketHeader candidateHeader;
            if (!decodeLongHeader(*candidate, candidateHeader) || candidateHeader.token.empty())
            {
                continue;
            }
            retriedDatagram = candidate;
            // 报文头里的视图指向 candidate 的缓冲，拷贝之后要按拷贝重新解一遍，否则循环结束即悬空
            ASSERT_TRUE(decodeLongHeader(*retriedDatagram, retriedHeader));
            break;
        }
        ASSERT_TRUE(retriedDatagram.has_value()) << "客户端收到大令牌 Retry 后没有重发 Initial";
        EXPECT_GE(retriedDatagram->size(), 1200U) << "带大令牌的客户端 Initial 仍要不小于 1200 字节（RFC 9000 §14.1）";
        EXPECT_EQ(copyOf(retriedHeader.token), retryToken) << "重发的 Initial 没原样带上 Retry 给的令牌";
        ASSERT_GT(retriedHeader.packetNumberAndPayloadByteCount, 0U) << "Length 域没解出来";
        EXPECT_EQ(retriedHeader.packetNumberAndPayloadByteCount, retriedDatagram->size() - retriedHeader.packetNumberOffset)
                << "Length 域与包号起点之后的字节数对不上：这条报文在线上读不出边界";
        EXPECT_TRUE(clientInitialPacketNumber(*retriedDatagram, retrySourceConnectionId).has_value()) << "按 Retry 源标识推 Initial 密钥后解不开这一包";

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "桩不是真服务端，这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩什么都没答，握手不该成功";
    }

    /**
     * @brief 完整性标签不对的 Retry 不得让客户端改目的标识、带上令牌，或占掉唯一一次 Retry
     * @details 先送一条标签被翻掉的 Retry，再送一条合法 Retry。拒绝坏标签的实现会接住后一条，
     *          重发同一份 ClientHello 时带的是后一条的令牌与目的标识；把坏标签当成真的实现会
     *          按前一条迁移，而且第二条 Retry 因「一个连接尝试至多接受一条」被整条丢弃。
     *          判据因此不依赖「客户端在窗口里一定说话」这条运气：客户端必须带着合法那条的令牌
     *          与目的标识重发，否则就是坏标签被接受了。标签校验本身的正负样例在
     *          QuicPacketBuilder 的用例里逐字节钉住。
     */
    TEST(QuicClientConnection, DoesNotContinueAfterARetryWithABrokenIntegrityTag)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));

        const std::vector<std::uint8_t> rejectedSourceConnectionId(8U, 0x7BU);
        const std::vector<std::uint8_t> rejectedToken(32U, 0x6CU);
        const std::vector<std::uint8_t> acceptedSourceConnectionId(8U, 0x7DU);
        const std::vector<std::uint8_t> acceptedToken(32U, 0x6EU);
        const std::vector<std::uint8_t> rejectedPacket = makeRetryForClientInitial(firstHeader, rejectedSourceConnectionId, rejectedToken, false);
        const std::vector<std::uint8_t> acceptedPacket = makeRetryForClientInitial(firstHeader, acceptedSourceConnectionId, acceptedToken);
        ASSERT_GT(stub.send(clientAddress, rejectedPacket.data(), rejectedPacket.size()), 0);
        // 隔开一小段，保证客户端先看见坏标签那条，再由它决定接不接后一条合法 Retry
        std::this_thread::sleep_for(50ms);
        ASSERT_GT(stub.send(clientAddress, acceptedPacket.data(), acceptedPacket.size()), 0);

        const std::optional<std::vector<std::uint8_t>> retriedDatagram = receiveInitialWithToken(stub, 500ms);
        ASSERT_TRUE(retriedDatagram.has_value()) << "坏标签的 Retry 之后客户端连合法 Retry 也没接住：坏标签可能被当成了真的并占掉了唯一一次机会";
        QuicPacketHeader retriedHeader;
        ASSERT_TRUE(decodeLongHeader(*retriedDatagram, retriedHeader));
        EXPECT_EQ(copyOf(retriedHeader.token), acceptedToken) << "坏标签的 Retry 被当成了真的：客户端带着它的令牌继续了握手";
        EXPECT_EQ(copyOf(retriedHeader.destinationConnectionId), acceptedSourceConnectionId) << "坏标签的 Retry 换掉了目的标识";
        EXPECT_NE(copyOf(retriedHeader.destinationConnectionId), rejectedSourceConnectionId);

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩只回了 Retry，握手不该完成";
    }

    /**
     * @brief 目的标识不是本端源标识的 Retry 必须丢弃，且不占掉随后合法 Retry 的机会
     * @details 桩把 Retry 的线上目的标识填成客户端第一个 Initial 的目的标识（原始目的标识），
     *          客户端按 §17.2.5.2 找不到本端那条标识，应整条丢弃；随后一条合法 Retry 仍须被接住。
     */
    TEST(QuicClientConnection, RejectsARetryWhoseDestinationConnectionIdIsNotOurs)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));

        const std::vector<std::uint8_t> rejectedSourceConnectionId(8U, 0x7BU);
        const std::vector<std::uint8_t> rejectedToken(32U, 0x6CU);
        const std::vector<std::uint8_t> acceptedSourceConnectionId(8U, 0x7DU);
        const std::vector<std::uint8_t> acceptedToken(32U, 0x6EU);
        const std::vector<std::uint8_t> rejectedPacket = makeRetryPacket(firstHeader, firstHeader.destinationConnectionId, rejectedSourceConnectionId, rejectedToken);
        const std::vector<std::uint8_t> acceptedPacket = makeRetryForClientInitial(firstHeader, acceptedSourceConnectionId, acceptedToken);
        ASSERT_GT(stub.send(clientAddress, rejectedPacket.data(), rejectedPacket.size()), 0);
        std::this_thread::sleep_for(50ms);
        ASSERT_GT(stub.send(clientAddress, acceptedPacket.data(), acceptedPacket.size()), 0);

        const std::optional<std::vector<std::uint8_t>> retriedDatagram = receiveInitialWithToken(stub, 500ms);
        ASSERT_TRUE(retriedDatagram.has_value()) << "目的标识不匹配的 Retry 之后客户端没接住合法 Retry";
        QuicPacketHeader retriedHeader;
        ASSERT_TRUE(decodeLongHeader(*retriedDatagram, retriedHeader));
        EXPECT_EQ(copyOf(retriedHeader.token), acceptedToken) << "目的标识不匹配的 Retry 被当成了真的";
        EXPECT_EQ(copyOf(retriedHeader.destinationConnectionId), acceptedSourceConnectionId);
        EXPECT_NE(copyOf(retriedHeader.destinationConnectionId), rejectedSourceConnectionId);

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩只回了 Retry，握手不该完成";
    }

    /**
     * @brief 源标识等于原始目的标识的 Retry 必须丢弃（§17.2.5.1 的 MUST NOT）
     * @details 这条形态组包器自己就拒发，因此手工拼一条：两条标识之后按 RFC 9001 §5.8 的
     *          固定密钥补一个合法完整性标签。客户端若只看令牌与标签、漏掉这条比较，就会把
     *          目的标识换成一个它本来就当成原始目的的值，后续 Initial 必然打错地址。
     */
    TEST(QuicClientConnection, RejectsARetryWhoseSourceConnectionIdEqualsTheOriginalDestination)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));

        const std::vector<std::uint8_t> rejectedToken(32U, 0x6CU);
        const std::vector<std::uint8_t> malformedRetry = makeRetryWithSourceEqualOriginalDestination(firstHeader, firstHeader.destinationConnectionId, rejectedToken);
        // 手工拼的这条必须除「源标识等于原始目的标识」之外处处合法，否则这条用例判不出那道比较
        const auto selfCheck = decodeQuicRetryPacket(malformedRetry, firstHeader.destinationConnectionId);
        ASSERT_HAS_VALUE(selfCheck) << selfCheck.error().message;
        EXPECT_EQ(copyOf(selfCheck->sourceConnectionId), copyOf(firstHeader.destinationConnectionId));
        ASSERT_TRUE(verifyQuicRetryPacketIntegrity(*selfCheck, malformedRetry)) << "手工拼的 Retry 标签自己都验不过";
        const std::vector<std::uint8_t> acceptedSourceConnectionId(8U, 0x7DU);
        const std::vector<std::uint8_t> acceptedToken(32U, 0x6EU);
        const std::vector<std::uint8_t> acceptedPacket = makeRetryForClientInitial(firstHeader, acceptedSourceConnectionId, acceptedToken);
        ASSERT_GT(stub.send(clientAddress, malformedRetry.data(), malformedRetry.size()), 0);
        std::this_thread::sleep_for(50ms);
        ASSERT_GT(stub.send(clientAddress, acceptedPacket.data(), acceptedPacket.size()), 0);

        const std::optional<std::vector<std::uint8_t>> retriedDatagram = receiveInitialWithToken(stub, 500ms);
        ASSERT_TRUE(retriedDatagram.has_value()) << "源标识等于原始目的标识的 Retry 之后客户端没接住合法 Retry";
        QuicPacketHeader retriedHeader;
        ASSERT_TRUE(decodeLongHeader(*retriedDatagram, retriedHeader));
        EXPECT_EQ(copyOf(retriedHeader.token), acceptedToken) << "源标识等于原始目的标识的 Retry 被当成了真的";
        EXPECT_EQ(copyOf(retriedHeader.destinationConnectionId), acceptedSourceConnectionId);

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩只回了 Retry，握手不该完成";
    }

    /**
     * @brief 一条连接尝试只接受第一条 Retry，后续 Retry 即便标签正确也整条丢弃
     * @details 先送合法 Retry 并读到客户端按它重发的 Initial，再送第二条合法 Retry（换源标识与
     *          令牌）。客户端之后带令牌的包必须仍指向第一条的源标识与令牌；出现第二条的任何
     *          痕迹就说明「至多一次」那道闸没关住。
     */
    TEST(QuicClientConnection, IgnoresASecondRetryAfterTheFirstIsAccepted)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));

        const std::vector<std::uint8_t> firstSourceConnectionId(8U, 0x7BU);
        const std::vector<std::uint8_t> firstToken(32U, 0x6CU);
        const std::vector<std::uint8_t> secondSourceConnectionId(8U, 0x7DU);
        const std::vector<std::uint8_t> secondToken(32U, 0x6EU);
        const std::vector<std::uint8_t> firstPacket = makeRetryForClientInitial(firstHeader, firstSourceConnectionId, firstToken);
        ASSERT_GT(stub.send(clientAddress, firstPacket.data(), firstPacket.size()), 0);

        const std::optional<std::vector<std::uint8_t>> firstRetried = receiveInitialWithToken(stub, 500ms);
        ASSERT_TRUE(firstRetried.has_value()) << "客户端没接住第一条合法 Retry";
        QuicPacketHeader firstRetriedHeader;
        ASSERT_TRUE(decodeLongHeader(*firstRetried, firstRetriedHeader));
        ASSERT_EQ(copyOf(firstRetriedHeader.token), firstToken);
        ASSERT_EQ(copyOf(firstRetriedHeader.destinationConnectionId), firstSourceConnectionId);

        const std::vector<std::uint8_t> secondPacket = makeRetryForClientInitial(firstHeader, secondSourceConnectionId, secondToken);
        ASSERT_GT(stub.send(clientAddress, secondPacket.data(), secondPacket.size()), 0);

        const auto secondDeadline = std::chrono::steady_clock::now() + 300ms;
        while (std::chrono::steady_clock::now() < secondDeadline)
        {
            const auto                                     remaining = std::chrono::duration_cast<std::chrono::milliseconds>(secondDeadline - std::chrono::steady_clock::now());
            Platform::SocketAddress                        from{};
            const std::optional<std::vector<std::uint8_t>> candidate = receiveDatagram(stub, remaining, from);
            if (!candidate.has_value())
            {
                break;
            }
            QuicPacketHeader candidateHeader;
            if (!decodeLongHeader(*candidate, candidateHeader) || candidateHeader.token.empty())
            {
                continue;
            }
            EXPECT_EQ(copyOf(candidateHeader.token), firstToken) << "第二条 Retry 被接受了：客户端换成了它的令牌";
            EXPECT_EQ(copyOf(candidateHeader.destinationConnectionId), firstSourceConnectionId) << "第二条 Retry 换掉了目的标识";
            EXPECT_NE(copyOf(candidateHeader.destinationConnectionId), secondSourceConnectionId);
        }

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩只回了 Retry，握手不该完成";
    }

    /**
     * @brief 处理过服务端 Initial 之后到达的 Retry 必须丢弃
     * @details 先送一条只带 PING 的服务端 Initial，等客户端把它处理掉并回 ACK；再送一条合法
     *          Retry，然后送第二条服务端 Initial。客户端若接了 Retry，Initial 密钥已换成 Retry
     *          源标识，第二条服务端 Initial 解不开，也就回不出 ACK；反过来，收到一条发往服务端
     *          标识、不带令牌的 ACK，就证明 Retry 被整条丢弃了。
     */
    TEST(QuicClientConnection, IgnoresARetryAfterAServerInitial)
    {
        const Platform::Socket::Initialization socketInitialization;
        ASSERT_TRUE(socketInitialization.isValid()) << "Winsock 初始化失败";
        const Platform::DatagramSocket stub = Platform::DatagramSocket::bindTo(Core::InetAddress("127.0.0.1", 0).platformAddress());
        ASSERT_TRUE(stub.isValid()) << "裸 UDP 桩绑定失败，错误码 " << Platform::PlatformError::lastSocketErrorCode();
        const Core::InetAddress stubAddress(stub.localAddress().storage, stub.localAddress().length);

        ConnectAttempt                                 attempt{makeConfiguration("127.0.0.1", kIpCertificatePath, std::chrono::milliseconds{800}), stubAddress};
        Platform::SocketAddress                        clientAddress{};
        const std::optional<std::vector<std::uint8_t>> firstDatagram = receiveDatagram(stub, kWaitTimeout, clientAddress);
        ASSERT_TRUE(firstDatagram.has_value()) << "客户端没发出第一个 Initial";
        QuicPacketHeader firstHeader;
        ASSERT_TRUE(decodeLongHeader(*firstDatagram, firstHeader));

        const std::vector<std::uint8_t> serverConnectionId(QuicConnection::kSourceConnectionIdLength, 0x5DU);
        const std::vector<std::uint8_t> firstServerInitial = makeServerInitialDatagram(firstHeader, serverConnectionId, 0);
        ASSERT_GT(stub.send(clientAddress, firstServerInitial.data(), firstServerInitial.size()), 0);
        // 等客户端把这条 Initial 处理掉，并把这次处理触发的 ACK 收干净，后面看到的包才算第二条 Initial 的回应
        std::this_thread::sleep_for(100ms);
        Platform::SocketAddress drainedFrom{};
        while (receiveDatagram(stub, 20ms, drainedFrom).has_value())
        {
        }

        const std::vector<std::uint8_t> retrySourceConnectionId(8U, 0x7BU);
        const std::vector<std::uint8_t> retryToken(32U, 0x6CU);
        const std::vector<std::uint8_t> retryPacket         = makeRetryForClientInitial(firstHeader, retrySourceConnectionId, retryToken);
        const std::vector<std::uint8_t> secondServerInitial = makeServerInitialDatagram(firstHeader, serverConnectionId, 1);
        ASSERT_GT(stub.send(clientAddress, retryPacket.data(), retryPacket.size()), 0);
        ASSERT_GT(stub.send(clientAddress, secondServerInitial.data(), secondServerInitial.size()), 0);

        std::optional<std::vector<std::uint8_t>> response{};
        const auto                               responseDeadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < responseDeadline)
        {
            const auto                               remaining = std::chrono::duration_cast<std::chrono::milliseconds>(responseDeadline - std::chrono::steady_clock::now());
            Platform::SocketAddress                  from{};
            std::optional<std::vector<std::uint8_t>> candidate = receiveDatagram(stub, remaining, from);
            if (!candidate.has_value())
            {
                break;
            }
            QuicPacketHeader candidateHeader;
            if (!decodeLongHeader(*candidate, candidateHeader))
            {
                continue;
            }
            if (!candidateHeader.token.empty() || copyOf(candidateHeader.destinationConnectionId) == serverConnectionId)
            {
                response = std::move(candidate);
                break;
            }
        }
        ASSERT_TRUE(response.has_value()) << "服务端 Initial 之后客户端既没回 ACK 也没发别的：无法判断 Retry 有没有被吞掉";
        QuicPacketHeader responseHeader;
        ASSERT_TRUE(decodeLongHeader(*response, responseHeader));
        EXPECT_TRUE(responseHeader.token.empty()) << "服务端 Initial 已处理，客户端却接了 Retry 并带上了它的令牌";
        EXPECT_EQ(copyOf(responseHeader.destinationConnectionId), serverConnectionId) << "目的标识被 Retry 换掉了";

        EXPECT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次尝试该在握手时限内收场";
        EXPECT_FALSE(attempt.isSuccessful()) << "桩只回了 Initial 与 Retry，握手不该完成";
    }

} // namespace AsynGyanis::Net
