#include "Net/Quic/QuicConnection.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/QuicConnectionCore.h"
#include "Net/Quic/Streams/QuicStreamLayer.h"

#include <openssl/rand.h>

#include <cstring>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一次 flush 里最多写出多少条报文：防止窗口充裕时在一条连接上转太久
        constexpr std::size_t kMaximumDatagramsPerFlush = 64;

        /// 一轮 flush 最多转几圈：应用回调里还能继续排数据，不设上限就没有终止保证
        constexpr std::size_t kMaximumFlushRounds = 4;

        /// 服务端宣告的流量控制额度：连接级 1 MiB、单条流 256 KiB，够放一次大响应又不放任吃内存
        constexpr std::uint64_t kInitialMaximumData = 1024ULL * 1024ULL;
        constexpr std::uint64_t kInitialMaximumStreamData = 256ULL * 1024ULL;

        /// 服务端允许对端发起的流数：HTTP/3 一条连接上并发几十个请求是常态
        constexpr std::uint64_t kInitialMaximumStreams = 100;

        /**
         * @brief 取一段随机字节
         * @details 随机失败没有安全的降级路径（QUIC 的密钥与连接标识都以随机数为前提），
         *          此时交回空 vector 由调用方放弃这条连接，而不是拿全零标识去接客端
         * @param length 需要的字节数
         * @return std::vector<std::uint8_t> 随机字节；随机数不可用时为空
         */
        std::vector<std::uint8_t> randomBytes(const std::size_t length)
        {
            std::vector<std::uint8_t> bytes(length);
            if (RAND_bytes(bytes.data(), static_cast<int>(length)) != 1)
            {
                return {};
            }
            return bytes;
        }
    } // namespace

    std::unique_ptr<QuicConnection> QuicConnection::accept(const Configuration &configuration, const Platform::SocketAddress &localAddress,
                                                           const Platform::SocketAddress &peerAddress,
                                                           const std::span<const std::uint8_t> clientInitial)
    {
        if (configuration.tlsContext == nullptr || !configuration.sendDatagram)
        {
            LOG_ERROR("QuicConnection: 连接配置不完整（缺 SSL_CTX 或报文出口），连接未建立");
            return nullptr;
        }

        // 只有能解出包头的 v1 Initial 才有资格起一条新连接：版本不认识的、短头的都直接不要。
        // 本端此刻还没装任何密钥，也不要去猜别的形态（RFC 9000 §5.2.2、§17.2.5）
        const std::expected<QuicPacketHeader, QuicDecodeError> decodedHeader = decodeQuicPacketHeader(clientInitial, kSourceConnectionIdLength);
        if (!decodedHeader.has_value() || !decodedHeader->isLongHeader || decodedHeader->longPacketType != QuicLongPacketType::Initial ||
            decodedHeader->version != kQuicVersion1)
        {
            LOG_DEBUG("QuicConnection: 报文不可接受（不是 v1 的 Initial），已丢弃");
            return nullptr;
        }
        const QuicPacketHeader &header = *decodedHeader;

        std::vector<std::uint8_t> sourceConnectionIdBytes = randomBytes(kSourceConnectionIdLength);
        if (sourceConnectionIdBytes.empty())
        {
            LOG_ERROR("QuicConnection: 生成连接标识失败（随机数不可用），连接未建立");
            return nullptr;
        }

        QuicConnectionCoreConfiguration coreConfiguration;
        coreConfiguration.tlsContext = configuration.tlsContext;
        coreConfiguration.localConnectionId = std::move(sourceConnectionIdBytes);
        // 回包要打在客户端**自报**的源标识上（RFC 9000 §7.2）；填成报文里的目的标识会让对端
        // 把所有回包当成无主报文丢掉
        coreConfiguration.peerConnectionId.assign(header.sourceConnectionId.begin(), header.sourceConnectionId.end());
        // 客户端首个 Initial 的目的标识是它自己造的：Initial 密钥与参数里的 ODCID 都由它算
        coreConfiguration.originalDestinationConnectionId.assign(header.destinationConnectionId.begin(),
                                                                 header.destinationConnectionId.end());

        QuicTransportParameters &parameters = coreConfiguration.transportParameters;
        parameters.initialMaximumData = kInitialMaximumData;
        parameters.initialMaximumStreamDataBidirectionalLocal = kInitialMaximumStreamData;
        parameters.initialMaximumStreamDataBidirectionalRemote = kInitialMaximumStreamData;
        parameters.initialMaximumStreamDataUnidirectional = kInitialMaximumStreamData;
        parameters.initialMaximumBidirectionalStreams = kInitialMaximumStreams;
        parameters.initialMaximumUnidirectionalStreams = kInitialMaximumStreams;
        parameters.maximumIdleTimeoutMilliseconds = static_cast<std::uint64_t>(configuration.idleTimeout.count());

        std::unique_ptr<QuicConnection> connection(new QuicConnection(configuration));
        connection->m_peerAddress = peerAddress;
        connection->m_localAddress = localAddress;
        connection->m_timeOrigin = std::chrono::steady_clock::now();
        connection->m_sourceConnectionId.assign(reinterpret_cast<const char *>(coreConfiguration.localConnectionId.data()),
                                                coreConfiguration.localConnectionId.size());
        try
        {
            connection->m_core = std::make_unique<QuicConnectionCore>(std::move(coreConfiguration));
        }
        catch (const Base::Exception &error)
        {
            // TLS 会话建不起来（上下文里没证书之类）：这一条报文不值得让服务端整体失败
            LOG_ERROR_FMT("QuicConnection: 状态机创建失败，连接未建立：{}", error.what());
            return nullptr;
        }

        LOG_DEBUG_FMT("QuicConnection: 已接受一条连接（连接标识 {} 字节，版本 0x{:08x}）", connection->m_sourceConnectionId.size(),
                      header.version);
        return connection;
    }

    QuicConnection::QuicConnection(Configuration configuration) :
        m_configuration(std::move(configuration))
    {
    }

    QuicConnection::~QuicConnection() = default;

    const std::string &QuicConnection::sourceConnectionId() const noexcept
    {
        return m_sourceConnectionId;
    }

    std::int64_t QuicConnection::openUnidirectionalStream()
    {
        if (m_core == nullptr || isClosed())
        {
            return -1;
        }
        const std::optional<std::uint64_t> streamId = m_core->streamLayer().openUnidirectionalStream();
        if (!streamId.has_value())
        {
            // 开不出来意味着这条连接上建不起 HTTP/3 的控制流与 QPACK 流，协议层只能降级不用
            LOG_WARN("QuicConnection: 打开本端单向流失败（对端给的单向流额度已用尽），HTTP/3 的控制流与 QPACK 流无法建立");
            return -1;
        }
        m_needsFlush = true;
        return static_cast<std::int64_t>(*streamId);
    }

    void QuicConnection::extendReceiveWindow(const std::int64_t streamId, const std::size_t consumedByteCount)
    {
        if (m_core == nullptr || consumedByteCount == 0 || streamId < 0)
        {
            return;
        }
        // 流级与连接级两本账一起还：只还流级的话，连接级窗口迟早也会被耗光而无人察觉——
        // 症状是对端安静地不再发数据，本端看不出任何异常
        m_core->streamLayer().releaseReceiveWindow(static_cast<std::uint64_t>(streamId), consumedByteCount);
        m_needsFlush = true;
    }

    void QuicConnection::queueStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
    {
        if (m_core == nullptr || streamId < 0)
        {
            return;
        }
        const std::size_t acceptedByteCount = m_core->streamLayer().writeStreamData(static_cast<std::uint64_t>(streamId), data, endStream);
        if (acceptedByteCount == 0 && !data.empty())
        {
            // 收不进通常是对端用 STOP_SENDING 叫停了这条流，或它超出了对端给的流数上限：响应没地方去
            LOG_DEBUG_FMT("QuicConnection: 流 {} 拒收了 {} 字节待发数据（流已收尾、被打断或超出对端给的流数上限）", streamId,
                          data.size());
            return;
        }
        m_needsFlush = true;
    }

    Core::Task<> QuicConnection::handleDatagram(const Platform::SocketAddress &peerAddress, const std::span<const std::uint8_t> datagram)
    {
        if (m_core == nullptr || m_isClosed)
        {
            co_return;
        }

        // 对端地址变了（NAT 重绑定）：本实现不做路径迁移，但回包仍要打到最新来源地址上，
        // 否则对端换了端口之后再也收不到东西
        if (peerAddress.length != m_peerAddress.length ||
            std::memcmp(&peerAddress.storage, &m_peerAddress.storage, peerAddress.length) != 0)
        {
            m_peerAddress = peerAddress;
        }

        const std::expected<void, QuicDecodeError> handled = m_core->onDatagramReceived(datagram, currentTime());
        if (!handled.has_value())
        {
            LOG_DEBUG_FMT("QuicConnection: 报文不合协议（{}），已按协议收口", handled.error().message);
        }
        // 交付排在 drive 之前：应用层是收到数据才排响应的，先 drive 就白跑一轮
        pumpStreamCallbacks();
        m_core->drive(currentTime());
        logHandshakeCompletionOnce();
        pumpStreamCallbacks();
        co_await flush();
    }

    Core::Task<> QuicConnection::flush()
    {
        if (m_core == nullptr || m_isClosed)
        {
            co_return;
        }

        // 同一条连接会被两条协程驱动 flush：收报文那条与定时器那条。两次 flush 交错进行会让
        // 产出队列被两头同时掏，一边刚判完「还剩这一包」就被另一边发走，随后按过期的判断再写一遍
        if (m_isFlushing)
        {
            m_hasFlushRequest = true;
            co_return;
        }

        // 本函数里早退的 co_return 有好几处，用作用域卫兵保证无论从哪条路退出都会复位标记
        struct FlushScope
        {
            explicit FlushScope(bool &isFlushing) : m_isFlushing(isFlushing)
            {
                m_isFlushing = true;
            }

            ~FlushScope()
            {
                m_isFlushing = false;
            }

            bool &m_isFlushing; ///< 被看管的标记
        };
        const FlushScope flushScope(m_isFlushing);

        for (std::size_t round = 0; round < kMaximumFlushRounds; ++round)
        {
            // 本轮会把当前攒下的都取走：标记在这里清掉，之后再有人排数据会重新置起来
            m_needsFlush = false;
            m_core->drive(currentTime());
            logHandshakeCompletionOnce();
            // 回调里可能又写下响应或归还额度，drive 得再转一圈才编得出去
            pumpStreamCallbacks();

            std::size_t sentDatagramCount = 0;
            while (sentDatagramCount < kMaximumDatagramsPerFlush)
            {
                const std::optional<std::vector<std::uint8_t>> datagram = m_core->takeOutboundDatagram();
                if (!datagram.has_value())
                {
                    break;
                }
                ++sentDatagramCount;
                if (!co_await m_configuration.sendDatagram(m_peerAddress, datagram->data(), datagram->size()))
                {
                    LOG_WARN("QuicConnection: 报文发送失败（对端可能已不可达），连接收口");
                    m_isClosed = true;
                    co_return;
                }
            }
            if (sentDatagramCount == kMaximumDatagramsPerFlush)
            {
                // 一轮没排空：留给下一轮，别在同一次调用里无限写下去
                m_needsFlush = true;
            }
            if (!std::exchange(m_hasFlushRequest, false) && !m_needsFlush)
            {
                break;
            }
        }
    }

    std::chrono::steady_clock::time_point QuicConnection::nextExpiry() const noexcept
    {
        if (m_core == nullptr)
        {
            return std::chrono::steady_clock::time_point::max();
        }
        const std::optional<std::chrono::microseconds> deadline = m_core->nextTimeout();
        if (!deadline.has_value())
        {
            return std::chrono::steady_clock::time_point::max();
        }
        return m_timeOrigin + *deadline;
    }

    Core::Task<> QuicConnection::handleExpiry()
    {
        if (m_core == nullptr || m_isClosed)
        {
            co_return;
        }
        m_core->onTimeout(currentTime());
        pumpStreamCallbacks();
        co_await flush();
    }

    void QuicConnection::requestClose() noexcept
    {
        m_isClosed = true;
    }

    bool QuicConnection::isClosed() const noexcept
    {
        // 状态机判定的空闲超时是「静默关闭」：不发收口报文也不留待发，此时 isFinished 即为真
        return m_core == nullptr || m_isClosed || m_core->isFinished();
    }

    bool QuicConnection::needsFlush() const noexcept
    {
        return m_needsFlush;
    }

    void QuicConnection::logHandshakeCompletionOnce()
    {
        if (m_isHandshakeLogged || m_core == nullptr || m_core->phase() != QuicConnectionPhase::Established)
        {
            return;
        }
        m_isHandshakeLogged = true;
        LOG_INFO_FMT("QuicConnection: 握手完成（连接标识 {} 字节，ALPN {}）", m_sourceConnectionId.size(),
                     m_core->selectedApplicationProtocol());
    }

    void QuicConnection::pumpStreamCallbacks()
    {
        QuicStreamLayer &streams = m_core->streamLayer();
        while (const std::optional<QuicStreamDelivery> delivery = streams.takeDelivery())
        {
            if (m_configuration.onStreamData)
            {
                m_configuration.onStreamData(*this, static_cast<std::int64_t>(delivery->streamId), delivery->bytes, delivery->isFinal);
            }
        }
        while (const std::optional<std::uint64_t> abortedStreamId = streams.takeAbortedStream())
        {
            notifyPeerStreamClosed(static_cast<std::int64_t>(*abortedStreamId));
        }
    }

    void QuicConnection::notifyPeerStreamClosed(const std::int64_t streamId)
    {
        // 只转交对端发起的双向流（流号低两位为 0）：请求跑在这类流上，控制流与 QPACK 流
        // 无论收口还是重置都有自己的规矩（RFC 9114 §6.2.1），不该被当成「请求被取消」
        if (streamId < 0 || (streamId & 0x03) != 0)
        {
            return;
        }
        if (m_configuration.onPeerStreamClosed)
        {
            m_configuration.onPeerStreamClosed(*this, streamId);
        }
    }

    std::chrono::microseconds QuicConnection::currentTime() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_timeOrigin);
    }
} // namespace AsynGyanis::Net
