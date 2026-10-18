// OpenSSL 的 Windows 头会把 windef.h 的 min/max 宏一起带进来，`std::max<...>` 会被展开成非法表达式。
// 必须在任何头文件之前定义 NOMINMAX 才能挡住这对宏：放在文件最顶部是唯一与包含顺序无关的写法。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Net/Quic/QuicConnectionCore.h"

#include "Platform/IO/DatagramSocket.h"

#include "Base/Exception/Exception.h"
#include "Core/Crypto/Random.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Crypto/Digest.h"
#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"
#include "Net/Quic/QuicPacketBuilder.h"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

#include <openssl/crypto.h>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 乱序早到的 CRYPTO 分片总量上限，超了直接收口。这是本实现自设的自我保护值，不是规范值
        constexpr std::size_t kQuicCryptoBufferByteLimit = 65536;

        /// 地址验证之前服务端最多可以回给对端多少倍已收字节（RFC 9000 §8.1）
        constexpr std::size_t kQuicAmplificationFactor = 3;

        /// 每空间跟踪的已收包号上限，超出后丢弃最小的那些（只影响 ACK 能覆盖多老的历史）
        constexpr std::size_t kQuicMaximumTrackedPacketNumbers = 4096;

        // 传输层错误码（RFC 9000 §11.1）与 TLS 告警的映射基值（RFC 9001 §4.8）
        constexpr std::uint64_t kQuicNoError                 = 0x00;
        constexpr std::uint64_t kQuicFrameEncodingError      = 0x07;
        constexpr std::uint64_t kQuicTransportParameterError = 0x08;
        constexpr std::uint64_t kQuicConnectionIdLimitError  = 0x09;
        constexpr std::uint64_t kQuicProtocolViolation       = 0x0a;
        constexpr std::uint64_t kQuicCryptoBufferExceeded    = 0x0d;
        constexpr std::uint64_t kQuicCryptoErrorBase         = 0x0100;

        /// CRYPTO 帧自身的最长帧头：类型 1 字节 + 偏移最多 8 + 长度最多 8，再留 1 字节余量
        constexpr std::size_t kQuicCryptoFrameHeaderByteLimit = 18;

        /**
         * @brief 客户端握手期一条 Initial 数据报的最小净载荷（RFC 9000 §8.1「Minimum Initial Packet Size」）
         * @details 与 §14.1 那个「端点收到的数据报小于 1200 字节即可丢弃」的门槛是同一个数：凑不够
         *          就等于把自己的握手送给对端丢。数值恰与 `kQuicMaximumDatagramPayloadByteLength` 相同，
         *          因此补足之后不会越过单数据报上限
         */
        constexpr std::size_t kQuicMinimumInitialDatagramByteLength = 1200;

        /**
         * @brief 二进制安全的字符串转字节视图
         * @param text 内容可以是任意字节
         * @return std::span<const std::uint8_t> 指向原文的视图，空串也返回空视图而非空指针
         */
        std::span<const std::uint8_t> asBytes(const std::string &text)
        {
            return text.empty() ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }

        /**
         * @brief 取一串密码学随机的字节（连接标识与无状态重置密钥都用它）
         * @details 随机失败没有安全的降级路径：拿一条可猜的标识签出去，旁观者就能提前把后续报文
         *          路由到它自己想去的地方。失败交回空 vector 由调用方处置
         * @param byteLength 要多少字节
         * @return std::vector<std::uint8_t> 取到的字节；随机数不可用时为空
         */
        /// 本连接自造无状态重置密钥的长度：取 HMAC-SHA-256 的分组宽度 32 字节。
        /// 配置里给的下限只有 16（§10.3.2 只要求不可预测的 128 位），自造这一把没有理由按下限取
        constexpr std::size_t kQuicSelfGeneratedStatelessResetKeyByteLength = 32;

        std::vector<std::uint8_t> randomBytes(const std::size_t byteLength)
        {
            // 取不到交回空 vector，由调用方处置（本类是纯计算件，不写日志也不抛出）
            try
            {
                return Core::Random::bytes(byteLength);
            } catch (const Base::Exception &)
            {
                return {};
            }
        }

        /**
         * @brief 校验配置里的连接标识没有超过 v1 上限
         * @param connectionId 待检的标识
         * @param label 出错文案里对这条标识的称呼
         * @throws Base::InvalidArgumentException 用法错误：长度超过 20 字节
         */
        void requireConnectionIdWithinLimit(const std::vector<std::uint8_t> &connectionId, const std::string_view label)
        {
            if (connectionId.size() > kQuicMaximumConnectionIdLength)
            {
                throw Base::InvalidArgumentException(
                        std::format("QUIC 连接建立失败：{}连接标识有 {} 字节，超过 v1 上限 {} 字节（RFC 9000 §5.1）", label, connectionId.size(), kQuicMaximumConnectionIdLength));
            }
        }

        /**
         * @brief 减去一个不应超过被减数的量
         * @param total 被减数
         * @param part 减数
         * @return std::size_t 差；算出负数表示「装不下」，收成 0 而不是回绕
         */
        std::size_t saturatingSubtract(const std::size_t total, const std::size_t part)
        {
            return part > total ? 0 : total - part;
        }

        /**
         * @brief 帧序列里是否含「必须被确认」的帧（RFC 9000 §13.2）
         * @details PADDING 与 ACK 单独出现时不触发确认，否则两端会互相空转发 ACK。本阶段能收到的
         *          其余帧（PING、CRYPTO、CONNECTION_CLOSE、流相关）一律算触发确认。
         * @param frame 一个已解出的帧
         * @return true 收到它就该回 ACK
         */
        bool isAcknowledgementEliciting(const QuicFrame &frame) noexcept
        {
            return !std::visit(
                    [](const auto &specificFrame)
                    {
                        using FrameType = std::remove_cvref_t<decltype(specificFrame)>;
                        return std::same_as<FrameType, QuicPaddingFrame> || std::same_as<FrameType, QuicAcknowledgementFrame>;
                    },
                    frame);
        }

        /**
         * @brief 这类帧是不是只有 1-RTT 包才允许携带
         * @details 依据是 §19 表 3 的 Pkts 列：0x04（RESET_STREAM）、0x05（STOP_SENDING）与
         *          0x08–0x1a 那一段是 `__01`（0-RTT 与 1-RTT），0x1b（PATH_RESPONSE）与
         *          0x1e（HANDSHAKE_DONE）是 `___1`（只 1-RTT）。本端没有 0-RTT 空间，两种写法
         *          在这里归并成同一件事：Initial 与 Handshake 包里出现即违规。
         * @param frameType §12.4 表 3 的取值
         * @return true 出现在 Initial 或 Handshake 包里即为 PROTOCOL_VIOLATION
         */
        bool isOneRttOnlyFrameType(const std::uint64_t frameType) noexcept
        {
            return frameType == static_cast<std::uint64_t>(QuicFrameType::ResetStream) || frameType == static_cast<std::uint64_t>(QuicFrameType::StopSending) ||
                   (frameType >= 0x08 && frameType <= 0x1b) || frameType == static_cast<std::uint64_t>(QuicFrameType::HandshakeDone);
        }

        /**
         * @brief 这一帧算不算探测帧（RFC 9000 §9.1 只列了四类：PATH_CHALLENGE、PATH_RESPONSE、
         *        NEW_CONNECTION_ID、PADDING），其余一律是非探测帧
         * @details 「带没带非探测帧」是 §9.3 判定「对端搬了家」的唯一信号：只带探测帧的报文
         *          （例如握手中途的一条 PADDING-only 包）不能把发包目标拽到另一个地址上
         * @param frame 一个已解出的帧
         * @return true 非探测帧
         */
        bool isNonProbingFrame(const QuicFrame &frame) noexcept
        {
            return !std::visit(
                    [](const auto &specificFrame)
                    {
                        using FrameType = std::remove_cvref_t<decltype(specificFrame)>;
                        return std::same_as<FrameType, QuicPaddingFrame> || std::same_as<FrameType, QuicPathChallengeFrame> || std::same_as<FrameType, QuicPathResponseFrame> ||
                               std::same_as<FrameType, QuicNewConnectionIdFrame>;
                    },
                    frame);
        }

        /// 「这条路径不受反放大上限约束」的返回值：比任何合法的剩余额度都大，参与取小不影响结论
        constexpr std::size_t kUnlimitedAmplificationByteCount = std::numeric_limits<std::size_t>::max();

        /**
         * @brief 本端出包时除帧以外占掉的字节数
         * @details 长头多带版本、源标识与 Token 长度域，且有 Length 字段；短头两者都没有（§17.3）。
         *          Length 域按 2 字节档预留：1200 的预算用不到 4 字节档，1 字节档又可能被加宽。
         * @param configuration 连接配置，取两条标识的长度
         * @param isLongHeader 本包用长头还是短头
         * @return std::size_t 包头 + 包号 + AEAD 标签 + CRYPTO 帧头上限
         */
        std::size_t packetOverheadByteLength(const QuicConnectionCoreConfiguration &configuration, const bool isLongHeader)
        {
            const std::size_t headerByteLength = isLongHeader
                                                         // 首字节、版本、两条「长度字节 + 标识」、Token 长度域、Length 域、包号
                                                         ? 1 + 4 + 1 + configuration.peerConnectionId.size() + 1 + configuration.localConnectionId.size() + 1 + 2 + 1
                                                         // 首字节、目的标识（没有长度字段）、包号
                                                         : 1 + configuration.peerConnectionId.size() + 1;
            return headerByteLength + kQuicAuthenticationTagByteLength + kQuicCryptoFrameHeaderByteLimit;
        }
        /**
         * @brief 收包路径上那两块与包长成比例的临时缓冲
         *
         * @details 解头部保护要一份可写副本、AEAD 解密要一份明文缓冲，两者都要到能算出长度之后
         *          才知道要多大——也就是说在判断「这条报文解不解得开」之前就已经付过一次分配。
         *          按线程复用之后高水位只留一份，代价是任何指向它们的视图都不许活到下一次收包：
         *          本层的交付路径（流层入队、TLS 缓冲、transport parameters 解析、错误文案）全部是
         *          拷贝入存储，因此这一条成立。状态机不跨挂起点持有这些视图（它不是协程）。
         */
        enum class ReceiveScratch : std::uint8_t
        {
            PacketCopy, ///< 整包的可写副本，只有头部那几个字节会被改写
            Plaintext,  ///< AEAD 解出来的明文帧
        };

        /// @return 本线程那份复用缓冲，调用方自行填长度
        std::vector<std::uint8_t> &receiveScratch(const ReceiveScratch kind) noexcept
        {
            thread_local std::vector<std::uint8_t> packetCopy{};
            thread_local std::vector<std::uint8_t> plaintext{};
            return kind == ReceiveScratch::PacketCopy ? packetCopy : plaintext;
        }

        /**
         * @brief 出站帧序列的复用缓冲（一包的帧先编进它，再由组包器抄进报文）
         * @details 与 `receiveScratch()` 同一套纪律：按线程一份、每包开始时清空，
         *          任何指向它的视图都不许活到下一次取用。这里成立的理由是组包器
         *          （`appendQuicPacket`）在返回前就把字节全部抄走了，中间的恢复层与拥塞层
         *          只读记账不读内容。
         * @return 本线程那份帧缓冲，调用方清空后往里追加
         */
        std::string &outboundFrameAssemblyBuffer() noexcept
        {
            thread_local std::string frames{};
            return frames;
        }

        /**
         * @brief 入向帧序列的复用缓冲（一包解出的帧先落它，再逐帧交给处理分支）
         * @details 与 `receiveScratch()` 同一套纪律：按线程一份、每次解码开头清空，因此指向其中某帧的
         *          视图不许活到下一次收包。这里成立的理由是 `handlePacket` 在返回前就把每一帧处理完，
         *          交付路径（流层入队、TLS 缓冲、错误文案）各自把要留的字节抄进自己的存储。
         * @return 本线程那份帧缓冲，解码函数负责清空
         */
        std::vector<QuicFrame> &inboundFrameScratch()
        {
            thread_local std::vector<QuicFrame> frames{};
            return frames;
        }
    } // namespace

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::spaceOf(const QuicEncryptionLevel level) noexcept
    {
        // 0-RTT 与 Initial 共用一个空间（RFC 9000 §17.2.3：0-RTT 包续用 Initial 的包号序列）
        switch (level)
        {
            case QuicEncryptionLevel::Initial:
            case QuicEncryptionLevel::ZeroRtt:
                return PacketNumberSpace::Initial;
            case QuicEncryptionLevel::Handshake:
                return PacketNumberSpace::Handshake;
            case QuicEncryptionLevel::Application:
                return PacketNumberSpace::Application;
        }
        return PacketNumberSpace::Application;
    }

    std::size_t QuicConnectionCore::spaceIndex(const QuicEncryptionLevel level) noexcept
    {
        return spaceIndex(spaceOf(level));
    }

    std::size_t QuicConnectionCore::spaceIndex(const PacketNumberSpace space) noexcept
    {
        return static_cast<std::size_t>(space);
    }

    QuicEncryptionLevel QuicConnectionCore::levelOf(const PacketNumberSpace space) noexcept
    {
        switch (space)
        {
            case PacketNumberSpace::Initial:
                return QuicEncryptionLevel::Initial;
            case PacketNumberSpace::Handshake:
                return QuicEncryptionLevel::Handshake;
            case PacketNumberSpace::Application:
                return QuicEncryptionLevel::Application;
        }
        return QuicEncryptionLevel::Application;
    }

    std::optional<QuicEncryptionLevel> QuicConnectionCore::levelOf(const QuicPacketHeader &header) noexcept
    {
        if (!header.isLongHeader)
        {
            return QuicEncryptionLevel::Application;
        }
        switch (header.longPacketType)
        {
            case QuicLongPacketType::Initial:
                return QuicEncryptionLevel::Initial;
            case QuicLongPacketType::Handshake:
                return QuicEncryptionLevel::Handshake;
            // 本端没开早数据，0-RTT 没有能解它的密钥；服务端也不会收到 Retry（§17.2.5）
            case QuicLongPacketType::ZeroRtt:
            case QuicLongPacketType::Retry:
                return std::nullopt;
        }
        return std::nullopt;
    }

    QuicConnectionCore::QuicConnectionCore(QuicConnectionCoreConfiguration configuration) :
        m_configuration(std::move(configuration)), m_streams(m_configuration.transportParameters, m_configuration.role)
    {
        // 拥塞算法在构造期一次选好（可插拔点见 makeQuicCongestionAlgorithm）：此后这条连接上
        // 的每一个窗口动作都走它，运行期不再换
        m_congestion = makeQuicCongestionAlgorithm(m_configuration.congestionAlgorithm, kQuicMaximumDatagramPayloadByteLength);

        if (m_configuration.tlsContext == nullptr)
        {
            throw Base::InvalidArgumentException("QUIC 连接建立失败：配置里的 TLS 上下文为空，调用方须先配好证书与 ALPN");
        }
        requireConnectionIdWithinLimit(m_configuration.localConnectionId, "本端签发");
        requireConnectionIdWithinLimit(m_configuration.peerConnectionId, "对端自报");
        requireConnectionIdWithinLimit(m_configuration.originalDestinationConnectionId, "客户端第一个 Initial 的目的");

        // §5.1.1：握手期那条标识就是序列号 0 的那一条，它随长头隐含发出（不必再发 NEW_CONNECTION_ID），
        // 但同样要有一条无状态重置令牌（§10.3.2）——对端从传输参数里读它，本端按同一把密钥算
        if (m_configuration.statelessResetKey.empty())
        {
            // 配置没给密钥就本连接随机取一把：单进程与单测够用，多进程 worker 要共享时由外壳显式给
            try
            {
                // 这是密钥材料（HMAC 密钥），走私有 DRBG 那一侧；取不到就留空，本连接不发重置令牌
                m_statelessResetKey = Core::Random::secretBytes(kQuicSelfGeneratedStatelessResetKeyByteLength);
            } catch (const Base::Exception &)
            {
                m_statelessResetKey.clear();
            }
        } else
        {
            m_statelessResetKey = m_configuration.statelessResetKey;
        }
        m_issuedConnectionIds.push_back(IssuedConnectionId{
                .id = m_configuration.localConnectionId, .sequence = 0, .statelessResetToken = statelessResetTokenOf(m_configuration.localConnectionId), .isAdvertised = true});
        // 服务端在构造期就知道对端第一条标识（客户端首个 Initial 的源标识，accept 时填进配置）；
        // 客户端要等服务端自报之后才知道，由 followPeerConnectionId 补种
        if (isLocalServer())
        {
            m_peerConnectionIds.push_back(PeerConnectionId{.id = m_configuration.peerConnectionId, .sequence = 0});
        }

        // §7.3 要求服务端把「收到的原始目的标识」和「自己第一个 Initial 的源标识」都写进参数：
        // 这两项由本类按配置补齐，调用方漏了也照样合法，免得每条连接都要手抄一遍再抄错。
        // 客户端一侧相反：原始目的标识只有在接住 Retry 之后才允许出现（本实现不做 Retry，故必须不写），
        // 写了会被对端按非法参数收口
        QuicTransportParameters localParameters = m_configuration.transportParameters;
        if (isLocalServer())
        {
            localParameters.originalDestinationConnectionId = m_configuration.originalDestinationConnectionId;
            // §18.2：发过 Retry 的服务端必须把 Retry 里那条源标识写进参数，客户端拿它核对这条 Retry
            // 确实出自看过自己第一个 Initial 的那台服务端；没发过 Retry 时这一项必须不出现
            if (!m_configuration.retrySourceConnectionId.empty())
            {
                localParameters.retrySourceConnectionId = m_configuration.retrySourceConnectionId;
            }
            // §10.3：服务端可以给它握手期选的那条标识配一个无状态重置令牌（§10.3.2 派生）。客户端在
            // 收到任何 NEW_CONNECTION_ID 之前只有靠它才认得出「服务端已经没了这条连接」；客户端的
            // 参数没有机密性保护，所以这一项只许服务端发（§18.2，解码侧也照此拒绝）
            localParameters.statelessResetToken = statelessResetTokenOf(m_configuration.localConnectionId);
        } else
        {
            localParameters.originalDestinationConnectionId.reset();
        }
        localParameters.initialSourceConnectionId = m_configuration.localConnectionId;
        std::string encodedParameters;
        appendQuicTransportParameters(encodedParameters, localParameters);
        m_configuration.transportParameters = std::move(localParameters);

        m_tls = std::make_unique<QuicTlsContext>(*m_configuration.tlsContext, isLocalServer(), asBytes(encodedParameters),
                                                 m_configuration.clientTlsSettings.has_value() ? &*m_configuration.clientTlsSettings : nullptr);

        // Initial 密钥只由客户端第一个 Initial 的目的标识决定（RFC 9001 §5.2），两个方向各自的标签
        // 由角色定：服务端读的是「客户端→服务端」那套，客户端反过来。走过 Retry 的那条连接例外——
        // §17.2.5.2 明写「换了目的标识就换了保护这一包的密钥」，此刻的算据是本端在 Retry 里给的源标识
        const std::vector<std::uint8_t> &initialKeyConnectionId =
                m_configuration.retrySourceConnectionId.empty() ? m_configuration.originalDestinationConnectionId : m_configuration.retrySourceConnectionId;
        const QuicPacketDirection inboundInitialDirection  = isLocalServer() ? QuicPacketDirection::ClientToServer : QuicPacketDirection::ServerToClient;
        const QuicPacketDirection outboundInitialDirection = isLocalServer() ? QuicPacketDirection::ServerToClient : QuicPacketDirection::ClientToServer;
        SpaceState               &initialSpace             = m_spaces[spaceIndex(PacketNumberSpace::Initial)];
        initialSpace.readKeys                              = deriveQuicInitialPacketKeys(initialKeyConnectionId, inboundInitialDirection);
        initialSpace.writeKeys                             = deriveQuicInitialPacketKeys(initialKeyConnectionId, outboundInitialDirection);

        // 建状态本身就是「收到了一份看起来属于本连接的报文」，因此这条连接的寿命从此刻起算，而不是
        // 从第一次解密成功起算：解不开的报文按 §10.1 不算活动，若截止时刻也只在解密成功后才亮，
        // 一个只发无法解密报文就消失的对端会让这条表项永远留在服务端的路由表里
        restartIdleTimer(Timestamp{});

        // 平台按不了标就把 ECN 这一格关掉，别让发包凭据记着「带标」而线上没带：§13.4.2.1 的校验是拿
        // 本端记的那一格比对端报上来的计数，本端记错就等于自己把一条好路判成验证失败
        if (!m_configuration.supportsPerDatagramEcnField)
        {
            m_recovery.disableEcn();
        }
    }

    bool QuicConnectionCore::isLocalServer() const noexcept
    {
        return m_configuration.role == QuicConnectionRole::Server;
    }

    bool QuicConnectionCore::isAmplificationLimited() const noexcept
    {
        // §8.1 那条上限是给「被陌生人打到地址上」的一方设的：客户端自己先出声，收到的字节可能一条
        // 都还没有，照它算连第一个 Initial 都发不出去
        if (!isLocalServer())
        {
            return false;
        }
        if (!m_isAddressValidated)
        {
            return true;
        }
        // §9.3.1：换地址之后给新地址的额度也只认从新地址收到的字节，按「当前发送路径没验证过」
        // 复用同一套判定。路径标识还没定下来的用法（单路径、留空）不套这条——那是 §8.1 在管
        if (isUnspecifiedQuicPathKey(m_sendPathKey))
        {
            return false;
        }
        const PeerPathState *const sendPathState = findPathState(m_sendPathKey);
        return sendPathState != nullptr && !sendPathState->isValidated;
    }

    void QuicConnectionCore::followPeerConnectionId(const QuicPacketHeader &header)
    {
        if (isLocalServer() || m_hasAdoptedPeerConnectionId || !header.isLongHeader || header.sourceConnectionId.empty())
        {
            return;
        }
        // §7.2：客户端只允许把长头报文里的源标识拿来当后续发包的目的标识（短头没有那个字段）。
        // 只跟第一次：服务端此后按 §5.1 可以换标识，跟错一次会把整条连接打成「对端认不出」
        m_hasAdoptedPeerConnectionId = true;
        m_configuration.peerConnectionId.assign(header.sourceConnectionId.begin(), header.sourceConnectionId.end());
        // 本端此前那条「目的标识」是自己挑的初始目的标识，不是对端签发的，因此不进集合；
        // 服务端自报的这一条才是它的序列号 0（§5.1.1）
        m_peerConnectionIds.clear();
        m_peerConnectionIds.push_back(PeerConnectionId{.id = m_configuration.peerConnectionId, .sequence = 0});
    }

    const QuicConnectionCore::PeerPathState *QuicConnectionCore::findPathState(const QuicPathKey &pathKey) const noexcept
    {
        const auto found = std::ranges::find(m_peerPaths, pathKey, &PeerPathState::key);
        return found == m_peerPaths.end() ? nullptr : &*found;
    }

    QuicConnectionCore::PeerPathState *QuicConnectionCore::pathStateFor(const QuicPathKey &pathKey) noexcept
    {
        const auto found = std::ranges::find(m_peerPaths, pathKey, &PeerPathState::key);
        if (found != m_peerPaths.end())
        {
            return &*found;
        }
        if (m_peerPaths.size() >= kMaximumTrackedPeerPaths)
        {
            // 表满：淘汰最老的一条「没在用」的——未验证、且不是当前发送路径（正在验证的候选就是当前
            // 发送路径，所以只保护它这一条就够）。全是在用的就返回空，调用方跳过这次记账：宁可
            // 少记一条账，也不能把在用的那条挤掉
            const auto evictable = std::ranges::find_if(m_peerPaths, [this](const PeerPathState &state) { return !state.isValidated && state.key != m_sendPathKey; });
            if (evictable == m_peerPaths.end())
            {
                return nullptr;
            }
            *evictable = PeerPathState{.key = pathKey};
            return &*evictable;
        }
        return &m_peerPaths.emplace_back(PeerPathState{.key = pathKey});
    }

    void QuicConnectionCore::followPeerPath(const QuicPathKey &pathKey, const PacketNumberSpace space, const std::uint64_t packetNumber, const bool carriesNonProbingFrame,
                                            const Timestamp now)
    {
        if (!m_hasAdoptedPeerPath)
        {
            // 第一条认证过的报文定下这条连接的路径：此前到达的字节可能来路不明，不能拿它们作数。
            // 全零是合法的路径键（单路径用法留空），所以「定没定过」要单独记
            m_hasAdoptedPeerPath = true;
            m_sendPathKey        = pathKey;
            return;
        }
        if (!carriesNonProbingFrame)
        {
            return; // §9.1/§9.3：只有非探测帧才算「对端搬了家」，光打挑战探测不算
        }
        std::optional<std::uint64_t> &largestNonProbing = m_largestNonProbingPacketNumbers[spaceIndex(space)];
        if (largestNonProbing.has_value() && packetNumber <= *largestNonProbing)
        {
            return; // §9.3：发包目标只跟编号最高的那一条非探测包，乱序的旧包不能把它拽回去
        }
        largestNonProbing = packetNumber;
        if (pathKey == m_sendPathKey || !isLocalServer())
        {
            return; // 还在原路上；或本端是客户端——§9 的丢弃门在 handlePacket 里已经拦了陌生地址
        }

        const QuicPathKey previousPathKey = m_sendPathKey;
        m_sendPathKey                     = pathKey;
        // §9.3：换过非探测包的目标地址之后，其它地址上的验证可以放弃
        static_cast<void>(std::erase_if(m_pendingPathChallenges, [](const PendingPathChallenge &challenge) { return challenge.isCandidate; }));
        if (const PeerPathState *const newPathState = findPathState(pathKey); newPathState == nullptr || !newPathState->isValidated)
        {
            armPathChallenge(pathKey, true, now); // §9.3 的 MUST：换了地址就要验；只有验证过的地址才允许跳过（§9.3 的 MAY）
        }
        // §9.3.3：疑似迁移时必须在原来那条活动路径上也验一次——那条挑战会把对端的真实流量引回来，
        // 离路劫持的包走得比真包快也照样会被拽回去
        if (const PeerPathState *const previousState = findPathState(previousPathKey); previousState != nullptr && previousState->isValidated)
        {
            armPathChallenge(previousPathKey, false, now);
        }
    }

    void QuicConnectionCore::armPathChallenge(const QuicPathKey &pathKey, const bool isCandidate, const Timestamp now)
    {
        // §8.2.1 的 MUST：每次挑战都用不可预测数据——可预测的话，谁抢答都算验证通过，等于没验
        const std::vector<std::uint8_t> random = randomBytes(kQuicPathValidationDataByteLength);
        if (random.size() != kQuicPathValidationDataByteLength)
        {
            return; // 随机数不可用：不发、不验证，绝不用可预测数据顶上
        }
        PendingPathChallenge challenge;
        challenge.key         = pathKey;
        challenge.isCandidate = isCandidate;
        std::copy(random.begin(), random.end(), challenge.data.begin());
        // §8.2.4 的推荐值：3×max(当前 PTO, 按 kInitialRtt 算的 PTO)。留得宽是有意的——挑战或回应
        // 丢一两条都不该把验证判死，这段时间里换一份新数据重发（§9.4 末段）
        challenge.abandonDeadline    = now + std::max(probeTimeoutPeriod(), m_recovery.initialProbeTimeout()) * 3;
        challenge.retransmitDeadline = now + probeTimeoutPeriod();
        // 同一条路径上只留最新的一条；候选至多一条（能走到这里就说明旧的候选已经让了位）
        static_cast<void>(
                std::erase_if(m_pendingPathChallenges, [&](const PendingPathChallenge &existing) { return existing.key == pathKey || (isCandidate && existing.isCandidate); }));
        if (m_pendingPathChallenges.size() >= kMaximumPendingPathChallenges)
        {
            // 表满先丢非候选的（复验那种丢了也无害）；还满就这条不发
            static_cast<void>(std::erase_if(m_pendingPathChallenges, [](const PendingPathChallenge &existing) { return !existing.isCandidate; }));
            if (m_pendingPathChallenges.size() >= kMaximumPendingPathChallenges)
            {
                return;
            }
        }
        m_pendingPathChallenges.push_back(challenge);
    }

    void QuicConnectionCore::abandonPathValidation(const QuicPathKey &pathKey)
    {
        if (m_configuration.onPeerPathAbandoned)
        {
            m_configuration.onPeerPathAbandoned(pathKey);
        }
        if (!m_hasValidatedPeerPath)
        {
            // §9.3.2 末段：一个已验证地址都没有，就没有可退的地方——静默收口，连 CLOSE 都不发
            m_phase               = QuicConnectionPhase::Closing;
            m_localCloseErrorCode = std::nullopt;
            m_localCloseReasonPhrase.clear();
            m_outboundDatagrams.clear();
            return;
        }
        // §9.3.2：退回上一个已验证地址。对端若真搬了家，它会从新地址再说话、再触发一轮跟随与验证
        m_sendPathKey = m_validatedPathKey;
    }

    void QuicConnectionCore::expirePathChallenges(const Timestamp now)
    {
        if (m_pendingPathChallenges.empty())
        {
            return;
        }
        std::vector<PendingPathChallenge> survivors;
        survivors.reserve(m_pendingPathChallenges.size());
        for (PendingPathChallenge challenge: m_pendingPathChallenges)
        {
            if (now >= challenge.abandonDeadline)
            {
                if (challenge.isCandidate)
                {
                    abandonPathValidation(challenge.key); // §8.2.4 到期放弃 → §9.3.2 退回
                }
                continue; // 复验那条（旧路 / MTU）到期只是销账，不影响连接
            }
            if (challenge.isCandidate && now >= challenge.retransmitDeadline)
            {
                if (!challenge.isSendPending)
                {
                    // §13.3 对 PATH_CHALLENGE 的要求是「换一份新数据重发」：同一份重发没有意义（对端可能
                    // 已经回过、也可能永远回不来）；§9.4 末段给的动作也正是「发一条新挑战并把计时拉长」
                    if (const std::vector<std::uint8_t> fresh = randomBytes(kQuicPathValidationDataByteLength); fresh.size() == kQuicPathValidationDataByteLength)
                    {
                        std::copy(fresh.begin(), fresh.end(), challenge.data.begin());
                        challenge.isSendPending = true;
                    }
                }
                // 计时无论如何都要往后推：还压着没发出去（窗口或额度不够）时也一样，否则这个时刻
                // 永远留在过去，外壳的闹钟会一拍接一拍地空转
                challenge.retransmitDeadline = now + probeTimeoutPeriod();
            }
            survivors.push_back(challenge);
        }
        m_pendingPathChallenges = std::move(survivors);
    }

    void QuicConnectionCore::queuePathValidationPackets(const Timestamp now)
    {
        // 两类帧都只许出现在 1-RTT 包里（§19 表 3 的 Pkts 列）：应用写密钥还没就位就什么都发不出，
        // 待发的那些留着——挑战要等到 1-RTT 就绪的那一刻发，而不是丢掉
        if (!m_spaces[spaceIndex(PacketNumberSpace::Application)].writeKeys.has_value())
        {
            return;
        }
        const std::size_t overheadByteLength = packetOverheadByteLength(m_configuration, false);

        // ① 欠着的 PATH_RESPONSE：逐条回应，且必须打在挑战来的那条路径上（§8.2.2）
        if (!m_pendingPathResponses.empty())
        {
            std::vector<PendingPathResponse> deferred;
            for (const PendingPathResponse &response: m_pendingPathResponses)
            {
                std::string frames;
                appendQuicFrame(frames, QuicFrame{QuicPathResponseFrame{response.data}});
                const std::size_t packetByteLength = overheadByteLength + frames.size();
                if (packetByteLength > amplificationLimitForPath(response.key) || !m_congestion->maySend(packetByteLength))
                {
                    // 额度或窗口腾不出来：留到下一轮。§8.2.2 允许回应被这两者压后
                    deferred.push_back(response);
                    continue;
                }
                std::optional<DatagramPadding> padding;
                if (canExpandPathValidationDatagram(response.key))
                {
                    padding = DatagramPadding{kQuicMinimumInitialDatagramByteLength, false};
                }
                emitPacket(PacketNumberSpace::Application, frames, now, true, std::nullopt, {}, false, {}, false, padding, response.key);
            }
            m_pendingPathResponses = std::move(deferred);
        }

        // ② 待发的 PATH_CHALLENGE：一条一包（§8.2.1 明写一个包里不该放多条挑战）
        for (PendingPathChallenge &challenge: m_pendingPathChallenges)
        {
            if (!challenge.isSendPending)
            {
                continue;
            }
            std::string frames;
            appendQuicFrame(frames, QuicFrame{QuicPathChallengeFrame{challenge.data}});
            const std::size_t packetByteLength = overheadByteLength + frames.size();
            if (packetByteLength > amplificationLimitForPath(challenge.key) || !m_congestion->maySend(packetByteLength))
            {
                continue; // 下一轮再试；放弃时限照旧在走（§8.2.4 只说到期放弃，没说这期间必须一直在试）
            }
            std::optional<DatagramPadding> padding;
            if (canExpandPathValidationDatagram(challenge.key))
            {
                padding = DatagramPadding{kQuicMinimumInitialDatagramByteLength, false};
            }
            emitPacket(PacketNumberSpace::Application, frames, now, true, std::nullopt, {}, false, {}, false, padding, challenge.key);
            challenge.isSendPending      = false;
            challenge.wasExpanded        = padding.has_value();
            challenge.sentTime           = now;
            challenge.retransmitDeadline = now + probeTimeoutPeriod();
        }
    }

    void QuicConnectionCore::confirmPeerPathValidated(const QuicPathKey &pathKey, const bool wasExpanded, const Timestamp now)
    {
        if (PeerPathState *const state = pathStateFor(pathKey); state != nullptr)
        {
            state->isValidated = true;
        }
        m_hasValidatedPeerPath = true;
        m_validatedPathKey     = pathKey;
        // §9.4 的 MUST：确认对端对新地址的所有权之后，立刻把拥塞控制器与 RTT 估算重置回初值。
        // 「只换了端口」那一档规范允许保留旧状态（NAT 重绑最常见的正是这种），但那要拿两个地址
        // 的 IP 比对——核心刻意不认识地址，这半条 MAY 就不做：一律重置仍是规范下侧合规的写法
        m_congestion->resetToInitialState();
        m_recovery.resetForPathChange();
        if (m_configuration.onPeerPathValidated)
        {
            m_configuration.onPeerPathValidated(pathKey);
        }
        // §8.2.3 末段：第一轮挑战没能扩到 1200（额度不够）的话，路径通了但 MTU 还没验过，
        // 必须再验一轮扩充的；这一刻路径已验证，反放大上限已经不在
        if (!wasExpanded)
        {
            armPathChallenge(pathKey, false, now);
        }
    }

    void QuicConnectionCore::queuePathResponse(const QuicPathKey &pathKey, const std::array<std::uint8_t, kQuicPathValidationDataByteLength> &data)
    {
        if (m_pendingPathResponses.size() >= kMaximumPendingPathResponses)
        {
            return; // 防滥用：对端在一个包里塞满挑战就等于拿本端当放大器（aioquic 为同一理由设了同量级的上限）
        }
        m_pendingPathResponses.push_back(PendingPathResponse{.key = pathKey, .data = data});
    }

    void QuicConnectionCore::handlePathResponseFrame(const QuicPathResponseFrame &frame, const Timestamp now)
    {
        const auto pending = std::ranges::find_if(m_pendingPathChallenges, [&](const PendingPathChallenge &challenge) { return challenge.data == frame.data; });
        if (pending == m_pendingPathChallenges.end())
        {
            // 对不上任何一条在途挑战：忽略。可能来得太晚，也可能属于别的连接，两种都不构成对端违规
            // （§8.2.3 只写了「对上了」意味着什么）
            return;
        }
        const PendingPathChallenge matched = *pending;
        m_pendingPathChallenges.erase(pending);
        if (matched.isCandidate)
        {
            confirmPeerPathValidated(matched.key, matched.wasExpanded, now);
        }
        // 非候选（旧路径复验 / MTU 复验）对上了什么都不做：那条路径本来就是已验证的
    }

    std::size_t QuicConnectionCore::amplificationLimitForPath(const QuicPathKey &pathKey) const noexcept
    {
        std::size_t remaining = kUnlimitedAmplificationByteCount;
        if (isLocalServer() && !m_isAddressValidated)
        {
            // §8.1 的握手期总额度：回出去的字节总数不许超过已收的三倍
            remaining = saturatingSubtract(kQuicAmplificationFactor * m_receivedByteCount, m_sentByteCount);
        }
        if (isLocalServer() && !isUnspecifiedQuicPathKey(pathKey))
        {
            if (const PeerPathState *const state = findPathState(pathKey); state != nullptr && !state->isValidated)
            {
                // §9.3.1 的逐路径额度：只认从这条路径收到的字节
                remaining = std::min(remaining, saturatingSubtract(kQuicAmplificationFactor * state->receivedByteCount, state->sentByteCount));
            }
        }
        return remaining;
    }

    bool QuicConnectionCore::canExpandPathValidationDatagram(const QuicPathKey &pathKey) const noexcept
    {
        // §8.2.1/§8.2.2 都要求扩到 1200，但都留了同一条例外：反放大额度装不下就不扩（照常发小包，
        // §8.2.1 末段另要求之后补一轮扩充的验证）。拥塞窗口同样要许可——§8.2.2 的「不得延迟」
        // 以「拥塞控制所限」为界
        if (amplificationLimitForPath(pathKey) < kQuicMinimumInitialDatagramByteLength)
        {
            return false;
        }
        return m_pathMtu.maximumDatagramPayloadByteLength() >= kQuicMinimumInitialDatagramByteLength && m_congestion->maySend(kQuicMinimumInitialDatagramByteLength);
    }

    void QuicConnectionCore::confirmHandshake(const Timestamp now)
    {
        if (m_isHandshakeConfirmed)
        {
            return;
        }
        m_isHandshakeConfirmed = true;
        // 握手顺带把对端地址验了一遍（§8.1.4 的路径验证，两个方向各自成立）：此刻在用的这条路径
        // 从此算已验证——它也是 §9.3.2 里「上一个已验证地址」的第一个候选。此后对端若从别处说话，
        // 验证失败时才有地方可退
        if (m_hasAdoptedPeerPath && !m_hasValidatedPeerPath)
        {
            m_hasValidatedPeerPath = true;
            m_validatedPathKey     = m_sendPathKey;
            if (PeerPathState *const state = pathStateFor(m_sendPathKey); state != nullptr)
            {
                state->isValidated = true;
            }
        }
        const std::uint64_t peerMaximumDelayMilliseconds =
                m_peerParameters.has_value() ? m_peerParameters->maximumAcknowledgmentDelayMilliseconds : kQuicDefaultMaximumAcknowledgmentDelayMilliseconds;
        m_recovery.onHandshakeConfirmed(std::chrono::duration_cast<QuicTime>(std::chrono::milliseconds{peerMaximumDelayMilliseconds}));
        // §14.3.1：BASE 态的起点就是握手完成。BASE_PLPMTU（1200）在这条路上已经被 §14.1 的补足要求真实
        // 确认过了，所以状态机不必为它单发一条探针，直接从这里开始往更大的尺寸探。DF 没设上时整台机器
        // 留在 Disabled：不探就不需要那两个计时器，也就不会有定时器为一条探不了的路一直亮着
        if (m_configuration.pathMtuProbeAllowed)
        {
            m_pathMtu.onHandshakeConfirmed(now);
        }
    }

    QuicDecodeStatus QuicConnectionCore::onDatagramReceived(const std::span<const std::uint8_t> datagram, const Timestamp arrivalTime,
                                                            const std::optional<std::uint8_t> ecnCodepoint, const QuicPathKey &pathKey)
    {
        // §13.4.1：整条 UDP 数据报只有一个 IP 头，里面那些合包的 QUIC 报文共用这一格读数
        m_incomingEcnCodepoint = ecnCodepoint;
        if (ecnCodepoint.has_value())
        {
            m_isEcnFieldReadable = true;
        }
        // §10.3.1：末 16 字节与「在用的那条对端标识的令牌」比对（常数时间），对上即整条连接进 draining。
        // 这一格必须排在解析之前：一条重置长得就像普通的短头包，真按短头去解只会当成无主报文静静丢掉
        if (datagram.size() >= kQuicStatelessResetTokenByteLength && isStatelessResetDatagram(datagram))
        {
            enterDrainingAfterStatelessReset();
            return {};
        }
        // 反放大额度按「收到的整条数据报」算，不看解没解出来：对端确实把这些字节打到了我们地址上
        m_receivedByteCount += datagram.size();
        // 逐路径再记一份（§9.3.1）：换地址之后给新地址的额度只认从新地址收到的字节。来源不明的
        // 数据报也照记——§8.1 的口径就是「不管有没有被处理」，而这条账只用于给未验证路径定额度
        if (PeerPathState *const receivedPath = pathStateFor(pathKey); receivedPath != nullptr)
        {
            receivedPath->receivedByteCount += datagram.size();
        }
        std::size_t offset = 0;
        while (offset < datagram.size())
        {
            const auto remainder     = datagram.subspan(offset);
            const auto decodedHeader = decodeQuicPacketHeader(remainder, m_configuration.localConnectionId.size());
            if (!decodedHeader.has_value() || decodedHeader->packetByteCount == 0 || offset + decodedHeader->packetByteCount > datagram.size())
            {
                // 连包头都解不出来，后面的字节也就没了依据：整条数据报丢弃（RFC 9000 §5.1）
                return {};
            }
            const QuicDecodeStatus handled = handlePacket(remainder.subspan(0, decodedHeader->packetByteCount), *decodedHeader, arrivalTime, pathKey);
            if (!handled.has_value())
            {
                return handled;
            }
            offset += decodedHeader->packetByteCount;
        }
        return {};
    }

    bool QuicConnectionCore::noteReceivedPacket(const PacketNumberSpace space, const std::uint64_t packetNumber) noexcept
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.receivedPacketNumbers.insert(packetNumber))
        {
            return false;
        }
        if (m_incomingEcnCodepoint.has_value())
        {
            // §13.4.1：计数只在「这条 QUIC 报文真的被处理」时涨，重复包一条都不算
            if (*m_incomingEcnCodepoint == Platform::kEcnCodepointEctZero)
            {
                ++m_receivedEcnCounts[spaceIndex(space)][0];
            } else if (*m_incomingEcnCodepoint == Platform::kEcnCodepointEctOne)
            {
                ++m_receivedEcnCounts[spaceIndex(space)][1];
            } else if (*m_incomingEcnCodepoint == Platform::kEcnCodepointCe)
            {
                ++m_receivedEcnCounts[spaceIndex(space)][2];
            }
        }
        // 额度按「记了多少个包号」算而不是「几段」：连号流量下几十万个包号也只占一段
        state.receivedPacketNumbers.dropOldestUntil(kQuicMaximumTrackedPacketNumbers);
        if (!state.largestReceivedPacketNumber.has_value() || packetNumber > *state.largestReceivedPacketNumber)
        {
            state.largestReceivedPacketNumber = packetNumber;
        }
        return true;
    }

    QuicDecodeStatus QuicConnectionCore::handlePacket(const std::span<const std::uint8_t> packet, const QuicPacketHeader &plainHeader, const Timestamp arrivalTime,
                                                      const QuicPathKey &pathKey)
    {
        const std::optional<QuicEncryptionLevel> level = levelOf(plainHeader);
        if (!level.has_value())
        {
            return {};
        }

        // 目的标识必须落在本连接上：Initial 允许对端凭空造的那个原始标识，其余只认本端签发过的
        // 每一条（§5.1.1 的 MUST：签发的标识要认到对端 RETIRE 为止）。只认握手期那一条是不够的——
        // 通告出去的备用标识一上线就可能被对端拿去用（§8.2 明写迁移中的探测就该换一条新标识），
        // 而外壳的别名路由已经把这种报文送到了本连接手上，两层对「是不是我的」必须同一个结论
        if (!std::ranges::equal(plainHeader.destinationConnectionId, m_configuration.localConnectionId) &&
            !std::ranges::any_of(m_issuedConnectionIds, [&](const IssuedConnectionId &issued) { return std::ranges::equal(plainHeader.destinationConnectionId, issued.id); }) &&
            !(*level == QuicEncryptionLevel::Initial && std::ranges::equal(plainHeader.destinationConnectionId, m_configuration.originalDestinationConnectionId)))
        {
            return {};
        }

        const PacketNumberSpace space = spaceOf(*level);
        SpaceState             &state = m_spaces[spaceIndex(space)];
        if (!state.readKeys.has_value())
        {
            // 密钥还没就绪（Handshake 早到、或本实现不收的形态）：丢弃但不算对端违规
            return {};
        }

        // 去头部保护是就地改写，所以在可写副本上做；原数据报的其余部分不受影响
        std::vector<std::uint8_t> &workingBytes = receiveScratch(ReceiveScratch::PacketCopy);
        workingBytes.assign(packet.begin(), packet.end());
        const std::span<std::uint8_t> workingPacket(workingBytes);

        const QuicDecodeExpected<std::span<const std::uint8_t>> sample = extractQuicHeaderProtectionSample(workingPacket, plainHeader);
        if (!sample.has_value())
        {
            return {};
        }
        const QuicHeaderProtectionMask         mask              = generateQuicHeaderProtectionMask(*state.readKeys, *sample);
        const QuicDecodeExpected<std::uint8_t> unmaskedFirstByte = removeQuicHeaderProtection(workingPacket, plainHeader, mask);
        if (!unmaskedFirstByte.has_value())
        {
            return {};
        }

        QuicPacketHeader header = plainHeader;
        if (const QuicDecodeStatus refreshed = refreshQuicPacketHeader(header, *unmaskedFirstByte, workingPacket); !refreshed.has_value())
        {
            return {};
        }
        if (!header.areReservedBitsClear())
        {
            // 去保护后保留位非 0：能去掉保护就说明密钥对上了，这是对端真这么发的，按 §17.2/§17.3.1 回错
            beginClose(kQuicProtocolViolation, "报文首字节的保留位在去头部保护后非 0（RFC 9000 §17.2/§17.3.1）", arrivalTime);
            return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed, "报文首字节的保留位非 0"});
        }

        const std::uint64_t packetNumber    = restoreQuicPacketNumber(state.largestReceivedPacketNumber.value_or(0), header.packetNumber, header.packetNumberByteCount);
        const std::size_t   headerByteCount = header.packetNumberOffset + header.packetNumberByteCount;
        if (workingPacket.size() <= headerByteCount + kQuicAuthenticationTagByteLength)
        {
            // 连一个 AEAD 标签都装不下，不可能是合法包
            return {};
        }

        // 相位位不合的那一包，要么是对端刚更新、要么是晚到的旧包：同一个相位位被前后两代密钥共用，
        // 只有包号能把它们分开（RFC 9001 §6.5）。头部保护密钥不随更新换（§6.1），所以上面那一步不必跟着分代
        const QuicPacketKeys *reading           = &*state.readKeys;
        bool                  opensWithNextKeys = false;
        if (space == PacketNumberSpace::Application && header.isKeyPhaseBitSet != m_isReadKeyPhaseSet)
        {
            const bool looksNewer = !state.largestReceivedPacketNumber.has_value() || packetNumber > *state.largestReceivedPacketNumber;
            if (looksNewer && state.nextReadKeys.has_value())
            {
                reading           = &*state.nextReadKeys;
                opensWithNextKeys = true;
            } else if (!looksNewer && state.previousReadKeys.has_value())
            {
                reading = &*state.previousReadKeys;
            } else
            {
                // 手上没有对应的那一代：和「解不开」同等处理。回错误码等于给攻击者一个免费的相位探针
                return {};
            }
        }

        const std::span<const std::uint8_t> additionalData   = workingPacket.subspan(0, headerByteCount);
        const std::span<const std::uint8_t> protectedPayload = workingPacket.subspan(headerByteCount);
        std::vector<std::uint8_t>          &plaintext        = receiveScratch(ReceiveScratch::Plaintext);
        plaintext.resize(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const QuicDecodeExpected<std::size_t> opened = openQuicProtectedPayload(plaintext, *reading, packetNumber, additionalData, protectedPayload);
        if (!opened.has_value())
        {
            // 标签不合按 RFC 9001 §4.1.4 静默丢弃：可能是密钥不合或在途被改，回错误码只会喂攻击者
            return {};
        }
        if (opensWithNextKeys)
        {
            // 真用下一代密钥解开了，才说明对端确实发起了更新（§6.2）
            applyPeerKeyUpdate(state, arrivalTime);
        }

        // §9：客户端收到来自「陌生服务端地址」的报文一律丢弃——服务端换地址讲话更像路径劫持，
        // 只有回到本端认下的那个地址的包才继续处理。放在认证之后、记账之前：整包不留任何痕迹
        // （解不开的报文到这一步本来就没了，伪造来源拽不动发包目标）
        if (!isLocalServer() && m_hasAdoptedPeerPath && pathKey != m_sendPathKey)
        {
            return {};
        }

        // 到这一步包才是「认证过」的：包号记账、重复判定与 ECN 计数都只在这个前提下推进
        if (!noteReceivedPacket(space, packetNumber))
        {
            return {};
        }
        if (*level == QuicEncryptionLevel::Initial && !m_peerFirstInitialSourceConnectionId.has_value())
        {
            m_peerFirstInitialSourceConnectionId = std::vector<std::uint8_t>(header.sourceConnectionId.begin(), header.sourceConnectionId.end());
        }
        followPeerConnectionId(header);
        if (!isLocalServer() && *level == QuicEncryptionLevel::Application)
        {
            // §4.1.2 给客户端的那条判据：能解开一条 1-RTT 报文，说明服务端的 Finished 已被 TLS 验过
            // （应用密钥就在那之后才导出），握手至此可算确认
            confirmHandshake(arrivalTime);
        }
        // §10.1：「收到并处理成功」才算活动，解不开的包不能拿来续命。自发的那一份活动在下面
        // emitPacket 里按「收包之后第一次发触发确认的包」补上
        restartIdleTimer(arrivalTime);
        m_hasSentAckElicitingSinceReceipt = false;
        if (!m_isAddressValidated && *level != QuicEncryptionLevel::Initial)
        {
            // 能解出 Handshake 及以上的包，就说明对端确实收到了我们发出去的东西（§8.1.4 的路径验证）：
            // 反放大上限到此解除，否则一条握手都握不完
            m_isAddressValidated = true;
        }

        std::vector<QuicFrame> &frames        = inboundFrameScratch();
        const QuicDecodeStatus  decodedFrames = decodeQuicFrames(std::span<const std::uint8_t>(plaintext).subspan(0, *opened), frames);
        if (!decodedFrames.has_value())
        {
            // 能解密就说明这包出自持有密钥的对端，帧解不开是对端违规（§19.1）
            beginClose(decodedFrames.error().kind == QuicDecodeErrorKind::Truncated ? kQuicProtocolViolation : kQuicFrameEncodingError,
                       std::format("帧序列不合 RFC 9000 §19：{}", decodedFrames.error().message), arrivalTime);
            return std::unexpected(decodedFrames.error());
        }
        return handleFrames(frames, state, space, packetNumber, pathKey, arrivalTime);
    }

    QuicDecodeStatus QuicConnectionCore::handleFrames(const std::vector<QuicFrame> &frames, SpaceState &state, const PacketNumberSpace space, const std::uint64_t packetNumber,
                                                      const QuicPathKey &pathKey, const Timestamp arrivalTime)
    {
        bool hasAckElicitingFrame   = false;
        bool carriesNonProbingFrame = false;
        for (const QuicFrame &frame: frames)
        {
            const std::uint64_t frameType = quicFrameTypeValue(frame);
            if (isOneRttOnlyFrameType(frameType) && space != PacketNumberSpace::Application)
            {
                // §19 表 3 的 Pkts 列逐条写明这些帧不许出现在 Initial/Handshake 包里
                beginClose(kQuicProtocolViolation, std::format("帧 0x{:x} 出现在非 1-RTT 包里（RFC 9000 §19 表 3 的 Pkts 列）", frameType), arrivalTime);
                return {};
            }
            hasAckElicitingFrame   = hasAckElicitingFrame || isAcknowledgementEliciting(frame);
            carriesNonProbingFrame = carriesNonProbingFrame || isNonProbingFrame(frame);
            handleFrame(frame, space, arrivalTime, pathKey);
        }
        if (hasAckElicitingFrame)
        {
            state.largestAckElicitingReceived = packetNumber;
            state.largestAckElicitingArrival  = arrivalTime;
            state.isAcknowledgementPending    = true;
        }
        // 路径只认认证过的报文（§9.3.1 的伪造来源正是靠这一点挡住的）
        followPeerPath(pathKey, space, packetNumber, carriesNonProbingFrame, arrivalTime);
        return {};
    }

    void QuicConnectionCore::handleFrame(const QuicFrame &frame, const PacketNumberSpace space, const Timestamp arrivalTime, const QuicPathKey &pathKey)
    {
        std::visit(
                [this, space, arrivalTime, &pathKey](const auto &specificFrame)
                {
                    using FrameType = std::remove_cvref_t<decltype(specificFrame)>;
                    if constexpr (std::same_as<FrameType, QuicAcknowledgementFrame>)
                    {
                        handleAcknowledgement(specificFrame, space, arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicCryptoFrame>)
                    {
                        handleCryptoBytes(space, specificFrame.offset, specificFrame.data, arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicStreamFrame>)
                    {
                        reportStreamViolation(m_streams.onStreamFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicMaxDataFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxDataFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicMaxStreamDataFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxStreamDataFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicMaxStreamsFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxStreamsFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicResetStreamFrame>)
                    {
                        reportStreamViolation(m_streams.onResetStreamFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicStopSendingFrame>)
                    {
                        reportStreamViolation(m_streams.onStopSendingFrame(specificFrame), arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicConnectionCloseFrame>)
                    {
                        // 对端已经收口：本端不再发任何东西，也不再回一个 CLOSE（§10.2.3）
                        m_phase = QuicConnectionPhase::Closing;
                    } else if constexpr (std::same_as<FrameType, QuicHandshakeDoneFrame>)
                    {
                        // §19.20：这一帧只有服务端会发。「服务端收到它」本身就写死了按 PROTOCOL_VIOLATION
                        // 收口；作客户端时它是合法信号，且正好是 §4.1.2 给客户端的另一条「握手已确认」判据
                        if (isLocalServer())
                        {
                            beginClose(kQuicProtocolViolation, "对端向服务端发了 HANDSHAKE_DONE（RFC 9000 §19.20）", arrivalTime);
                        } else
                        {
                            confirmHandshake(arrivalTime);
                        }
                    } else if constexpr (std::same_as<FrameType, QuicNewConnectionIdFrame>)
                    {
                        handleNewConnectionIdFrame(specificFrame, arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicRetireConnectionIdFrame>)
                    {
                        handleRetireConnectionIdFrame(specificFrame, arrivalTime);
                    } else if constexpr (std::same_as<FrameType, QuicPathChallengeFrame>)
                    {
                        // §8.2.2：原样回显一条 PATH_RESPONSE，且必须打在挑战来的这条路径上
                        queuePathResponse(pathKey, specificFrame.data);
                    } else if constexpr (std::same_as<FrameType, QuicPathResponseFrame>)
                    {
                        handlePathResponseFrame(specificFrame, arrivalTime);
                    }
                    // PADDING 与 PING 不需要动作：PING 的确认由触发确认的记账统一处理。
                    // DATA_BLOCKED 那三类只是对端的自述，本层不需要反应（§19.12–§19.14））
                },
                frame);
    }

    void QuicConnectionCore::handleAcknowledgement(const QuicAcknowledgementFrame &frame, const PacketNumberSpace space, const Timestamp arrivalTime)
    {
        // 报告延迟是线上值，按对端自己声明的指数还原成时间（§19.3）；它的参数还没到之前用默认指数
        const std::uint64_t peerExponent  = m_peerParameters.has_value() ? m_peerParameters->acknowledgmentDelayExponent : kQuicDefaultAcknowledgmentDelayExponent;
        const std::uint64_t shiftBitCount = std::min<std::uint64_t>(peerExponent, 20U);
        // 这个字段允许对端塞任意大的数：先按上限夹一次再移位，免得乘出一个回绕的负延迟。
        // 60 秒远超过任何合法的 max_ack_delay，超出的部分本来就只会走「不合理就不减」那条路
        constexpr std::uint64_t kQuicMaximumAcknowledgementDelayMicroseconds = 60ULL * 1000ULL * 1000ULL;
        const std::uint64_t     unscaledLimit                                = kQuicMaximumAcknowledgementDelayMicroseconds >> shiftBitCount;
        const QuicTime          acknowledgementDelay{static_cast<std::int64_t>(std::min(frame.acknowledgementDelay, unscaledLimit) << shiftBitCount)};

        const QuicAcknowledgementUpdate update = m_recovery.onAcknowledgementReceived(recoverySpaceOf(space), frame, arrivalTime, acknowledgementDelay);
        if (space == PacketNumberSpace::Application && m_lowestPacketNumberSentInKeyPhase.has_value())
        {
            // §6.1：本相位里任意一个包被确认，就代表两侧都有了新密钥，下一次更新的门禁随之打开
            for (const QuicSentPacketInfo &packet: update.acknowledged)
            {
                if (packet.packetNumber >= *m_lowestPacketNumberSentInKeyPhase)
                {
                    m_isKeyPhaseAcknowledged = true;
                    break;
                }
            }
        }
        // 每帧确认前把恢复层的 srtt 刷给拥塞算法：CUBIC 的友好区与「下一个 RTT 的目标窗」都要用它，
        // NewReno 不读这一格。放在 ECN 与更新之前——两者之后的算法计算都该看到本帧最新的 RTT 估计
        m_congestion->setSmoothedRoundTripTime(m_recovery.roundTripTimeEstimate().smoothed);
        if (update.ecnCongestionEventTime.has_value())
        {
            // §B.7 的反应排在 §B.8（丢包那一步）之前：CE 是早到的拥塞信号，先按它降窗；
            // 同一帧里真的也丢了包时，§7.3.2 那道「一轮只降一次」的闸由拥塞层统一判
            m_congestion->noteEcnCongestionEvent(*update.ecnCongestionEventTime);
        }
        m_congestion->onCongestionUpdate(update.acknowledged, update.lost, arrivalTime);
        if (update.isPersistentCongestionDetected)
        {
            // §7.6.2：排在同一帧的普通降窗之后，否则这一轮的「减半」会把最小窗那份重启盖掉。
            // 本层不打日志（日志归外壳），因此把次数留在读数上供外壳与用例取
            m_congestion->restartAfterPersistentCongestion();
            ++m_persistentCongestionEventCount;
            // 「发出去的东西一概没人答」在 RFC 8899 图 5 里就是「PL indicates loss of connectivity」那一格，
            // QUIC 里的对应物正是持久拥塞：尺寸可能已经过不去（路被改了 MTU），掉回 BASE 从头再探
            m_pathMtu.onConnectivityLost(arrivalTime);
        }
        for (const QuicSentPacketInfo &packet: update.acknowledged)
        {
            m_streams.onSendRangesAcknowledged(packet.streamRanges);
            m_streams.onStreamAnnouncementsAcknowledged(packet.streamAnnouncements);
            if (packet.carriesHandshakeDone)
            {
                // §19.20 的「重发到被确认为止」到此为止：确认回来之后这一帧这辈子不再发第二遍
                m_isHandshakeDoneAcknowledged = true;
                m_isHandshakeDoneInFlight     = false;
            }
        }
        if (std::ranges::any_of(update.acknowledged, [](const QuicSentPacketInfo &packet) { return packet.isPathMtuProbe; }))
        {
            // §14.4：只有被确认的那个尺寸才算证据，所以这一步排在本帧的确认账之后、判丢那一段之前——
            // 同一帧里探针既可能被确认也可能被别的包牵连判丢，确认优先（探针活着走过去就是走过去）
            m_pathMtu.onProbeAcknowledged(arrivalTime);
        }
        for (const QuicSentPacketInfo &packet: update.lost)
        {
            m_streams.onSendRangesLost(packet.streamRanges);
            if (packet.carriesConnectionIdFrames)
            {
                // §13.3：NEW_CONNECTION_ID 与 RETIRE_CONNECTION_ID 判丢要重发。这两类帧允许重复
                // （§19.15 明写同一条帧收两遍不算错，RETIRE 一个已退的序列号是幂等动作），因此不必
                // 记住「哪一条在哪个包里」——下一次出包整批发一遍就够
                m_isConnectionIdFramesDirty = true;
            }
        }

        // §4.1.2：服务端「握手已确认」的判据就是对端确认了 Handshake 空间的包。确认之后才允许给
        // 进入用空间武装 PTO，也才该发 HANDSHAKE_DONE
        if (space == PacketNumberSpace::Handshake && !update.acknowledged.empty())
        {
            confirmHandshake(arrivalTime);
        }
        if (!update.lost.empty())
        {
            queueRetransmissions(space, update.lost);
        }
    }

    void QuicConnectionCore::handleCryptoBytes(const PacketNumberSpace space, const std::uint64_t offset, const std::span<const std::uint8_t> bytes, const Timestamp arrivalTime)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (state.reassembly.bufferedByteCount() + bytes.size() > kQuicCryptoBufferByteLimit)
        {
            // 上限只管「还没能交给 TLS 的那部分」：连着已交付的前缀一起算会把正常握手也判成越界
            beginClose(kQuicCryptoBufferExceeded, "乱序早到的 CRYPTO 字节超过本端缓存上限（RFC 9000 §11.1）", arrivalTime);
            return;
        }
        // 交入即按覆盖区合并，再一次性排干：换了分片大小的重传也补得回来（§7.5）
        state.reassembly.insert(offset, bytes);
        std::vector<std::uint8_t> contiguous;
        if (state.reassembly.drain(contiguous) == 0)
        {
            return;
        }
        m_tls->feedHandshakeData(levelOf(space), contiguous);
    }

    void QuicConnectionCore::adoptTlsKeys()
    {
        // Initial 的密钥在建连接时就按 §5.2 推导好了，这里只接 TLS 交出的后两个级别
        for (const PacketNumberSpace space: {PacketNumberSpace::Handshake, PacketNumberSpace::Application})
        {
            SpaceState               &state = m_spaces[spaceIndex(space)];
            const QuicEncryptionLevel level = levelOf(space);
            if (const QuicPacketKeys *reading = m_tls->keys(level, QuicKeyDirection::Reading); reading != nullptr && !state.readKeys.has_value())
            {
                state.readKeys = *reading;
                if (space == PacketNumberSpace::Application)
                {
                    // §6.3：当前与下一代两套读密钥常驻，更新到来时才不必在解包路径上现推一把密钥
                    state.nextReadKeys = deriveQuicUpdatedPacketKeys(*state.readKeys);
                }
            }
            if (const QuicPacketKeys *writing = m_tls->keys(level, QuicKeyDirection::Writing); writing != nullptr && !state.writeKeys.has_value())
            {
                state.writeKeys = *writing;
            }
        }
    }

    void QuicConnectionCore::adoptTlsRecords()
    {
        while (const std::optional<QuicTlsRecord> record = m_tls->takeOutboundRecord())
        {
            SpaceState &state = m_spaces[spaceIndex(record->level)];
            // 交来的字节并进本空间的握手流：发过的部分不能丢，判丢与探针都要按偏移重发
            state.cryptoStream.insert(state.cryptoStream.end(), record->data.begin(), record->data.end());
        }
    }

    void QuicConnectionCore::adoptPeerTransportParameters(const Timestamp now)
    {
        if (m_peerParameters.has_value() || m_tls->peerTransportParameters().empty())
        {
            return;
        }
        const QuicDecodeExpected<QuicTransportParameters> decoded = decodeQuicTransportParameters(
                m_tls->peerTransportParameters(), isLocalServer() ? QuicTransportParameterSenderRole::Client : QuicTransportParameterSenderRole::Server);
        if (!decoded.has_value())
        {
            beginClose(kQuicTransportParameterError, std::format("对端传输参数不合格：{}", decoded.error().message), now);
            return;
        }
        // §7.3：对端参数里的 initial_source_connection_id 必须等于它第一个 Initial 的源标识
        if (!m_peerFirstInitialSourceConnectionId.has_value() || !decoded->initialSourceConnectionId.has_value() ||
            !std::ranges::equal(*decoded->initialSourceConnectionId, *m_peerFirstInitialSourceConnectionId))
        {
            beginClose(kQuicTransportParameterError, "对端的 initial_source_connection_id 与它第一个 Initial 里的源标识不符（RFC 9000 §7.3）", now);
            return;
        }
        if (!isLocalServer())
        {
            // §7.3 给客户端的那条：服务端必须把「客户端第一个 Initial 的目的标识」原样回传，
            // 客户端拿它核对自己当初挑的那个值——不一致就说明中间有人换了连接标识，或这条连接不是
            // 自己发起的那一条
            if (!decoded->originalDestinationConnectionId.has_value() ||
                !std::ranges::equal(*decoded->originalDestinationConnectionId, m_configuration.originalDestinationConnectionId))
            {
                beginClose(kQuicTransportParameterError, "服务端的 original_destination_connection_id 与本端第一个 Initial 的目的标识不符（RFC 9000 §7.3）", now);
                return;
            }
        }
        m_peerParameters = *decoded;
        // §10.3：对端（服务端）在参数里给握手标识配了无状态重置令牌就收下——本端此后一直拿那条
        // 标识当发送目标，「服务端进程没了」这件事才有一条不靠计时器的判据
        if (decoded->statelessResetToken.has_value())
        {
            notePeerHandshakeStatelessResetToken(*decoded->statelessResetToken);
        }
        // 流层的发送额度全部来自对端参数：到手之前一条流数据也发不出去（§4.1）
        m_streams.adoptPeerParameters(*decoded);
        // §14.2 / RFC 8899 §5.3.1：MAX_PLPMTU 是「本端愿意收的下限」与「对端宣告的上限」的较小者，
        // 对端这一半到手之前不探——探一个对面明说收不下的尺寸，只是白丢一条探针
        m_pathMtu.onPeerMaximumPayload(decoded->maximumUdpPayloadSize);
    }

    void QuicConnectionCore::reportStreamViolation(const QuicStreamViolationStatus &result, const Timestamp now)
    {
        if (!result.has_value())
        {
            beginClose(result.error().errorCode, result.error().reasonPhrase, now);
        }
    }

    QuicStreamLayer &QuicConnectionCore::streamLayer() noexcept
    {
        return m_streams;
    }

    const QuicStreamLayer &QuicConnectionCore::streamLayer() const noexcept
    {
        return m_streams;
    }

    void QuicConnectionCore::discardSpace(const PacketNumberSpace space)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.readKeys.has_value() && !state.writeKeys.has_value())
        {
            return;
        }
        state.readKeys  = std::nullopt;
        state.writeKeys = std::nullopt;
        state.cryptoStream.clear();
        state.pendingRetransmissions.clear();
        // 入站缓存连着交付点一起丢掉：这个空间再也不会用到，留着只会占着一份乱序字节
        state.reassembly = QuicReassemblyBuffer{};
        if (m_probeSpace.has_value() && *m_probeSpace == space)
        {
            // 欠的探针作废：给一个已经退休的空间发探测包，只会白对端一条报文
            m_probeSpace.reset();
        }
        // 在途账一起清：留着它们，定时器还会为一个已经退休的空间亮起来，白挨探针（§A.11）
        const QuicRecoverySpace               retiredSpace   = recoverySpaceOf(space);
        const std::vector<QuicSentPacketInfo> retiredPackets = m_recovery.unacknowledgedPackets(retiredSpace);
        m_recovery.discardSpace(retiredSpace);
        // 恢复层与拥塞层各记一本在途账，只清一本就让这条连接永久背着退休空间的字节：
        // Initial 空间几乎总在握手期退休，之后每次算可用窗口都要少几十 KB（RFC 9002 §7.2 的负荷口径）
        m_congestion->onPacketsDiscarded(retiredPackets);
    }

    void QuicConnectionCore::drive(const Timestamp now)
    {
        retireStaleKeyPhase(now);
        // 解到对端 Handshake 及以上级别的报文，这批 Initial 就没有下一跳了：密钥、握手流与在途账一起
        // 退休，免得定时器为一个再也发不出包的空间亮着（RFC 9001 §4.9.1、RFC 9002 §A.11）
        if (m_isAddressValidated)
        {
            discardSpace(PacketNumberSpace::Initial);
        }
        adoptTlsKeys();
        if (m_phase != QuicConnectionPhase::Closing)
        {
            const QuicTlsProgress progress = m_tls->drive();
            // 记录是在同一次 drive 里跟着写方向的密钥一起交出来的，先补密钥再取记录
            adoptTlsKeys();
            adoptTlsRecords();
            if (progress == QuicTlsProgress::Failed)
            {
                if (const std::optional<std::uint8_t> alert = m_tls->alert(); alert.has_value())
                {
                    beginClose(kQuicCryptoErrorBase + *alert, "TLS 握手失败，错误码是 RFC 9001 §4.8 映射出的 CRYPTO_ERROR", now);
                } else
                {
                    beginClose(kQuicProtocolViolation, "TLS 拒绝了握手数据", now);
                }
            } else if (m_tls->isHandshakeCompleted())
            {
                m_phase = QuicConnectionPhase::Established;
            }
            adoptPeerTransportParameters(now);
        }

        if (m_phase != QuicConnectionPhase::Closing)
        {
            // 拥塞层的计量单位跟着已确认的数据报尺寸走：窗口增减与最小窗都以 max_datagram_size 为一格
            // （§B.2、§7.2），探到更大的尺寸却不换它，涨窗就按比线上实际小的那一格算，长期偏慢
            m_congestion->setMaximumDatagramByteLength(m_pathMtu.maximumDatagramPayloadByteLength());
            // 路径验证的挑战与回应先排出去：§8.2.2 明写回应「不得延迟」，而这一轮 flush 正是
            // 刚把挑战收进来的那一轮
            queuePathValidationPackets(now);
            for (const PacketNumberSpace space: {PacketNumberSpace::Initial, PacketNumberSpace::Handshake, PacketNumberSpace::Application})
            {
                queueSpacePackets(space, now);
            }
        }
    }

    void QuicConnectionCore::appendAcknowledgementIfPending(SpaceState &state, const PacketNumberSpace space, const Timestamp now, std::string &frames)
    {
        if (!std::exchange(state.isAcknowledgementPending, false) || !state.largestAckElicitingReceived.has_value())
        {
            return;
        }
        QuicAcknowledgementFrame acknowledgement;
        acknowledgement.largestAcknowledgedPacketNumber = *state.largestAckElicitingReceived;
        acknowledgement.ranges                          = buildQuicAcknowledgementRanges(state.receivedPacketNumbers, *state.largestAckElicitingReceived);
        // §19.3：线上值是微秒数右移本端声明的 ack_delay_exponent，指数上限 20 由参数校验保证
        const std::int64_t elapsedMicroseconds = std::max<std::int64_t>(0, (now - *state.largestAckElicitingArrival).count());
        acknowledgement.acknowledgementDelay =
                static_cast<std::uint64_t>(elapsedMicroseconds) >> std::min<std::uint64_t>(m_configuration.transportParameters.acknowledgmentDelayExponent, 20U);
        if (m_isEcnFieldReadable)
        {
            // 本端读得到 ECN 字段就报计数（§13.4.1 的「即使本端不标也要报收到的标记」）。读不到的平台
            // 交回普通 ACK（类型 0x02）：规范明确允许不报，对端会因此把这条路判成不可用——诚实的退化
            acknowledgement.ecnCounts    = m_receivedEcnCounts[spaceIndex(space)];
            acknowledgement.hasEcnCounts = true;
        }
        appendQuicFrame(frames, QuicFrame{acknowledgement});
    }

    void QuicConnectionCore::prepareOneRttPackets(const Timestamp now)
    {
        // 探针只走 1-RTT：状态机在握手完成前不离开 Disabled，而那时应用空间的写密钥本来就还没武装
        queuePathMtuProbe(now);
        // §5.1.1 的 SHOULD：给对端留一条没在用的标识，它才可能换一条来迁移（§9.5 明写不许拿同一条
        // 标识打两个目的地址，池子耗尽就等于迁移被锁死）。上限由对端通告的 active_connection_id_limit
        // 把住：本端从不超发，退了一条才补一条
        if (hasRoomForAnotherConnectionId())
        {
            static_cast<void>(issueConnectionId(false));
        }
    }

    void QuicConnectionCore::queueSpacePackets(const PacketNumberSpace space, const Timestamp now)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.writeKeys.has_value())
        {
            return;
        }
        const bool        isLongHeader            = space != PacketNumberSpace::Application;
        const std::size_t fixedOverheadByteLength = packetOverheadByteLength(m_configuration, isLongHeader);
        if (isBlockedByAmplificationLimit(fixedOverheadByteLength))
        {
            // 连一个包头都装不进 3 倍额度：本空间这一轮什么都发不出去，等对端多打些字节再来（§8.1）
            return;
        }
        if (space == PacketNumberSpace::Application)
        {
            // 1-RTT 才有的两件事：路径 MTU 探针与「给对端补一条没用过的标识」，理由见各自的方法
            prepareOneRttPackets(now);
        }
        // §19.20：握手完成、且对端确认过 Handshake 空间的包之后才发 HANDSHAKE_DONE，且它是 1-RTT
        // 帧（§19 表 3 的 Protection 列只有 1）——塞进 Handshake 空间的包会被对端按「该级别不该
        // 出现这种帧」判 PROTOCOL_VIOLATION（aioquic 即如此）。这一帧也只归服务端发
        bool owesHandshakeDone =
                isLocalServer() && space == PacketNumberSpace::Application && m_phase == QuicConnectionPhase::Established && m_isHandshakeConfirmed && isHandshakeDonePending();
        bool owesProbe = m_probeSpace.has_value() && *m_probeSpace == space;
        // 整轮探测都豁免窗口（§7.5）：欠的那一条可能分两包出去，只豁免第一包等于把后半段卡在门外
        const bool isProbingSpace = owesProbe;

        // 整轮的帧序列都编进同一块缓冲：它只活到 emitPacket 把字节抄进报文为止，留在循环里逐包新建
        // 就是每个出站包都付一次「从空长到一包大小」的几何扩容
        std::string &frames = outboundFrameAssemblyBuffer();
        for (;;)
        {
            frames.clear();
            bool elicitsAcknowledgement = false;
            appendAcknowledgementIfPending(state, space, now, frames);
            bool carriesHandshakeDone = false;
            // 连接标识那两类帧只许出现在 1-RTT 里（§19 表 3），编进去的都触发确认
            const bool carriesConnectionIdFrames = space == PacketNumberSpace::Application && appendPendingConnectionIdFrames(frames);
            elicitsAcknowledgement               = elicitsAcknowledgement || carriesConnectionIdFrames;
            if (owesHandshakeDone)
            {
                appendQuicFrame(frames, QuicFrame{QuicHandshakeDoneFrame{}});
                carriesHandshakeDone = true;
                // 一轮里只带一次：这一包出去之后在途就有了它的副本，下一包不必再重复占字节
                owesHandshakeDone      = false;
                elicitsAcknowledgement = true;
            }

            const std::size_t              remainingByteBudget = sendByteBudget(fixedOverheadByteLength + frames.size(), isProbingSpace);
            std::optional<QuicCryptoRange> carriedRange;
            if (!state.pendingRetransmissions.empty())
            {
                // 重发队列优先：判丢的那段最可能正是对端缺的
                QuicCryptoRange  &front              = state.pendingRetransmissions.front();
                const std::size_t fragmentByteLength = std::min(front.endOffset - front.beginOffset, remainingByteBudget);
                front.beginOffset += fragmentByteLength;
                if (front.beginOffset == front.endOffset)
                {
                    state.pendingRetransmissions.erase(state.pendingRetransmissions.begin());
                }
                if (fragmentByteLength > 0)
                {
                    carriedRange = QuicCryptoRange{front.beginOffset - fragmentByteLength, front.beginOffset};
                }
            } else if (state.cryptoWriteOffset < state.cryptoStream.size())
            {
                const std::size_t fragmentByteLength = std::min(state.cryptoStream.size() - state.cryptoWriteOffset, remainingByteBudget);
                carriedRange                         = QuicCryptoRange{state.cryptoWriteOffset, state.cryptoWriteOffset + fragmentByteLength};
                state.cryptoWriteOffset += fragmentByteLength;
            }
            if (carriedRange.has_value() && carriedRange->endOffset > carriedRange->beginOffset)
            {
                QuicCryptoFrame crypto;
                crypto.offset = carriedRange->beginOffset;
                crypto.data   = std::span<const std::uint8_t>(state.cryptoStream).subspan(carriedRange->beginOffset, carriedRange->endOffset - carriedRange->beginOffset);
                appendQuicFrame(frames, QuicFrame{crypto});
                elicitsAcknowledgement = true;
            } else
            {
                // 这一包没真带上字节：别把空区间记进重发账，否则它永远排在队头
                carriedRange = std::nullopt;
            }

            std::vector<QuicStreamRange>        sentRanges;
            std::vector<QuicStreamAnnouncement> announcements;
            bool                                carriesStreamFrames = false;
            if (space == PacketNumberSpace::Application)
            {
                // 握手字节的产出排在流数据之前：进入用空间通常只有会话票据会占那条通道
                carriesStreamFrames    = m_streams.collectFrames(frames, sendByteBudget(fixedOverheadByteLength + frames.size(), isProbingSpace), sentRanges, announcements);
                elicitsAcknowledgement = elicitsAcknowledgement || carriesStreamFrames;
            }

            if (owesProbe)
            {
                // §6.2.2：探针优先带新数据或重发数据，两样都没有就单发一条 PING，保证对端一定会回确认
                if (!elicitsAcknowledgement)
                {
                    appendQuicFrame(frames, QuicFrame{QuicPingFrame{}});
                    elicitsAcknowledgement = true;
                }
                m_probeSpace.reset();
                owesProbe = false;
            }

            if (frames.empty())
            {
                return;
            }
            const bool hasMoreCrypto = !state.pendingRetransmissions.empty() || state.cryptoWriteOffset < state.cryptoStream.size();
            const bool hasMoreWork   = hasMoreCrypto || (space == PacketNumberSpace::Application && m_streams.hasOutgoingFrames());
            emitPacket(space, frames, now, elicitsAcknowledgement, carriedRange, std::move(sentRanges), carriesHandshakeDone, std::move(announcements), carriesConnectionIdFrames,
                       std::nullopt, m_sendPathKey);
            if (!hasMoreWork || (!carriedRange.has_value() && !carriesStreamFrames))
            {
                // 已经没有下文，或这一包被预算挤得一个字节都没带上：再转一圈也只是空转
                return;
            }
        }
    }

    void QuicConnectionCore::queueRetransmissions(const PacketNumberSpace space, const std::span<const QuicSentPacketInfo> packets)
    {
        SpaceState                  &state  = m_spaces[spaceIndex(space)];
        std::vector<QuicCryptoRange> ranges = state.pendingRetransmissions;
        // 判丢的那批已经从在途账里划掉了，所以字节区间只能由调用方把包本身交进来：
        // 只扫在途集合会把「刚判丢」这一路变成空操作
        for (const QuicSentPacketInfo &packet: packets)
        {
            if (packet.cryptoRange.has_value() && packet.cryptoRange->endOffset > packet.cryptoRange->beginOffset)
            {
                ranges.push_back(*packet.cryptoRange);
            }
            if (packet.carriesHandshakeDone)
            {
                // 这一包没了指望，它带的 DONE 也就没人再认账了：销账，下一轮出包补一份（§19.20）
                m_isHandshakeDoneInFlight = false;
            }
            // §13.3：复位与停发都「发到被确认为止」，这一包没了指望就得把宣告重新排回待发。
            // 判丢（按包号）与探测超时（按时间）两条路都走这里，所以登记只在这一处
            m_streams.onStreamAnnouncementsLost(packet.streamAnnouncements);
        }
        // 相邻或重叠的区间合并：同一段字节被两个包各带过一次时不该重发两遍，队列也不该无限增长
        std::ranges::sort(ranges, {}, &QuicCryptoRange::beginOffset);
        std::vector<QuicCryptoRange> merged;
        for (const QuicCryptoRange &range: ranges)
        {
            if (!merged.empty() && range.beginOffset <= merged.back().endOffset)
            {
                merged.back().endOffset = std::max(merged.back().endOffset, range.endOffset);
                continue;
            }
            merged.push_back(range);
        }
        state.pendingRetransmissions = std::move(merged);
    }

    bool QuicConnectionCore::isHandshakeDonePending() const noexcept
    {
        // 确认回来之前一直欠着一份（§19.20）；在途还有一份时不必再发第二份，等它被判丢或探测超时
        // 之后由 queueRetransmissions 把「在途那份」的账销掉，下一轮自然补发
        return !m_isHandshakeDoneAcknowledged && !m_isHandshakeDoneInFlight;
    }

    void QuicConnectionCore::queuePathMtuProbe(const Timestamp now)
    {
        // 探针照样吃拥塞窗口（§14.4 明写 probes consume congestion window），所以交给状态机的预算是
        // 窗口余量：腾不出待探尺寸就不算这次到点，免得把「窗口小」记成「这个尺寸走不通」连着三次
        const std::optional<std::size_t> probeByteLength = m_pathMtu.probeByteLengthIfDue(now, m_congestion->remainingByteBudget());
        if (!probeByteLength.has_value())
        {
            return;
        }
        std::string frames;
        appendQuicFrame(frames, QuicFrame{QuicPingFrame{}});
        emitPacket(PacketNumberSpace::Application, frames, now, true, std::nullopt, {}, false, {}, false, DatagramPadding{*probeByteLength, true}, m_sendPathKey);
    }

    void QuicConnectionCore::emitPacket(const PacketNumberSpace space, const std::string &frames, const Timestamp now, const bool isAckEliciting,
                                        const std::optional<QuicCryptoRange> cryptoRange, std::vector<QuicStreamRange> streamRanges, const bool carriesHandshakeDone,
                                        std::vector<QuicStreamAnnouncement> streamAnnouncements, const bool carriesConnectionIdFrames, const std::optional<DatagramPadding> padding,
                                        const QuicPathKey &pathKey)
    {
        SpaceState         &state        = m_spaces[spaceIndex(space)];
        const std::uint64_t packetNumber = state.nextPacketNumber;
        // 两处补尺寸补的都是 PADDING 帧（0x00 一字节一帧），落在受保护载荷里，所以差多少要等包头与标签
        // 都算得出来才知道：①客户端握手期的 Initial 按 §14.1 补到不小于 1200；②路径 MTU 探针与路径
        // 验证的挑战/回应按各自章节补到 1200——探针的意义就在那个数上，验证的那两个是让新路先证明
        // 自己过得去大包（§8.2.1/§8.2.2）
        const std::size_t plainOverheadByteLength = packetOverheadByteLength(m_configuration, space != PacketNumberSpace::Application) - kQuicCryptoFrameHeaderByteLimit;
        std::string       paddedFrameAssembly;
        if (!isLocalServer() && space == PacketNumberSpace::Initial && plainOverheadByteLength + frames.size() < kQuicMinimumInitialDatagramByteLength)
        {
            paddedFrameAssembly = frames;
            paddedFrameAssembly.append(kQuicMinimumInitialDatagramByteLength - plainOverheadByteLength - frames.size(), '\0');
        } else if (padding.has_value() && plainOverheadByteLength + frames.size() < padding->datagramByteLength)
        {
            paddedFrameAssembly = frames;
            paddedFrameAssembly.append(padding->datagramByteLength - plainOverheadByteLength - frames.size(), '\0');
        }
        QuicOutboundPacket packet;
        packet.isLongHeader            = space != PacketNumberSpace::Application;
        packet.longPacketType          = space == PacketNumberSpace::Handshake ? QuicLongPacketType::Handshake : QuicLongPacketType::Initial;
        packet.version                 = kQuicVersion1;
        packet.destinationConnectionId = m_configuration.peerConnectionId;
        packet.sourceConnectionId      = m_configuration.localConnectionId;
        packet.packetNumber            = packetNumber;
        packet.packetNumberByteCount   = 1;
        packet.isKeyPhaseBitSet        = m_isSendKeyPhaseSet;
        packet.frames                  = paddedFrameAssembly.empty() ? asBytes(frames) : asBytes(paddedFrameAssembly);

        std::string datagram;
        appendQuicPacket(datagram, packet, *state.writeKeys);
        // 试探期与已验证期带 ECT(0)，只有验证失败才不标（这一格由恢复层的 ECN 状态机定，见 QuicRecovery.h）。
        // 这里取一次、同时交给队列与下面的发包凭据：§13.4.2.1 的校验拿「当时标了哪一格」比对端报的计数，
        // 线上与账上错开一格就会让校验判出假结论
        const std::uint8_t outgoingEcnCodepoint = m_recovery.outgoingEcnCodepoint();
        ++state.nextPacketNumber;
        const std::size_t datagramByteCount = datagram.size();
        // 目标路径由调用方解析好传进来（见头文件那一格的理由：全零是合法键值，这里不做「留空＝当前路径」的兜底）。
        // 待发队列拿的是这条报文的本体而不是它的副本：下面 record 与字节数都只需要长度，
        // 整包再抄一遍是白付的一次分配加一次 memcpy
        m_outboundDatagrams.push_back(QuicOutboundDatagram{std::move(datagram), outgoingEcnCodepoint, pathKey});

        QuicSentPacketInfo record;
        record.packetNumber              = packetNumber;
        record.timeSent                  = now;
        record.byteCount                 = datagramByteCount;
        record.isAckEliciting            = isAckEliciting;
        record.ecnCodepoint              = outgoingEcnCodepoint;
        record.cryptoRange               = cryptoRange;
        record.streamRanges              = std::move(streamRanges);
        record.streamAnnouncements       = std::move(streamAnnouncements);
        record.carriesHandshakeDone      = carriesHandshakeDone;
        record.carriesConnectionIdFrames = carriesConnectionIdFrames;
        // 探针被丢不算拥塞证据、也不算 §7.6.2 那一段的端点（RFC 9000 §14.4），所以这一格要留在凭据上；
        // 被确认时它又是「这个尺寸走过去了」的唯一证据，两条出口都靠它分辨
        record.isPathMtuProbe = padding.has_value() && padding->isPathMtuProbe;
        if (carriesHandshakeDone)
        {
            m_isHandshakeDoneInFlight = true;
        }
        if (isAckEliciting && !m_hasSentAckElicitingSinceReceipt && m_idlePeriod.has_value())
        {
            // §10.1：本端主动发起的通信也算活动，但只有收包之后的第一包作数，且沿用本期额度——
            // 否则每趟探测都续一期、且用的还是翻倍后的 PTO，空闲超时永远等不到
            m_idleDeadline                    = now + *m_idlePeriod;
            m_hasSentAckElicitingSinceReceipt = true;
        }
        m_congestion->onPacketSent(record);
        m_recovery.onPacketSent(recoverySpaceOf(space), std::move(record));
        m_sentByteCount += datagramByteCount;
        // 逐路径的已发字节（§9.3.1 给未验证路径定额度用它）：按这条报文真正的目标记账
        if (PeerPathState *const sentPath = pathStateFor(pathKey); sentPath != nullptr)
        {
            sentPath->sentByteCount += datagramByteCount;
        }
    }

    std::size_t QuicConnectionCore::sendByteBudget(const std::size_t reservedByteLength, const bool ignoresCongestionWindow) const
    {
        // 上限听路径 MTU 状态机的：没探到更大的尺寸时它就是 1200（§14.1 的那个下限），
        // 探到了就按被真实确认过的那一格发
        std::size_t budget = saturatingSubtract(m_pathMtu.maximumDatagramPayloadByteLength(), reservedByteLength);
        if (!ignoresCongestionWindow)
        {
            // 窗口余量也得先减掉这一包的固定开销，否则算出来的分片一定超窗
            budget = std::min(budget, saturatingSubtract(m_congestion->remainingByteBudget(), reservedByteLength));
        }
        if (isAmplificationLimited())
        {
            // §8.1：地址验证之前最多回三倍已收字节。这条排在最后，因为它是硬上限，探针也不例外
            budget = std::min(budget, saturatingSubtract(amplificationRemainingByteCount(), reservedByteLength));
        }
        return budget;
    }

    bool QuicConnectionCore::isBlockedByAmplificationLimit(const std::size_t reservedByteLength) const noexcept
    {
        return isAmplificationLimited() && reservedByteLength > amplificationRemainingByteCount();
    }

    std::size_t QuicConnectionCore::amplificationRemainingByteCount() const noexcept
    {
        // 两条上限（§8.1 的握手期总额度、§9.3.1 的逐路径额度）谁小听谁，判据都收在 amplificationLimitForPath
        return amplificationLimitForPath(m_sendPathKey);
    }

    void QuicConnectionCore::requestClose(const std::uint64_t errorCode, const std::string_view reasonPhrase, const Timestamp now)
    {
        beginClose(errorCode, reasonPhrase, now);
    }

    void QuicConnectionCore::beginClose(const std::uint64_t errorCode, const std::string_view reasonPhrase, const Timestamp now)
    {
        if (m_phase == QuicConnectionPhase::Closing)
        {
            return;
        }
        m_localCloseErrorCode    = errorCode;
        m_localCloseReasonPhrase = reasonPhrase;
        m_phase                  = QuicConnectionPhase::Closing;
        queueConnectionClosePacket(now);
    }

    void QuicConnectionCore::queueConnectionClosePacket(const Timestamp now)
    {
        QuicConnectionCloseFrame close;
        close.errorCode    = m_localCloseErrorCode.value_or(kQuicNoError);
        close.reasonPhrase = asBytes(m_localCloseReasonPhrase);
        std::string frames;
        appendQuicFrame(frames, QuicFrame{close});
        const PacketNumberSpace space = highestSpaceWithWriteKeys();
        if (isBlockedByAmplificationLimit(packetOverheadByteLength(m_configuration, space != PacketNumberSpace::Application) + frames.size()))
        {
            // 收口也受 §8.1 约束：额度不够就宁可不发，反正对端已经在把我们当放大器打了
            return;
        }
        emitPacket(space, frames, now, false, std::nullopt, {}, false, {}, false, std::nullopt, m_sendPathKey);
    }

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::highestSpaceWithWriteKeys() const noexcept
    {
        for (const PacketNumberSpace space: {PacketNumberSpace::Application, PacketNumberSpace::Handshake, PacketNumberSpace::Initial})
        {
            if (m_spaces[spaceIndex(space)].writeKeys.has_value())
            {
                return space;
            }
        }
        return PacketNumberSpace::Initial;
    }

    QuicRecoverySpace QuicConnectionCore::recoverySpaceOf(const PacketNumberSpace space) noexcept
    {
        switch (space)
        {
            case PacketNumberSpace::Initial:
                return QuicRecoverySpace::Initial;
            case PacketNumberSpace::Handshake:
                return QuicRecoverySpace::Handshake;
            case PacketNumberSpace::Application:
                return QuicRecoverySpace::Application;
        }
        return QuicRecoverySpace::Application;
    }

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::spaceOf(const QuicRecoverySpace space) noexcept
    {
        switch (space)
        {
            case QuicRecoverySpace::Initial:
                return PacketNumberSpace::Initial;
            case QuicRecoverySpace::Handshake:
                return PacketNumberSpace::Handshake;
            case QuicRecoverySpace::Application:
                return PacketNumberSpace::Application;
        }
        return PacketNumberSpace::Application;
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::nextTimeout() const noexcept
    {
        std::optional<Timestamp> deadline = m_recovery.nextDeadline();
        if (const std::optional<Timestamp> pathDeadline = m_pathMtu.nextDeadline(); pathDeadline.has_value() && (!deadline.has_value() || *pathDeadline < *deadline))
        {
            deadline = pathDeadline;
        }
        if (const std::optional<Timestamp> idleDeadline = idleDeadlineTime(); idleDeadline.has_value() && (!deadline.has_value() || *idleDeadline < *deadline))
        {
            deadline = idleDeadline;
        }
        for (const PendingPathChallenge &challenge: m_pendingPathChallenges)
        {
            // 候选路径两把计时都要看（换数据重发与放弃）；复验那种只看放弃期限——它不重发
            const Timestamp challengeDeadline = challenge.isCandidate ? std::min(challenge.abandonDeadline, challenge.retransmitDeadline) : challenge.abandonDeadline;
            if (!deadline.has_value() || challengeDeadline < *deadline)
            {
                deadline = challengeDeadline;
            }
        }
        return deadline;
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::idleDeadlineTime() const noexcept
    {
        return m_idleDeadline;
    }

    bool QuicConnectionCore::requestKeepalive(const Timestamp now)
    {
        // 没宣告空闲超时就没有会被掐的东西，握手没完成或已在收口时也不替它续命
        if (!m_idlePeriod.has_value() || !m_idleDeadline.has_value() || m_phase != QuicConnectionPhase::Established)
        {
            return false;
        }
        // 只在「剩不到半期」时才顶：一条 PING 换一期额度，早顶只是白发包（每拍都被调用）
        if (*m_idleDeadline - now > *m_idlePeriod / 2)
        {
            return false;
        }
        if (m_probeSpace.has_value())
        {
            // 已经欠着一条探针（恢复层的丢包探测优先）：不重复排队，那一包本来就是触发确认的
            return false;
        }
        // 节律：一条 PING 管四分之一期，之后就算还落在「半期之内」也不重复发。承载层的驱动是
        // 每个节拍都调这里的，没有这道闸就成了每拍一个包
        if (m_lastKeepalive.has_value() && now - *m_lastKeepalive < *m_idlePeriod / 4)
        {
            return false;
        }
        m_probeSpace    = PacketNumberSpace::Application;
        m_lastKeepalive = now;
        return true;
    }

    void QuicConnectionCore::restartIdleTimer(const Timestamp now)
    {
        // 两端都没宣告（或还没拿到对端参数）时这条超时不启用，也就没有截止时刻可记
        m_idlePeriod = effectiveIdleTimeout();
        if (m_idlePeriod.has_value())
        {
            m_idleDeadline = now + *m_idlePeriod;
        }
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::effectiveIdleTimeout() const noexcept
    {
        // §18.2：两端都宣告取较小的那个；只有一端宣告非 0 就用那一个；两个都是 0 表示不启用
        const std::uint64_t localMilliseconds     = m_configuration.transportParameters.maximumIdleTimeoutMilliseconds;
        const std::uint64_t peerMilliseconds      = m_peerParameters.has_value() ? m_peerParameters->maximumIdleTimeoutMilliseconds : 0;
        std::uint64_t       effectiveMilliseconds = 0;
        if (localMilliseconds != 0 && peerMilliseconds != 0)
        {
            effectiveMilliseconds = std::min(localMilliseconds, peerMilliseconds);
        } else
        {
            effectiveMilliseconds = localMilliseconds != 0 ? localMilliseconds : peerMilliseconds;
        }
        if (effectiveMilliseconds == 0)
        {
            return std::nullopt;
        }
        // 变长整数的上界（2^62-1 毫秒 ≈ 1.46 亿年）远超计时器能表达的跨度：Timestamp 是微秒，毫秒折成
        // 微秒要乘 1000，乘完的数还得留得下「最后一次活动的时刻」本身。因此取可表达范围的一半，另一半
        // 让给 now——按 µs 计的纪元值约 1.8×10^15，连这半数的十亿分之一都不到。越界的取值走与 0 同一条
        // 出口（不启用）：RFC 9000 §18.2 里 0 的含义就是「没设这一项」，而一个折不进计时器的天文数字说的
        // 也是同一件事。两端里较小的那个才会到这里，所以本端自己配得过大也一样收在这道闸上
        constexpr std::uint64_t kMaximumUsableIdleTimeoutMilliseconds = static_cast<std::uint64_t>(Timestamp::max().count()) / 2000ULL;
        if (effectiveMilliseconds > kMaximumUsableIdleTimeoutMilliseconds)
        {
            return std::nullopt;
        }
        // §10.1 的硬要求：至少留够 3 倍当前 PTO，否则一次抖动就把好端端的连接判死。
        // 这个下限只在重算截止时刻时取一次，之后 PTO 因退避翻倍不再往后推它
        const Timestamp probePeriod = m_recovery.roundTripTimeEstimate().probeTimeout;
        return std::max(Timestamp{std::chrono::milliseconds{effectiveMilliseconds}}, probePeriod * 3);
    }

    void QuicConnectionCore::onTimeout(const Timestamp now)
    {
        if (m_phase == QuicConnectionPhase::Closing)
        {
            return;
        }
        if (const std::optional<Timestamp> idleDeadline = idleDeadlineTime(); idleDeadline.has_value() && *idleDeadline <= now)
        {
            // §10.1：空闲超时是「静默关闭」——不发 CONNECTION_CLOSE（对端本来就没在说话，发了也没人收），
            // 也不留待发数据报：本连接到此为止，外层看 isFinished() 就能回收
            m_phase               = QuicConnectionPhase::Closing;
            m_localCloseErrorCode = std::nullopt;
            m_localCloseReasonPhrase.clear();
            m_outboundDatagrams.clear();
            return;
        }
        // 路径 MTU 那两个计时器都记在这里：探针到点没被确认算一次失败（连满三条才收口），抬升计时器到点
        // 就重新往上找。真正把探针发出去的是下面那次按空间排队——它从 Application 那格走进 queuePathMtuProbe
        m_pathMtu.onDeadlineReached(now);
        // 路径验证的两把计时：到点先按 §13.3/§9.4 换一份新数据重发，到期限就按 §8.2.4 放弃这次验证
        expirePathChallenges(now);
        if (m_phase == QuicConnectionPhase::Closing)
        {
            return; // 放弃时可能刚刚静默收口（§9.3.2 末段）：这一拍不再产出任何报文
        }
        const QuicRecoveryTimeoutAction action = m_recovery.onDeadlineReached(now);
        if (!action.lost.empty())
        {
            // 这一侧不重启窗口：§7.6.2 把「建立持久拥塞」放在收到确认之后，恢复层在这里只把新判丢
            // 并进那一段的锚点，等到下一帧确认再给结论
            m_congestion->onCongestionUpdate({}, action.lost, now);
            queueRetransmissions(spaceOf(action.lostSpace), action.lost);
            for (const QuicSentPacketInfo &packet: action.lost)
            {
                m_streams.onSendRangesLost(packet.streamRanges);
            }
        }
        if (action.isProbeTimeout)
        {
            // 判丢之外还要探一条包出去：先重发仍未确认的握手字节，没有可发的就补一条 PING（§6.2.2）
            m_probeSpace                                        = spaceOf(action.probeSpace);
            const std::vector<QuicSentPacketInfo> stillInFlight = m_recovery.unacknowledgedPackets(action.probeSpace);
            queueRetransmissions(spaceOf(action.probeSpace), stillInFlight);
        }
        for (const PacketNumberSpace space: {PacketNumberSpace::Initial, PacketNumberSpace::Handshake, PacketNumberSpace::Application})
        {
            queueSpacePackets(space, now);
        }
        // 重发的那条挑战（或验证通过后补的一轮 MTU 验证）在这一拍上线
        queuePathValidationPackets(now);
    }

    auto QuicConnectionCore::takeOutboundDatagram() -> std::optional<QuicOutboundDatagram>
    {
        if (m_outboundDatagrams.empty())
        {
            return std::nullopt;
        }
        std::optional<QuicOutboundDatagram> datagram = std::move(m_outboundDatagrams.front());
        m_outboundDatagrams.pop_front();
        return datagram;
    }

    bool QuicConnectionCore::canInitiateKeyUpdate(const Timestamp now) const noexcept
    {
        // §6.1：握手确认之前不许更新；上一个相位没被确认过也不许再来一次
        if (!m_isHandshakeConfirmed || m_phase != QuicConnectionPhase::Established)
        {
            return false;
        }
        if (m_lowestPacketNumberSentInKeyPhase.has_value() && !m_isKeyPhaseAcknowledged)
        {
            return false;
        }
        if (!m_keyPhaseChangedAt.has_value())
        {
            return true;
        }
        // §6.5 建议等满 3 倍 PTO：对端可能还留着上一代读密钥，太早翻它会把它的数据打成丢包
        const Timestamp probePeriod = m_recovery.roundTripTimeEstimate().probeTimeout;
        return now - *m_keyPhaseChangedAt >= probePeriod * 3;
    }

    bool QuicConnectionCore::initiateKeyUpdate(const Timestamp now)
    {
        if (!canInitiateKeyUpdate(now))
        {
            return false;
        }
        SpaceState &state = m_spaces[spaceIndex(PacketNumberSpace::Application)];
        if (!state.writeKeys.has_value())
        {
            return false;
        }
        state.writeKeys     = deriveQuicUpdatedPacketKeys(*state.writeKeys);
        m_isSendKeyPhaseSet = !m_isSendKeyPhaseSet;
        // 本相位的计数从下一个要发的包号起算，确认只要落在它之上就算这次更新到位
        m_lowestPacketNumberSentInKeyPhase = state.nextPacketNumber;
        m_isKeyPhaseAcknowledged           = false;
        m_keyPhaseChangedAt                = now;
        return true;
    }

    std::string_view QuicConnectionCore::selectedApplicationProtocol() const noexcept
    {
        return m_tls->selectedApplicationProtocol();
    }

    QuicConnectionCore::Timestamp QuicConnectionCore::probeTimeoutPeriod() const noexcept
    {
        return m_recovery.roundTripTimeEstimate().probeTimeout;
    }

    std::size_t QuicConnectionCore::issuedConnectionIdCount() const noexcept
    {
        return m_issuedConnectionIds.size();
    }

    std::size_t QuicConnectionCore::peerConnectionIdCount() const noexcept
    {
        return m_peerConnectionIds.size();
    }

    std::span<const std::uint8_t> QuicConnectionCore::activePeerConnectionId() const noexcept
    {
        return std::span<const std::uint8_t>(m_configuration.peerConnectionId);
    }

    QuicRoundTripTimeEstimate QuicConnectionCore::roundTripTimeEstimate() const noexcept
    {
        return m_recovery.roundTripTimeEstimate();
    }

    std::array<std::uint8_t, 16> QuicConnectionCore::statelessResetTokenOf(const std::span<const std::uint8_t> connectionId) const
    {
        return deriveQuicStatelessResetToken(std::span<const std::uint8_t>(m_statelessResetKey), connectionId);
    }

    std::array<std::uint8_t, kQuicStatelessResetTokenByteLength> deriveQuicStatelessResetToken(const std::span<const std::uint8_t> staticKey,
                                                                                               const std::span<const std::uint8_t> connectionId) noexcept
    {
        // 密钥过短就算不出来：§8.1.4 那条「难猜」是给这类令牌定的，而全零令牌会把「末尾 16 个零
        // 字节的报文」变成一条人人可发的掐线指令（§10.3.1 只许认对端真发过来的令牌）
        if (staticKey.size() < kQuicMinimumStatelessResetKeyByteLength || connectionId.empty())
        {
            return std::array<std::uint8_t, kQuicStatelessResetTokenByteLength>{};
        }
        // §10.3.2：令牌 = HMAC-SHA256(静态密钥, 标识) 截前 16 字节。这样任何一份进程只要拿着同一把
        // 密钥，就能为一条已经不存在的连接算出「它当初发的令牌该是什么」——无状态重置的全部前提。
        // 算不出来时不抛：签署路径在出包循环与收包路径上，抛出去会带着半个包离开驱动；交全零由调用方
        // 按「这一档没开」处置（签发侧与发送侧都按全零即无效处理）
        try
        {
            const Core::Digest::Sha256Value digest = Core::Digest::hmacSha256(std::string_view(reinterpret_cast<const char *>(staticKey.data()), staticKey.size()),
                                                                              std::string_view(reinterpret_cast<const char *>(connectionId.data()), connectionId.size()));
            std::array<std::uint8_t, kQuicStatelessResetTokenByteLength> token{};
            std::copy_n(digest.begin(), token.size(), token.begin());
            return token;
        } catch (const Base::Exception &)
        {
            return std::array<std::uint8_t, kQuicStatelessResetTokenByteLength>{};
        }
    }

    bool QuicConnectionCore::isStatelessResetDatagram(const std::span<const std::uint8_t> datagram) const noexcept
    {
        const auto active = std::ranges::find(m_peerConnectionIds, m_configuration.peerConnectionId, &PeerConnectionId::id);
        if (active == m_peerConnectionIds.end() || !active->hasStatelessResetToken)
        {
            return false;
        }
        // 常数时间比对（§10.3.1 的 MUST：不得通过计时泄露令牌取值；正文给的另一条路是把令牌变换后存，
        // 这里走第一条路）。CRYPTO_memcmp 在首个不等字节处也走完全程
        return CRYPTO_memcmp(active->statelessResetToken.data(), datagram.data() + datagram.size() - kQuicStatelessResetTokenByteLength, kQuicStatelessResetTokenByteLength) == 0;
    }

    void QuicConnectionCore::enterDrainingAfterStatelessReset()
    {
        // §10.3.1 末段：对上令牌即进 draining——此后不再往这条连接上发任何东西，也不发 CLOSE
        // （向一个显然已经不认识我们的对端继续付出，全是白费）。形状与空闲静默收口一致
        m_phase               = QuicConnectionPhase::Closing;
        m_localCloseErrorCode = std::nullopt;
        m_localCloseReasonPhrase.clear();
        m_outboundDatagrams.clear();
        if (m_configuration.onStatelessResetDetected)
        {
            m_configuration.onStatelessResetDetected();
        }
    }

    void QuicConnectionCore::notePeerHandshakeStatelessResetToken(const std::array<std::uint8_t, 16> &token) noexcept
    {
        // 令牌随它对应的那条标识走，而传输参数里这一份只配给握手期那条（序列号 0）。此刻它还没被
        // 收下（客户端要等服务端自报源标识之后才补种）也没关系：参数到手时那条一定在册了
        const auto handshakeConnectionId = std::ranges::find(m_peerConnectionIds, std::uint64_t{0}, &PeerConnectionId::sequence);
        if (handshakeConnectionId == m_peerConnectionIds.end())
        {
            return;
        }
        handshakeConnectionId->statelessResetToken    = token;
        handshakeConnectionId->hasStatelessResetToken = true;
    }

    std::size_t QuicConnectionCore::issueConnectionId(const bool isAdvertised)
    {
        // 长度必须与握手那条一致：短头线上没有长度字段，收包侧按定长切标识（§5.1 不禁止换长度，
        // 但换了就再也认不回来，本实现因此把长度钉成握手那条的长度）
        const std::vector<std::uint8_t> id = randomBytes(m_configuration.localConnectionId.size());
        if (id.empty())
        {
            // 随机数不可用：不签一条可猜的标识出去（旁观者能提前把报文路由到自己想去的地方），
            // 交回表尾表示这次没签成——调用方本轮就不再补位
            return m_issuedConnectionIds.size();
        }
        const std::size_t index = m_issuedConnectionIds.size();
        m_issuedConnectionIds.push_back(
                IssuedConnectionId{.id = id, .sequence = m_nextConnectionIdSequence++, .statelessResetToken = statelessResetTokenOf(id), .isAdvertised = isAdvertised});
        // 登记早于上线：通知之后才可能被编进 NEW_CONNECTION_ID（见 appendPendingConnectionIdFrames）
        if (m_configuration.onConnectionIdIssued)
        {
            m_configuration.onConnectionIdIssued(std::span<const std::uint8_t>(m_issuedConnectionIds[index].id));
        }
        return index;
    }

    bool QuicConnectionCore::hasRoomForAnotherConnectionId() const noexcept
    {
        // 对端没送这一项参数时按 §18.2 的缺省值 2 算（也就是本端自己通告的那一档）
        const std::uint64_t peerLimit = m_peerParameters.has_value() ? m_peerParameters->activeConnectionIdLimit : kQuicDefaultActiveConnectionIdLimit;
        return m_issuedConnectionIds.size() < peerLimit;
    }

    void QuicConnectionCore::retirePeerConnectionIdsBelow(const std::uint64_t retirePriorTo)
    {
        for (auto iterator = m_peerConnectionIds.begin(); iterator != m_peerConnectionIds.end();)
        {
            if (iterator->sequence >= retirePriorTo)
            {
                ++iterator;
                continue;
            }
            // §5.1.2：不许不打招呼就忘掉一条标识——每退一条都要发一条 RETIRE_CONNECTION_ID。
            // 序列号进「已通报」表而不是当场出队：判丢要整批重发（§13.3）
            if (iterator->sequence > m_largestRetiredSequenceSent)
            {
                m_largestRetiredSequenceSent = iterator->sequence;
            }
            if (std::ranges::find(m_retiredSequencesSent, iterator->sequence) == m_retiredSequencesSent.end())
            {
                m_retiredSequencesSent.push_back(iterator->sequence);
            }
            iterator = m_peerConnectionIds.erase(iterator);
        }
    }

    void QuicConnectionCore::ensureActivePeerConnectionIdIsStillHeld()
    {
        // 在用的那条不在了（本端把它退了、或对端用 Retire Prior To 收回了）就换到剩下序列号最小的一条上：
        // 本端此后仍要回包，手上必须有标识可打。调用点排在「添新的那条」之后——§5.1.2 要求先退再添，
        // 而换用的对象正是刚添进来的那条
        const bool isActiveStillHeld =
                std::ranges::any_of(m_peerConnectionIds, [this](const PeerConnectionId &candidate) { return candidate.id == m_configuration.peerConnectionId; });
        if (isActiveStillHeld || m_peerConnectionIds.empty())
        {
            return;
        }
        const auto lowest                = std::ranges::min_element(m_peerConnectionIds, {}, [](const PeerConnectionId &candidate) { return candidate.sequence; });
        m_configuration.peerConnectionId = lowest->id;
    }

    void QuicConnectionCore::handleNewConnectionIdFrame(const QuicNewConnectionIdFrame &frame, const Timestamp arrivalTime)
    {
        // §19.15：Retire Prior To 大于序列号＝这一帧自己坏了，按 FRAME_ENCODING_ERROR 收口
        if (frame.retirePriorTo > frame.sequenceNumber)
        {
            beginClose(kQuicFrameEncodingError, "对端 NEW_CONNECTION_ID 的 Retire Prior To 大于序列号（RFC 9000 §19.15）", arrivalTime);
            return;
        }
        // 序列号与标识的对应关系是这条帧的骨架：同一个号报另一条标识、或同一条标识报成两个号，
        // 都会让「退哪一条」失去依据。§19.15 给的是 MAY（可按 PROTOCOL_VIOLATION 收口），本端从严
        for (const PeerConnectionId &existing: m_peerConnectionIds)
        {
            if (existing.sequence == frame.sequenceNumber)
            {
                if (!std::ranges::equal(existing.id, frame.connectionId) || !std::ranges::equal(existing.statelessResetToken, frame.statelessResetToken))
                {
                    beginClose(kQuicProtocolViolation, "对端用同一个序列号报了另一条连接标识（RFC 9000 §19.15）", arrivalTime);
                    return;
                }
                // 重复收到同一条帧：同节明写不算错，静默忽略（重发是常态，见 §13.3）
                return;
            }
            if (std::ranges::equal(existing.id, frame.connectionId))
            {
                beginClose(kQuicProtocolViolation, "对端用两个序列号报了同一条连接标识（RFC 9000 §19.15）", arrivalTime);
                return;
            }
        }
        // Retire Prior To 只认递增的值：较小的那些不再有效（§19.15），本端按「先退再添」的顺序办
        if (frame.retirePriorTo > m_peerRetirePriorTo)
        {
            m_peerRetirePriorTo = frame.retirePriorTo;
            retirePeerConnectionIdsBelow(frame.retirePriorTo);
        }
        m_peerConnectionIds.push_back(PeerConnectionId{});
        PeerConnectionId &added = m_peerConnectionIds.back();
        added.id.assign(frame.connectionId.begin(), frame.connectionId.end());
        added.sequence = frame.sequenceNumber;
        std::copy_n(frame.statelessResetToken.begin(), added.statelessResetToken.size(), added.statelessResetToken.begin());
        added.hasStatelessResetToken = true; // §10.3.1：这一帧带来的令牌是「对端真发过来的」，从此可以拿它比对
        // 换用排在添新之后：§5.1.2 要求先退再添，而「在用那条被退了」时要换过去的正是刚添的这条
        ensureActivePeerConnectionIdIsStillHeld();
        // §5.1.1：处理完这一帧之后，本端持有的条数超出本端通告的 active_connection_id_limit 就只能收口。
        // 本端通告的是 2，正常客户端按它发就不会走到这里；走到这里说明对端多给了
        if (m_peerConnectionIds.size() > m_configuration.transportParameters.activeConnectionIdLimit)
        {
            beginClose(kQuicConnectionIdLimitError, "对端给的连接标识条数超过本端通告的 active_connection_id_limit（RFC 9000 §5.1.1）", arrivalTime);
        }
    }

    void QuicConnectionCore::handleRetireConnectionIdFrame(const QuicRetireConnectionIdFrame &frame, const Timestamp arrivalTime)
    {
        // §19.16：撤回一个本端从没发出去过的序列号＝PROTOCOL_VIOLATION
        if (frame.sequenceNumber >= m_nextConnectionIdSequence)
        {
            beginClose(kQuicProtocolViolation, "对端撤回了一条本端从未签发的连接标识（RFC 9000 §19.16）", arrivalTime);
            return;
        }
        const auto retired = std::ranges::find(m_issuedConnectionIds, frame.sequenceNumber, [](const IssuedConnectionId &issued) { return issued.sequence; });
        if (retired == m_issuedConnectionIds.end())
        {
            // 退一条已经退过的：重发导致的重复帧，无害（幂等），按同一条帧收两遍处理
            return;
        }
        if (m_configuration.onConnectionIdRetired)
        {
            m_configuration.onConnectionIdRetired(std::span<const std::uint8_t>(retired->id));
        }
        m_issuedConnectionIds.erase(retired);
        // §5.1.1 的 SHOULD（对端退回来一条就补一条）不在这里办：补位统一由 1-RTT 出包前的
        // prepareOneRttPackets 按「在册条数 < 对端上限」补齐——退一条与丢一条因此走同一条补位路径，
        // 少一处会忘、也就少一处会与上限打架
    }

    bool QuicConnectionCore::appendPendingConnectionIdFrames(std::string &frames)
    {
        bool appendedAny = false;
        for (const IssuedConnectionId &issued: m_issuedConnectionIds)
        {
            // 握手那条随长头隐含（isAdvertised 为真且不重发）；判丢整批重发时连它也不用发
            if (issued.isAdvertised && !m_isConnectionIdFramesDirty)
            {
                continue;
            }
            QuicNewConnectionIdFrame frame;
            frame.sequenceNumber      = issued.sequence;
            frame.retirePriorTo       = 0;
            frame.connectionId        = issued.id;
            frame.statelessResetToken = issued.statelessResetToken;
            appendQuicFrame(frames, QuicFrame{frame});
            appendedAny = true;
        }
        for (const std::uint64_t retiredSequence: m_retiredSequencesSent)
        {
            QuicRetireConnectionIdFrame frame;
            frame.sequenceNumber = retiredSequence;
            appendQuicFrame(frames, QuicFrame{frame});
            appendedAny = true;
        }
        if (appendedAny)
        {
            // 上线了：把「还没上线」的标记清掉，并把整批重发标记复位——这一包就是那批重发
            for (IssuedConnectionId &issued: m_issuedConnectionIds)
            {
                issued.isAdvertised = true;
            }
            m_isConnectionIdFramesDirty = false;
        }
        return appendedAny;
    }

    const QuicPacketKeys *QuicConnectionCore::applicationWriteKeys() const noexcept
    {
        const SpaceState &state = m_spaces[spaceIndex(PacketNumberSpace::Application)];
        return state.writeKeys.has_value() ? &*state.writeKeys : nullptr;
    }

    void QuicConnectionCore::applyPeerKeyUpdate(SpaceState &state, const Timestamp now)
    {
        // §6.3：读侧始终备着两套。提成当前之后立刻推一把新的出来，下一轮更新仍不必在解包路径上现算
        state.previousReadKeys = std::move(state.readKeys);
        state.readKeys         = std::move(state.nextReadKeys);
        state.nextReadKeys     = deriveQuicUpdatedPacketKeys(*state.readKeys);
        m_isReadKeyPhaseSet    = !m_isReadKeyPhaseSet;
        m_keyPhaseChangedAt    = now;

        // §6.2：回确认之前，发密钥必须也推进到同一相位；带新相位的确认就是「这次更新已完成」的信号
        if (m_isSendKeyPhaseSet != m_isReadKeyPhaseSet)
        {
            SpaceState &application = m_spaces[spaceIndex(PacketNumberSpace::Application)];
            if (application.writeKeys.has_value())
            {
                application.writeKeys = deriveQuicUpdatedPacketKeys(*application.writeKeys);
            }
            m_isSendKeyPhaseSet                = m_isReadKeyPhaseSet;
            m_lowestPacketNumberSentInKeyPhase = application.nextPacketNumber;
            m_isKeyPhaseAcknowledged           = false;
        }
    }

    void QuicConnectionCore::retireStaleKeyPhase(const Timestamp now)
    {
        SpaceState &state = m_spaces[spaceIndex(PacketNumberSpace::Application)];
        if (!state.previousReadKeys.has_value() || !m_keyPhaseChangedAt.has_value())
        {
            return;
        }
        // §6.5：旧读密钥最多留 3 倍 PTO，过了就该收，否则网络里再晚到的包也不再算「还认得」
        const Timestamp probePeriod = m_recovery.roundTripTimeEstimate().probeTimeout;
        if (now - *m_keyPhaseChangedAt > probePeriod * 3)
        {
            state.previousReadKeys = std::nullopt;
        }
    }

    QuicConnectionPhase QuicConnectionCore::phase() const noexcept
    {
        return m_phase;
    }

    std::size_t QuicConnectionCore::bytesInFlightByteCount() const noexcept
    {
        return m_congestion->bytesInFlight();
    }

    std::size_t QuicConnectionCore::congestionWindowByteLength() const noexcept
    {
        return m_congestion->congestionWindowByteLength();
    }

    const QuicCongestionAlgorithm &QuicConnectionCore::congestionAlgorithm() const noexcept
    {
        return *m_congestion;
    }

    QuicEcnState QuicConnectionCore::ecnState() const noexcept
    {
        return m_recovery.ecnState();
    }

    std::array<std::uint64_t, 3> QuicConnectionCore::receivedEcnCount(const QuicEncryptionLevel level) const noexcept
    {
        return m_receivedEcnCounts[spaceIndex(spaceOf(level))];
    }

    std::size_t QuicConnectionCore::persistentCongestionEventCount() const noexcept
    {
        return m_persistentCongestionEventCount;
    }

    std::size_t QuicConnectionCore::maximumDatagramPayloadByteLength() const noexcept
    {
        return m_pathMtu.maximumDatagramPayloadByteLength();
    }

    QuicPathMtuPhase QuicConnectionCore::pathMtuPhase() const noexcept
    {
        return m_pathMtu.phase();
    }

    std::pair<std::size_t, std::size_t> QuicConnectionCore::pathMtuProbeState() const noexcept
    {
        return {m_pathMtu.probedDatagramPayloadByteLength(), m_pathMtu.failedProbeCount()};
    }

    bool QuicConnectionCore::isFinished() const noexcept
    {
        return m_phase == QuicConnectionPhase::Closing && m_outboundDatagrams.empty();
    }

    const QuicTransportParameters *QuicConnectionCore::peerTransportParameters() const noexcept
    {
        return m_peerParameters.has_value() ? &*m_peerParameters : nullptr;
    }
} // namespace AsynGyanis::Net
