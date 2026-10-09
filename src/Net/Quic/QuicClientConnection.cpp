#include "Net/Quic/QuicClientConnection.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Net/Quic/Crypto/QuicTlsContext.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"

#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 收包缓冲的尺寸：与平台层允许的单条数据报上限一致（QUIC 自己会再按 1200 组包）
        constexpr std::size_t kReceiveBufferByteLength = Platform::DatagramSocket::kMaximumDatagramBytes;

        /**
         * @brief 按目标地址的地址族造一个「任意地址 + 端口 0」的绑定地址
         * @param serverAddress 目标地址，只取它的地址族
         * @return Platform::SocketAddress 交给内核挑源地址与源端口
         */
        Platform::SocketAddress makeEphemeralBindAddress(const Platform::SocketAddress &serverAddress) noexcept
        {
            Platform::SocketAddress bindAddress{};
            bindAddress.storage.ss_family = serverAddress.storage.ss_family;
            bindAddress.length            = serverAddress.storage.ss_family == AF_INET ? static_cast<socklen_t>(sizeof(sockaddr_in)) : static_cast<socklen_t>(sizeof(sockaddr_in6));
            return bindAddress;
        }
    } // namespace

    QuicClientConnection::QuicClientConnection(Core::EventLoop &loop, Configuration configuration) : m_loop(loop), m_configuration(std::move(configuration))
    {
        // 空主机名意味着「出去之后不校验对端身份」：SNI 与证书里的校验目标都取自它。这种配置不该
        // 被放到碰网络之后才发现，更不该有一个「可以关掉校验」的档位（见 Configuration 的说明）
        if (m_configuration.hostName.empty())
        {
            throw Base::InvalidArgumentException("QUIC 出站连接配置不合格：主机名为空，SNI 与证书校验目标都无处可取");
        }
        // 客户端证书与私钥只能成对出现：只给一项的后果不是在启动时报「少一个参数」，而是到握手里
        // 出示证书那一刻才发现没有可签名的私钥——失败点离成因隔了一整条握手
        if (m_configuration.clientCertificateFile.empty() != m_configuration.clientPrivateKeyFile.empty())
        {
            throw Base::InvalidArgumentException("QUIC 出站连接配置不合格：客户端证书与私钥必须同时给或同时不给（证书=\"" + m_configuration.clientCertificateFile + "\"，私钥=\"" +
                                                 m_configuration.clientPrivateKeyFile + "\"）");
        }
    }

    QuicClientConnection::~QuicClientConnection()
    {
        // 先关套接字再销毁连接：连接析构时若有协程还挂在可读上，会一直等下去（AsyncUdpSocket::close()
        // 的注释记着这条顺序的理由）。销毁顺序本身按成员声明的逆序，m_connection 已先于 m_socket 走完
        if (m_socket != nullptr)
        {
            m_socket->close();
        }
    }

    Core::Task<bool> QuicClientConnection::connect(const Core::InetAddress &serverAddress)
    {
        const Platform::SocketAddress destination = serverAddress.platformAddress();
        m_serverAddress                           = destination;

        Platform::DatagramSocket datagramSocket = Platform::DatagramSocket::bindTo(makeEphemeralBindAddress(destination));
        if (!datagramSocket.isValid())
        {
            throw Base::SystemException("QUIC 出站连接失败：UDP 套接字建不起来（套接字错误码 " + std::to_string(Platform::PlatformError::lastSocketErrorCode()) + "）");
        }
        m_socket = std::make_unique<Core::AsyncUdpSocket>(m_loop, std::move(datagramSocket));
        m_receiveBuffer.assign(kReceiveBufferByteLength, 0U);

        // 就地建：Core::TlsContext 持有 SSL_CTX 的裸句柄且不可搬动，先造局部量再搬进 unique_ptr 是白费
        m_tlsContext = std::make_unique<Core::TlsContext>(m_configuration.tlsPolicy, Core::TlsContext::Role::Client);
        // 这一句是必须的：`Role::Client` 只把上下文摆成出站形状，真正把 SSL_VERIFY_PEER 打开的是这里
        // （与 `HttpClient` 同一处调用，判据只在 `enableClientPeerVerification` 一处）。漏掉的形态是
        // 握手照成、证书照收，只是没人核对对端身份——用例 RejectsATrustedCertificateWhoseNameDoesNotMatch 钉它
        m_tlsContext->enableClientPeerVerification();

        // 本端身份要在建连接之前装好：SSL_new 之后改证书等于给每条连接重配一次，而这里只有一个上下文
        if (!m_configuration.clientCertificateFile.empty() && !m_tlsContext->loadCertificate(m_configuration.clientCertificateFile, m_configuration.clientPrivateKeyFile))
        {
            // 不抛：本函数的契约是「bool 交结果」，抛出会落进无人接住的协程里被丢弃（连接对象连同
            // 已建好的套接字留在原地）。装不上身份是部署错误，日志要点名两个路径
            LOG_ERROR_FMT("QuicClientConnection：客户端身份装不上，本次握手不发出证书（证书=\"%s\"，私钥=\"%s\"）", m_configuration.clientCertificateFile.c_str(),
                          m_configuration.clientPrivateKeyFile.c_str());
            co_return false;
        }

        // 读得到才报 ECN 计数；读不到（Windows）就按 §13.4.1 不报，对端会因此不在这条路上用 ECN
        static_cast<void>(m_socket->enableEcnFieldVisibility());

        QuicConnection::Configuration connectionConfiguration;
        connectionConfiguration.tlsContext   = m_tlsContext->nativeHandle();
        connectionConfiguration.idleTimeout  = m_configuration.idleTimeout;
        connectionConfiguration.sendDatagram = [this](const Platform::SocketAddress &peerAddress, const std::uint8_t *data, const std::size_t length,
                                                      const std::uint8_t ecnCodepoint) -> Core::Task<bool>
        {
            const ssize_t sentByteCount = co_await m_socket->asyncSendTo(peerAddress, data, length, ecnCodepoint);
            co_return sentByteCount >= 0 && static_cast<std::size_t>(sentByteCount) == length;
        };
        connectionConfiguration.onStreamData = [this](QuicConnection &, const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
        { noteStreamData(streamId, data, isEndStream); };
        // 对端复位或叫停了某条请求流：转交给上层（h3 出站客户端据此收掉那条请求的账，并按对端给的
        // 应用层错误码判它能不能安全重发）。没有这一路，本端只会等到请求时限
        connectionConfiguration.onPeerStreamClosed = [this](QuicConnection &, const std::int64_t streamId, const std::uint64_t applicationErrorCode, const bool isResetByPeer)
        {
            if (m_peerStreamAbortSink)
            {
                m_peerStreamAbortSink(streamId, applicationErrorCode, isResetByPeer);
            }
        };

        QuicClientTlsSettings clientTlsSettings;
        clientTlsSettings.hostName                       = m_configuration.hostName;
        clientTlsSettings.applicationProtocolIdentifiers = m_configuration.applicationProtocolIdentifiers;
        m_connection                                     = QuicConnection::connect(connectionConfiguration, m_socket->localAddress(), destination, clientTlsSettings);
        if (m_connection == nullptr)
        {
            // 具体原因（TLS 会话建不起来、随机数不可用）由 QuicConnection::connect 那条错误日志给出，
            // 这里不重复解释，只把「没起起来」交回调用方
            co_return false;
        }

        // 看门狗与握手收包都在 `driveHandshake()` 里：那一条协程帧结束就是看门狗撤销的时刻
        if (!co_await driveHandshake())
        {
            // 前缀按本件的名（原先写成 QuicConnection:，按名 grep 的人会翻到另一个文件）；
            // 等级从 DEBUG 抬到 WARN：调用方拿到的只是一句 false，而「被对端拒绝」这条通路
            // 是本端主动出站产生的，不该只在调试档看得见
            LOG_WARN("QuicClientConnection: 出站握手未完成即收场（被对端拒绝、时限掐断，或本端已关）");
            co_return false;
        }
        co_return true;
    }

    Core::Task<bool> QuicClientConnection::driveHandshake()
    {
        // 看门狗刻意限定在本协程的作用域里：本框架的协程帧是在**调用方丢掉 Task 时**才销毁的，帧内局部量
        // 跟着一起走。放在 connect() 的函数体上就会出现「握手已完成、看门狗却还醒着，到点把这条连接的
        // 套接字关掉」这种极难复现的形态——本条协程一收场即撤销，之后不再有掐套接字的动作
        const Core::DeadlineGuard<Core::AsyncUdpSocket> handshakeWatchdog(m_loop, *m_socket, m_configuration.handshakeTimeout, "QUIC 出站握手");
        // 第一个 Initial 由 flush 里的 drive 产出：本端先出声，与「收到报文才推进」的服务端侧相反
        co_await m_connection->flush();
        while (!m_connection->isHandshakeComplete() && !m_connection->isClosed())
        {
            const Core::AsyncUdpSocket::DatagramReceiveResult received = co_await m_socket->asyncReceiveFrom(m_receiveBuffer.data(), m_receiveBuffer.size());
            if (received.receivedByteCount <= 0)
            {
                // -1 且带错误码＝对端不可达那一类 ICMP 回声：套接字还能用，QUIC 自己的丢包与
                // 空闲计时器才是这条连接的裁判，这里不据此收场。
                // 0 长报文对 QUIC 没有意义、-1 且无码＝套接字被关（时限掐断）：两种都收场
                if (received.socketErrorCode != 0)
                {
                    continue;
                }
                break;
            }
            // 读得到才把这一格交下去（Windows 读不到，那一侧恒为「非 ECN」且不该报计数，RFC 9000 §13.4.1）：
            // 服务端外壳同一条判据，见 QuicServer 里 datagramEcnCodepoint 那一格
            const std::optional<std::uint8_t> ecnCodepoint = m_socket->isEcnFieldVisible() ? std::optional<std::uint8_t>{received.ecnCodepoint} : std::nullopt;
            co_await m_connection->handleDatagram(received.peerAddress, std::span<const std::uint8_t>(m_receiveBuffer.data(), static_cast<std::size_t>(received.receivedByteCount)),
                                                  ecnCodepoint);
        }
        co_return m_connection->isHandshakeComplete();
    }

    Core::Task<> QuicClientConnection::pumpOnce()
    {
        if (m_connection == nullptr || m_socket == nullptr || m_isStopped)
        {
            co_return;
        }
        const Core::AsyncUdpSocket::DatagramReceiveResult received = co_await m_socket->asyncReceiveFrom(m_receiveBuffer.data(), m_receiveBuffer.size());
        // 本方法一轮只读一条，两种「没有报文」（-1 与 0 长）都是直接收手：留着这条连接等下一轮。
        // 带错误码的 -1 尤其不能当成对端已死——那是 ICMP 捎来的回声，判死这条连接的是它自己的
        // 丢包与空闲计时器（理由见 connect() 里同一段）
        if (received.receivedByteCount <= 0)
        {
            co_return;
        }
        co_await m_connection->handleDatagram(received.peerAddress, std::span<const std::uint8_t>(m_receiveBuffer.data(), static_cast<std::size_t>(received.receivedByteCount)));
        co_return;
    }

    bool QuicClientConnection::isReady() const noexcept
    {
        return m_connection != nullptr && m_connection->isHandshakeComplete();
    }

    bool QuicClientConnection::isClosed() const noexcept
    {
        return m_isStopped || m_connection == nullptr || m_connection->isClosed();
    }

    std::string QuicClientConnection::negotiatedApplicationProtocol() const
    {
        if (m_connection == nullptr)
        {
            return {};
        }
        return std::string{m_connection->selectedApplicationProtocol()};
    }

    void QuicClientConnection::abortStream(const std::int64_t streamId, const std::uint64_t applicationErrorCode) noexcept
    {
        if (m_connection == nullptr)
        {
            return; // 连接还没建起来（或已交还）：没有流可收口，调用方的结论不受影响
        }
        m_connection->abortStream(streamId, applicationErrorCode);
    }

    std::int64_t QuicClientConnection::openStream()
    {
        if (!isReady())
        {
            return -1;
        }
        return m_connection->openBidirectionalStream();
    }

    std::int64_t QuicClientConnection::openUnidirectionalStream()
    {
        if (!isReady())
        {
            return -1;
        }
        return m_connection->openUnidirectionalStream();
    }

    Core::Task<> QuicClientConnection::sendPending()
    {
        if (m_connection != nullptr && !m_connection->isClosed())
        {
            co_await m_connection->flush();
        }
        co_return;
    }

    std::size_t QuicClientConnection::writeStream(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        if (m_connection == nullptr || streamId < 0)
        {
            return 0U;
        }
        return m_connection->queueStreamData(streamId, data, isEndStream);
    }

    std::vector<std::uint8_t> QuicClientConnection::takeReceivedData(const std::int64_t streamId)
    {
        auto incoming = m_incoming.find(streamId);
        if (incoming == m_incoming.end())
        {
            return {};
        }
        std::vector<std::uint8_t> taken = std::move(incoming->second.receivedBytes);
        incoming->second.receivedBytes.clear();
        return taken;
    }

    bool QuicClientConnection::isEndStreamReceived(const std::int64_t streamId) const noexcept
    {
        const auto incoming = m_incoming.find(streamId);
        return incoming != m_incoming.end() && incoming->second.isEndStreamReceived;
    }

    void QuicClientConnection::extendReceiveWindow(const std::int64_t streamId, const std::size_t consumedByteCount)
    {
        if (m_connection != nullptr)
        {
            m_connection->extendReceiveWindow(streamId, consumedByteCount);
        }
    }

    void QuicClientConnection::close()
    {
        if (m_connection != nullptr && !m_connection->isClosed())
        {
            m_connection->requestClose();
        }
        m_isStopped = true;
        // 套接字要跟着关掉：叫醒挂在可读上的协程的是销毁注册对象，不是把标记翻上去，而本类不自带后台
        // 协程——对端一旦不再发东西，那条等待就没人收得回来（与析构里同一条理由）。顺序上先置标记再关：
        // 醒来那一轮看到 m_isStopped / isClosed() 就自己收场，不会再往发送口写。
        // 记一句实话：这条改动钉不出**它自己**的用例（回环上任何一条入站报文都算一次叫醒，把下面两行
        // 摘掉现有用例照样绿），依据是析构那条同型规则与一次实测——满载并行跑全量时一条 h3 请求挂在
        // 读上停过 25 秒不返回。叫醒机制本身由 `AsyncUdpSocket.CloseWakesCoroutineBlockedOnReceive` 钉。
        if (m_socket != nullptr)
        {
            m_socket->close();
        }
    }

    Core::EventLoop &QuicClientConnection::eventLoop() const noexcept
    {
        return m_loop;
    }

    Platform::SocketAddress QuicClientConnection::localAddress() const noexcept
    {
        return m_socket != nullptr ? m_socket->localAddress() : Platform::SocketAddress{};
    }

    void QuicClientConnection::setStreamDataSink(std::function<void(std::int64_t, std::span<const std::uint8_t>, bool)> sink)
    {
        m_streamDataSink = std::move(sink);
    }

    void QuicClientConnection::setPeerStreamAbortSink(std::function<void(std::int64_t, std::uint64_t, bool)> sink)
    {
        m_peerStreamAbortSink = std::move(sink);
    }

    void QuicClientConnection::noteStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        if (m_streamDataSink)
        {
            // 转交档：不在这里排队，收尾也要原样带上去（h3 那层靠它判这条流的消息收齐了没有）
            m_streamDataSink(streamId, data, isEndStream);
            return;
        }
        IncomingStreamState &state = m_incoming[streamId];
        state.receivedBytes.insert(state.receivedBytes.end(), data.begin(), data.end());
        state.isEndStreamReceived = state.isEndStreamReceived || isEndStream;
    }

} // namespace AsynGyanis::Net
