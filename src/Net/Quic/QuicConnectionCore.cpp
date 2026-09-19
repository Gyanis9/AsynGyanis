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
        /// 乱序早到的 CRYPTO 分片总量上限，超了直接收口。这是本实现自设的自我保护值，不是规范值
        constexpr std::size_t kQuicCryptoBufferByteLimit = 65536;

        /// 地址验证之前服务端最多可以回给对端多少倍已收字节（RFC 9000 §8.1）
        constexpr std::size_t kQuicAmplificationFactor = 3;

        /// 每空间跟踪的已收包号上限，超出后丢弃最小的那些（只影响 ACK 能覆盖多老的历史）
        constexpr std::size_t kQuicMaximumTrackedPacketNumbers = 4096;

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
         * @brief 这类帧是不是只有 1-RTT 包才允许携带
         * @details 覆盖 §19.4–§19.14 的全部流与流量控制帧：RESET_STREAM、STOP_SENDING、STREAM 家族
         *          （0x08..0x0f 按低位变体）以及 0x10..0x17。连接标识与路径验证那几类不在其中，
         *          它们按 §19.15–§19.18 可以在任意级别出现。
         * @param frameType §12.4 表 3 的取值
         * @return true 出现在 Initial 或 Handshake 包里即为 PROTOCOL_VIOLATION
         */
        bool isOneRttOnlyFrameType(const std::uint64_t frameType) noexcept
        {
            return frameType == static_cast<std::uint64_t>(QuicFrameType::ResetStream) ||
                   frameType == static_cast<std::uint64_t>(QuicFrameType::StopSending) ||
                   (frameType >= 0x08 && frameType <= 0x17);
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
        : m_configuration(std::move(configuration)),
          m_streams(m_configuration.transportParameters)
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
        // 反放大额度按「收到的整条数据报」算，不看解没解出来：对端确实把这些字节打到了我们地址上
        m_receivedByteCount += datagram.size();
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
            beginClose(kQuicProtocolViolation, "报文首字节的保留位在去头部保护后非 0（RFC 9000 §17.2/§17.3.1）", arrivalTime);
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
        // §10.1：「收到并处理成功」才算活动，解不开的包不能拿来续命。本端自己发出去的包不另算一份
        // 活动：§10.1 那条「自发也要重启」的规则，由下面 3 倍 PTO 的下限覆盖（探测越久，额度越宽）
        m_lastActivityTime = arrivalTime;
        if (!m_isAddressValidated && *level != QuicEncryptionLevel::Initial)
        {
            // 能解出 Handshake 及以上的包，就说明对端确实收到了我们发出去的东西（§8.1.4 的路径验证）：
            // 反放大上限到此解除，否则一条握手都握不完
            m_isAddressValidated = true;
        }

        const std::expected<std::vector<QuicFrame>, QuicDecodeError> frames =
                decodeQuicFrames(std::span<const std::uint8_t>(plaintext).subspan(0, *opened));
        if (!frames.has_value())
        {
            // 能解密就说明这包出自持有密钥的对端，帧解不开是对端违规（§19.1）
            beginClose(frames.error().kind == QuicDecodeErrorKind::Truncated ? kQuicProtocolViolation : kQuicFrameEncodingError,
                       std::format("帧序列不合 RFC 9000 §19：{}", frames.error().message), arrivalTime);
            return std::unexpected(frames.error());
        }

        bool hasAckElicitingFrame = false;
        for (const QuicFrame &frame : *frames)
        {
            const std::uint64_t frameType = quicFrameTypeValue(frame);
            if (isOneRttOnlyFrameType(frameType) && space != PacketNumberSpace::Application)
            {
                // §19.4–§19.14 逐条写明这些帧不得出现在 Initial/Handshake 包里
                beginClose(kQuicProtocolViolation,
                           std::format("流与流量控制帧 0x{:x} 出现在非 1-RTT 包里（RFC 9000 §19.4–§19.14）", frameType), arrivalTime);
                return {};
            }
            hasAckElicitingFrame = hasAckElicitingFrame || isAcknowledgementEliciting(frame);
            handleFrame(frame, space, arrivalTime);
        }
        if (hasAckElicitingFrame)
        {
            state.largestAckElicitingReceived = packetNumber;
            state.largestAckElicitingArrival = arrivalTime;
            state.isAcknowledgementPending = true;
        }
        return {};
    }

    void QuicConnectionCore::handleFrame(const QuicFrame &frame, const PacketNumberSpace space, const Timestamp arrivalTime)
    {
        std::visit(
                [this, space, arrivalTime](const auto &specificFrame)
                {
                    using FrameType = std::remove_cvref_t<decltype(specificFrame)>;
                    if constexpr (std::same_as<FrameType, QuicAcknowledgementFrame>)
                    {
                        handleAcknowledgement(specificFrame, space, arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicCryptoFrame>)
                    {
                        handleCryptoBytes(space, specificFrame.offset, specificFrame.data, arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicStreamFrame>)
                    {
                        reportStreamViolation(m_streams.onStreamFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicMaxDataFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxDataFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicMaxStreamDataFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxStreamDataFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicMaxStreamsFrame>)
                    {
                        reportStreamViolation(m_streams.onMaxStreamsFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicResetStreamFrame>)
                    {
                        reportStreamViolation(m_streams.onResetStreamFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicStopSendingFrame>)
                    {
                        reportStreamViolation(m_streams.onStopSendingFrame(specificFrame), arrivalTime);
                    }
                    else if constexpr (std::same_as<FrameType, QuicConnectionCloseFrame>)
                    {
                        // 对端已经收口：本端不再发任何东西，也不再回一个 CLOSE（§10.2.3）
                        m_phase = QuicConnectionPhase::Closing;
                    }
                    // PADDING 与 PING 不需要动作：PING 的确认由触发确认的记账统一处理。
                    // DATA_BLOCKED 那三类只是对端的自述，本层不需要反应（§19.12–§19.14）。
                    // 连接标识与路径验证那几类要到后续里程碑才有人接；HANDSHAKE_DONE 只有服务端会发，
                    // 收到它说明对端把本端当成了客户端，同样忽略。
                },
                frame);
    }

    void QuicConnectionCore::handleAcknowledgement(const QuicAcknowledgementFrame &frame, const PacketNumberSpace space,
                                                  const Timestamp arrivalTime)
    {
        // 报告延迟是线上值，按**对端自己声明**的指数还原成时间（§19.3）；它的参数还没到之前用默认指数
        const std::uint64_t peerExponent = m_peerParameters.has_value()
                                                ? m_peerParameters->acknowledgmentDelayExponent
                                                : kQuicDefaultAcknowledgmentDelayExponent;
        const std::uint64_t shiftBitCount = std::min<std::uint64_t>(peerExponent, 20U);
        // 这个字段允许对端塞任意大的数：先按上限夹一次再移位，免得乘出一个回绕的负延迟。
        // 60 秒远超过任何合法的 max_ack_delay，超出的部分本来就只会走「不合理就不减」那条路
        constexpr std::uint64_t kQuicMaximumAcknowledgementDelayMicroseconds = 60ULL * 1000ULL * 1000ULL;
        const std::uint64_t unscaledLimit = kQuicMaximumAcknowledgementDelayMicroseconds >> shiftBitCount;
        const QuicTime acknowledgementDelay{static_cast<std::int64_t>(
                std::min(frame.acknowledgementDelay, unscaledLimit) << shiftBitCount)};

        const QuicAcknowledgementUpdate update =
                m_recovery.onAcknowledgementReceived(recoverySpaceOf(space), frame, arrivalTime, acknowledgementDelay);
        m_congestion.onCongestionUpdate(update.acknowledged, update.lost, arrivalTime);
        for (const QuicSentPacketInfo &packet : update.acknowledged)
        {
            m_streams.onSendRangesAcknowledged(packet.streamRanges);
        }
        for (const QuicSentPacketInfo &packet : update.lost)
        {
            m_streams.onSendRangesLost(packet.streamRanges);
        }

        // §4.1.2：服务端「握手已确认」的判据就是对端确认了 Handshake 空间的包。确认之后才允许给
        // 进入用空间武装 PTO，也才该发 HANDSHAKE_DONE
        if (!m_isHandshakeConfirmed && space == PacketNumberSpace::Handshake && !update.acknowledged.empty())
        {
            m_isHandshakeConfirmed = true;
            const std::uint64_t peerMaximumDelayMilliseconds = m_peerParameters.has_value()
                                                                    ? m_peerParameters->maximumAcknowledgmentDelayMilliseconds
                                                                    : kQuicDefaultMaximumAcknowledgmentDelayMilliseconds;
            m_recovery.onHandshakeConfirmed(std::chrono::duration_cast<QuicTime>(
                    std::chrono::milliseconds{peerMaximumDelayMilliseconds}));
        }
        if (!update.lost.empty())
        {
            queueRetransmissions(space, update.lost);
        }
    }

    void QuicConnectionCore::handleCryptoBytes(const PacketNumberSpace space, const std::uint64_t offset,
                                               const std::span<const std::uint8_t> bytes, const Timestamp arrivalTime)
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
        const std::expected<QuicTransportParameters, QuicDecodeError> decoded =
                decodeQuicTransportParameters(m_tls->peerTransportParameters(), QuicTransportParameterSenderRole::Client);
        if (!decoded.has_value())
        {
            beginClose(kQuicTransportParameterError, std::format("对端传输参数不合格：{}", decoded.error().message), now);
            return;
        }
        // §7.3：对端参数里的 initial_source_connection_id 必须等于它第一个 Initial 的源标识
        if (!m_peerFirstInitialSourceConnectionId.has_value() || !decoded->initialSourceConnectionId.has_value() ||
            !std::ranges::equal(*decoded->initialSourceConnectionId, *m_peerFirstInitialSourceConnectionId))
        {
            beginClose(kQuicTransportParameterError,
                       "对端的 initial_source_connection_id 与它第一个 Initial 里的源标识不符（RFC 9000 §7.3）", now);
            return;
        }
        m_peerParameters = *decoded;
        // 流层的发送额度全部来自对端参数：到手之前一条流数据也发不出去（§4.1）
        m_streams.adoptPeerParameters(*decoded);
    }

    void QuicConnectionCore::reportStreamViolation(const std::expected<void, QuicStreamViolation> &result, const Timestamp now)
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
        state.readKeys = std::nullopt;
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
        m_recovery.discardSpace(recoverySpaceOf(space));
    }

    void QuicConnectionCore::drive(const Timestamp now)
    {
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
                }
                else
                {
                    beginClose(kQuicProtocolViolation, "TLS 拒绝了握手数据", now);
                }
            }
            else if (m_tls->isHandshakeCompleted())
            {
                m_phase = QuicConnectionPhase::Established;
            }
            adoptPeerTransportParameters(now);
        }

        if (m_phase != QuicConnectionPhase::Closing)
        {
            for (const PacketNumberSpace space : {PacketNumberSpace::Initial, PacketNumberSpace::Handshake,
                                                  PacketNumberSpace::Application})
            {
                queueSpacePackets(space, now);
            }
        }
    }

    void QuicConnectionCore::queueSpacePackets(const PacketNumberSpace space, const Timestamp now)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        if (!state.writeKeys.has_value())
        {
            return;
        }
        const bool isLongHeader = space != PacketNumberSpace::Application;
        const std::size_t fixedOverheadByteLength = packetOverheadByteLength(m_configuration, isLongHeader);
        if (isBlockedByAmplificationLimit(fixedOverheadByteLength))
        {
            // 连一个包头都装不进 3 倍额度：本空间这一轮什么都发不出去，等对端多打些字节再来（§8.1）
            return;
        }
        // §19.20：握手完成、且对端确认过 Handshake 空间的包之后，服务端只发一次 HANDSHAKE_DONE
        const bool wantsHandshakeDone = space == PacketNumberSpace::Handshake && m_phase == QuicConnectionPhase::Established &&
                                        !m_hasSentHandshakeDone && m_isHandshakeConfirmed;
        bool owesProbe = m_probeSpace.has_value() && *m_probeSpace == space;
        // 整轮探测都豁免窗口（§7.5）：欠的那一条可能分两包出去，只豁免第一包等于把后半段卡在门外
        const bool isProbingSpace = owesProbe;

        for (;;)
        {
            std::string frames;
            bool elicitsAcknowledgement = false;
            if (std::exchange(state.isAcknowledgementPending, false) && state.largestAckElicitingReceived.has_value())
            {
                QuicAcknowledgementFrame acknowledgement;
                acknowledgement.largestAcknowledgedPacketNumber = *state.largestAckElicitingReceived;
                acknowledgement.ranges = buildQuicAcknowledgementRanges(state.receivedPacketNumbers,
                                                                        *state.largestAckElicitingReceived);
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
                elicitsAcknowledgement = true;
            }

            const std::size_t remainingByteBudget = sendByteBudget(fixedOverheadByteLength + frames.size(), isProbingSpace);
            std::optional<QuicCryptoRange> carriedRange;
            if (!state.pendingRetransmissions.empty())
            {
                // 重发队列优先：判丢的那段最可能正是对端缺的
                QuicCryptoRange &front = state.pendingRetransmissions.front();
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
            }
            else if (state.cryptoWriteOffset < state.cryptoStream.size())
            {
                const std::size_t fragmentByteLength =
                        std::min(state.cryptoStream.size() - state.cryptoWriteOffset, remainingByteBudget);
                carriedRange = QuicCryptoRange{state.cryptoWriteOffset, state.cryptoWriteOffset + fragmentByteLength};
                state.cryptoWriteOffset += fragmentByteLength;
            }
            if (carriedRange.has_value() && carriedRange->endOffset > carriedRange->beginOffset)
            {
                QuicCryptoFrame crypto;
                crypto.offset = carriedRange->beginOffset;
                crypto.data = std::span<const std::uint8_t>(state.cryptoStream)
                                      .subspan(carriedRange->beginOffset, carriedRange->endOffset - carriedRange->beginOffset);
                appendQuicFrame(frames, QuicFrame{crypto});
                elicitsAcknowledgement = true;
            }
            else
            {
                // 这一包没真带上字节：别把空区间记进重发账，否则它永远排在队头
                carriedRange = std::nullopt;
            }

            std::vector<QuicStreamRange> sentRanges;
            bool carriesStreamFrames = false;
            if (space == PacketNumberSpace::Application)
            {
                // 握手字节的产出排在流数据之前：进入用空间通常只有会话票据会占那条通道
                carriesStreamFrames = m_streams.collectFrames(
                        frames, sendByteBudget(fixedOverheadByteLength + frames.size(), isProbingSpace), sentRanges);
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
            const bool hasMoreWork = hasMoreCrypto || (space == PacketNumberSpace::Application && m_streams.hasOutgoingFrames());
            emitPacket(space, frames, now, elicitsAcknowledgement, carriedRange, std::move(sentRanges));
            if (!hasMoreWork || (!carriedRange.has_value() && !carriesStreamFrames))
            {
                // 已经没有下文，或这一包被预算挤得一个字节都没带上：再转一圈也只是空转
                return;
            }
        }
    }

    void QuicConnectionCore::queueRetransmissions(const PacketNumberSpace space, const std::span<const QuicSentPacketInfo> packets)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        std::vector<QuicCryptoRange> ranges = state.pendingRetransmissions;
        // 判丢的那批已经从在途账里划掉了，所以字节区间只能由调用方把包本身交进来：
        // 只扫在途集合会把「刚判丢」这一路变成空操作
        for (const QuicSentPacketInfo &packet : packets)
        {
            if (packet.cryptoRange.has_value() && packet.cryptoRange->endOffset > packet.cryptoRange->beginOffset)
            {
                ranges.push_back(*packet.cryptoRange);
            }
        }
        // 相邻或重叠的区间合并：同一段字节被两个包各带过一次时不该重发两遍，队列也不该无限增长
        std::ranges::sort(ranges, {}, &QuicCryptoRange::beginOffset);
        std::vector<QuicCryptoRange> merged;
        for (const QuicCryptoRange &range : ranges)
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

    void QuicConnectionCore::emitPacket(const PacketNumberSpace space, const std::string &frames, const Timestamp now,
                                        const bool isAckEliciting, const std::optional<QuicCryptoRange> cryptoRange,
                                        std::vector<QuicStreamRange> streamRanges)
    {
        SpaceState &state = m_spaces[spaceIndex(space)];
        const std::uint64_t packetNumber = state.nextPacketNumber;
        QuicOutboundPacket packet;
        packet.isLongHeader = space != PacketNumberSpace::Application;
        packet.longPacketType = space == PacketNumberSpace::Handshake ? QuicLongPacketType::Handshake : QuicLongPacketType::Initial;
        packet.version = kQuicVersion1;
        packet.destinationConnectionId = m_configuration.peerConnectionId;
        packet.sourceConnectionId = m_configuration.localConnectionId;
        packet.packetNumber = packetNumber;
        packet.packetNumberByteCount = 1;
        packet.frames = asBytes(frames);

        std::string datagram;
        appendQuicPacket(datagram, packet, *state.writeKeys);
        ++state.nextPacketNumber;
        const std::span<const std::uint8_t> datagramBytes = asBytes(datagram);
        m_outboundDatagrams.emplace_back(datagramBytes.begin(), datagramBytes.end());

        QuicSentPacketInfo record;
        record.packetNumber = packetNumber;
        record.timeSent = now;
        record.byteCount = datagram.size();
        record.isAckEliciting = isAckEliciting;
        record.cryptoRange = cryptoRange;
        record.streamRanges = std::move(streamRanges);
        m_congestion.onPacketSent(record);
        m_recovery.onPacketSent(recoverySpaceOf(space), std::move(record));
        m_sentByteCount += datagram.size();
    }

    std::size_t QuicConnectionCore::sendByteBudget(const std::size_t reservedByteLength, const bool ignoresCongestionWindow) const
    {
        std::size_t budget = saturatingSubtract(kQuicMaximumDatagramPayloadByteLength, reservedByteLength);
        if (!ignoresCongestionWindow)
        {
            // 窗口余量也得先减掉这一包的固定开销，否则算出来的分片一定超窗
            budget = std::min(budget, saturatingSubtract(m_congestion.remainingByteBudget(), reservedByteLength));
        }
        if (!m_isAddressValidated)
        {
            // §8.1：地址验证之前最多回三倍已收字节。这条排在最后，因为它是硬上限，探针也不例外
            budget = std::min(budget, saturatingSubtract(amplificationRemainingByteCount(), reservedByteLength));
        }
        return budget;
    }

    bool QuicConnectionCore::isBlockedByAmplificationLimit(const std::size_t reservedByteLength) const noexcept
    {
        return !m_isAddressValidated && reservedByteLength > amplificationRemainingByteCount();
    }

    std::size_t QuicConnectionCore::amplificationRemainingByteCount() const noexcept
    {
        return saturatingSubtract(kQuicAmplificationFactor * m_receivedByteCount, m_sentByteCount);
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
        m_localCloseErrorCode = errorCode;
        m_localCloseReasonPhrase = reasonPhrase;
        m_phase = QuicConnectionPhase::Closing;
        queueConnectionClosePacket(now);
    }

    void QuicConnectionCore::queueConnectionClosePacket(const Timestamp now)
    {
        QuicConnectionCloseFrame close;
        close.errorCode = m_localCloseErrorCode.value_or(kQuicNoError);
        close.reasonPhrase = asBytes(m_localCloseReasonPhrase);
        std::string frames;
        appendQuicFrame(frames, QuicFrame{close});
        const PacketNumberSpace space = highestSpaceWithWriteKeys();
        if (isBlockedByAmplificationLimit(packetOverheadByteLength(m_configuration, space != PacketNumberSpace::Application)
                                          + frames.size()))
        {
            // 收口也受 §8.1 约束：额度不够就宁可不发，反正对端已经在把我们当放大器打了
            return;
        }
        emitPacket(space, frames, now, false, std::nullopt);
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

    QuicRecoverySpace QuicConnectionCore::recoverySpaceOf(const PacketNumberSpace space) noexcept
    {
        switch (space)
        {
        case PacketNumberSpace::Initial: return QuicRecoverySpace::Initial;
        case PacketNumberSpace::Handshake: return QuicRecoverySpace::Handshake;
        case PacketNumberSpace::Application: return QuicRecoverySpace::Application;
        }
        return QuicRecoverySpace::Application;
    }

    QuicConnectionCore::PacketNumberSpace QuicConnectionCore::spaceOf(const QuicRecoverySpace space) noexcept
    {
        switch (space)
        {
        case QuicRecoverySpace::Initial: return PacketNumberSpace::Initial;
        case QuicRecoverySpace::Handshake: return PacketNumberSpace::Handshake;
        case QuicRecoverySpace::Application: return PacketNumberSpace::Application;
        }
        return PacketNumberSpace::Application;
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::nextTimeout() const noexcept
    {
        std::optional<Timestamp> deadline = m_recovery.nextDeadline();
        if (const std::optional<Timestamp> idleDeadline = idleDeadlineTime(); idleDeadline.has_value() &&
            (!deadline.has_value() || *idleDeadline < *deadline))
        {
            deadline = idleDeadline;
        }
        return deadline;
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::idleDeadlineTime() const noexcept
    {
        if (!m_lastActivityTime.has_value())
        {
            return std::nullopt;
        }
        if (const std::optional<Timestamp> period = effectiveIdleTimeout(); period.has_value())
        {
            return *m_lastActivityTime + *period;
        }
        return std::nullopt;
    }

    std::optional<QuicConnectionCore::Timestamp> QuicConnectionCore::effectiveIdleTimeout() const noexcept
    {
        // §18.2：两端都宣告取较小的那个；只有一端宣告非 0 就用那一个；两个都是 0 表示不启用
        const std::uint64_t localMilliseconds = m_configuration.transportParameters.maximumIdleTimeoutMilliseconds;
        const std::uint64_t peerMilliseconds = m_peerParameters.has_value()
                                                  ? m_peerParameters->maximumIdleTimeoutMilliseconds
                                                  : 0;
        std::uint64_t effectiveMilliseconds = 0;
        if (localMilliseconds != 0 && peerMilliseconds != 0)
        {
            effectiveMilliseconds = std::min(localMilliseconds, peerMilliseconds);
        }
        else
        {
            effectiveMilliseconds = localMilliseconds != 0 ? localMilliseconds : peerMilliseconds;
        }
        if (effectiveMilliseconds == 0)
        {
            return std::nullopt;
        }
        // §10.1 的硬要求：至少留够 3 倍 PTO，否则一次抖动就把好端端的连接判死
        const Timestamp probePeriod = m_recovery.roundTripTimeEstimate().probeTimeout;
        return std::max(Timestamp{std::chrono::milliseconds{effectiveMilliseconds}}, probePeriod * 3);
    }

    void QuicConnectionCore::onTimeout(const Timestamp now)
    {
        if (m_phase == QuicConnectionPhase::Closing)
        {
            return;
        }
        if (const std::optional<Timestamp> idleDeadline = idleDeadlineTime(); idleDeadline.has_value() &&
            *idleDeadline <= now)
        {
            // §10.1：空闲超时是「静默关闭」——不发 CONNECTION_CLOSE（对端本来就没在说话，发了也没人收），
            // 也不留待发数据报：本连接到此为止，外层看 isFinished() 就能回收
            m_phase = QuicConnectionPhase::Closing;
            m_localCloseErrorCode = std::nullopt;
            m_localCloseReasonPhrase.clear();
            m_outboundDatagrams.clear();
            return;
        }
        const QuicRecoveryTimeoutAction action = m_recovery.onDeadlineReached(now);
        if (!action.lost.empty())
        {
            m_congestion.onCongestionUpdate({}, action.lost, now);
            queueRetransmissions(spaceOf(action.lostSpace), action.lost);
            for (const QuicSentPacketInfo &packet : action.lost)
            {
                m_streams.onSendRangesLost(packet.streamRanges);
            }
        }
        if (action.isProbeTimeout)
        {
            // 判丢之外还要探一条包出去：先重发仍未确认的握手字节，没有可发的就补一条 PING（§6.2.2）
            m_probeSpace = spaceOf(action.probeSpace);
            const std::vector<QuicSentPacketInfo> stillInFlight = m_recovery.unacknowledgedPackets(action.probeSpace);
            queueRetransmissions(spaceOf(action.probeSpace), stillInFlight);
        }
        for (const PacketNumberSpace space : {PacketNumberSpace::Initial, PacketNumberSpace::Handshake,
                                              PacketNumberSpace::Application})
        {
            queueSpacePackets(space, now);
        }
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
