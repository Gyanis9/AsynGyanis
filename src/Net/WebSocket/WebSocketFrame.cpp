#include "Net/WebSocket/WebSocketFrame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 长度域里 126 这个取值的含义是「后面还跟着 2 字节扩展长度」（RFC 6455 §5.2）
        constexpr std::uint8_t kSixteenBitLengthEscape = 126;

        /// 长度域里 127 这个取值的含义是「后面还跟着 8 字节扩展长度」
        constexpr std::uint8_t kSixtyFourBitLengthEscape = 127;

        /// 16 位档与 64 位档的分界：小于它才用 16 位档
        constexpr std::uint64_t kSixtyFourBitLengthThreshold = 65536;

        /// 64 位扩展长度可表示的最大值：最高位必须为 0（RFC 6455 §5.2）
        constexpr std::uint64_t kMaximumSixtyFourBitLength = (1ull << 63) - 1ull;

        /// 掩码键长度，单位字节（RFC 6455 §5.3）
        constexpr std::size_t kMaskKeyLength = 4;

        /**
         * @brief 判断操作码取值是否落在 RFC 6455 §5.2 定义过的取值里
         * @param opCodeValue 帧首字节低 4 位的原始取值
         * @return true 表示是已定义的操作码
         */
        bool isKnownOpCodeValue(const std::uint8_t opCodeValue) noexcept
        {
            return opCodeValue == 0x0 || opCodeValue == 0x1 || opCodeValue == 0x2 || opCodeValue == 0x8 || opCodeValue == 0x9 ||
                   opCodeValue == 0xA;
        }

        /**
         * @brief 判断操作码取值是否为控制帧
         * @param opCodeValue 操作码原始取值
         * @return true 表示是 Close/Ping/Pong 之一（RFC 6455 §5.2 把 0x8 及以上划为控制帧）
         */
        bool isControlOpCodeValue(const std::uint8_t opCodeValue) noexcept
        {
            return (opCodeValue & 0x08U) != 0;
        }

        /**
         * @brief 把整数按大端追加到目标串末尾
         * @param frame 目标串
         * @param value 待写入的数值
         * @param byteCount 写入字节数（2 或 8），不得超过 8
         */
        void appendBigEndian(std::string &frame, const std::uint64_t value, const std::size_t byteCount)
        {
            // 线上长度是大端：先写高位字节，因此每轮右移的位数由剩余字节数决定
            for (std::size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex)
            {
                const std::size_t shiftBitCount = (byteCount - 1 - byteIndex) * 8;
                frame.push_back(static_cast<char>((value >> shiftBitCount) & 0xFFU));
            }
        }
    } // namespace

    std::string encodeWebSocketFrame(const WebSocketOpCode opCode, const std::string_view payload, const bool isFinal, const bool isCompressed)
    {
        const auto opCodeValue = static_cast<std::uint8_t>(opCode);
        if (!isKnownOpCodeValue(opCodeValue))
        {
            // 枚举可以被强转成任意值，放行就会产出一条对端必然判错的帧，因此在本地当场拒绝
            throw Base::InvalidArgumentException(std::format("帧操作码 0x{:X} 未定义（RFC 6455 §5.2 只定义了 0x0、0x1、0x2、0x8、0x9、0xA），"
                                                             "请改用已定义的操作码",
                                                             opCodeValue));
        }

        // 控制帧的两条硬约束（RFC 6455 §5.5）：负载不超过 125 字节、不得分片。
        // 违反它们属于用法错误，与其发出去被对端断连，不如在本地就把这次调用拒掉
        const bool isControlFrame = isControlOpCodeValue(opCodeValue);
        if (isControlFrame && payload.size() > kWebSocketMaximumControlPayloadLength)
        {
            throw Base::InvalidArgumentException(std::format("控制帧负载不得超过 {} 字节（RFC 6455 §5.5），本次为 {} 字节："
                                                             "请把长数据放进 Text/Binary 帧",
                                                             kWebSocketMaximumControlPayloadLength, payload.size()));
        }
        if (isControlFrame && !isFinal)
        {
            throw Base::InvalidArgumentException("控制帧不得分片（RFC 6455 §5.5）：Close/Ping/Pong 的 FIN 位必须为 1，请改用完整帧发送");
        }

        std::string frame;
        // 预留首字节、最长 10 字节的长度域与负载，避免拼接过程中反复扩容
        frame.reserve(10 + payload.size());

        // 首字节布局：FIN(1) RSV1 RSV2 RSV3 操作码(4)。RSV1 只属于「压缩过的数据消息首帧」
        // （RFC 7692 §6）：控制帧从不压缩，继续帧也不是首帧。
        // 这里当场拒绝而不是发出去让对端断连——本地失败比线上失败好查得多
        if (isCompressed && (isControlFrame || opCodeValue == 0x0U))
        {
            throw Base::InvalidArgumentException(isControlFrame ? "控制帧不得压缩（RFC 7692 §6.1）：RSV1 只能出现在数据消息的首帧上"
                                                                : "继续帧不得置 RSV1（RFC 7692 §6.1）：压缩标记只写在数据消息的首帧上");
        }

        // RSV1（0x40）只在负载确实被压缩时置位；RSV2/RSV3 永远为 0
        frame.push_back(static_cast<char>(opCodeValue | (isFinal ? 0x80U : 0x00U) | (isCompressed ? 0x40U : 0x00U)));

        // 长度按 7 / 16 / 64 位三档编码，服务端发出的帧一律不置掩码位（RFC 6455 §5.1），
        // 因此第二个字节的最高位恒为 0
        if (payload.size() < kSixteenBitLengthEscape)
        {
            frame.push_back(static_cast<char>(payload.size()));
        }
        else if (payload.size() < kSixtyFourBitLengthThreshold)
        {
            frame.push_back(static_cast<char>(kSixteenBitLengthEscape));
            appendBigEndian(frame, payload.size(), 2);
        }
        else
        {
            // 64 位长度必须是不超过 2^63-1 的无符号数（RFC 6455 §5.2）:size_t 在 64 位平台上
            // 可达 2^64-1，因此显式判一次，不靠「平台不可能给出这么大的串」这种假设
            if (payload.size() > kMaximumSixtyFourBitLength)
            {
                throw Base::InvalidArgumentException(
                        std::format("负载长度 {} 字节超出 64 位长度域可表示的范围（RFC 6455 §5.2 要求最高位为 0）", payload.size()));
            }
            frame.push_back(static_cast<char>(kSixtyFourBitLengthEscape));
            appendBigEndian(frame, payload.size(), 8);
        }

        frame.append(payload);
        return frame;
    }

    WebSocketDecodeStatus WebSocketFrameDecoder::parse(const char *const data, const std::size_t length)
    {
        // 已产出的帧还没取走：一字节都不能再吃。这些字节属于下一帧，喂进去会被当成当前帧的开头，
        // 把已经产出的结果改坏
        if (m_hasPendingFrame)
        {
            m_consumedByteCount = 0;
            return WebSocketDecodeStatus::Frame;
        }

        // 错误粘滞：非 reset() 不能恢复，调用方要么重置要么关闭连接
        if (m_stage == Stage::Failed)
        {
            m_consumedByteCount = 0;
            return WebSocketDecodeStatus::Error;
        }

        std::size_t consumed = 0;
        while (consumed < length && m_stage != Stage::Failed && !m_hasPendingFrame)
        {
            if (m_stage == Stage::FirstByte)
            {
                const auto firstByte = static_cast<std::uint8_t>(data[consumed]);
                ++consumed;
                // 失败原因已由 acceptFirstByte 记下，循环条件随即退出
                if (!acceptFirstByte(firstByte))
                {
                    break;
                }
                m_stage = Stage::LengthFirstByte;
                continue;
            }

            if (m_stage == Stage::LengthFirstByte)
            {
                const auto secondByte = static_cast<std::uint8_t>(data[consumed]);
                ++consumed;
                if (!acceptLengthFirstByte(secondByte))
                {
                    break;
                }
                continue;
            }

            if (m_stage == Stage::ExtendedLength)
            {
                // 扩展长度按大端逐字节拼：切在任意字节之间都靠已读字节数续上
                m_payloadLength = (m_payloadLength << 8) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[consumed]));
                ++consumed;
                ++m_extendedLengthBytesSeen;
                if (m_extendedLengthBytesSeen < m_extendedLengthByteCount)
                {
                    continue;
                }

                // 收齐才校验：16 位档要等两个字节都到齐才能判「不足 126」这类非最短编码
                if (!acceptPayloadLength(m_payloadLength))
                {
                    break;
                }
                m_stage = Stage::MaskKey;
                continue;
            }

            if (m_stage == Stage::MaskKey)
            {
                m_maskKey[m_maskKeyBytesSeen] = static_cast<std::uint8_t>(data[consumed]);
                ++consumed;
                ++m_maskKeyBytesSeen;
                if (m_maskKeyBytesSeen < kMaskKeyLength)
                {
                    continue;
                }
                m_stage = Stage::Payload;

                // 零长度负载的帧在掩码键收齐这一刻就算完整：不能留到负载阶段再判，
                // 否则掩码键恰好是本次输入的最后一个字节时，调用方得再喂一个字节才拿得到帧
                if (m_payloadLength == 0)
                {
                    completeFrame();
                }
                continue;
            }

            // 走到这里只剩负载阶段，且长度已过上限校验，本次能收多少收多少
            const std::size_t remainingLength = static_cast<std::size_t>(m_payloadLength - m_framePayloadBytesSeen);
            const std::size_t chunkLength = std::min(remainingLength, length - consumed);

            // 先整段追加、再就地解掩码：一次 append 比逐字节 push_back 少若干次扩容判断，
            // 而掩码必须解除（RFC 6455 §5.3），键按 4 字节循环、每个帧用自己的键。
            // 控制帧的负载落在自己的缓冲里：它可能插在分片消息中间，共用一块会把
            // 已重组的那半条消息冲掉（RFC 6455 §5.4）
            std::string &payloadSink = isControlOpCodeValue(m_opCodeValue) ? m_controlPayloadBuffer : m_payloadBuffer;
            const std::size_t appendedBegin = payloadSink.size();
            payloadSink.append(data + consumed, chunkLength);
            for (std::size_t offset = 0; offset < chunkLength; ++offset)
            {
                const std::size_t maskIndex = (m_framePayloadBytesSeen + offset) % kMaskKeyLength;
                char &targetByte = payloadSink[appendedBegin + offset];
                targetByte = static_cast<char>(static_cast<std::uint8_t>(targetByte) ^ m_maskKey[maskIndex]);
            }
            m_framePayloadBytesSeen += chunkLength;
            consumed += chunkLength;

            if (m_framePayloadBytesSeen == static_cast<std::size_t>(m_payloadLength))
            {
                completeFrame();
            }
        }

        m_consumedByteCount = consumed;
        if (m_stage == Stage::Failed)
        {
            return WebSocketDecodeStatus::Error;
        }
        return m_hasPendingFrame ? WebSocketDecodeStatus::Frame : WebSocketDecodeStatus::NeedMore;
    }

    std::size_t WebSocketFrameDecoder::consumedByteCount() const
    {
        return m_consumedByteCount;
    }

    WebSocketFrame WebSocketFrameDecoder::takeFrame()
    {
        // 没有产出就取属于用法错误：静默返回一个空帧会让上层把「还没有数据」当成一条空消息发出去
        if (!m_hasPendingFrame)
        {
            throw Base::LogicException("WebSocket 帧解码器：当前没有待取走的帧，请先等 parse() 返回 Frame 再调用 takeFrame()");
        }

        WebSocketFrame frame = std::move(m_pendingFrame);
        // 取走即清标记：此后 parse() 才能继续消费字节。负载只清内容、保留容量供下一帧复用
        m_pendingFrame.payload.clear();
        m_hasPendingFrame = false;
        return frame;
    }

    void WebSocketFrameDecoder::reset()
    {
        clearFrameScratch();
        m_payloadBuffer.clear();
        m_controlPayloadBuffer.clear();
        m_isFragmentedMessageInProgress = false;
        m_isCurrentMessageCompressed = false;
        m_fragmentedMessageOpCodeValue = 0;
        m_pendingFrame = WebSocketFrame{};
        m_hasPendingFrame = false;
        m_hasError = false;
        m_isLimitExceeded = false;
        m_errorMessage.clear();
        m_consumedByteCount = 0;
        m_stage = Stage::FirstByte;
    }

    bool WebSocketFrameDecoder::hasError() const
    {
        return m_hasError;
    }

    bool WebSocketFrameDecoder::isLimitExceeded() const
    {
        return m_isLimitExceeded;
    }

    std::string WebSocketFrameDecoder::errorMessage() const
    {
        return m_errorMessage;
    }

    void WebSocketFrameDecoder::setPerMessageDeflateEnabled(const bool enabled) noexcept
    {
        m_isPerMessageDeflateEnabled = enabled;
    }

    bool WebSocketFrameDecoder::acceptFirstByte(const std::uint8_t firstByte)
    {
        // 首字节布局（RFC 6455 §5.2）：FIN(1) RSV1 RSV2 RSV3 操作码(4)
        const bool isFinal = (firstByte & 0x80U) != 0;
        const auto reservedBits = static_cast<std::uint8_t>(firstByte & 0x70U);
        const auto opCodeValue = static_cast<std::uint8_t>(firstByte & 0x0FU);

        // RSV 位口径（RFC 6455 §5.2 + RFC 7692 §6）：RSV2/RSV3 必须为 0；RSV1 只在协商过
        // permessage-deflate 且出现在数据消息首帧上时才合法——没协商就使用扩展必须判错，
        // 否则本端会接受一条自己解不开的消息
        const bool isCompressed = (reservedBits & 0x40U) != 0;
        const auto otherReservedBits = static_cast<std::uint8_t>(reservedBits & 0x30U);
        if (otherReservedBits != 0)
        {
            recordFailure(false, std::format("RSV2/RSV3 必须为 0（RFC 6455 §5.2），收到 RSV 位为 0x{:02X} 的帧", reservedBits));
            return false;
        }
        if (isCompressed && !m_isPerMessageDeflateEnabled)
        {
            recordFailure(false, "收到置了 RSV1 的帧，但本连接没有协商 permessage-deflate："
                                 "未协商就使用扩展（RFC 7692 §6），请去掉 RSV1 或先完成扩展协商");
            return false;
        }

        if (!isKnownOpCodeValue(opCodeValue))
        {
            recordFailure(false, std::format("操作码 0x{:X} 未定义（RFC 6455 §5.2 只定义了 0x0、0x1、0x2、0x8、0x9、0xA），"
                                             "本实现不接收保留操作码",
                                             opCodeValue));
            return false;
        }

        const bool isControlFrame = isControlOpCodeValue(opCodeValue);

        if (isCompressed && (isControlFrame || opCodeValue == 0x0U))
        {
            recordFailure(false, isControlFrame ? "控制帧不得压缩（RFC 7692 §6.1）：RSV1 只能出现在数据消息的首帧上"
                                               : "继续帧不得置 RSV1（RFC 7692 §6.1）：压缩标记只写在数据消息的首帧上");
            return false;
        }

        // 控制帧不得分片（RFC 6455 §5.5），否则对端永远等不到它的末尾。
        // 插在分片消息中间的控制帧是允许的（§5.4：control frames MAY be injected in the middle
        // of a fragmented message），下面按独立帧处理，分片状态不受影响
        if (isControlFrame && !isFinal)
        {
            recordFailure(false, "控制帧不得分片（RFC 6455 §5.5）：Close/Ping/Pong 的 FIN 位必须为 1，请改用完整帧发送");
            return false;
        }

        if (isControlFrame)
        {
            // 控制帧独立成帧，负载落点从零开始（缓冲区里可能还留着上一帧移走前的残留内容）。
            // 落在 m_controlPayloadBuffer：分片消息的 m_payloadBuffer 此刻可能正存着半条消息
            m_controlPayloadBuffer.clear();
        }
        else if (opCodeValue == 0x0)
        {
            // 继续帧必须接在一条未收尾的数据帧之后（RFC 6455 §5.4）
            if (!m_isFragmentedMessageInProgress)
            {
                recordFailure(false, "收到孤立的继续帧（Continuation）：它必须紧跟在一条未收尾的分片消息之后（RFC 6455 §5.4）");
                return false;
            }
        }
        else
        {
            // 数据帧开启一条新消息：此前不能有没收尾的分片，负载落点也从零开始
            if (m_isFragmentedMessageInProgress)
            {
                recordFailure(false, std::format("分片消息尚未结束时收到新的数据帧（操作码 0x{:X}）："
                                                 "请先发完当前消息的末帧，或改用继续帧",
                                                 opCodeValue));
                return false;
            }
            m_payloadBuffer.clear();
            m_fragmentedMessageOpCodeValue = opCodeValue;
            m_isFragmentedMessageInProgress = !isFinal;
            // 压缩标记只认消息首帧的 RSV1：后面的分片不带 RSV1，也改变不了本条消息的结论
            m_isCurrentMessageCompressed = isCompressed;
        }

        m_isFinal = isFinal;
        m_opCodeValue = opCodeValue;
        return true;
    }

    bool WebSocketFrameDecoder::acceptLengthFirstByte(const std::uint8_t secondByte)
    {
        // 第二个字节布局（RFC 6455 §5.2）：MASK(1) 负载长度域(7)
        const bool isMasked = (secondByte & 0x80U) != 0;
        const auto lengthFieldValue = static_cast<std::uint8_t>(secondByte & 0x7FU);

        // RFC 6455 §5.1：客户端发来的帧必须带掩码，服务端收到未掩码帧必须关闭连接。
        // 这里当场判错而不是「容忍一下」——掩码是这条连接上防止中间设施预测与重放帧内容的唯一防线
        if (!isMasked)
        {
            recordFailure(false, "客户端发来的帧必须带掩码（RFC 6455 §5.1）：请把帧第二个字节的 MASK 位置 1 后重发");
            return false;
        }

        if (isControlOpCodeValue(m_opCodeValue))
        {
            // 控制帧负载不得超过 125 字节（RFC 6455 §5.5），因此 126/127 两个转义取值在此就是越界，
            // 不必再读扩展长度
            if (lengthFieldValue > kWebSocketMaximumControlPayloadLength)
            {
                recordFailure(false, std::format("控制帧负载不得超过 {} 字节（RFC 6455 §5.5）：请把长数据放进 Text/Binary 帧",
                                                 kWebSocketMaximumControlPayloadLength));
                return false;
            }
            m_payloadLength = lengthFieldValue;
        }
        else if (lengthFieldValue < kSixteenBitLengthEscape)
        {
            m_payloadLength = lengthFieldValue;
        }
        else if (lengthFieldValue == kSixteenBitLengthEscape)
        {
            m_extendedLengthByteCount = 2;
        }
        else
        {
            m_extendedLengthByteCount = 8;
        }

        if (m_extendedLengthByteCount != 0)
        {
            // 扩展长度收齐之前不校验：16 位档要等两个字节都到齐才能判「不足 126」
            m_payloadLength = 0;
            m_extendedLengthBytesSeen = 0;
            m_stage = Stage::ExtendedLength;
            return true;
        }

        if (!acceptPayloadLength(m_payloadLength))
        {
            return false;
        }
        m_stage = Stage::MaskKey;
        return true;
    }

    bool WebSocketFrameDecoder::acceptPayloadLength(const std::uint64_t payloadLength)
    {
        // 非最短编码：RFC 6455 §5.2 要求用最少的字节数表示长度，否则同一个长度会有多种写法，
        // 不同的解析实现之间就会对同一段字节得出不同的帧边界
        if (m_extendedLengthByteCount == 2 && payloadLength < kSixteenBitLengthEscape)
        {
            recordFailure(false, std::format("负载长度 {} 用了非最短编码（RFC 6455 §5.2）：16 位长度不得小于 126，请改用 7 位长度",
                                             payloadLength));
            return false;
        }
        if (m_extendedLengthByteCount == 8)
        {
            if ((payloadLength & (1ull << 63)) != 0)
            {
                recordFailure(false, "64 位负载长度的最高位必须为 0（RFC 6455 §5.2）：请把长度限制在 2^63-1 以内");
                return false;
            }
            if (payloadLength < kSixtyFourBitLengthThreshold)
            {
                recordFailure(false,
                              std::format("负载长度 {} 用了非最短编码（RFC 6455 §5.2）：64 位长度不得小于 65536，请改用更短的档位",
                                          payloadLength));
                return false;
            }
        }

        // 单帧上限先卡在「声明」上：否则对端只要声明一个天文数字的长度，本端就会一直等下去，
        // 内存与连接都被一条永不完成的帧占着
        if (payloadLength > static_cast<std::uint64_t>(kMaximumFramePayloadLength))
        {
            recordFailure(true, std::format("单帧负载 {} 字节超出上限 {} 字节：请改用分片消息（RFC 6455 §5.4）拆成多条帧发送",
                                            payloadLength, kMaximumFramePayloadLength));
            return false;
        }

        // 消息总上限按「已重组 + 本帧声明」判断：分片消息的体量不设防同样能撑爆内存
        if (m_isFragmentedMessageInProgress &&
            static_cast<std::uint64_t>(m_payloadBuffer.size()) + payloadLength > static_cast<std::uint64_t>(kMaximumMessagePayloadLength))
        {
            recordFailure(true, std::format("分片消息重组后超出总上限 {} 字节：请缩小消息体量或拆成多条消息发送",
                                            kMaximumMessagePayloadLength));
            return false;
        }

        m_payloadLength = payloadLength;
        return true;
    }

    void WebSocketFrameDecoder::completeFrame()
    {
        const bool isControlFrame = isControlOpCodeValue(m_opCodeValue);

        // 数据帧的非末帧只是分片消息的一个中间片段：负载已经接进 m_payloadBuffer，不产出帧
        if (!isControlFrame && !m_isFinal)
        {
            clearFrameScratch();
            m_stage = Stage::FirstByte;
            return;
        }

        // 交付：控制帧就是它自己；数据帧的末帧交出的是重组好的整条消息，操作码取消息首帧的
        // ——继续帧自身的操作码只表示「我是后续片段」
        const bool isMessageEnd = !isControlFrame && m_opCodeValue == 0x0;
        m_pendingFrame.opCode = isControlFrame ? static_cast<WebSocketOpCode>(m_opCodeValue)
                                               : static_cast<WebSocketOpCode>(isMessageEnd ? m_fragmentedMessageOpCodeValue : m_opCodeValue);
        m_pendingFrame.isFinal = true;
        m_pendingFrame.isCompressed = !isControlFrame && m_isCurrentMessageCompressed;
        // 控制帧的负载在自己的缓冲里（数据消息的缓冲要留给还在进行中的分片）
        std::string &payloadSource = isControlFrame ? m_controlPayloadBuffer : m_payloadBuffer;
        m_pendingFrame.payload = std::move(payloadSource);
        // 移动之后源串的状态未指定：显式清空，让容量留着供下一帧复用
        payloadSource.clear();
        m_hasPendingFrame = true;

        // 控制帧不改变分片状态：RFC 6455 §5.4 允许控制帧插在分片消息中间，此时那条消息
        // 仍在进行中，后面的继续帧必须照常接上（把状态清掉会让它变成「孤立继续帧」被判错）
        if (!isControlFrame)
        {
            // 消息到此收尾（未分片的数据帧与分片消息的末帧都走这里），下一帧要么开新消息，要么是控制帧
            m_isFragmentedMessageInProgress = false;
            m_isCurrentMessageCompressed = false;
        }
        clearFrameScratch();
        m_stage = Stage::FirstByte;
    }

    void WebSocketFrameDecoder::recordFailure(const bool isLimitExceeded, std::string reason)
    {
        m_hasError = true;
        m_isLimitExceeded = isLimitExceeded;
        // 前缀统一在这里补：调用点只写原因，文案风格不会因为某个分支漏写而不一致
        m_errorMessage = "WebSocket 帧解码失败：" + std::move(reason);
        m_stage = Stage::Failed;
    }

    void WebSocketFrameDecoder::clearFrameScratch() noexcept
    {
        m_opCodeValue = 0;
        m_isFinal = true;
        m_payloadLength = 0;
        m_extendedLengthByteCount = 0;
        m_extendedLengthBytesSeen = 0;
        m_maskKey = {};
        m_maskKeyBytesSeen = 0;
        m_framePayloadBytesSeen = 0;
    }
} // namespace AsynGyanis::Net
