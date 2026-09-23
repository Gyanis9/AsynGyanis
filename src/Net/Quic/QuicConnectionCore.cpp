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
        /**
         * @brief 收包路径上那两块与包长成比例的临时缓冲
         *
         * @details 解头部保护要一份可写副本、AEAD 解密要一份明文缓冲，两者都要到能算出长度之后
         *          才知道要多大——也就是说在判断「这条报文解不解得开」之前就已经付过一次分配。
         *          按线程复用之后高水位只留一份，代价是**任何指向它们的视图都不许活到下一次收包**：
         *          本层的交付路径（流层入队、TLS 缓冲、transport parameters 解析、错误文案）全部是
         *          拷贝入存储，因此这一条成立。状态机不跨挂起点持有这些视图（它不是协程）。
         */
        enum class ReceiveScratch : std::uint8_t
        {
            PacketCopy,  ///< 整包的可写副本，只有头部那几个字节会被改写
            Plaintext,   ///< AEAD 解出来的明文帧
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

        // 建状态本身就是「收到了一份看起来属于本连接的报文」，因此这条连接的寿命从此刻起算，而不是
        // 从第一次解密成功起算：解不开的报文按 §10.1 不算活动，若截止时刻也只在解密成功后才亮，
        // 一个只发无法解密报文就消失的对端会让这条表项永远留在服务端的路由表里
        restartIdleTimer(Timestamp{});
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
        std::vector<std::uint8_t> &workingBytes = receiveScratch(ReceiveScratch::PacketCopy);
        workingBytes.assign(packet.begin(), packet.end());
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

        // 相位位不合的那一包，要么是对端刚更新、要么是晚到的旧包：同一个相位位被前后两代密钥共用，
        // 只有包号能把它们分开（RFC 9001 §6.5）。头部保护密钥不随更新换（§6.1），所以上面那一步不必跟着分代
        const QuicPacketKeys *reading = &*state.readKeys;
        bool opensWithNextKeys = false;
        if (space == PacketNumberSpace::Application && header.isKeyPhaseBitSet != m_isReadKeyPhaseSet)
        {
            const bool looksNewer = !state.largestReceivedPacketNumber.has_value() ||
                                    packetNumber > *state.largestReceivedPacketNumber;
            if (looksNewer && state.nextReadKeys.has_value())
            {
                reading = &*state.nextReadKeys;
                opensWithNextKeys = true;
            }
            else if (!looksNewer && state.previousReadKeys.has_value())
            {
                reading = &*state.previousReadKeys;
            }
            else
            {
                // 手上没有对应的那一代：和「解不开」同等处理。回错误码等于给攻击者一个免费的相位探针
                return {};
            }
        }

        const std::span<const std::uint8_t> additionalData = workingPacket.subspan(0, headerByteCount);
        const std::span<const std::uint8_t> protectedPayload = workingPacket.subspan(headerByteCount);
        std::vector<std::uint8_t> &plaintext = receiveScratch(ReceiveScratch::Plaintext);
        plaintext.resize(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const std::expected<std::size_t, QuicDecodeError> opened =
                openQuicProtectedPayload(plaintext, *reading, packetNumber, additionalData, protectedPayload);
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

        // 到这一步包才是「认证过」的，包号记账与重复判定都只在这个前提下推进
        if (!state.receivedPacketNumbers.insert(packetNumber))
        {
            return {};
        }
        // 额度按「记了多少个包号」算而不是「几段」：连号流量下几十万个包号也只占一段
        state.receivedPacketNumbers.dropOldestUntil(kQuicMaximumTrackedPacketNumbers);
        if (!state.largestReceivedPacketNumber.has_value() || packetNumber > *state.largestReceivedPacketNumber)
        {
            state.largestReceivedPacketNumber = packetNumber;
        }
        if (*level == QuicEncryptionLevel::Initial && !m_peerFirstInitialSourceConnectionId.has_value())
        {
            m_peerFirstInitialSourceConnectionId = std::vector<std::uint8_t>(header.sourceConnectionId.begin(),
                                                                            header.sourceConnectionId.end());
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

        std::vector<QuicFrame> &frames = inboundFrameScratch();
        const std::expected<void, QuicDecodeError> decodedFrames =
                decodeQuicFrames(std::span<const std::uint8_t>(plaintext).subspan(0, *opened), frames);
        if (!decodedFrames.has_value())
        {
            // 能解密就说明这包出自持有密钥的对端，帧解不开是对端违规（§19.1）
            beginClose(decodedFrames.error().kind == QuicDecodeErrorKind::Truncated ? kQuicProtocolViolation
                                                                                    : kQuicFrameEncodingError,
                       std::format("帧序列不合 RFC 9000 §19：{}", decodedFrames.error().message), arrivalTime);
            return std::unexpected(decodedFrames.error());
        }

        bool hasAckElicitingFrame = false;
        for (const QuicFrame &frame : frames)
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
                    else if constexpr (std::same_as<FrameType, QuicHandshakeDoneFrame>)
                    {
                        // §19.20：只有服务端会发这一帧，而「服务端收到它」本身就写死了要按
                        // PROTOCOL_VIOLATION 收口——本实现只有服务端一侧，不必再看它出现在哪个空间
                        beginClose(kQuicProtocolViolation, "对端向服务端发了 HANDSHAKE_DONE（RFC 9000 §19.20）", arrivalTime);
                    }
                    // PADDING 与 PING 不需要动作：PING 的确认由触发确认的记账统一处理。
                    // DATA_BLOCKED 那三类只是对端的自述，本层不需要反应（§19.12–§19.14）。
                    // 连接标识与路径验证那几类本实现没有对应能力（不轮换标识、不迁移路径），收到即忽略。
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
        if (space == PacketNumberSpace::Application && m_lowestPacketNumberSentInKeyPhase.has_value())
        {
            // §6.1：本相位里任意一个包被确认，就代表两侧都有了新密钥，下一次更新的门禁随之打开
            for (const QuicSentPacketInfo &packet : update.acknowledged)
            {
                if (packet.packetNumber >= *m_lowestPacketNumberSentInKeyPhase)
                {
                    m_isKeyPhaseAcknowledged = true;
                    break;
                }
            }
        }
        m_congestion.onCongestionUpdate(update.acknowledged, update.lost, arrivalTime);
        for (const QuicSentPacketInfo &packet : update.acknowledged)
        {
            m_streams.onSendRangesAcknowledged(packet.streamRanges);
            m_streams.onStreamAnnouncementsAcknowledged(packet.streamAnnouncements);
            if (packet.carriesHandshakeDone)
            {
                // §19.20 的「重发到被确认为止」到此为止：确认回来之后这一帧这辈子不再发第二遍
                m_isHandshakeDoneAcknowledged = true;
                m_isHandshakeDoneInFlight = false;
            }
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
                if (space == PacketNumberSpace::Application)
                {
                    // §6.3：当前与下一代两套读密钥常驻，更新到来时才不必在解包路径上现推一把密钥
                    state.nextReadKeys = deriveQuicUpdatedPacketKeys(*state.readKeys);
                }
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
        const QuicRecoverySpace retiredSpace = recoverySpaceOf(space);
        const std::vector<QuicSentPacketInfo> retiredPackets = m_recovery.unacknowledgedPackets(retiredSpace);
        m_recovery.discardSpace(retiredSpace);
        // 恢复层与拥塞层各记一本在途账，只清一本就让这条连接永久背着退休空间的字节：
        // Initial 空间几乎总在握手期退休，之后每次算可用窗口都要少几十 KB（RFC 9002 §7.2 的负荷口径）
        m_congestion.onPacketsDiscarded(retiredPackets);
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
        // §19.20：握手完成、且对端确认过 Handshake 空间的包之后才发 HANDSHAKE_DONE，且它是 **1-RTT
        // 帧**（§19 表 3 的 Protection 列只有 1）——塞进 Handshake 空间的包会被对端按「该级别不该
        // 出现这种帧」判 PROTOCOL_VIOLATION（aioquic 即如此）
        bool owesHandshakeDone = space == PacketNumberSpace::Application && m_phase == QuicConnectionPhase::Established &&
                                 m_isHandshakeConfirmed && isHandshakeDonePending();
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
            bool carriesHandshakeDone = false;
            if (owesHandshakeDone)
            {
                appendQuicFrame(frames, QuicFrame{QuicHandshakeDoneFrame{}});
                carriesHandshakeDone = true;
                // 一轮里只带一次：这一包出去之后在途就有了它的副本，下一包不必再重复占字节
                owesHandshakeDone = false;
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
            std::vector<QuicStreamAnnouncement> announcements;
            bool carriesStreamFrames = false;
            if (space == PacketNumberSpace::Application)
            {
                // 握手字节的产出排在流数据之前：进入用空间通常只有会话票据会占那条通道
                carriesStreamFrames = m_streams.collectFrames(
                        frames, sendByteBudget(fixedOverheadByteLength + frames.size(), isProbingSpace), sentRanges, announcements);
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
            emitPacket(space, frames, now, elicitsAcknowledgement, carriedRange, std::move(sentRanges), carriesHandshakeDone,
                       std::move(announcements));
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

    bool QuicConnectionCore::isHandshakeDonePending() const noexcept
    {
        // 确认回来之前一直欠着一份（§19.20）；在途还有一份时不必再发第二份，等它被判丢或探测超时
        // 之后由 queueRetransmissions 把「在途那份」的账销掉，下一轮自然补发
        return !m_isHandshakeDoneAcknowledged && !m_isHandshakeDoneInFlight;
    }

    void QuicConnectionCore::emitPacket(const PacketNumberSpace space, const std::string &frames, const Timestamp now,
                                        const bool isAckEliciting, const std::optional<QuicCryptoRange> cryptoRange,
                                        std::vector<QuicStreamRange> streamRanges, const bool carriesHandshakeDone,
                                        std::vector<QuicStreamAnnouncement> streamAnnouncements)
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
        packet.isKeyPhaseBitSet = m_isSendKeyPhaseSet;
        packet.frames = asBytes(frames);

        std::string datagram;
        appendQuicPacket(datagram, packet, *state.writeKeys);
        ++state.nextPacketNumber;
        const std::size_t datagramByteCount = datagram.size();
        // 待发队列拿的是这条报文的本体而不是它的副本：下面 record 与字节数都只需要长度，
        // 整包再抄一遍是白付的一次分配加一次 memcpy
        m_outboundDatagrams.push_back(std::move(datagram));

        QuicSentPacketInfo record;
        record.packetNumber = packetNumber;
        record.timeSent = now;
        record.byteCount = datagramByteCount;
        record.isAckEliciting = isAckEliciting;
        record.cryptoRange = cryptoRange;
        record.streamRanges = std::move(streamRanges);
        record.streamAnnouncements = std::move(streamAnnouncements);
        record.carriesHandshakeDone = carriesHandshakeDone;
        if (carriesHandshakeDone)
        {
            m_isHandshakeDoneInFlight = true;
        }
        if (isAckEliciting && !m_hasSentAckElicitingSinceReceipt && m_idlePeriod.has_value())
        {
            // §10.1：本端主动发起的通信也算活动，但只有收包之后的第一包作数，且沿用本期额度——
            // 否则每趟探测都续一期、且用的还是翻倍后的 PTO，空闲超时永远等不到
            m_idleDeadline = now + *m_idlePeriod;
            m_hasSentAckElicitingSinceReceipt = true;
        }
        m_congestion.onPacketSent(record);
        m_recovery.onPacketSent(recoverySpaceOf(space), std::move(record));
        m_sentByteCount += datagramByteCount;
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
        return m_idleDeadline;
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

    std::optional<std::string> QuicConnectionCore::takeOutboundDatagram()
    {
        if (m_outboundDatagrams.empty())
        {
            return std::nullopt;
        }
        std::optional<std::string> datagram = std::move(m_outboundDatagrams.front());
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
        state.writeKeys = deriveQuicUpdatedPacketKeys(*state.writeKeys);
        m_isSendKeyPhaseSet = !m_isSendKeyPhaseSet;
        // 本相位的计数从下一个要发的包号起算，确认只要落在它之上就算这次更新到位
        m_lowestPacketNumberSentInKeyPhase = state.nextPacketNumber;
        m_isKeyPhaseAcknowledged = false;
        m_keyPhaseChangedAt = now;
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

    const QuicPacketKeys *QuicConnectionCore::applicationWriteKeys() const noexcept
    {
        const SpaceState &state = m_spaces[spaceIndex(PacketNumberSpace::Application)];
        return state.writeKeys.has_value() ? &*state.writeKeys : nullptr;
    }

    void QuicConnectionCore::applyPeerKeyUpdate(SpaceState &state, const Timestamp now)
    {
        // §6.3：读侧始终备着两套。提成当前之后立刻推一把新的出来，下一轮更新仍不必在解包路径上现算
        state.previousReadKeys = std::move(state.readKeys);
        state.readKeys = std::move(state.nextReadKeys);
        state.nextReadKeys = deriveQuicUpdatedPacketKeys(*state.readKeys);
        m_isReadKeyPhaseSet = !m_isReadKeyPhaseSet;
        m_keyPhaseChangedAt = now;

        // §6.2：回确认之前，发密钥必须也推进到同一相位；带新相位的确认就是「这次更新已完成」的信号
        if (m_isSendKeyPhaseSet != m_isReadKeyPhaseSet)
        {
            SpaceState &application = m_spaces[spaceIndex(PacketNumberSpace::Application)];
            if (application.writeKeys.has_value())
            {
                application.writeKeys = deriveQuicUpdatedPacketKeys(*application.writeKeys);
            }
            m_isSendKeyPhaseSet = m_isReadKeyPhaseSet;
            m_lowestPacketNumberSentInKeyPhase = application.nextPacketNumber;
            m_isKeyPhaseAcknowledged = false;
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
        return m_congestion.bytesInFlight();
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
