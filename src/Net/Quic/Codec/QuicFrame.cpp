#include "Net/Quic/Codec/QuicFrame.h"

#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicRawBytes.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 帧解码用的读位置游标
         *
         * @details 每个读法都自带「还剩够不够」的判断，失败原因里带上是哪一帧的哪个字段，
         *          否则一个 0x08 开头的载荷解到一半失败，调用方只能看到「字节不够」而无从定位。
         */
        class FrameReader
        {
        public:
            /**
             * @brief 构造游标
             * @param bytes 一个报文的净载荷
             * @param ownerOffset 本帧在净载荷里的起始偏移，出错时格式化成帧名
             */
            FrameReader(std::span<const std::uint8_t> bytes, std::size_t ownerOffset)
                : m_bytes(bytes), m_ownerOffset(ownerOffset)
            {
            }

            /**
             * @brief 读一个变长整数
             * @return std::expected<std::uint64_t, QuicDecodeError> 成功返回数值，失败返回截断错误
             */
            [[nodiscard]] std::expected<std::uint64_t, QuicDecodeError> readInteger()
            {
                const auto decoded = decodeQuicVariableLengthInteger(m_bytes.subspan(m_offset));
                if (!decoded.has_value())
                {
                    return std::unexpected(makeError(QuicDecodeErrorKind::Truncated, decoded.error().message));
                }
                m_offset += decoded->byteCount;
                return decoded->value;
            }

            /**
             * @brief 读帧类型：变长整数里唯一要求最短编码的一处（RFC 9000 §16 末段与 §12.4）
             * @return std::expected<std::uint64_t, QuicDecodeError> 成功返回类型值，非最短编码判 Malformed
             */
            [[nodiscard]] std::expected<std::uint64_t, QuicDecodeError> readFrameType()
            {
                const auto decoded = decodeQuicVariableLengthInteger(m_bytes.subspan(m_offset));
                if (!decoded.has_value())
                {
                    return std::unexpected(makeError(QuicDecodeErrorKind::Truncated, decoded.error().message));
                }
                m_offset += decoded->byteCount;
                if (decoded->byteCount != quicVariableLengthIntegerByteCount(decoded->value))
                {
                    return std::unexpected(makeError(QuicDecodeErrorKind::Malformed,
                                                     std::format("{}的帧类型用了 {} 字节而非最短的 {} 字节编码（RFC 9000 §16 里帧类型是唯一要求最短编码的字段）："
                                                                 "按 FRAME_ENCODING_ERROR 处理",
                                                                 ownerDescription(), decoded->byteCount,
                                                                 quicVariableLengthIntegerByteCount(decoded->value))));
                }
                return decoded->value;
            }

            /**
             * @brief 读一个 8 位无符号整数
             * @return std::expected<std::uint8_t, QuicDecodeError> 成功返回字节值
             */
            [[nodiscard]] std::expected<std::uint8_t, QuicDecodeError> readByte()
            {
                const auto bytes = readBytes(1);
                if (!bytes.has_value())
                {
                    return std::unexpected(bytes.error());
                }
                return (*bytes)[0];
            }

            /**
             * @brief 读定长字节
             * @param count 需要的字节数
             * @return std::expected<std::span<const std::uint8_t>, QuicDecodeError> 成功返回指向载荷的视图
             */
            [[nodiscard]] std::expected<std::span<const std::uint8_t>, QuicDecodeError> readBytes(const std::size_t count)
            {
                if (remainingByteCount() < count)
                {
                    return std::unexpected(makeError(QuicDecodeErrorKind::Truncated,
                                                     std::format("需要 {} 字节，载荷只剩 {} 字节", count, remainingByteCount())));
                }
                const std::span<const std::uint8_t> view = m_bytes.subspan(m_offset, count);
                m_offset += count;
                return view;
            }

            /**
             * @brief 读 16 位定长大端整数（连接标识长度等短字段用）
             * @return std::expected<std::uint16_t, QuicDecodeError> 成功返回数值
             */
            [[nodiscard]] std::expected<std::uint16_t, QuicDecodeError> readBigEndian16()
            {
                const auto bytes = readBytes(2);
                if (!bytes.has_value())
                {
                    return std::unexpected(bytes.error());
                }
                return static_cast<std::uint16_t>((static_cast<std::uint16_t>((*bytes)[0]) << 8) |
                                                  static_cast<std::uint16_t>((*bytes)[1]));
            }

            /**
             * @brief 取剩余全部字节并把游标推到末尾
             * @return std::span<const std::uint8_t> 剩余字节的视图（LEN 位为 0 的 STREAM 数据就这么取）
             */
            [[nodiscard]] std::span<const std::uint8_t> takeRemaining()
            {
                const std::span<const std::uint8_t> view = m_bytes.subspan(m_offset);
                m_offset = m_bytes.size();
                return view;
            }

            /// 剩余可读字节数
            [[nodiscard]] std::size_t remainingByteCount() const noexcept
            {
                return m_bytes.size() - m_offset;
            }

            /// 是否已到载荷末尾
            [[nodiscard]] bool atEnd() const noexcept
            {
                return m_offset >= m_bytes.size();
            }

            /**
             * @brief 造一条本帧的错误，文案自动带上帧名
             * @param kind 失败类别
             * @param detail 具体差在哪里
             * @return QuicDecodeError 带帧名的错误
             */
            [[nodiscard]] QuicDecodeError makeError(QuicDecodeErrorKind kind, std::string detail) const
            {
                return QuicDecodeError{kind, std::format("{}：{}", ownerDescription(), detail)};
            }

        private:
            /// 本帧的中文名：只在造错误时才格式化，成功路径上一帧都不碰堆
            [[nodiscard]] std::string ownerDescription() const
            {
                return std::format("载荷偏移 {} 处的帧", m_ownerOffset);
            }

            std::span<const std::uint8_t> m_bytes; ///< 整个净载荷，视图都指向它
            std::size_t m_offset{0};               ///< 当前读位置
            std::size_t m_ownerOffset{0};          ///< 本帧在净载荷里的起始偏移，出错时才格式化成帧名
        };

        /**
         * @brief 造一条与游标无关的解码错误
         * @param kind 失败类别
         * @param message 中文原因
         * @return QuicDecodeError 错误对象
         */
        QuicDecodeError makeFrameError(QuicDecodeErrorKind kind, std::string message)
        {
            return QuicDecodeError{kind, std::move(message)};
        }

        /**
         * @brief 按「偏移非 0 才带 Offset 字段、Length 字段恒带」算出 STREAM 帧的类型值
         * @param offset 流上偏移
         * @param isFinal 是否 FIN
         * @return std::uint64_t 0x08..0x0f 之一
         */
        std::uint64_t streamFrameTypeValue(const std::uint64_t offset, const bool isFinal) noexcept
        {
            const std::uint64_t offsetBit = offset != 0 ? kQuicStreamFrameOffsetBit : 0;
            return static_cast<std::uint64_t>(QuicFrameType::Stream) | kQuicStreamFrameLengthBit | offsetBit |
                   (isFinal ? kQuicStreamFrameFinalBit : 0);
        }

        /**
         * @brief 校验并解出 ACK 的区间序列（RFC 9000 §19.3.1）
         * @details 递推式 `largest = previous_smallest - gap - 2`：任何一步算出负包号都必须判
         *          FRAME_ENCODING_ERROR，否则会解出一段根本不存在的确认区间，把丢包检测带偏。
         * @param reader 游标
         * @param largestAcknowledged 最大确认包号
         * @param firstAcknowledgedRange 首区间长度
         * @param rangeCount 后续区间个数
         * @return std::expected<std::vector<QuicAcknowledgementRange>, QuicDecodeError> 成功返回绝对包号区间
         */
        std::expected<std::vector<QuicAcknowledgementRange>, QuicDecodeError> decodeAcknowledgementRanges(
                FrameReader &reader, const std::uint64_t largestAcknowledged, const std::uint64_t firstAcknowledgedRange,
                const std::uint64_t rangeCount)
        {
            if (largestAcknowledged < firstAcknowledgedRange)
            {
                return std::unexpected(reader.makeError(QuicDecodeErrorKind::Malformed,
                                                        std::format("最大确认包号 {} 小于首区间长度 {}，首区间最小包号为负",
                                                                    largestAcknowledged, firstAcknowledgedRange)));
            }

            std::vector<QuicAcknowledgementRange> ranges;
            ranges.push_back(QuicAcknowledgementRange{largestAcknowledged - firstAcknowledgedRange, largestAcknowledged});

            // 区间个数按载荷长度兜底：每个区间至少 2 字节，超限的计数只可能是攻击或解错了字段
            if (rangeCount > reader.remainingByteCount() / 2)
            {
                return std::unexpected(reader.makeError(QuicDecodeErrorKind::Malformed,
                                                        std::format("区间计数 {} 超过载荷剩余 {} 字节所能容纳的上限",
                                                                    rangeCount, reader.remainingByteCount() / 2)));
            }

            for (std::uint64_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
            {
                const auto gap = reader.readInteger();
                if (!gap.has_value())
                {
                    return std::unexpected(gap.error());
                }
                const auto rangeLength = reader.readInteger();
                if (!rangeLength.has_value())
                {
                    return std::unexpected(rangeLength.error());
                }
                const std::uint64_t previousSmallest = ranges.back().smallestAcknowledged;
                // gap + 2 可能溢出：先分别判，避免回绕后把非法区间当成合法
                if (previousSmallest < *gap + 2 || (previousSmallest - *gap - 2) < *rangeLength)
                {
                    return std::unexpected(reader.makeError(QuicDecodeErrorKind::Malformed,
                                                            std::format("第 {} 个区间的 gap {} 与长度 {} 使包号算成负数（RFC 9000 §19.3.1 要求按 "
                                                                        "FRAME_ENCODING_ERROR 处理）",
                                                                        rangeIndex + 1, *gap, *rangeLength)));
                }
                const std::uint64_t rangeLargest = previousSmallest - *gap - 2;
                ranges.push_back(QuicAcknowledgementRange{rangeLargest - *rangeLength, rangeLargest});
            }
            return ranges;
        }

        /**
         * @brief 编码侧的区间校验：确认这帧区间能按 §19.3.1 换成线格式，且不算出负包号
         * @details 换算结果（首项 (0, First ACK Range) 与其后的每个 (gap, 区间长度) 对）都能由相邻
         *          区间当场算出来，因此不在此处物化一份中间向量：这条路径每个带确认的出站包都要走
         *          一遍，物化就是每包一次分配。校验整体先于写入完成，抛异常时一个字节都不会落进缓冲。
         * @param frame ACK 帧
         * @throws Base::InvalidArgumentException 用法错误：区间为空、首区间最大值不等于声明的最大包号、
         *         区间不递减或按 §19.3.1 会算出负包号
         */
        void requireEncodableAcknowledgementRanges(const QuicAcknowledgementFrame &frame)
        {
            if (frame.ranges.empty())
            {
                throw Base::InvalidArgumentException("ACK 帧的区间列表为空：线格式的 First ACK Range 恒描述含最大包号那一段，至少要给一个区间");
            }
            if (frame.ranges.front().largestAcknowledged != frame.largestAcknowledgedPacketNumber)
            {
                throw Base::InvalidArgumentException(std::format("ACK 帧首个区间的最大包号 {} 与声明的最大确认包号 {} 不一致（RFC 9000 §19.3.1）："
                                                                 "请让区间覆盖最大确认包号",
                                                                 frame.ranges.front().largestAcknowledged,
                                                                 frame.largestAcknowledgedPacketNumber));
            }
            for (std::size_t rangeIndex = 1; rangeIndex < frame.ranges.size(); ++rangeIndex)
            {
                const QuicAcknowledgementRange &previous = frame.ranges[rangeIndex - 1];
                const QuicAcknowledgementRange &current = frame.ranges[rangeIndex];
                if (current.smallestAcknowledged > current.largestAcknowledged ||
                    previous.smallestAcknowledged < current.largestAcknowledged + 2)
                {
                    throw Base::InvalidArgumentException(std::format("ACK 帧第 {} 个区间 [{}, {}] 与上一区间的最小值 {} 之间留不出 gap 所要求的"
                                                                     "至少一个未确认包（RFC 9000 §19.3.1 的 largest = previous_smallest - gap - 2 "
                                                                     "会算出负包号）：请合并区间或修正区间",
                                                                     rangeIndex + 1, current.largestAcknowledged, current.smallestAcknowledged,
                                                                     previous.smallestAcknowledged));
                }
            }
        }

        /**
         * @brief 帧编码器：一种帧一个 operator() 重载
         *
         * @details 用重载而不是 switch：QuicFrame 是新成员时编译器会直接报「visit 没有覆盖该分支」，
         *          比漏写一个 case 等到运行期才发现强。
         */
        struct FrameAppender
        {
            std::string &bytes; ///< 目标缓冲

            void operator()(const QuicPaddingFrame &) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::Padding));
            }

            void operator()(const QuicPingFrame &) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::Ping));
            }

            void operator()(const QuicHandshakeDoneFrame &) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::HandshakeDone));
            }

            void operator()(const QuicAcknowledgementFrame &frame) const
            {
                requireEncodableAcknowledgementRanges(frame);
                appendQuicVariableLengthInteger(bytes,
                                                static_cast<std::uint64_t>(frame.hasEcnCounts ? QuicFrameType::AcknowledgementEcn
                                                                                              : QuicFrameType::Acknowledgement));
                appendQuicVariableLengthInteger(bytes, frame.largestAcknowledgedPacketNumber);
                appendQuicVariableLengthInteger(bytes, frame.acknowledgementDelay);
                // ACK Range Count 就是「首区间之外还有几段」，不必先物化区间对才知道
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(frame.ranges.size() - 1));
                // 首区间由 First ACK Range 单独表达：含最大包号那一段的跨度
                appendQuicVariableLengthInteger(bytes, frame.largestAcknowledgedPacketNumber - frame.ranges.front().smallestAcknowledged);
                for (std::size_t rangeIndex = 1; rangeIndex < frame.ranges.size(); ++rangeIndex)
                {
                    const QuicAcknowledgementRange &previous = frame.ranges[rangeIndex - 1];
                    const QuicAcknowledgementRange &current = frame.ranges[rangeIndex];
                    appendQuicVariableLengthInteger(bytes, previous.smallestAcknowledged - current.largestAcknowledged - 2);
                    appendQuicVariableLengthInteger(bytes, current.largestAcknowledged - current.smallestAcknowledged);
                }
                if (frame.hasEcnCounts)
                {
                    for (const std::uint64_t ecnCount: frame.ecnCounts)
                    {
                        appendQuicVariableLengthInteger(bytes, ecnCount);
                    }
                }
            }

            void operator()(const QuicResetStreamFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::ResetStream));
                appendQuicVariableLengthInteger(bytes, frame.streamId);
                appendQuicVariableLengthInteger(bytes, frame.applicationErrorCode);
                appendQuicVariableLengthInteger(bytes, frame.finalSize);
            }

            void operator()(const QuicStopSendingFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::StopSending));
                appendQuicVariableLengthInteger(bytes, frame.streamId);
                appendQuicVariableLengthInteger(bytes, frame.applicationErrorCode);
            }

            void operator()(const QuicCryptoFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::Crypto));
                appendQuicVariableLengthInteger(bytes, frame.offset);
                appendQuicVariableLengthInteger(bytes, frame.data.size());
                appendQuicRawBytes(bytes, frame.data);
            }

            void operator()(const QuicNewTokenFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::NewToken));
                appendQuicVariableLengthInteger(bytes, frame.token.size());
                appendQuicRawBytes(bytes, frame.token);
            }

            void operator()(const QuicStreamFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, streamFrameTypeValue(frame.offset, frame.isFinal));
                appendQuicVariableLengthInteger(bytes, frame.streamId);
                if (frame.offset != 0)
                {
                    // 偏移为 0 时省掉 Offset 字段，正是 §19.8 里 OFF 位为 0 的含义
                    appendQuicVariableLengthInteger(bytes, frame.offset);
                }
                appendQuicVariableLengthInteger(bytes, frame.data.size());
                appendQuicRawBytes(bytes, frame.data);
            }

            void operator()(const QuicMaxDataFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::MaxData));
                appendQuicVariableLengthInteger(bytes, frame.maximumData);
            }

            void operator()(const QuicMaxStreamDataFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::MaxStreamData));
                appendQuicVariableLengthInteger(bytes, frame.streamId);
                appendQuicVariableLengthInteger(bytes, frame.maximumStreamData);
            }

            void operator()(const QuicMaxStreamsFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes,
                                                static_cast<std::uint64_t>(frame.isUnidirectional ? QuicFrameType::MaxStreamsUni
                                                                                                  : QuicFrameType::MaxStreamsBidi));
                appendQuicVariableLengthInteger(bytes, frame.maximumStreams);
            }

            void operator()(const QuicDataBlockedFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::DataBlocked));
                appendQuicVariableLengthInteger(bytes, frame.maximumData);
            }

            void operator()(const QuicStreamDataBlockedFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::StreamDataBlocked));
                appendQuicVariableLengthInteger(bytes, frame.streamId);
                appendQuicVariableLengthInteger(bytes, frame.streamDataLimit);
            }

            void operator()(const QuicStreamsBlockedFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes,
                                                static_cast<std::uint64_t>(frame.isUnidirectional ? QuicFrameType::StreamsBlockedUni
                                                                                                  : QuicFrameType::StreamsBlockedBidi));
                appendQuicVariableLengthInteger(bytes, frame.streamLimit);
            }

            void operator()(const QuicNewConnectionIdFrame &frame) const
            {
                // §19.15：长度小于 1 或大于 20 都是 FRAME_ENCODING_ERROR，本端自己也不能发出去
                if (frame.connectionId.size() < kQuicMinimumIssuedConnectionIdLength ||
                    frame.connectionId.size() > kQuicMaximumConnectionIdLength)
                {
                    throw Base::InvalidArgumentException(std::format("NEW_CONNECTION_ID 里连接标识长度 {} 不在 {}..{} 字节内（RFC 9000 §19.15）："
                                                                     "0 长度或超长标识都不允许签发",
                                                                     frame.connectionId.size(), kQuicMinimumIssuedConnectionIdLength,
                                                                     kQuicMaximumConnectionIdLength));
                }
                if (frame.statelessResetToken.size() != kQuicStatelessResetTokenByteLength)
                {
                    throw Base::InvalidArgumentException(std::format("NEW_CONNECTION_ID 的无状态重置令牌长度是 {} 字节，需要 {} 字节（RFC 9000 §19.15 图 39）："
                                                                     "请先用 HKDF 从服务端密钥派生满 16 字节再签",
                                                                     frame.statelessResetToken.size(), kQuicStatelessResetTokenByteLength));
                }
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::NewConnectionId));
                appendQuicVariableLengthInteger(bytes, frame.sequenceNumber);
                appendQuicVariableLengthInteger(bytes, frame.retirePriorTo);
                bytes.push_back(static_cast<char>(frame.connectionId.size()));
                appendQuicRawBytes(bytes, frame.connectionId);
                appendQuicRawBytes(bytes, frame.statelessResetToken);
            }

            void operator()(const QuicRetireConnectionIdFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::RetireConnectionId));
                appendQuicVariableLengthInteger(bytes, frame.sequenceNumber);
            }

            void operator()(const QuicPathChallengeFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::PathChallenge));
                appendQuicRawBytes(bytes, std::span<const std::uint8_t>(frame.data));
            }

            void operator()(const QuicPathResponseFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(QuicFrameType::PathResponse));
                appendQuicRawBytes(bytes, std::span<const std::uint8_t>(frame.data));
            }

            void operator()(const QuicConnectionCloseFrame &frame) const
            {
                appendQuicVariableLengthInteger(bytes, static_cast<std::uint64_t>(
                        frame.triggeredFrameType.has_value() ? QuicFrameType::ConnectionClose : QuicFrameType::ApplicationClose));
                appendQuicVariableLengthInteger(bytes, frame.errorCode);
                if (frame.triggeredFrameType.has_value())
                {
                    // 0x1c 形态里这个字段恒在，不知道触发帧时按 §19.19 填 0（等同 PADDING）
                    appendQuicVariableLengthInteger(bytes, *frame.triggeredFrameType);
                }
                appendQuicVariableLengthInteger(bytes, frame.reasonPhrase.size());
                appendQuicRawBytes(bytes, frame.reasonPhrase);
            }
        };
    } // namespace

    std::uint64_t quicFrameTypeValue(const QuicFrame &frame) noexcept
    {
        return std::visit(
                [](const auto &concreteFrame) -> std::uint64_t
                {
                    using Frame = std::remove_cvref_t<decltype(concreteFrame)>;
                    if constexpr (std::is_same_v<Frame, QuicPaddingFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::Padding);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicPingFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::Ping);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicHandshakeDoneFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::HandshakeDone);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicAcknowledgementFrame>)
                    {
                        return static_cast<std::uint64_t>(concreteFrame.hasEcnCounts ? QuicFrameType::AcknowledgementEcn
                                                                                     : QuicFrameType::Acknowledgement);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicResetStreamFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::ResetStream);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicStopSendingFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::StopSending);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicCryptoFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::Crypto);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicNewTokenFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::NewToken);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicStreamFrame>)
                    {
                        return streamFrameTypeValue(concreteFrame.offset, concreteFrame.isFinal);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicMaxDataFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::MaxData);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicMaxStreamDataFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::MaxStreamData);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicMaxStreamsFrame>)
                    {
                        return static_cast<std::uint64_t>(concreteFrame.isUnidirectional ? QuicFrameType::MaxStreamsUni
                                                                                         : QuicFrameType::MaxStreamsBidi);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicDataBlockedFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::DataBlocked);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicStreamDataBlockedFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::StreamDataBlocked);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicStreamsBlockedFrame>)
                    {
                        return static_cast<std::uint64_t>(concreteFrame.isUnidirectional ? QuicFrameType::StreamsBlockedUni
                                                                                         : QuicFrameType::StreamsBlockedBidi);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicNewConnectionIdFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::NewConnectionId);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicRetireConnectionIdFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::RetireConnectionId);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicPathChallengeFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::PathChallenge);
                    }
                    else if constexpr (std::is_same_v<Frame, QuicPathResponseFrame>)
                    {
                        return static_cast<std::uint64_t>(QuicFrameType::PathResponse);
                    }
                    else
                    {
                        return static_cast<std::uint64_t>(concreteFrame.triggeredFrameType.has_value() ? QuicFrameType::ConnectionClose
                                                                                                       : QuicFrameType::ApplicationClose);
                    }
                },
                frame);
    }

    std::expected<void, QuicDecodeError> decodeQuicFrames(const std::span<const std::uint8_t> payload,
                                                           std::vector<QuicFrame> &frames)
    {
        // 进出自明：容量留下、内容清空，调用方才敢把同一块缓冲一包接一包地交回来
        frames.clear();
        std::size_t readOffset = 0;
        while (readOffset < payload.size())
        {
            // 游标逐帧新建：失败文案要指出错在哪一帧，一轮用到底会把第 5 帧的错安到第一帧头上
            FrameReader reader(payload.subspan(readOffset), readOffset);
            const auto typeValue = reader.readFrameType();
            if (!typeValue.has_value())
            {
                return std::unexpected(typeValue.error());
            }

            QuicFrame frame;
            switch (*typeValue)
            {
            case static_cast<std::uint64_t>(QuicFrameType::Padding):
                // 连续 PADDING 合成一帧交出去没有意义，逐帧保留让上层按字节数记账（它决定拥塞窗口的用量）
                frame = QuicPaddingFrame{};
                break;
            case static_cast<std::uint64_t>(QuicFrameType::Ping):
                frame = QuicPingFrame{};
                break;
            case static_cast<std::uint64_t>(QuicFrameType::HandshakeDone):
                frame = QuicHandshakeDoneFrame{};
                break;
            case static_cast<std::uint64_t>(QuicFrameType::Acknowledgement):
            case static_cast<std::uint64_t>(QuicFrameType::AcknowledgementEcn):
            {
                QuicAcknowledgementFrame acknowledgement;
                const auto largestAcknowledged = reader.readInteger();
                if (!largestAcknowledged.has_value())
                {
                    return std::unexpected(largestAcknowledged.error());
                }
                const auto acknowledgementDelay = reader.readInteger();
                if (!acknowledgementDelay.has_value())
                {
                    return std::unexpected(acknowledgementDelay.error());
                }
                const auto rangeCount = reader.readInteger();
                if (!rangeCount.has_value())
                {
                    return std::unexpected(rangeCount.error());
                }
                const auto firstAcknowledgedRange = reader.readInteger();
                if (!firstAcknowledgedRange.has_value())
                {
                    return std::unexpected(firstAcknowledgedRange.error());
                }
                auto ranges = decodeAcknowledgementRanges(reader, *largestAcknowledged, *firstAcknowledgedRange, *rangeCount);
                if (!ranges.has_value())
                {
                    return std::unexpected(ranges.error());
                }
                acknowledgement.largestAcknowledgedPacketNumber = *largestAcknowledged;
                acknowledgement.acknowledgementDelay = *acknowledgementDelay;
                acknowledgement.ranges = std::move(*ranges);
                if (*typeValue == static_cast<std::uint64_t>(QuicFrameType::AcknowledgementEcn))
                {
                    // 三个计数缺一即截断：漏读会让 ECN 拥塞标记的统计整体错位
                    for (std::uint64_t &ecnCount: acknowledgement.ecnCounts)
                    {
                        const auto count = reader.readInteger();
                        if (!count.has_value())
                        {
                            return std::unexpected(count.error());
                        }
                        ecnCount = *count;
                    }
                    acknowledgement.hasEcnCounts = true;
                }
                frame = acknowledgement;
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::ResetStream):
            {
                const auto streamId = reader.readInteger();
                if (!streamId.has_value())
                {
                    return std::unexpected(streamId.error());
                }
                const auto errorCode = reader.readInteger();
                if (!errorCode.has_value())
                {
                    return std::unexpected(errorCode.error());
                }
                const auto finalSize = reader.readInteger();
                if (!finalSize.has_value())
                {
                    return std::unexpected(finalSize.error());
                }
                frame = QuicResetStreamFrame{*streamId, *errorCode, *finalSize};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::StopSending):
            {
                const auto streamId = reader.readInteger();
                if (!streamId.has_value())
                {
                    return std::unexpected(streamId.error());
                }
                const auto errorCode = reader.readInteger();
                if (!errorCode.has_value())
                {
                    return std::unexpected(errorCode.error());
                }
                frame = QuicStopSendingFrame{*streamId, *errorCode};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::Crypto):
            {
                const auto offset = reader.readInteger();
                if (!offset.has_value())
                {
                    return std::unexpected(offset.error());
                }
                const auto length = reader.readInteger();
                if (!length.has_value())
                {
                    return std::unexpected(length.error());
                }
                const auto data = reader.readBytes(static_cast<std::size_t>(*length));
                if (!data.has_value())
                {
                    return std::unexpected(data.error());
                }
                frame = QuicCryptoFrame{*offset, *data};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::NewToken):
            {
                const auto length = reader.readInteger();
                if (!length.has_value())
                {
                    return std::unexpected(length.error());
                }
                const auto token = reader.readBytes(static_cast<std::size_t>(*length));
                if (!token.has_value())
                {
                    return std::unexpected(token.error());
                }
                frame = QuicNewTokenFrame{*token};
                break;
            }
            case 0x08:
            case 0x09:
            case 0x0a:
            case 0x0b:
            case 0x0c:
            case 0x0d:
            case 0x0e:
            case 0x0f:
            {
                QuicStreamFrame stream;
                const auto streamId = reader.readInteger();
                if (!streamId.has_value())
                {
                    return std::unexpected(streamId.error());
                }
                stream.streamId = *streamId;
                if ((*typeValue & kQuicStreamFrameOffsetBit) != 0)
                {
                    const auto offset = reader.readInteger();
                    if (!offset.has_value())
                    {
                        return std::unexpected(offset.error());
                    }
                    stream.offset = *offset;
                }
                if ((*typeValue & kQuicStreamFrameLengthBit) != 0)
                {
                    // LEN 位决定数据是否自带长度；不带长度的写法数据延伸到载荷末尾（§19.8）
                    const auto length = reader.readInteger();
                    if (!length.has_value())
                    {
                        return std::unexpected(length.error());
                    }
                    const auto data = reader.readBytes(static_cast<std::size_t>(*length));
                    if (!data.has_value())
                    {
                        return std::unexpected(data.error());
                    }
                    stream.data = *data;
                }
                else
                {
                    stream.data = reader.takeRemaining();
                }
                stream.isFinal = (*typeValue & kQuicStreamFrameFinalBit) != 0;
                frame = stream;
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::MaxData):
            case static_cast<std::uint64_t>(QuicFrameType::DataBlocked):
            {
                const auto maximumData = reader.readInteger();
                if (!maximumData.has_value())
                {
                    return std::unexpected(maximumData.error());
                }
                frame = *typeValue == static_cast<std::uint64_t>(QuicFrameType::MaxData)
                        ? QuicFrame{QuicMaxDataFrame{*maximumData}}
                        : QuicFrame{QuicDataBlockedFrame{*maximumData}};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::MaxStreamData):
            case static_cast<std::uint64_t>(QuicFrameType::StreamDataBlocked):
            {
                const auto streamId = reader.readInteger();
                if (!streamId.has_value())
                {
                    return std::unexpected(streamId.error());
                }
                const auto limit = reader.readInteger();
                if (!limit.has_value())
                {
                    return std::unexpected(limit.error());
                }
                frame = *typeValue == static_cast<std::uint64_t>(QuicFrameType::MaxStreamData)
                        ? QuicFrame{QuicMaxStreamDataFrame{*streamId, *limit}}
                        : QuicFrame{QuicStreamDataBlockedFrame{*streamId, *limit}};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::MaxStreamsBidi):
            case static_cast<std::uint64_t>(QuicFrameType::MaxStreamsUni):
            {
                const auto maximumStreams = reader.readInteger();
                if (!maximumStreams.has_value())
                {
                    return std::unexpected(maximumStreams.error());
                }
                frame = QuicMaxStreamsFrame{*maximumStreams, *typeValue == static_cast<std::uint64_t>(QuicFrameType::MaxStreamsUni)};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::StreamsBlockedBidi):
            case static_cast<std::uint64_t>(QuicFrameType::StreamsBlockedUni):
            {
                const auto streamLimit = reader.readInteger();
                if (!streamLimit.has_value())
                {
                    return std::unexpected(streamLimit.error());
                }
                frame = QuicStreamsBlockedFrame{*streamLimit, *typeValue == static_cast<std::uint64_t>(QuicFrameType::StreamsBlockedUni)};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::RetireConnectionId):
            {
                const auto sequenceNumber = reader.readInteger();
                if (!sequenceNumber.has_value())
                {
                    return std::unexpected(sequenceNumber.error());
                }
                frame = QuicRetireConnectionIdFrame{*sequenceNumber};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::NewConnectionId):
            {
                const auto sequenceNumber = reader.readInteger();
                if (!sequenceNumber.has_value())
                {
                    return std::unexpected(sequenceNumber.error());
                }
                const auto retirePriorTo = reader.readInteger();
                if (!retirePriorTo.has_value())
                {
                    return std::unexpected(retirePriorTo.error());
                }
                const auto lengthByte = reader.readByte();
                if (!lengthByte.has_value())
                {
                    return std::unexpected(lengthByte.error());
                }
                // §19.15：长度小于 1 或大于 20 直接是 FRAME_ENCODING_ERROR，不当成普通截断
                if (*lengthByte < kQuicMinimumIssuedConnectionIdLength || *lengthByte > kQuicMaximumConnectionIdLength)
                {
                    return std::unexpected(reader.makeError(QuicDecodeErrorKind::Malformed,
                                                            std::format("连接标识长度 {} 不在 {}..{} 字节内（RFC 9000 §19.15 要求按 "
                                                                        "FRAME_ENCODING_ERROR 处理）",
                                                                        static_cast<unsigned int>(*lengthByte),
                                                                        kQuicMinimumIssuedConnectionIdLength, kQuicMaximumConnectionIdLength)));
                }
                const auto connectionId = reader.readBytes(*lengthByte);
                if (!connectionId.has_value())
                {
                    return std::unexpected(connectionId.error());
                }
                const auto resetToken = reader.readBytes(kQuicStatelessResetTokenByteLength);
                if (!resetToken.has_value())
                {
                    return std::unexpected(reader.makeError(QuicDecodeErrorKind::Truncated,
                                                            std::format("缺 {} 字节无状态重置令牌（RFC 9000 §19.15 图 39）",
                                                                        kQuicStatelessResetTokenByteLength)));
                }
                frame = QuicNewConnectionIdFrame{*sequenceNumber, *retirePriorTo, *connectionId, *resetToken};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::PathChallenge):
            case static_cast<std::uint64_t>(QuicFrameType::PathResponse):
            {
                const auto data = reader.readBytes(kQuicPathValidationDataByteLength);
                if (!data.has_value())
                {
                    return std::unexpected(data.error());
                }
                std::array<std::uint8_t, kQuicPathValidationDataByteLength> validationData{};
                std::copy(data->begin(), data->end(), validationData.begin());
                frame = *typeValue == static_cast<std::uint64_t>(QuicFrameType::PathChallenge)
                        ? QuicFrame{QuicPathChallengeFrame{validationData}}
                        : QuicFrame{QuicPathResponseFrame{validationData}};
                break;
            }
            case static_cast<std::uint64_t>(QuicFrameType::ConnectionClose):
            case static_cast<std::uint64_t>(QuicFrameType::ApplicationClose):
            {
                QuicConnectionCloseFrame close;
                const auto errorCode = reader.readInteger();
                if (!errorCode.has_value())
                {
                    return std::unexpected(errorCode.error());
                }
                close.errorCode = *errorCode;
                if (*typeValue == static_cast<std::uint64_t>(QuicFrameType::ConnectionClose))
                {
                    // 0x1c 形态的 Frame Type 字段恒在，取 0 表示触发帧未知（§19.19）
                    const auto triggeredFrameType = reader.readInteger();
                    if (!triggeredFrameType.has_value())
                    {
                        return std::unexpected(triggeredFrameType.error());
                    }
                    close.triggeredFrameType = *triggeredFrameType;
                }
                const auto reasonLength = reader.readInteger();
                if (!reasonLength.has_value())
                {
                    return std::unexpected(reasonLength.error());
                }
                const auto reason = reader.readBytes(static_cast<std::size_t>(*reasonLength));
                if (!reason.has_value())
                {
                    return std::unexpected(reason.error());
                }
                close.reasonPhrase = *reason;
                frame = close;
                break;
            }
            default:
                return std::unexpected(makeFrameError(
                        QuicDecodeErrorKind::Malformed,
                        std::format("未定义的帧类型 0x{:X}（RFC 9000 §12.4 表 3 未列，且未协商任何扩展帧）：按 PROTOCOL_VIOLATION 处理",
                                    static_cast<unsigned long long>(*typeValue))));
            }

            frames.push_back(std::move(frame));
            readOffset = payload.size() - reader.remainingByteCount();
        }
        return {};
    }

    std::expected<std::vector<QuicFrame>, QuicDecodeError> decodeQuicFrames(const std::span<const std::uint8_t> payload)
    {
        std::vector<QuicFrame> frames;
        if (const std::expected<void, QuicDecodeError> decoded = decodeQuicFrames(payload, frames); !decoded.has_value())
        {
            return std::unexpected(decoded.error());
        }
        return frames;
    }

    void appendQuicFrame(std::string &bytes, const QuicFrame &frame)
    {
        std::visit(FrameAppender{bytes}, frame);
    }

    std::vector<QuicAcknowledgementRange> buildQuicAcknowledgementRanges(const QuicReceivedPacketNumbers &receivedPacketNumbers,
                                                                        const std::uint64_t acknowledgedUpTo)
    {
        std::vector<QuicAcknowledgementRange> ranges;
        const std::map<std::uint64_t, std::uint64_t> &spans = receivedPacketNumbers.ranges();
        // 从最高的一段往下走：§19.3.1 要的是递减区间，而集合里相邻的号早已并成一段，不必再合并
        for (auto cursor = spans.rbegin(); cursor != spans.rend(); ++cursor)
        {
            if (cursor->first > acknowledgedUpTo)
            {
                // 整段都在这次确认值之上：本次不认（下一次确认才会带上）
                continue;
            }
            if (ranges.size() >= kQuicMaximumAcknowledgementRanges)
            {
                break;
            }
            // 只有最高那一段会被夹掉尾端：夹出来的尾端就是本次的确认值
            ranges.push_back(QuicAcknowledgementRange{cursor->first, std::min(cursor->second, acknowledgedUpTo)});
        }
        if (ranges.empty())
        {
            // 不变式：区间永不为空且首段必须含最大确认值（见 QuicAcknowledgementFrame 的 @note）
            ranges.push_back(QuicAcknowledgementRange{acknowledgedUpTo, acknowledgedUpTo});
        }
        return ranges;
    }
} // namespace AsynGyanis::Net
