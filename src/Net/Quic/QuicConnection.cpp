#include "Net/Quic/QuicConnection.h"

#include "Net/Quic/QuicSupport.h"
#include "Base/Exception/Exception.h"
#include "Core/Crypto/Random.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LogThrottle.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/QuicConnectionCore.h"
#include "Net/Quic/Streams/QuicStreamLayer.h"
#include "Platform/IO/DatagramSocket.h"


#include <cstring>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        using QuicDetail::randomBytes;

        /// 一次 flush 里最多写出多少条报文：防止窗口充裕时在一条连接上转太久
        constexpr std::size_t kMaximumDatagramsPerFlush = 64;

        /// 一轮 flush 最多转几圈：应用回调里还能继续排数据，不设上限就没有终止保证
        constexpr std::size_t kMaximumFlushRounds = 4;

        /// 服务端宣告的流量控制额度：连接级 1 MiB、单条流 256 KiB，够放一次大响应又不放任吃内存
        constexpr std::uint64_t kInitialMaximumData       = 1024ULL * 1024ULL;
        constexpr std::uint64_t kInitialMaximumStreamData = 256ULL * 1024ULL;

        /// 服务端允许对端发起的流数：HTTP/3 一条连接上并发几十个请求是常态
        constexpr std::uint64_t kInitialMaximumStreams = 100;

        /// 空闲超时越界的告警窗口：这条判据每建一条连接都走一次，配置错了就是每条都错，不限流会自己把日志刷爆
        constexpr std::chrono::hours kInvalidIdleTimeoutLogWindow{1};

        /**
         * @brief 把本端要宣告的流量控制额度与空闲超时填进状态机配置
         * @details 两个角色共用同一份宣告值：这些额度管的是「本端愿意收多少、允许对端开几条流」，
         *          与谁是发起方无关。写成一处是因为它们必须逐字一致——服务端与客户端各抄一份的话，
         *          改一处漏一处的结果是同一套限额在两个方向上不同，而这种差别没人会去测
         * @param coreConfiguration 待填的状态机配置
         * @param configuration 外壳配置，取其中的 idleTimeout
         */
        void announceLocalLimits(QuicConnectionCoreConfiguration &coreConfiguration, const QuicConnection::Configuration &configuration)
        {
            QuicTransportParameters &parameters                    = coreConfiguration.transportParameters;
            parameters.initialMaximumData                          = kInitialMaximumData;
            parameters.initialMaximumStreamDataBidirectionalLocal  = kInitialMaximumStreamData;
            parameters.initialMaximumStreamDataBidirectionalRemote = kInitialMaximumStreamData;
            parameters.initialMaximumStreamDataUnidirectional      = kInitialMaximumStreamData;
            parameters.initialMaximumBidirectionalStreams          = kInitialMaximumStreams;
            parameters.initialMaximumUnidirectionalStreams         = kInitialMaximumStreams;
            // 空闲超时必须先落在可编码域里：负值直接换算会得到约 1.8×10^19，超过 QUIC 变长整数的上限
            // 2^62-1，而编码器在为服务端 Initial 写这份参数时抛异常——那是在事件循环线程上、连接建立的
            // 中途，看着像「循环崩了」而不是「配置写错了」。越界时按 RFC 9000 §18.2 交 0（该参数的语义
            // 就是「不启用空闲超时」），并出声点名；告警按小时限流，因为每条连接都会走这一判据
            if (isEncodableIdleTimeout(configuration.idleTimeout))
            {
                parameters.maximumIdleTimeoutMilliseconds = idleTimeoutParameterMilliseconds(configuration.idleTimeout);
            } else
            {
                if (auto &throttle = ASYN_LOG_THROTTLED(kInvalidIdleTimeoutLogWindow); throttle.acquire())
                {
                    LOG_WARN_FMT("QuicConnection: 配置的空闲超时 {} 毫秒不在 QUIC 可编码域 [0, 2^62-1] 内，本端按 RFC 9000 §18.2 "
                                 "宣告 0（不启用空闲超时）；负值请改成 0 或一个正数",
                                 configuration.idleTimeout.count());
                }
                parameters.maximumIdleTimeoutMilliseconds = 0;
            }
        }

        /// 客户端第一个 Initial 里那个自造目的标识的长度：RFC 9000 §7.2 只要求「至少 8 字节以免撞车」，
        /// 取 16 是给同一台机器上的大量出站连接留出足够的区分度
        constexpr std::size_t kInitialPeerConnectionIdLength = 16;

        /// 来源地址表的容量：路径验证至多同时用三条（当前路径、候选、§9.3.3 的旧路径复验），留一倍余量
        constexpr std::size_t kMaximumRememberedPathAddressCount = 8;

        /// 「查不到路径地址」那条告警的限流窗口：错配要么不发生，要么每条待发都发生，不限流会刷爆日志
        constexpr std::chrono::minutes kUnknownPathAddressLogWindow{1};

        /// 路径地址表与覆盖路径标识长度的约定必须一致：折出来的键就按这个长度比（库层各改各的会静默对不上）
        static_assert(kQuicPathKeyByteLength == Core::InetAddress::kCanonicalByteCount, "路径标识长度必须跟地址规范化字节的定长一致");

        /**
         * @brief 把一条来源地址折成路径标识（RFC 9000 §9.3 的「路径」：本端地址与对端地址的二元组，
         *        收包侧只看对端那一半）
         * @details 折的是 `Core::InetAddress::canonicalBytes()` 的规范化字节，不是 sockaddr 的原始字节：
         *          后者带着 sin_zero 之类的填充字段，同一个地址在不同收包路径下这些字节的值可能不同
         *          （本仓实测过，Retry 令牌曾因此偶发验不过）
         * @param address 平台层收来的对端地址
         * @return QuicPathKey 对状态机不透明的路径标识
         */
        QuicPathKey quicPathKeyOf(const Platform::SocketAddress &address)
        {
            return Core::InetAddress(address.storage, address.length).canonicalBytes();
        }
    } // namespace

    std::unique_ptr<QuicConnection> QuicConnection::accept(const Configuration &configuration, const Platform::SocketAddress &localAddress,
                                                           const Platform::SocketAddress &peerAddress, const std::span<const std::uint8_t> clientInitial,
                                                           const std::optional<RetryContext> &retryContext)
    {
        if (configuration.tlsContext == nullptr || (!configuration.sendDatagram && !configuration.sendDatagramBatch))
        {
            // 点名缺的是哪一件：原先「缺 SSL_CTX 或报文出口」这种写法等于把两种完全不同的
            // 装配错误合成一条看不出名目的告警。报文出口给逐条与批次两条口任一即可
            LOG_ERROR_FMT("QuicConnection: 连接配置不完整（缺{}），连接未建立",
                          configuration.tlsContext == nullptr ? " TLS 上下文" : " 报文出口（sendDatagram 或 sendDatagramBatch）");
            return nullptr;
        }

        // 只有能解出包头的 v1 Initial 才有资格起一条新连接：版本不认识的、短头的都直接不要。
        // 本端此刻还没装任何密钥，也不要去猜别的形态（RFC 9000 §5.2.2、§17.2.5）
        const QuicDecodeExpected<QuicPacketHeader> decodedHeader = decodeQuicPacketHeader(clientInitial, kSourceConnectionIdLength);
        if (!decodedHeader.has_value() || !decodedHeader->isLongHeader || decodedHeader->longPacketType != QuicLongPacketType::Initial || decodedHeader->version != kQuicVersion1)
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

        // 走过 Retry 的这一条：本端标识就是 Retry 里给出去的那一条，参数与 Initial 密钥要的原始目的标识
        // 由令牌恢复。长度不合（不是 kSourceConnectionIdLength）说明交来的凭据不是本端签的那条标识，
        // 短头又没有长度字段，收下就会让后续报文再也认不回来——宁可不建这条连接
        if (retryContext.has_value() && retryContext->retrySourceConnectionId.size() != kSourceConnectionIdLength)
        {
            LOG_ERROR_FMT("QuicConnection: Retry 凭据里的源标识有 {} 字节，与本端约定的 {} 字节不符，连接未建立", retryContext->retrySourceConnectionId.size(),
                          kSourceConnectionIdLength);
            return nullptr;
        }
        if (retryContext.has_value())
        {
            sourceConnectionIdBytes = retryContext->retrySourceConnectionId;
        }

        QuicConnectionCoreConfiguration coreConfiguration;
        coreConfiguration.role              = QuicConnectionRole::Server;
        coreConfiguration.tlsContext        = configuration.tlsContext;
        coreConfiguration.localConnectionId = std::move(sourceConnectionIdBytes);
        // 回包要打在客户端自报的源标识上（RFC 9000 §7.2）；填成报文里的目的标识会让对端
        // 把所有回包当成无主报文丢掉
        coreConfiguration.peerConnectionId.assign(header.sourceConnectionId.begin(), header.sourceConnectionId.end());
        // 客户端首个 Initial 的目的标识是它自己造的：Initial 密钥与参数里的 ODCID 都由它算
        coreConfiguration.originalDestinationConnectionId.assign(header.destinationConnectionId.begin(), header.destinationConnectionId.end());
        if (retryContext.has_value())
        {
            // 走过 Retry：报文里的目的标识已是本端给出去的那一条，ODCID 得用令牌恢复的那个原值；
            // 而 Initial 的包密钥跟着换成本端给的那一条（§17.2.5.2），两件事都由核心按这两格算
            coreConfiguration.originalDestinationConnectionId = retryContext->originalDestinationConnectionId;
            coreConfiguration.retrySourceConnectionId         = retryContext->retrySourceConnectionId;
        }
        // 本平台能不能按单条报文读写 IP 的 ECN 字段是平台事实（Windows 两侧都没有入口），由外壳带给状态机：
        // 状态机自己是纯计算件，让它去问平台就等于让用例的结果跟着编译它的那台机器变。理由见配置里那一格
        coreConfiguration.supportsPerDatagramEcnField = Platform::DatagramSocket::supportsPerDatagramEcnField();
        // DF 设没设上是套接字的事实，只有外壳知道：交给核心去决定要不要发路径 MTU 探针
        coreConfiguration.pathMtuProbeAllowed = configuration.pathMtuProbeAllowed;
        // 拥塞算法是调用方的选择（缺省 NewReno）：外壳不猜、原样透传给状态机
        coreConfiguration.congestionAlgorithm = configuration.congestionAlgorithm;
        // 无状态重置的派生密钥同样原样透传：服务端一份密钥喂给它的每条连接，令牌才算得出来（§10.3.2）
        coreConfiguration.statelessResetKey = configuration.statelessResetKey;
        announceLocalLimits(coreConfiguration, configuration);

        std::unique_ptr<QuicConnection> connection(new QuicConnection(configuration));
        connection->m_peerAddress  = peerAddress;
        connection->m_localAddress = localAddress;
        connection->m_timeOrigin   = std::chrono::steady_clock::now();
        // 服务端这一侧要跟来源地址的变化（§9.3：报文发往哪，最终由状态机按「编号最高的非探测包」定）
        connection->m_followsPeerAddressOnReceive = true;
        connection->m_sourceConnectionId.assign(reinterpret_cast<const char *>(coreConfiguration.localConnectionId.data()), coreConfiguration.localConnectionId.size());
        connection->wireCoreNotifications(coreConfiguration);
        try
        {
            connection->m_core = std::make_unique<QuicConnectionCore>(std::move(coreConfiguration));
        } catch (const Base::Exception &error)
        {
            // TLS 会话建不起来（上下文里没证书之类）：这一条报文不值得让服务端整体失败
            LOG_ERROR_FMT("QuicConnection: 状态机创建失败，连接未建立：{}", error.what());
            return nullptr;
        }

        LOG_DEBUG_FMT("QuicConnection: 已接受一条连接（连接标识 {} 字节，版本 0x{:08x}）", connection->m_sourceConnectionId.size(), header.version);
        return connection;
    }

    void QuicConnection::wireCoreNotifications(QuicConnectionCoreConfiguration &coreConfiguration)
    {
        // 三条通知都要转一道：核心给的是「哪条标识/哪条路径」，外壳的调用方要的是「属于哪条连接」，
        // 而连接对象此刻才存在（配置在它之前建好，回调又要指回它），所以只能在这里接
        if (m_configuration.onConnectionIdIssued)
        {
            coreConfiguration.onConnectionIdIssued = [this](const std::span<const std::uint8_t> connectionId) { m_configuration.onConnectionIdIssued(*this, connectionId); };
        }
        if (m_configuration.onConnectionIdRetired)
        {
            coreConfiguration.onConnectionIdRetired = [this](const std::span<const std::uint8_t> connectionId) { m_configuration.onConnectionIdRetired(*this, connectionId); };
        }
        // 路径验证的两条事件转成日志（核心不打日志，理由是纯计算件）：地址文本从本类的来源表里取
        coreConfiguration.onPeerPathValidated = [this](const QuicPathKey &pathKey)
        { LOG_INFO_FMT("QuicConnection: 对端新地址 {} 验证通过（RFC 9000 §8.2.3），后续报文发往这里", pathAddressText(pathKey)); };
        coreConfiguration.onPeerPathAbandoned = [this](const QuicPathKey &pathKey)
        { LOG_WARN_FMT("QuicConnection: 对端新地址 {} 验证没通过（RFC 9000 §8.2.4），退回上一个已验证地址", pathAddressText(pathKey)); };
    }

    std::unique_ptr<QuicConnection> QuicConnection::connect(const Configuration &configuration, const Platform::SocketAddress &localAddress,
                                                            const Platform::SocketAddress &peerAddress, const QuicClientTlsSettings &clientTlsSettings)
    {
        if (configuration.tlsContext == nullptr || (!configuration.sendDatagram && !configuration.sendDatagramBatch))
        {
            LOG_ERROR("QuicConnection: 连接配置不完整（缺 SSL_CTX 或报文出口），出站连接未建立");
            return nullptr;
        }

        std::vector<std::uint8_t> sourceConnectionIdBytes        = randomBytes(kSourceConnectionIdLength);
        std::vector<std::uint8_t> initialDestinationConnectionId = randomBytes(kInitialPeerConnectionIdLength);
        if (sourceConnectionIdBytes.empty() || initialDestinationConnectionId.empty())
        {
            LOG_ERROR("QuicConnection: 生成连接标识失败（随机数不可用），出站连接未建立");
            return nullptr;
        }

        QuicConnectionCoreConfiguration coreConfiguration;
        coreConfiguration.role              = QuicConnectionRole::Client;
        coreConfiguration.tlsContext        = configuration.tlsContext;
        coreConfiguration.localConnectionId = std::move(sourceConnectionIdBytes);
        // 第一个 Initial 打给谁由本端自造；它同时是 Initial 密钥的推导依据与参数里的 ODCID
        // （RFC 9000 §7.3 要求服务端把同一个值回传，客户端据此核对这条连接是不是自己发起的那一条）。
        // 收到服务端的长头报文后状态机会按 §7.2 把目的标识换成它自报的那个，这里只是起点
        coreConfiguration.peerConnectionId                = initialDestinationConnectionId;
        coreConfiguration.originalDestinationConnectionId = std::move(initialDestinationConnectionId);
        coreConfiguration.clientTlsSettings               = clientTlsSettings;
        // 同服务端那一侧：按平台事实决定这条连接用不用 ECN
        coreConfiguration.supportsPerDatagramEcnField = Platform::DatagramSocket::supportsPerDatagramEcnField();
        // DF 设没设上是套接字的事实，只有外壳知道：交给核心去决定要不要发路径 MTU 探针
        coreConfiguration.pathMtuProbeAllowed = configuration.pathMtuProbeAllowed;
        // 拥塞算法是调用方的选择（缺省 NewReno）：外壳不猜、原样透传给状态机
        coreConfiguration.congestionAlgorithm = configuration.congestionAlgorithm;
        // 无状态重置的派生密钥同样原样透传：服务端一份密钥喂给它的每条连接，令牌才算得出来（§10.3.2）
        coreConfiguration.statelessResetKey = configuration.statelessResetKey;
        announceLocalLimits(coreConfiguration, configuration);

        std::unique_ptr<QuicConnection> connection(new QuicConnection(configuration));
        connection->m_peerAddress  = peerAddress;
        connection->m_localAddress = localAddress;
        connection->m_timeOrigin   = std::chrono::steady_clock::now();
        connection->m_sourceConnectionId.assign(reinterpret_cast<const char *>(coreConfiguration.localConnectionId.data()), coreConfiguration.localConnectionId.size());
        try
        {
            connection->m_core = std::make_unique<QuicConnectionCore>(std::move(coreConfiguration));
        } catch (const Base::Exception &error)
        {
            // TLS 会话建不起来（上下文里没装信任锚、SNI/ALPN 落不上去之类）：原因照实记下来，
            // 出站失败的排查全靠这一句
            LOG_ERROR_FMT("QuicConnection: 出站连接的状态机创建失败，未发起握手：{}", error.what());
            return nullptr;
        }

        LOG_DEBUG_FMT("QuicConnection: 已就绪一条出站连接（本端连接标识 {} 字节）", connection->m_sourceConnectionId.size());
        return connection;
    }

    QuicConnection::QuicConnection(Configuration configuration) : m_configuration(std::move(configuration))
    {
    }

    QuicConnection::~QuicConnection() = default;

    const std::string &QuicConnection::sourceConnectionId() const noexcept
    {
        return m_sourceConnectionId;
    }

    std::string QuicConnection::remoteAddress() const
    {
        if (m_peerAddress.length == 0)
        {
            // 还没有任何带来源的报文落到本连接上：给空串而不是一个半截地址
            return {};
        }
        return Core::InetAddress(m_peerAddress.storage, m_peerAddress.length).toString();
    }

    Core::TlsPeerIdentityPtr QuicConnection::peerIdentity() const
    {
        return m_core != nullptr ? m_core->peerIdentity() : nullptr;
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

    std::int64_t QuicConnection::openBidirectionalStream()
    {
        if (m_core == nullptr || isClosed())
        {
            return -1;
        }
        const std::optional<std::uint64_t> streamId = m_core->streamLayer().openBidirectionalStream();
        if (!streamId.has_value())
        {
            // 对端参数还没到手、或它给的双向流额度已用尽：这两种都开不出请求流，日志要说清是哪一种，
            // 否则出站侧只会看到「请求发不出去」
            LOG_WARN_FMT("QuicConnection: 打开本端双向流失败（对端参数{}到手），出站请求无法建立", m_core->peerTransportParameters() == nullptr ? "还没" : "已");
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

    std::size_t QuicConnection::queueStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool endStream)
    {
        if (m_core == nullptr || streamId < 0)
        {
            return 0;
        }
        const std::size_t acceptedByteCount = m_core->streamLayer().writeStreamData(static_cast<std::uint64_t>(streamId), data, endStream);
        if (acceptedByteCount == 0 && !data.empty())
        {
            // 一字节也没收下：对端用 STOP_SENDING 叫停了这条流、它超出了对端给的流数上限，
            // 或该流的待发队列已到上界。前两种是响应没地方去，第三种靠上层的续交闸门
            LOG_DEBUG_FMT("QuicConnection: 流 {} 拒收了 {} 字节待发数据（流已收尾、被打断或待发队列到上界）", streamId, data.size());
            return 0;
        }
        m_needsFlush = true;
        return acceptedByteCount;
    }

    void QuicConnection::abortStream(const std::int64_t streamId, const std::uint64_t applicationErrorCode)
    {
        if (m_core == nullptr || m_isClosed || streamId < 0)
        {
            return;
        }
        // 双向流的两头各管各的：本端不再发用 RESET_STREAM，请对端别再发用 STOP_SENDING（§3.5）。
        // 方向不合法的那一头（例如本端只能发的单向流）由流层自己跳过
        QuicStreamLayer &streams = m_core->streamLayer();
        streams.resetStreamSending(static_cast<std::uint64_t>(streamId), applicationErrorCode);
        streams.stopStreamReceiving(static_cast<std::uint64_t>(streamId), applicationErrorCode);
        m_needsFlush = true;
    }

    Core::Task<> QuicConnection::handleDatagram(const Platform::SocketAddress peerAddress, const std::span<const std::uint8_t> datagram,
                                                const std::optional<std::uint8_t> ecnCodepoint)
    {
        if (m_core == nullptr || m_isClosed)
        {
            co_return;
        }

        // 来源地址折成路径标识、记进表里（状态机产出的数据报按它换回地址）：
        // 对端换地址（NAT 重绑定、迁移）之后这一格就变，标识与账都按它记（§9.3）
        const QuicPathKey pathKey = quicPathKeyOf(peerAddress);
        rememberPathAddress(pathKey, peerAddress);
        if (m_followsPeerAddressOnReceive && (peerAddress.length != m_peerAddress.length || std::memcmp(&peerAddress.storage, &m_peerAddress.storage, peerAddress.length) != 0))
        {
            // 服务端：回包仍要打到最新来源地址上（发往哪最终由状态机按「编号最高的非探测包」定）。
            // 客户端不跟（§9 的 MUST：陌生服务端地址的报文一律丢弃），它只认配置里那个服务端地址
            m_peerAddress = peerAddress;
        }

        const QuicDecodeStatus handled = m_core->onDatagramReceived(datagram, currentTime(), ecnCodepoint, pathKey);
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
            // 一轮里攒出的报文先收在这条栈上的临时缓冲里：设了批次出口就一次交出去，没设就逐条发。
            // 报文本身从核心层的队列移交过来由本 vector 持有，因此整次 await 期间地址都有效
            std::vector<OutboundDatagram> outboundBatch;
            while (sentDatagramCount < kMaximumDatagramsPerFlush)
            {
                std::optional<OutboundDatagram> datagram = m_core->takeOutboundDatagram();
                if (!datagram.has_value())
                {
                    break;
                }
                outboundBatch.push_back(std::move(*datagram));
                ++sentDatagramCount;
            }

            if (sentDatagramCount > 0)
            {
                if (!co_await sendOutboundBatch(outboundBatch))
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
            // 本层从待发队列里掏走了多少字节就通知上层一次：上层（HTTP/3）因队列到上界而留下的那段
            // 要靠这个信号续交。对端只回窗口更新、不发数据的连接不会触发任何流数据回调，缺了这一笔
            // 就是丢唤醒——生产者一直挂在背压闸门上，尽管线上早就有了地方
            if (m_core->streamLayer().takeDrainedSendByteCount() > 0 && m_configuration.onSendSpaceAvailable)
            {
                m_configuration.onSendSpaceAvailable(*this);
                // 上层多半又排进了新字节：再来一轮把它们编出去，别压到下一次触发
                m_needsFlush = true;
            }
            if (!std::exchange(m_hasFlushRequest, false) && !m_needsFlush)
            {
                break;
            }
        }
    }

    Core::Task<bool> QuicConnection::sendOutboundBatch(const std::vector<OutboundDatagram> &outboundBatch)
    {
        // 目标地址按条解析：迁移期一批里可能同时装着两个目标（新路径的正常包 + 旧路径的复验探测，
        // §9.3.3），而两条出口都只收一个地址。按标识切连续段，段内保持编帧顺序原样交出去
        std::size_t runBeginIndex = 0;
        while (runBeginIndex < outboundBatch.size())
        {
            const QuicPathKey runKey      = outboundBatch[runBeginIndex].pathKey;
            std::size_t       runEndIndex = runBeginIndex + 1;
            while (runEndIndex < outboundBatch.size() && outboundBatch[runEndIndex].pathKey == runKey)
            {
                ++runEndIndex;
            }
            const std::optional<Platform::SocketAddress> runTarget = targetAddressFor(runKey);
            if (!runTarget.has_value())
            {
                // 键是收包时登记过的，查不到只可能是表被挤掉了：宁可丢这一条，也不能发给别的地址
                if (auto &throttle = ASYN_LOG_THROTTLED(kUnknownPathAddressLogWindow); throttle.acquire())
                {
                    LOG_WARN_FMT("QuicConnection: 一条待发数据报指向没登记过的对端路径，已丢弃（过去 {} 分钟里另有 {} 条同类）", kUnknownPathAddressLogWindow.count(),
                                 throttle.droppedCount());
                }
            } else if (m_configuration.sendDatagramBatch)
            {
                // 整批一次交出：每条报文带的 ECN 标记随条目一起过去，本层不再替它改写
                const std::span<const OutboundDatagram> run(outboundBatch.data() + runBeginIndex, runEndIndex - runBeginIndex);
                if (!co_await m_configuration.sendDatagramBatch(*runTarget, run))
                {
                    co_return false;
                }
            } else
            {
                for (std::size_t index = runBeginIndex; index < runEndIndex; ++index)
                {
                    // 报文本体从队列移交到这份 vector，因此这份指针在整次 await 期间都有效
                    const OutboundDatagram &datagram = outboundBatch[index];
                    if (!co_await m_configuration.sendDatagram(*runTarget, reinterpret_cast<const std::uint8_t *>(datagram.bytes.data()), datagram.bytes.size(),
                                                               datagram.ecnCodepoint))
                    {
                        co_return false;
                    }
                }
            }
            runBeginIndex = runEndIndex;
        }
        co_return true;
    }

    void QuicConnection::rememberPathAddress(const QuicPathKey &pathKey, const Platform::SocketAddress &address)
    {
        const auto existing = std::ranges::find(m_pathAddresses, pathKey, &std::pair<QuicPathKey, Platform::SocketAddress>::first);
        if (existing != m_pathAddresses.end())
        {
            // 挪到最新一格：淘汰从最老的开始，活跃的那几条因此不会被刚来的挤掉
            std::pair<QuicPathKey, Platform::SocketAddress> entry = *existing;
            m_pathAddresses.erase(existing);
            m_pathAddresses.push_back(std::move(entry));
            m_pathAddresses.back().second = address;
            return;
        }
        if (m_pathAddresses.size() >= kMaximumRememberedPathAddressCount)
        {
            m_pathAddresses.erase(m_pathAddresses.begin());
        }
        m_pathAddresses.emplace_back(pathKey, address);
    }

    std::optional<Platform::SocketAddress> QuicConnection::targetAddressFor(const QuicPathKey &pathKey) const
    {
        if (isUnspecifiedQuicPathKey(pathKey))
        {
            // 留空那一档＝本连接的默认目标：出站客户端与「路径还没定下来」的第一批包走这里
            return m_peerAddress;
        }
        const auto found = std::ranges::find(m_pathAddresses, pathKey, &std::pair<QuicPathKey, Platform::SocketAddress>::first);
        if (found == m_pathAddresses.end())
        {
            return std::nullopt;
        }
        // 拷一份返回：发送要 await，期间别的任务可能往表里加条目把这份引用挪走
        return found->second;
    }

    std::string QuicConnection::pathAddressText(const QuicPathKey &pathKey) const
    {
        const std::optional<Platform::SocketAddress> address = targetAddressFor(pathKey);
        if (!address.has_value() || address->length == 0)
        {
            return "（未登记的路径）";
        }
        return Core::InetAddress(address->storage, address->length).toString();
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

    bool QuicConnection::requestKeepalive()
    {
        if (m_core == nullptr || m_isClosed)
        {
            return false;
        }
        if (!m_core->requestKeepalive(currentTime()))
        {
            return false;
        }
        // 探针只在这一拍排进待发编舞里：不置这个标记，本拍就没有人 drive，PING 会一直欠着
        m_needsFlush = true;
        return true;
    }

    void QuicConnection::requestClose() noexcept
    {
        m_isClosed = true;
    }

    Core::Task<> QuicConnection::closeNow(const std::uint64_t errorCode, const std::string_view reasonPhrase)
    {
        if (m_core == nullptr || m_isClosed)
        {
            co_return;
        }

        LOG_INFO_FMT("QuicConnection: 本端主动收口（错误码 {}，原因：{}）", errorCode, reasonPhrase);
        // 顺序不能反：先把收口报文交给发送口，再认这条连接已收口——反过来的话 flush() 会因为
        // 标志已置而不再产出任何字节，对端就什么都收不到
        m_core->requestClose(errorCode, reasonPhrase, currentTime());
        co_await flush();
        m_isClosed = true;
        co_return;
    }

    bool QuicConnection::isClosed() const noexcept
    {
        // 状态机判定的空闲超时是「静默关闭」：不发收口报文也不留待发，此时 isFinished 即为真
        return m_core == nullptr || m_isClosed || m_core->isFinished();
    }

    bool QuicConnection::isHandshakeComplete() const noexcept
    {
        return m_core != nullptr && m_core->phase() == QuicConnectionPhase::Established;
    }

    std::string_view QuicConnection::selectedApplicationProtocol() const noexcept
    {
        return m_core == nullptr ? std::string_view{} : m_core->selectedApplicationProtocol();
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
        LOG_INFO_FMT("QuicConnection: 握手完成（连接标识 {} 字节，ALPN {}）", m_sourceConnectionId.size(), m_core->selectedApplicationProtocol());
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
        while (const std::optional<QuicAbortedStream> abortedStream = streams.takeAbortedStream())
        {
            notifyPeerStreamClosed(static_cast<std::int64_t>(abortedStream->streamId), abortedStream->applicationErrorCode, abortedStream->isResetByPeer);
        }
    }

    void QuicConnection::notifyPeerStreamClosed(const std::int64_t streamId, const std::uint64_t applicationErrorCode, const bool isResetByPeer)
    {
        // 只转交请求流（流号低两位为 0）：两种角色的请求都跑在这类流上，控制流与 QPACK 流
        // 无论收口还是重置都有自己的规矩（RFC 9114 §6.2.1），不该被当成「请求被取消」
        if (streamId < 0 || (streamId & 0x03) != 0)
        {
            return;
        }
        if (m_configuration.onPeerStreamClosed)
        {
            m_configuration.onPeerStreamClosed(*this, streamId, applicationErrorCode, isResetByPeer);
        }
    }

    std::chrono::microseconds QuicConnection::currentTime() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_timeOrigin);
    }
} // namespace AsynGyanis::Net
