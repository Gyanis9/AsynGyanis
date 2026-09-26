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

#include <cstring>
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

        /**
         * @brief 把框架的地址换成平台层的数据报地址
         * @param address 框架地址（`InetAddress::resolve` 的产出）
         * @return Platform::SocketAddress 直接可交给 `asyncSendTo` 的形状
         */
        Platform::SocketAddress toPlatformAddress(const Core::InetAddress &address)
        {
            Platform::SocketAddress platformAddress{};
            platformAddress.length = address.nativeAddressLength();
            std::memcpy(&platformAddress.storage, address.nativeAddress(), address.nativeAddressLength());
            return platformAddress;
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
        const Platform::SocketAddress destination = toPlatformAddress(serverAddress);
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

        QuicConnection::Configuration connectionConfiguration;
        connectionConfiguration.tlsContext   = m_tlsContext->nativeHandle();
        connectionConfiguration.idleTimeout  = m_configuration.idleTimeout;
        connectionConfiguration.sendDatagram = [this](const Platform::SocketAddress &peerAddress, const std::uint8_t *data, const std::size_t length) -> Core::Task<bool>
        {
            const ssize_t sentByteCount = co_await m_socket->asyncSendTo(peerAddress, data, length);
            co_return sentByteCount >= 0 && static_cast<std::size_t>(sentByteCount) == length;
        };
        connectionConfiguration.onStreamData = [this](QuicConnection &, const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
        { noteStreamData(streamId, data, isEndStream); };

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

        // 看门狗刻意限定在这块作用域里：本框架的协程帧是在**调用方丢掉 Task 时**才销毁的，帧内局部量
        // 跟着一起走。放在函数体上就会出现「握手已完成、看门狗却还醒着，到点把这条连接的套接字关掉」
        // 这种极难复现的形态——块结尾即撤销，之后不再有掐套接字的协程
        bool isHandshakeDone = false;
        {
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
                co_await m_connection->handleDatagram(received.peerAddress,
                                                      std::span<const std::uint8_t>(m_receiveBuffer.data(), static_cast<std::size_t>(received.receivedByteCount)));
            }
            isHandshakeDone = m_connection->isHandshakeComplete();
        }

        if (!isHandshakeDone)
        {
            LOG_DEBUG("QuicConnection: 出站握手未完成即收场（被对端拒绝、时限掐断，或本端已关）");
            co_return false;
        }
        co_return true;
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

    std::int64_t QuicClientConnection::openStream()
    {
        if (!isReady())
        {
            return -1;
        }
        return m_connection->openBidirectionalStream();
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
    }

    Platform::SocketAddress QuicClientConnection::localAddress() const noexcept
    {
        return m_socket != nullptr ? m_socket->localAddress() : Platform::SocketAddress{};
    }

    void QuicClientConnection::noteStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        IncomingStreamState &state = m_incoming[streamId];
        state.receivedBytes.insert(state.receivedBytes.end(), data.begin(), data.end());
        state.isEndStreamReceived = state.isEndStreamReceived || isEndStream;
    }

} // namespace AsynGyanis::Net
