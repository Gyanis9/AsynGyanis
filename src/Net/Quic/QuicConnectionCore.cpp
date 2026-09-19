// OpenSSL 的 Windows 头会把 windef.h 的 min/max 宏一起带进来，`std::max<...>` 会被展开成非法表达式。
// 必须在任何头文件之前定义 NOMINMAX 才能挡住这对宏：放在文件最顶部是唯一与包含顺序无关的写法。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Net/Quic/QuicConnectionCore.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"
#include "Net/Quic/QuicPacketBuilder.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 没做 PMTU 探测、也没学到对端 max_udp_payload_size 之前的数据报上限（RFC 9000 §14.1）
        constexpr std::size_t kQuicMaximumDatagramPayloadByteLength = 1200;

        /// 乱序早到的 CRYPTO 分片总量上限，超了直接收口。这是本实现自设的自我保护值，不是规范值
        constexpr std::size_t kQuicCryptoBufferByteLimit = 65536;

        /// 每空间跟踪的已收包号上限，超出后丢弃最小的那些（只影响 ACK 能覆盖多老的历史）
        constexpr std::size_t kQuicMaximumTrackedPacketNumbers = 4096;

        /// 一条 ACK 最多带几段区间：砍掉尾部老区间不改变「哪些包到了」的结论，却能保证 ACK 帧本身
        /// 不会大到把整包预算吃光
        constexpr std::size_t kQuicMaximumAcknowledgementRanges = 32;

        // 传输层错误码（RFC 9000 §11.1）与 TLS 告警的映射基值（RFC 9001 §4.8）
        constexpr std::uint64_t kQuicNoError = 0x00;
        constexpr std::uint64_t kQuicFrameEncodingError = 0x07;
        constexpr std::uint64_t kQuicTransportParameterError = 0x08;
        constexpr std::uint64_t kQuicProtocolViolation = 0x0a;
        constexpr std::uint64_t kQuicCryptoBufferExceeded = 0x0d;
        constexpr std::uint64_t kQuicCryptoErrorBase = 0x0100;

        /// CRYPTO 帧自身的最长帧头：类型 1 字节 + 偏移最多 8 + 长度最多 8，再留 1 字节余量
        constexpr std::size_t kQuicCryptoFrameHeaderByteLimit = 18;

        /**
         * @brief 二进制安全的字符串转字节视图
         * @param text 内容可以是任意字节
         * @return std::span<const std::uint8_t> 指向原文的视图，空串也返回空视图而非空指针
         */
        std::span<const std::uint8_t> asBytes(const std::string &text)
        {
            return text.empty() ? std::span<const std::uint8_t>{}
                                : std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
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
                throw Base::InvalidArgumentException(std::format("QUIC 连接建立失败：{}连接标识有 {} 字节，超过 v1 上限 {} 字节（RFC 9000 §5.1）",
                                                                 label, connectionId.size(), kQuicMaximumConnectionIdLength));
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
         * @brief 把已收到的包号集合折成 ACK 区间
         * @details §19.3.1 要求区间按包号递减、互不重叠且相邻的不合并；`QuicAcknowledgementRange`
         *          存绝对包号，递推出来的 gap 由帧编码器负责。
         * @param receivedPacketNumbers 升序的已收包号
         * @param acknowledgedUpTo 本次确认到的包号（含）
         * @return std::vector<QuicAcknowledgementRange> 递减的区间，至多 `kQuicMaximumAcknowledgementRanges` 段
         */
        std::vector<QuicAcknowledgementRange> buildAcknowledgementRanges(const std::set<std::uint64_t> &receivedPacketNumbers,
                                                                         const std::uint64_t acknowledgedUpTo)
        {
            std::vector<QuicAcknowledgementRange> ranges;
            for (const std::uint64_t packetNumber : receivedPacketNumbers | std::views::reverse)
            {
                if (packetNumber > acknowledgedUpTo)
                {
                    continue;
                }
                if (!ranges.empty() && ranges.back().smallestAcknowledged == packetNumber + 1)
                {
                    ranges.back().smallestAcknowledged = packetNumber;
                    continue;
                }
                if (ranges.size() >= kQuicMaximumAcknowledgementRanges)
                {
                    break;
                }
                ranges.push_back(QuicAcknowledgementRange{packetNumber, packetNumber});
            }
            return ranges;
        }

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
    } // namespace

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::spaceOf(const QuicEncryptionLevel level) noexcept
    {
        // 0-RTT 与 Initial 共用一个空间（RFC 9000 §17.2.3：0-RTT 包续用 Initial 的包号序列）
        switch (level)
        {
        case QuicEncryptionLevel::Initial:
        case QuicEncryptionLevel::ZeroRtt: return PacketNumberSpace::Initial;
        case QuicEncryptionLevel::Handshake: return PacketNumberSpace::Handshake;
        case QuicEncryptionLevel::Application: return PacketNumberSpace::Application;
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
        case PacketNumberSpace::Initial: return QuicEncryptionLevel::Initial;
        case PacketNumberSpace::Handshake: return QuicEncryptionLevel::Handshake;
        case PacketNumberSpace::Application: return QuicEncryptionLevel::Application;
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
        case QuicLongPacketType::Initial: return QuicEncryptionLevel::Initial;
        case QuicLongPacketType::Handshake: return QuicEncryptionLevel::Handshake;
        // 本端没开早数据，0-RTT 没有能解它的密钥；服务端也不会收到 Retry（§17.2.5）
        case QuicLongPacketType::ZeroRtt:
        case QuicLongPacketType::Retry: return std::nullopt;
        }
        return std::nullopt;
    }

    QuicConnectionCore::QuicConnectionCore(QuicConnectionCoreConfiguration configuration)
        : m_configuration(std::move(configuration))
    {
        if (m_configuration.tlsContext == nullptr)
        {
            throw Base::InvalidArgumentException("QUIC 连接建立失败：配置里的 TLS 上下文为空，调用方须先配好证书与 ALPN");
        }
        requireConnectionIdWithinLimit(m_configuration.localConnectionId, "本端签发");
        requireConnectionIdWithinLimit(m_configuration.peerConnectionId, "对端自报");
        requireConnectionIdWithinLimit(m_configuration.originalDestinationConnectionId, "客户端第一个 Initial 的目的");

        // §7.3 要求服务端把「收到的原始目的标识」和「自己第一个 Initial 的源标识」都写进参数：
        // 这两项由本类按配置补齐，调用方漏了也照样合法，免得每条连接都要手抄一遍再抄错
        QuicTransportParameters localParameters = m_configuration.transportParameters;
        localParameters.originalDestinationConnectionId = m_configuration.originalDestinationConnectionId;
        localParameters.initialSourceConnectionId = m_configuration.localConnectionId;
        std::string encodedParameters;
        appendQuicTransportParameters(encodedParameters, localParameters);
        m_configuration.transportParameters = std::move(localParameters);

        m_tls = std::make_unique<QuicTlsContext>(*m_configuration.tlsContext, true, asBytes(encodedParameters));

        // Initial 密钥只由客户端第一个 Initial 的目的标识决定（RFC 9001 §5.2）：服务端在收到任何字节
        // 之前就知道该用什么密钥解，这正是 Initial 不需要协商的原因
        SpaceState &initialSpace = m_spaces[spaceIndex(PacketNumberSpace::Initial)];
        initialSpace.readKeys = deriveQuicInitialPacketKeys(m_configuration.originalDestinationConnectionId,
                                                            QuicPacketDirection::ClientToServer);
        initialSpace.writeKeys = deriveQuicInitialPacketKeys(m_configuration.originalDestinationConnectionId,
                                                             QuicPacketDirection::ServerToClient);
    }

    std::expected<void, QuicDecodeError> QuicConnectionCore::onDatagramReceived(const std::span<const std::uint8_t> datagram,
                                                                                const Timestamp arrivalTime)
    {
        std::size_t offset = 0;
        while (offset < datagram.size())
        {
            const auto remainder = datagram.subspan(offset);
            const auto decodedHeader = decodeQuicPacketHeader(remainder, m_configuration.localConnectionId.size());
            if (!decodedHeader.has_value() || decodedHeader->packetByteCount == 0 ||
                offset + decodedHeader->packetByteCount > datagram.size())
            {
                // 连包头都解不出来，后面的字节也就没了依据：整条数据报丢弃（RFC 9000 §5.1）
                return {};
            }
            const std::expected<void, QuicDecodeError> handled = handlePacket(remainder.subspan(0, decodedHeader->packetByteCount),
                                                                              *decodedHeader, arrivalTime);
            if (!handled.has_value())
            {
                return handled;
            }
            offset += decodedHeader->packetByteCount;
        }
        return {};
    }

    std::expected<void, QuicDecodeError> QuicConnectionCore::handlePacket(const std::span<const std::uint8_t> packet,
                                                                          const QuicPacketHeader &plainHeader,
                                                                          const Timestamp arrivalTime)
    {
        const std::optional<QuicEncryptionLevel> level = levelOf(plainHeader);
        if (!level.has_value())
        {
            return {};
        }

        // 目的标识必须落在本连接上：Initial 允许对端凭空造的那个原始标识，其余只认本端签发的
        if (!std::ranges::equal(plainHeader.destinationConnectionId, m_configuration.localConnectionId) &&
            !(*level == QuicEncryptionLevel::Initial &&
              std::ranges::equal(plainHeader.destinationConnectionId, m_configuration.originalDestinationConnectionId)))
        {
            return {};
        }

        const PacketNumberSpace space = spaceOf(*level);
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.readKeys.has_value())
        {
            // 密钥还没就绪（Handshake 早到、或本实现不收的形态）：丢弃但不算对端违规
            return {};
        }

        // 去头部保护是就地改写，所以在可写副本上做；原数据报的其余部分不受影响
        std::vector<std::uint8_t> workingBytes(packet.begin(), packet.end());
        const std::span<std::uint8_t> workingPacket(workingBytes);

        const std::expected<std::span<const std::uint8_t>, QuicDecodeError> sample =
                extractQuicHeaderProtectionSample(workingPacket, plainHeader);
        if (!sample.has_value())
        {
            return {};
        }
        const QuicHeaderProtectionMask mask = generateQuicHeaderProtectionMask(*state.readKeys, *sample);
        const std::expected<std::uint8_t, QuicDecodeError> unmaskedFirstByte =
                removeQuicHeaderProtection(workingPacket, plainHeader, mask);
        if (!unmaskedFirstByte.has_value())
        {
            return {};
        }

        QuicPacketHeader header = plainHeader;
        if (const std::expected<void, QuicDecodeError> refreshed = refreshQuicPacketHeader(header, *unmaskedFirstByte, workingPacket);
            !refreshed.has_value())
        {
            return {};
        }
        if (!header.areReservedBitsClear())
        {
            // 去保护后保留位非 0：能去掉保护就说明密钥对上了，这是对端真这么发的，按 §17.2/§17.3.1 回错
            beginClose(kQuicProtocolViolation, "报文首字节的保留位在去头部保护后非 0（RFC 9000 §17.2/§17.3.1）");
            return std::unexpected(QuicDecodeError{QuicDecodeErrorKind::Malformed, "报文首字节的保留位非 0"});
        }

        const std::uint64_t packetNumber = restoreQuicPacketNumber(state.largestReceivedPacketNumber.value_or(0),
                                                                   header.packetNumber, header.packetNumberByteCount);
        const std::size_t headerByteCount = header.packetNumberOffset + header.packetNumberByteCount;
        if (workingPacket.size() <= headerByteCount + kQuicAuthenticationTagByteLength)
        {
            // 连一个 AEAD 标签都装不下，不可能是合法包
            return {};
        }

        const std::span<const std::uint8_t> additionalData = workingPacket.subspan(0, headerByteCount);
        const std::span<const std::uint8_t> protectedPayload = workingPacket.subspan(headerByteCount);
        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const std::expected<std::size_t, QuicDecodeError> opened =
                openQuicProtectedPayload(plaintext, *state.readKeys, packetNumber, additionalData, protectedPayload);
        if (!opened.has_value())
        {
            // 标签不合按 RFC 9001 §4.1.4 静默丢弃：可能是密钥不合或在途被改，回错误码只会喂攻击者
            return {};
        }

        // 到这一步包才是「认证过」的，包号记账与重复判定都只在这个前提下推进
        if (!state.receivedPacketNumbers.insert(packetNumber).second)
        {
            return {};
        }
        while (state.receivedPacketNumbers.size() > kQuicMaximumTrackedPacketNumbers)
        {
            state.receivedPacketNumbers.erase(state.receivedPacketNumbers.begin());
        }
        if (!state.largestReceivedPacketNumber.has_value() || packetNumber > *state.largestReceivedPacketNumber)
        {
            state.largestReceivedPacketNumber = packetNumber;
        }
        if (*level == QuicEncryptionLevel::Initial && !m_peerFirstInitialSourceConnectionId.has_value())
        {
            m_peerFirstInitialSourceConnectionId = std::vector<std::uint8_t>(header.sourceConnectionId.begin(),
                                                                            header.sourceConnectionId.end());
        }

        const std::expected<std::vector<QuicFrame>, QuicDecodeError> frames =
                decodeQuicFrames(std::span<const std::uint8_t>(plaintext).subspan(0, *opened));
        if (!frames.has_value())
        {
            // 能解密就说明这包出自持有密钥的对端，帧解不开是对端违规（§19.1）
            beginClose(frames.error().kind == QuicDecodeErrorKind::Truncated ? kQuicProtocolViolation : kQuicFrameEncodingError,
                       std::format("帧序列不合 RFC 9000 §19：{}", frames.error().message));
            return std::unexpected(frames.error());
        }

        bool hasAckElicitingFrame = false;
        for (const QuicFrame &frame : *frames)
        {
            hasAckElicitingFrame = hasAckElicitingFrame || isAcknowledgementEliciting(frame);
            handleFrame(frame, space);
        }
        if (hasAckElicitingFrame)
        {
            state.largestAckElicitingReceived = packetNumber;
            state.largestAckElicitingArrival = arrivalTime;
            state.isAcknowledgementPending = true;
        }
        return {};
    }

    void QuicConnectionCore::handleFrame(const QuicFrame &frame, const PacketNumberSpace space)
    {
        std::visit(
                [this, space](const auto &specificFrame)
                {
                    using FrameType = std::remove_cvref_t<decltype(specificFrame)>;
                    SpaceState &state = m_spaces[spaceIndex(space)];
                    if constexpr (std::same_as<FrameType, QuicAcknowledgementFrame>)
                    {
                        // 只接受落在「我们真发过」范围内的确认值：对端吹一个更大的数，不该让我们
                        // 以为握手已被确认；nextPacketNumber 为 0 时本分支被 hasSentPacket 挡在前面
                        if (state.hasSentPacket && specificFrame.largestAcknowledgedPacketNumber < state.nextPacketNumber &&
                            (!state.largestAcknowledgedPacketNumber.has_value() ||
                             specificFrame.largestAcknowledgedPacketNumber > *state.largestAcknowledgedPacketNumber))
                        {
                            state.largestAcknowledgedPacketNumber = specificFrame.largestAcknowledgedPacketNumber;
                        }
                    }
                    else if constexpr (std::same_as<FrameType, QuicCryptoFrame>)
                    {
                        handleCryptoBytes(space, specificFrame.offset, specificFrame.data);
                    }
                    else if constexpr (std::same_as<FrameType, QuicConnectionCloseFrame>)
                    {
                        // 对端已经收口：本端不再发任何东西，也不再回一个 CLOSE（§10.2.3）
                        m_phase = QuicConnectionPhase::Closing;
                    }
                    // PADDING 与 PING 不需要动作：PING 的确认由触发确认的记账统一处理。
                    // 流、流量控制、连接标识与路径验证那几类帧要到后续里程碑才有人接，本阶段忽略；
                    // HANDSHAKE_DONE 只有服务端会发，收到它说明对端把本端当成了客户端，同样忽略。
                },
                frame);
    }

    void QuicConnectionCore::handleCryptoBytes(const PacketNumberSpace space, const std::uint64_t offset,
                                               const std::span<const std::uint8_t> bytes)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (offset + bytes.size() <= state.receivedCryptoOffset)
        {
            // 整段都是重传（落在已交字节之前），丢掉即可
            return;
        }
        if (offset > state.receivedCryptoOffset)
        {
            if (state.bufferedOutOfOrderByteCount + bytes.size() > kQuicCryptoBufferByteLimit)
            {
                beginClose(kQuicCryptoBufferExceeded, "乱序早到的 CRYPTO 字节超过本端缓存上限（RFC 9000 §11.1）");
                return;
            }
            state.laterCryptoFragments.emplace(offset, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
            state.bufferedOutOfOrderByteCount += bytes.size();
            return;
        }

        m_tls->feedHandshakeData(levelOf(space), bytes);
        state.receivedCryptoOffset += bytes.size();
        // 这段接上之后，可能有更早到的分片正好续上，按偏移依次交给 TLS
        for (auto fragment = state.laterCryptoFragments.find(state.receivedCryptoOffset);
             fragment != state.laterCryptoFragments.end();
             fragment = state.laterCryptoFragments.find(state.receivedCryptoOffset))
        {
            m_tls->feedHandshakeData(levelOf(space), fragment->second);
            state.receivedCryptoOffset += fragment->second.size();
            state.bufferedOutOfOrderByteCount -= fragment->second.size();
            state.laterCryptoFragments.erase(fragment);
        }
    }

    void QuicConnectionCore::adoptTlsKeys()
    {
        // Initial 的密钥在建连接时就按 §5.2 推导好了，这里只接 TLS 交出的后两个级别
        for (const PacketNumberSpace space : {PacketNumberSpace::Handshake, PacketNumberSpace::Application})
        {
            SpaceState &state = m_spaces[spaceIndex(space)];
            const QuicEncryptionLevel level = levelOf(space);
            if (const QuicPacketKeys *reading = m_tls->keys(level, QuicKeyDirection::Reading);
                reading != nullptr && !state.readKeys.has_value())
            {
                state.readKeys = *reading;
            }
            if (const QuicPacketKeys *writing = m_tls->keys(level, QuicKeyDirection::Writing);
                writing != nullptr && !state.writeKeys.has_value())
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
            state.pendingCryptoBytes.insert(state.pendingCryptoBytes.end(), record->data.begin(), record->data.end());
        }
    }

    void QuicConnectionCore::adoptPeerTransportParameters()
    {
        if (m_peerParameters.has_value() || m_tls->peerTransportParameters().empty())
        {
            return;
        }
        const std::expected<QuicTransportParameters, QuicDecodeError> decoded =
                decodeQuicTransportParameters(m_tls->peerTransportParameters(), QuicTransportParameterSenderRole::Client);
        if (!decoded.has_value())
        {
            beginClose(kQuicTransportParameterError, std::format("对端传输参数不合格：{}", decoded.error().message));
            return;
        }
        // §7.3：对端参数里的 initial_source_connection_id 必须等于它第一个 Initial 的源标识
        if (!m_peerFirstInitialSourceConnectionId.has_value() || !decoded->initialSourceConnectionId.has_value() ||
            !std::ranges::equal(*decoded->initialSourceConnectionId, *m_peerFirstInitialSourceConnectionId))
        {
            beginClose(kQuicTransportParameterError,
                       "对端的 initial_source_connection_id 与它第一个 Initial 里的源标识不符（RFC 9000 §7.3）");
            return;
        }
        m_peerParameters = *decoded;
    }

    void QuicConnectionCore::drive(const Timestamp now)
    {
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
                    beginClose(kQuicCryptoErrorBase + *alert, "TLS 握手失败，错误码是 RFC 9001 §4.8 映射出的 CRYPTO_ERROR");
                }
                else
                {
                    beginClose(kQuicProtocolViolation, "TLS 拒绝了握手数据");
                }
            }
            else if (m_tls->isHandshakeCompleted())
            {
                m_phase = QuicConnectionPhase::Established;
            }
            adoptPeerTransportParameters();
        }

        if (m_phase != QuicConnectionPhase::Closing)
        {
            queueSpacePackets(PacketNumberSpace::Initial, now);
            queueSpacePackets(PacketNumberSpace::Handshake, now);
            queueSpacePackets(PacketNumberSpace::Application, now);
        }
    }

    void QuicConnectionCore::queueSpacePackets(const PacketNumberSpace space, const Timestamp now)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.writeKeys.has_value())
        {
            return;
        }
        // §19.20：握手完成、且对端确认过本空间的包之后，服务端在 Handshake 包里有且只发一次 HANDSHAKE_DONE
        const bool wantsHandshakeDone = space == PacketNumberSpace::Handshake && m_phase == QuicConnectionPhase::Established &&
                                        !m_hasSentHandshakeDone && state.hasSentPacket &&
                                        state.largestAcknowledgedPacketNumber.has_value();
        const bool isLongHeader = space != PacketNumberSpace::Application;

        for (;;)
        {
            std::string frames;
            if (std::exchange(state.isAcknowledgementPending, false) && state.largestAckElicitingReceived.has_value())
            {
                QuicAcknowledgementFrame acknowledgement;
                acknowledgement.largestAcknowledgedPacketNumber = *state.largestAckElicitingReceived;
                acknowledgement.ranges = buildAcknowledgementRanges(state.receivedPacketNumbers,
                                                                    *state.largestAckElicitingReceived);
                if (acknowledgement.ranges.empty())
                {
                    // 不变式：区间永不为空且首段必须含最大确认值（见 QuicAcknowledgementFrame 的 @note）
                    acknowledgement.ranges.push_back(QuicAcknowledgementRange{acknowledgement.largestAcknowledgedPacketNumber,
                                                                              acknowledgement.largestAcknowledgedPacketNumber});
                }
                // §19.3：线上值是微秒数右移本端声明的 ack_delay_exponent，指数上限 20 由参数校验保证
                const std::int64_t elapsedMicroseconds = std::max<std::int64_t>(0, (now - *state.largestAckElicitingArrival).count());
                acknowledgement.acknowledgementDelay = static_cast<std::uint64_t>(elapsedMicroseconds) >>
                                                       std::min<std::uint64_t>(m_configuration.transportParameters.acknowledgmentDelayExponent, 20U);
                appendQuicFrame(frames, QuicFrame{acknowledgement});
            }
            if (wantsHandshakeDone && !m_hasSentHandshakeDone)
            {
                appendQuicFrame(frames, QuicFrame{QuicHandshakeDoneFrame{}});
                m_hasSentHandshakeDone = true;
            }

            const std::size_t remainingByteBudget = saturatingSubtract(
                    kQuicMaximumDatagramPayloadByteLength,
                    packetOverheadByteLength(m_configuration, isLongHeader) + frames.size());
            const std::size_t fragmentByteLength = std::min(state.pendingCryptoBytes.size(), remainingByteBudget);
            if (fragmentByteLength > 0)
            {
                QuicCryptoFrame crypto;
                crypto.offset = state.nextCryptoOffset;
                crypto.data = std::span<const std::uint8_t>(state.pendingCryptoBytes).subspan(0, fragmentByteLength);
                appendQuicFrame(frames, QuicFrame{crypto});
                state.nextCryptoOffset += fragmentByteLength;
                state.pendingCryptoBytes.erase(state.pendingCryptoBytes.begin(),
                                               state.pendingCryptoBytes.begin() + static_cast<std::ptrdiff_t>(fragmentByteLength));
            }

            if (frames.empty())
            {
                return;
            }
            emitPacket(space, frames);
            if (state.pendingCryptoBytes.empty())
            {
                return;
            }
        }
    }

    void QuicConnectionCore::emitPacket(const PacketNumberSpace space, const std::string &frames)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        QuicOutboundPacket packet;
        packet.isLongHeader = space != PacketNumberSpace::Application;
        packet.longPacketType = space == PacketNumberSpace::Handshake ? QuicLongPacketType::Handshake : QuicLongPacketType::Initial;
        packet.version = kQuicVersion1;
        packet.destinationConnectionId = m_configuration.peerConnectionId;
        packet.sourceConnectionId = m_configuration.localConnectionId;
        packet.packetNumber = state.nextPacketNumber;
        packet.packetNumberByteCount = 1;
        packet.frames = asBytes(frames);

        std::string datagram;
        appendQuicPacket(datagram, packet, *state.writeKeys);
        ++state.nextPacketNumber;
        state.hasSentPacket = true;
        const std::span<const std::uint8_t> datagramBytes = asBytes(datagram);
        m_outboundDatagrams.emplace_back(datagramBytes.begin(), datagramBytes.end());
    }

    void QuicConnectionCore::requestClose(const std::uint64_t errorCode, const std::string_view reasonPhrase)
    {
        beginClose(errorCode, reasonPhrase);
    }

    void QuicConnectionCore::beginClose(const std::uint64_t errorCode, const std::string_view reasonPhrase)
    {
        if (m_phase == QuicConnectionPhase::Closing)
        {
            return;
        }
        m_localCloseErrorCode = errorCode;
        m_localCloseReasonPhrase = reasonPhrase;
        m_phase = QuicConnectionPhase::Closing;
        queueConnectionClosePacket();
    }

    void QuicConnectionCore::queueConnectionClosePacket()
    {
        QuicConnectionCloseFrame close;
        close.errorCode = m_localCloseErrorCode.value_or(kQuicNoError);
        close.reasonPhrase = asBytes(m_localCloseReasonPhrase);
        std::string frames;
        appendQuicFrame(frames, QuicFrame{close});
        emitPacket(highestSpaceWithWriteKeys(), frames);
    }

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::highestSpaceWithWriteKeys() const noexcept
    {
        for (const PacketNumberSpace space : {PacketNumberSpace::Application, PacketNumberSpace::Handshake, PacketNumberSpace::Initial})
        {
            if (m_spaces[spaceIndex(space)].writeKeys.has_value())
            {
                return space;
            }
        }
        return PacketNumberSpace::Initial;
    }

    std::optional<std::vector<std::uint8_t>> QuicConnectionCore::takeOutboundDatagram()
    {
        if (m_outboundDatagrams.empty())
        {
            return std::nullopt;
        }
        std::optional<std::vector<std::uint8_t>> datagram = std::move(m_outboundDatagrams.front());
        m_outboundDatagrams.pop_front();
        return datagram;
    }

    QuicConnectionPhase QuicConnectionCore::phase() const noexcept
    {
        return m_phase;
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
