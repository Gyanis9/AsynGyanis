#include "Net/Quic/Streams/QuicStreamLayer.h"

#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <algorithm>
#include <format>
#include <memory>
#include <ranges>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        // 传输层错误码（RFC 9000 §11.1）
        constexpr std::uint64_t kFlowControlError = 0x03;
        constexpr std::uint64_t kStreamLimitError = 0x04;
        constexpr std::uint64_t kStreamStateError = 0x05;
        constexpr std::uint64_t kFinalSizeError = 0x06;
        constexpr std::uint64_t kProtocolViolation = 0x0a;

        /// 流号低位（§2.1）：bit0 标发起方，bit1 标单向
        constexpr std::uint64_t kStreamInitiatorBitMask = 0x01;
        constexpr std::uint64_t kStreamUnidirectionalBitMask = 0x02;

        /**
         * @brief 一条上界还剩多少地方：已占超过上界时给 0，不做无符号回绕
         * @details 判丢重排会把字节退回队列，实际占用可以短暂越过上界；直接相减会绕成天文数字，
         *          那道闸当场翻转成「放行一切」
         */
        [[nodiscard]] std::size_t saturatingRoomOf(const std::size_t limitByteCount, const std::size_t usedByteCount) noexcept
        {
            return usedByteCount < limitByteCount ? limitByteCount - usedByteCount : 0;
        }

        /// 同类流号每隔 4 一条：0x00、0x04、0x08……（§2.1）
        constexpr std::uint64_t kStreamIdStride = 4;

        /// STREAM 帧的类型字段取值不超过 0x0f，永远落在变长整数的 1 字节档里（§19.8）
        constexpr std::size_t kQuicStreamFrameTypeByteLength = 1;

        /**
         * @brief 这条流是不是本端发起的（服务端即低位为 1 的那两档）
         * @param streamId 流号
         * @return true 本端发起
         */
        bool isLocallyInitiated(const std::uint64_t streamId) noexcept
        {
            return (streamId & kStreamInitiatorBitMask) != 0;
        }

        /**
         * @brief 这条流是不是单向的
         * @param streamId 流号
         * @return true 单向
         */
        bool isUnidirectional(const std::uint64_t streamId) noexcept
        {
            return (streamId & kStreamUnidirectionalBitMask) != 0;
        }

        /**
         * @brief 这条流是「同类型里的第几条」，从 0 计（§4.6 的流数就是按它数的）
         * @param streamId 流号
         * @return std::uint64_t 序号
         */
        std::uint64_t streamIndexOfType(const std::uint64_t streamId) noexcept
        {
            return streamId / kStreamIdStride;
        }

        /**
         * @brief 「已作废流号」边界按哪一档记：只分双向与单向
         * @details 只对对端发起的流有意义（本端发起的流不摘），所以发起方那一位不必进档位
         * @param streamId 流号
         * @return std::size_t 0 为双向、1 为单向
         */
        std::size_t retiredBoundarySlotOf(const std::uint64_t streamId) noexcept
        {
            return isUnidirectional(streamId) ? 1U : 0U;
        }

        /**
         * @brief 造一条带中文文案的违规
         * @param errorCode §11.1 的传输错误码
         * @param reasonPhrase 进 CONNECTION_CLOSE 的原因文案
         * @return QuicStreamViolation 交回核心
         */
        QuicStreamViolation makeViolation(const std::uint64_t errorCode, std::string reasonPhrase)
        {
            return QuicStreamViolation{errorCode, std::move(reasonPhrase)};
        }

        /**
         * @brief 试着把一帧编进缓冲：装不下就原样退回，什么都不改
         * @details 控制帧的宽度取决于变长整数落在哪一档，先编再看真实长度比先算再编省事；
         *          退回必须把 `frames` 裁回原样，否则这一包会超出调用方给的预算。
         * @param frames 目标帧缓冲
         * @param byteBudget 本包还能装多少字节
         * @param usedByteCount 已经用掉的字节数，就地累加
         * @param frame 要编出去的帧
         * @return true 已编进去并累加了长度；false 预算不够，缓冲保持原样
         */
        bool tryAppendFrame(std::string &frames, const std::size_t byteBudget, std::size_t &usedByteCount,
                            const QuicFrame &frame)
        {
            const std::size_t beforeByteCount = frames.size();
            appendQuicFrame(frames, frame);
            if (frames.size() - beforeByteCount > byteBudget - usedByteCount)
            {
                frames.resize(beforeByteCount);
                return false;
            }
            usedByteCount += frames.size() - beforeByteCount;
            return true;
        }

        /**
         * @brief 一条 STREAM 帧除载荷以外的字节数
         * @details 与 `appendQuicFrame` 的写法严格对应：类型 1 字节、流号与偏移各一个变长整数、
         *          偏移为 0 时省掉偏移域（RFC 9000 §19.8）。长度域另算，因为它的宽度取决于载荷多长。
         * @param streamId 流号
         * @param offset 本段起始偏移
         * @return std::size_t 帧头字节数
         */
        std::size_t streamFrameHeaderByteLength(const std::uint64_t streamId, const std::uint64_t offset)
        {
            const std::size_t offsetByteLength = offset == 0 ? 0 : quicVariableLengthIntegerByteCount(offset);
            return kQuicStreamFrameTypeByteLength + quicVariableLengthIntegerByteCount(streamId) + offsetByteLength;
        }

        /**
         * @brief 按「消费掉多少才抬窗口」的规则抬一档额度
         * @details §2.2 建议留够一半再抬：每读一次就发一条更新帧，会把包预算全喂给控制帧。
         *          抬到「消费点 + 初始窗口」，因此对端可用的窗口始终不超过初始值。
         * @param consumedBytes 本层已消化掉的字节数
         * @param initialWindowBytes 本端最初宣告的窗口
         * @param limit 已宣告的上限，就地抬高
         * @param isUpdatePending 就地置真，表示欠对端一帧
         */
        void raiseWindowAfterConsumption(const std::uint64_t consumedBytes, const std::uint64_t initialWindowBytes,
                                         std::uint64_t &limit, bool &isUpdatePending)
        {
            const std::uint64_t remaining = limit > consumedBytes ? limit - consumedBytes : 0;
            if (remaining > initialWindowBytes / 2)
            {
                return;
            }
            const std::uint64_t refreshed = consumedBytes + initialWindowBytes;
            if (refreshed <= limit)
            {
                // 初始窗口为 0：本就不打算收第二口字节，抬不起来也不报错
                return;
            }
            limit = refreshed;
            isUpdatePending = true;
        }
    } // namespace

    QuicStreamLayer::QuicStreamLayer(const QuicTransportParameters &localParameters)
        : m_localParameters(localParameters),
          m_connectionAdvertisedLimit(localParameters.initialMaximumData),
          m_advertisedBidirectionalStreams(localParameters.initialMaximumBidirectionalStreams),
          m_advertisedUnidirectionalStreams(localParameters.initialMaximumUnidirectionalStreams)
    {
    }

    void QuicStreamLayer::adoptPeerParameters(const QuicTransportParameters &peerParameters)
    {
        m_peerParameters = peerParameters;
        m_hasPeerParameters = true;
        m_connectionSendLimit = peerParameters.initialMaximumData;
        m_outgoingBidirectionalLimit = peerParameters.initialMaximumBidirectionalStreams;
        m_outgoingUnidirectionalLimit = peerParameters.initialMaximumUnidirectionalStreams;
        // 参数到手之前写进来的数据都还没有额度，这里统一按类别补上
        for (auto &[streamId, stream] : m_outgoing)
        {
            if (const std::optional<std::uint64_t> window = initialSendWindowFor(streamId); window.has_value())
            {
                stream.streamLimit = *window;
            }
        }
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onStreamFrame(const QuicStreamFrame &frame)
    {
        const std::uint64_t streamId = frame.streamId;
        const std::uint64_t endOffset = frame.offset + frame.data.size();
        auto incoming = m_incoming.find(streamId);
        if (incoming == m_incoming.end())
        {
            if (isPeerStreamSideRetired(streamId, true))
            {
                // 这条流早就收口作废了：来的是迟到的重传或重复段，按已收齐处理。这里若建一份新记录，
                // 那份新额度会把对端合法的重传判成越界（§4.5）
                return {};
            }
            if (isLocallyInitiated(streamId))
            {
                if (isUnidirectional(streamId))
                {
                    // 本端发起的单向流上对端没有发言权（§2.1）
                    return std::unexpected(makeViolation(kProtocolViolation,
                                                         std::format("对端在本端发起的单向流 {} 上发了数据（RFC 9000 §2.1）", streamId)));
                }
                if (m_outgoing.find(streamId) == m_outgoing.end())
                {
                    // 本端发起的双向流可以回数据，但前提是这条流确实是本端开出来的（§3）
                    return std::unexpected(makeViolation(kProtocolViolation,
                                                         std::format("对端在本端还没发起的双向流 {} 上回了数据（RFC 9000 §3）", streamId)));
                }
            }
            else if (const std::uint64_t index = streamIndexOfType(streamId);
                     index >= (isUnidirectional(streamId) ? m_advertisedUnidirectionalStreams
                                                          : m_advertisedBidirectionalStreams))
            {
                // 超出本端宣告的流数上限：0x04（§4.6）
                return std::unexpected(makeViolation(
                        kStreamLimitError,
                        std::format("对端发起了第 {} 条{}向流，超出本端宣告的上限 {}（RFC 9000 §4.6）", index,
                                    isUnidirectional(streamId) ? "单" : "双",
                                    isUnidirectional(streamId) ? m_advertisedUnidirectionalStreams
                                                               : m_advertisedBidirectionalStreams)));
            }
            IncomingStream &fresh = m_incoming[streamId];
            fresh.streamLimit = incomingInitialWindowOf(streamId);
            if (isUnidirectional(streamId))
            {
                m_incomingUnidirectionalCount = std::max(m_incomingUnidirectionalCount, streamIndexOfType(streamId) + 1);
            }
            else
            {
                m_incomingBidirectionalCount = std::max(m_incomingBidirectionalCount, streamIndexOfType(streamId) + 1);
            }
            // 新建流本身就该触发一次上限检查：应用还没消费时也得把额度续上，否则对端开不出下一条流
            raiseAdvertisedStreamLimits();
            incoming = m_incoming.find(streamId);
        }
        IncomingStream &stream = incoming->second;
        if (stream.isReset)
        {
            // 复位之后的迟到数据：本端已经不再读这条流，丢掉即可（§4.5）
            return {};
        }

        // 额度检查排在记账之前：连接级看的是各流最大结束偏移之和（§4.1）
        if (endOffset > stream.streamLimit)
        {
            return std::unexpected(makeViolation(kFlowControlError,
                                                 std::format("流 {} 的偏移到了 {}，超出本端宣告的接收上限 {}（RFC 9000 §4.5）", streamId,
                                                             endOffset, stream.streamLimit)));
        }
        if (endOffset > stream.receivedHighWaterOffset &&
            m_connectionReceivedBytes + (endOffset - stream.receivedHighWaterOffset) > m_connectionAdvertisedLimit)
        {
            return std::unexpected(makeViolation(kFlowControlError,
                                                 std::format("各流累计偏移将超过连接级上限 {}（RFC 9000 §4.1）",
                                                             m_connectionAdvertisedLimit)));
        }
        if (stream.finalOffset.has_value() && endOffset > *stream.finalOffset)
        {
            return std::unexpected(makeViolation(kFinalSizeError,
                                                 std::format("流 {} 已经收尾在 {}，又收到越过该长度的数据（RFC 9000 §4.5）", streamId,
                                                             *stream.finalOffset)));
        }
        if (frame.isFinal)
        {
            if (stream.finalOffset.has_value() && *stream.finalOffset != endOffset)
            {
                return std::unexpected(makeViolation(kFinalSizeError,
                                                     std::format("流 {} 两次收尾长度不一致：{} 与 {}（RFC 9000 §4.5）", streamId,
                                                                 *stream.finalOffset, endOffset)));
            }
            stream.finalOffset = endOffset;
        }
        if (stream.isFinished)
        {
            return {};
        }

        if (endOffset > stream.receivedHighWaterOffset)
        {
            m_connectionReceivedBytes += endOffset - stream.receivedHighWaterOffset;
            stream.receivedHighWaterOffset = endOffset;
        }

        // 重复与重叠的分片由缓存按覆盖区合并，头部重叠不必在这里先剪（§7.5）；
        // 缓存也不必另设上限：能进来的偏移都已经在流级与连接级额度之内
        stream.reassembly.insert(frame.offset, frame.data);
        drainContiguousBytes(stream, streamId);
        if (stream.finalOffset.has_value() && stream.reassembly.deliveredOffset() == *stream.finalOffset)
        {
            stream.isFinished = true;
            // 收齐了对端的字节：再请它停发就是多余的一帧（§3.5）
            stream.receiveStop.reset();
        }
        return {};
    }

    void QuicStreamLayer::drainContiguousBytes(IncomingStream &stream, const std::uint64_t streamId)
    {
        std::vector<std::uint8_t> payload;
        stream.reassembly.drain(payload);
        const bool isFinal = stream.finalOffset.has_value() && stream.reassembly.deliveredOffset() == *stream.finalOffset;
        if (payload.empty() && !(isFinal && !stream.isFinalDelivered))
        {
            // 空的一段不通知；但纯 FIN 例外——上层靠它收尾，只看字节数会漏掉零长结尾
            return;
        }
        if (isFinal)
        {
            stream.isFinalDelivered = true;
        }
        m_deliveries.pushBack(QuicStreamDelivery{streamId, std::move(payload), isFinal});
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onMaxDataFrame(const QuicMaxDataFrame &frame)
    {
        // 只抬高不回落：§4.1 明确「更小的上限没有效果」，也不算违规
        m_connectionSendLimit = std::max(m_connectionSendLimit, frame.maximumData);
        return {};
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onMaxStreamDataFrame(const QuicMaxStreamDataFrame &frame)
    {
        if (isUnidirectional(frame.streamId) && !isLocallyInitiated(frame.streamId))
        {
            // 对端发起的单向流本端只能收，给它抬发送额度是状态冲突（§4.6）
            return std::unexpected(makeViolation(kStreamStateError,
                                                 std::format("对端在本端无权发送的单向流 {} 上发了 MAX_STREAM_DATA（RFC 9000 §4.6）",
                                                             frame.streamId)));
        }
        auto stream = m_outgoing.find(frame.streamId);
        if (stream == m_outgoing.end())
        {
            if (isPeerStreamSideRetired(frame.streamId, false))
            {
                // 这条流的发送侧早已收口确认：额度抬给谁都没用了，别为它凭空建一份状态
                return {};
            }
            // 本端还没发起就收到额度：留着这个上限等着，等本端真开流时直接用（§4.6 允许）
            stream = m_outgoing.emplace(frame.streamId, OutgoingStream{}).first;
            stream->second.streamLimit = initialSendWindowFor(frame.streamId).value_or(0);
        }
        stream->second.streamLimit = std::max(stream->second.streamLimit, frame.maximumStreamData);
        return {};
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onMaxStreamsFrame(const QuicMaxStreamsFrame &frame)
    {
        std::uint64_t &target = frame.isUnidirectional ? m_outgoingUnidirectionalLimit : m_outgoingBidirectionalLimit;
        target = std::max(target, frame.maximumStreams);
        return {};
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onResetStreamFrame(const QuicResetStreamFrame &frame)
    {
        if (isUnidirectional(frame.streamId) && isLocallyInitiated(frame.streamId))
        {
            // 本端发起的单向流由本端发送，对端没有可复位的东西（§4.5）
            return std::unexpected(makeViolation(kStreamStateError,
                                                 std::format("对端复位了本端发起的单向流 {}（RFC 9000 §4.5）", frame.streamId)));
        }
        auto incoming = m_incoming.find(frame.streamId);
        if (incoming == m_incoming.end())
        {
            // 本端还不认识这条流：没有记账要作废，忽略即可（复位只影响发送方的额度）
            return {};
        }
        IncomingStream &stream = incoming->second;
        if (stream.finalOffset.has_value() && *stream.finalOffset != frame.finalSize)
        {
            return std::unexpected(makeViolation(kFinalSizeError,
                                                 std::format("流 {} 的 RESET_STREAM 收尾长度是 {}，与已知的 {} 不符（RFC 9000 §4.5）",
                                                             frame.streamId, frame.finalSize, *stream.finalOffset)));
        }
        const std::uint64_t deliveredOffset = stream.reassembly.deliveredOffset();
        if (frame.finalSize < deliveredOffset)
        {
            return std::unexpected(makeViolation(kFinalSizeError,
                                                 std::format("流 {} 的 RESET_STREAM 收尾长度 {} 小于已交付的 {}", frame.streamId,
                                                             frame.finalSize, deliveredOffset)));
        }
        stream.finalOffset = frame.finalSize;
        // 只有第一次复位要结算作废量：缓存已在复位时清空，重复复位上再算会把交付点当成 0
        if (!stream.isReset)
        {
            // 缓存里剩下的那段再也不会交付，上层也就永远不会为它报回额度：记成「作废」，
            // 回收判据据此才认得出这条被复位的流已经结清
            stream.discardedByteCount = stream.receivedHighWaterOffset - deliveredOffset;
            // 乱序缓存作废，但 receivedHighWaterOffset 不动：复位不退还已经占掉的连接级额度（§4.1）
            stream.reassembly = QuicReassemblyBuffer{};
        }
        stream.isReset = true;
        // 对端既然已经复位，欠着的停发请求就没有必要再发了（§3.5）
        stream.receiveStop.reset();
        m_abortedStreams.pushBack(frame.streamId);
        return {};
    }

    std::expected<void, QuicStreamViolation> QuicStreamLayer::onStopSendingFrame(const QuicStopSendingFrame &frame)
    {
        if (isUnidirectional(frame.streamId) && !isLocallyInitiated(frame.streamId))
        {
            // 本端在这条流上本来就不发东西，叫停一个不存在的发送方是状态冲突（§4.5）
            return std::unexpected(makeViolation(kStreamStateError,
                                                 std::format("对端让本端停止发送本端只能收的单向流 {}（RFC 9000 §4.5）", frame.streamId)));
        }
        if (isPeerStreamSideRetired(frame.streamId, false))
        {
            // 本端已经把这条流发完并得到确认：再补一份状态就会凭空回一条收尾长度为 0 的 RESET_STREAM，
            // 把已经收齐的对端判成越界
            return {};
        }
        OutgoingStream &stream = outgoingStream(frame.streamId);
        if (stream.isAborted)
        {
            return {};
        }
        // 待发队列作废：本端既然被叫停，就不该再把字节往这条流上排
        stream.pendingQueue.clear();
        stream.isAborted = true;
        // §3.5：收到停发请求而本端还没发完，就必须回一条 RESET_STREAM，错误码照抄过去。FIN 已经
        // 上线的除外——那种情况对端迟早收齐，补复位反而多余
        if (!stream.isFinalSentToPeer && !stream.sendAbort.has_value())
        {
            stream.sendAbort = AbortAnnouncement{frame.applicationErrorCode, stream.sentHighWater, false, false};
            stream.finalOffset = stream.sentHighWater;
            stream.inFlight.clear();
        }
        m_abortedStreams.pushBack(frame.streamId);
        return {};
    }

    std::size_t QuicStreamLayer::writeStreamData(const std::uint64_t streamId, const std::span<const std::uint8_t> bytes,
                                                 const bool isFinal)
    {
        if (isLocallyInitiated(streamId) &&
            streamIndexOfType(streamId) >= (isUnidirectional(streamId) ? m_outgoingUnidirectionalLimit
                                                                       : m_outgoingBidirectionalLimit))
        {
            // 超出对端给的流数上限：这条流开不出来，一字节也不收（§4.6）
            return 0;
        }
        if (isPeerStreamSideRetired(streamId, false))
        {
            // 这条对端流的两侧都已收口：本端却还想往上写，说明上层留着了一条已经作废的流。收下只会
            // 凭空建一份状态，把已经确认收尾的流又开出正文来
            return 0;
        }
        OutgoingStream &stream = outgoingStream(streamId);
        if (stream.isAborted || stream.finalOffset.has_value())
        {
            return 0;
        }
        if (bytes.empty() && !isFinal)
        {
            // 空写又没有 FIN，编出去就是一条零长 STREAM 帧：白占包预算，也不推进任何状态
            return 0;
        }
        // 上界先行：排空只发生在编帧那一刻，而编帧要等对端给窗口，所以对端不授窗口时这里就是唯一的
        // 闸口。收不下的部分原样退回给调用方留住——本层不替它存副本，否则同一段字节占两份内存
        const std::size_t streamRoomByteCount = saturatingRoomOf(kMaximumPendingSendByteCount, pendingQueueByteCount(stream));
        // 连接级同上一道闸：单流上界乘以流数仍是一条与流数同增的账，逐流各卡一点就能绕过它
        const std::size_t connectionRoomByteCount =
                saturatingRoomOf(kMaximumConnectionPendingSendByteCount, totalPendingSendByteCount());
        const std::size_t roomByteCount = std::min(streamRoomByteCount, connectionRoomByteCount);
        const std::size_t acceptedByteCount = std::min(bytes.size(), roomByteCount);
        if (acceptedByteCount == 0 && !bytes.empty())
        {
            // 队列已满。零长的收尾写不吃地方，仍然照收——否则这条流永远收不了口
            return 0;
        }
        const bool isWholeSegmentAccepted = acceptedByteCount == bytes.size();

        QuicStreamChunk chunk;
        chunk.beginOffset = stream.nextWriteOffset;
        chunk.bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(acceptedByteCount));
        // FIN 只随整段收下一起落定：半段就收尾等于把正文截断了还告诉对端「发完了」
        chunk.isFinal = isWholeSegmentAccepted && isFinal;
        stream.nextWriteOffset += acceptedByteCount;
        if (chunk.isFinal)
        {
            stream.finalOffset = stream.nextWriteOffset;
        }
        stream.pendingQueue.push_back(std::move(chunk));
        return acceptedByteCount;
    }

    std::size_t QuicStreamLayer::pendingSendByteCount(const std::uint64_t streamId) const noexcept
    {
        const auto stream = m_outgoing.find(streamId);
        return stream == m_outgoing.end() ? 0 : pendingQueueByteCount(stream->second);
    }

    std::size_t QuicStreamLayer::totalPendingSendByteCount() const noexcept
    {
        // 逐条流现算：编帧本来就要走一遍 m_outgoing，这条闸不比它更贵，也不值得另记一本账
        std::size_t totalByteCount = 0;
        for (const auto &[streamId, stream]: m_outgoing)
        {
            static_cast<void>(streamId);
            totalByteCount += pendingQueueByteCount(stream);
        }
        return totalByteCount;
    }

    std::size_t QuicStreamLayer::takeDrainedSendByteCount() noexcept
    {
        return std::exchange(m_drainedSendByteCount, 0);
    }

    std::size_t QuicStreamLayer::pendingQueueByteCount(const OutgoingStream &stream) noexcept
    {
        // 逐段相加而不另记一本账：队列本就短（上界封顶、每次编帧都在掏），多一本账就多一处漏记
        std::size_t totalByteCount = 0;
        for (const QuicStreamChunk &chunk: stream.pendingQueue)
        {
            totalByteCount += chunk.bytes.size();
        }
        return totalByteCount;
    }

    void QuicStreamLayer::resetStreamSending(const std::uint64_t streamId, const std::uint64_t applicationErrorCode)
    {
        if (isPeerStreamSideRetired(streamId, false))
        {
            // 发送侧早已收口确认：这条流没什么可复位的，建一份状态反而会被编成收尾长度为 0 的 RESET_STREAM
            return;
        }
        OutgoingStream &stream = outgoingStream(streamId);
        if (stream.sendAbort.has_value() || stream.isFinalSentToPeer || stream.finalOffset.has_value())
        {
            // 已经放弃过：RESET 的内容一旦上线就不许改（§13.3）。FIN 已上线、或上层已经把收尾长度
            // 定下来（排在队列里还没出去）的都不必再复位：那种情况对端迟早收齐，抢一份 RESET 反而
            // 会把已经交出去的收尾字节作废
            return;
        }
        // 收尾长度取「曾上线的最大结束偏移」而不是上层写过的字节数：没上过线的偏移对端没见过，
        // 报上去会被判越界（§4.5）
        const std::uint64_t finalSize = stream.sentHighWater;
        stream.sendAbort = AbortAnnouncement{applicationErrorCode, finalSize, false, false};
        stream.finalOffset = finalSize; // 从此拒绝再写这条流
        stream.pendingQueue.clear();
        // 在途账一并丢掉：这些包若判丢，不该再重发数据，改由 RESET_STREAM 交代收尾（§3.5）
        stream.inFlight.clear();
    }

    void QuicStreamLayer::stopStreamReceiving(const std::uint64_t streamId, const std::uint64_t applicationErrorCode)
    {
        auto incoming = m_incoming.find(streamId);
        if (incoming == m_incoming.end())
        {
            // 本端没有这条流的入站记账：要么它还不存在，要么是本端只能发的那一档，两种都没什么可停的
            return;
        }
        IncomingStream &stream = incoming->second;
        if (stream.isReset || stream.receiveStop.has_value())
        {
            // 对端已复位（或本端已请过）：停发只对还在收的流有意义（§3.5）
            return;
        }
        stream.receiveStop = AbortAnnouncement{applicationErrorCode, 0, false, false};
    }

    std::optional<std::uint64_t> QuicStreamLayer::openUnidirectionalStream()
    {
        const std::uint64_t streamId = m_nextUnidirectionalStreamId;
        if (streamIndexOfType(streamId) >= m_outgoingUnidirectionalLimit)
        {
            return std::nullopt;
        }
        m_nextUnidirectionalStreamId += kStreamIdStride;
        // 条目当场建好：这条流的额度与后续写入都记在它身上
        static_cast<void>(outgoingStream(streamId));
        return streamId;
    }

    bool QuicStreamLayer::collectFrames(std::string &frames, const std::size_t byteBudget,
                                        std::vector<QuicStreamRange> &sentRanges,
                                        std::vector<QuicStreamAnnouncement> &announcements)
    {
        // 每次编帧前先摘一轮已作废的记录：下面几个收集环节都要过这两张表，留着只会让它们越扫越长
        retireSettledStreams();
        // 收口宣告最先编：它不占流量控制额度，却是「对端还要不要等下去」的答案
        const std::size_t announcementByteCount = collectAbortAnnouncements(frames, byteBudget, announcements);
        const std::size_t remainingAfterAnnouncements =
                byteBudget > announcementByteCount ? byteBudget - announcementByteCount : 0;
        const std::size_t windowUpdateByteCount = collectWindowUpdates(frames, remainingAfterAnnouncements);
        // 数据帧只能花窗口更新剩下的那一截：各算各的预算会让这一包超出调用方给的上限
        const std::size_t remainingBudget =
                remainingAfterAnnouncements > windowUpdateByteCount ? remainingAfterAnnouncements - windowUpdateByteCount : 0;
        return announcementByteCount + windowUpdateByteCount +
                       collectStreamData(frames, remainingBudget, sentRanges) >
               0;
    }

    std::size_t QuicStreamLayer::collectAbortAnnouncements(
            std::string &frames, const std::size_t byteBudget, std::vector<QuicStreamAnnouncement> &announcements)
    {
        std::size_t usedByteCount = 0;
        for (auto &[streamId, stream] : m_outgoing)
        {
            const std::optional<AbortAnnouncement> &abort = stream.sendAbort;
            if (!abort.has_value() || abort->isInFlight || abort->isAcknowledged)
            {
                continue;
            }
            QuicResetStreamFrame reset;
            reset.streamId = streamId;
            reset.applicationErrorCode = abort->applicationErrorCode;
            reset.finalSize = abort->finalSize;
            if (!tryAppendFrame(frames, byteBudget, usedByteCount, QuicFrame{reset}))
            {
                continue; // 预算不够：这一包不带，下一包再补同一份内容
            }
            stream.sendAbort->isInFlight = true;
            announcements.push_back(QuicStreamAnnouncement{streamId, true});
        }
        for (auto &[streamId, stream] : m_incoming)
        {
            const std::optional<AbortAnnouncement> &stop = stream.receiveStop;
            if (!stop.has_value() || stop->isInFlight || stop->isAcknowledged)
            {
                continue;
            }
            QuicStopSendingFrame stopSending;
            stopSending.streamId = streamId;
            stopSending.applicationErrorCode = stop->applicationErrorCode;
            if (!tryAppendFrame(frames, byteBudget, usedByteCount, QuicFrame{stopSending}))
            {
                continue;
            }
            stream.receiveStop->isInFlight = true;
            announcements.push_back(QuicStreamAnnouncement{streamId, false});
        }
        return usedByteCount;
    }

    std::size_t QuicStreamLayer::collectWindowUpdates(std::string &frames, const std::size_t byteBudget)
    {
        std::size_t usedByteCount = 0;
        // 编码后量一次真实长度，装不下就退回原样：控制帧的宽度取决于变长整数档位，算不如量
        const auto tryAppend = [&](const QuicFrame &frame, bool &isPending)
        {
            if (tryAppendFrame(frames, byteBudget, usedByteCount, frame))
            {
                isPending = false;
            }
        };

        if (m_connectionWindowUpdatePending)
        {
            QuicMaxDataFrame maxData;
            maxData.maximumData = m_connectionAdvertisedLimit;
            tryAppend(QuicFrame{maxData}, m_connectionWindowUpdatePending);
        }
        if (m_streamsBidirectionalUpdatePending)
        {
            QuicMaxStreamsFrame maxStreams;
            maxStreams.isUnidirectional = false;
            maxStreams.maximumStreams = m_advertisedBidirectionalStreams;
            tryAppend(QuicFrame{maxStreams}, m_streamsBidirectionalUpdatePending);
        }
        if (m_streamsUnidirectionalUpdatePending)
        {
            QuicMaxStreamsFrame maxStreams;
            maxStreams.isUnidirectional = true;
            maxStreams.maximumStreams = m_advertisedUnidirectionalStreams;
            tryAppend(QuicFrame{maxStreams}, m_streamsUnidirectionalUpdatePending);
        }
        if (usedByteCount == byteBudget)
        {
            return usedByteCount;
        }
        for (auto &[streamId, stream] : m_incoming)
        {
            if (!stream.windowUpdatePending)
            {
                continue;
            }
            QuicMaxStreamDataFrame maxStreamData;
            maxStreamData.streamId = streamId;
            maxStreamData.maximumStreamData = stream.streamLimit;
            tryAppend(QuicFrame{maxStreamData}, stream.windowUpdatePending);
            if (usedByteCount == byteBudget)
            {
                break;
            }
        }
        return usedByteCount;
    }

    std::size_t QuicStreamLayer::collectStreamData(std::string &frames, const std::size_t byteBudget,
                                                   std::vector<QuicStreamRange> &sentRanges)
    {
        if (!m_hasPeerParameters)
        {
            // 对端参数还没到，连接级与流级额度都是 0，一字节也不该发（§4.1）
            return 0;
        }
        std::size_t usedByteCount = 0;
        for (auto &[streamId, stream] : m_outgoing)
        {
            if (stream.isAborted)
            {
                continue;
            }
            while (!stream.pendingQueue.empty())
            {
                const std::size_t remainingBudget = byteBudget > usedByteCount ? byteBudget - usedByteCount : 0;
                const QuicStreamChunk &front = stream.pendingQueue.front();
                const std::size_t headerByteLength = streamFrameHeaderByteLength(streamId, front.beginOffset);
                const std::size_t creditByteCount = sendCreditOf(stream);
                if (creditByteCount == 0)
                {
                    // 这条流的额度见底，换下一条：额度是分流的，后面的流可能还很宽裕
                    break;
                }
                if (remainingBudget <= headerByteLength + 1)
                {
                    // 连「帧头 + 最窄长度域」都装不下，后面的流同样装不下
                    return usedByteCount;
                }
                // 长度域的宽度取决于载荷多长，而载荷多长又取决于留出多少长度域：先按最窄档算，
                // 算出来的值跨档就按宽一档重算一次。第二次一定收敛（载荷只会变小）
                std::size_t payloadByteLength = std::min({front.bytes.size(), creditByteCount,
                                                          remainingBudget - headerByteLength - 1});
                if (const std::size_t widenedByteCount = quicVariableLengthIntegerByteCount(
                        static_cast<std::uint64_t>(payloadByteLength)); widenedByteCount > 1)
                {
                    if (remainingBudget <= headerByteLength + widenedByteCount)
                    {
                        return usedByteCount;
                    }
                    payloadByteLength = std::min({front.bytes.size(), creditByteCount,
                                                  remainingBudget - headerByteLength - widenedByteCount});
                }
                if (payloadByteLength == 0 && !front.isFinal)
                {
                    return usedByteCount;
                }

                const bool carriesFinal = front.isFinal && payloadByteLength == front.bytes.size();
                QuicStreamFrame streamFrame;
                streamFrame.streamId = streamId;
                streamFrame.offset = front.beginOffset;
                streamFrame.data = std::span<const std::uint8_t>(front.bytes).subspan(0, payloadByteLength);
                streamFrame.isFinal = carriesFinal;

                const std::size_t beforeByteCount = frames.size();
                appendQuicFrame(frames, QuicFrame{streamFrame});
                usedByteCount += frames.size() - beforeByteCount;

                const std::uint64_t endOffset = front.beginOffset + payloadByteLength;
                if (endOffset > stream.sentHighWater)
                {
                    m_connectionSentHighWater += endOffset - stream.sentHighWater;
                    stream.sentHighWater = endOffset;
                }
                sentRanges.push_back(QuicStreamRange{streamId, front.beginOffset, endOffset, carriesFinal});
                if (carriesFinal)
                {
                    // 记一笔「FIN 已上线」：本端发送侧就此收口，之后不必也不该再发 RESET_STREAM（§3.5）
                    stream.isFinalSentToPeer = true;
                }
                stream.inFlight[front.beginOffset] = QuicStreamChunk{
                        front.beginOffset,
                        std::vector<std::uint8_t>(front.bytes.begin(), front.bytes.begin() + static_cast<std::ptrdiff_t>(payloadByteLength)),
                        carriesFinal};

                if (payloadByteLength == front.bytes.size())
                {
                    stream.pendingQueue.pop_front();
                }
                else
                {
                    // 只排出去一段：剩下的仍留在队首，偏移跟着后移，下一段续在同一处
                    QuicStreamChunk &mutableFront = stream.pendingQueue.front();
                    mutableFront.bytes.erase(mutableFront.bytes.begin(),
                                             mutableFront.bytes.begin() + static_cast<std::ptrdiff_t>(payloadByteLength));
                    mutableFront.beginOffset += payloadByteLength;
                }
                // 记一笔「队列又空出这么多」：上层因本层到界而留下的那段字节靠这个数续交
                m_drainedSendByteCount += payloadByteLength;
            }
        }
        return usedByteCount;
    }

    bool QuicStreamLayer::hasOutgoingFrames() const noexcept
    {
        // 在途或已确认的宣告都不算「还欠着」：否则这一条会把出包循环永远吊住
        const auto owesAnnouncement = [](const std::optional<AbortAnnouncement> &announcement)
        { return announcement.has_value() && !announcement->isInFlight && !announcement->isAcknowledged; };
        return m_connectionWindowUpdatePending || m_streamsBidirectionalUpdatePending ||
               m_streamsUnidirectionalUpdatePending ||
               std::ranges::any_of(m_incoming, [](const auto &entry) { return entry.second.windowUpdatePending; }) ||
               std::ranges::any_of(m_incoming,
                                   [&](const auto &entry) { return owesAnnouncement(entry.second.receiveStop); }) ||
               std::ranges::any_of(m_outgoing,
                                   [&](const auto &entry) { return owesAnnouncement(entry.second.sendAbort); }) ||
               std::ranges::any_of(m_outgoing, [](const auto &entry)
               { return !entry.second.isAborted && !entry.second.pendingQueue.empty(); });
    }

    void QuicStreamLayer::releaseReceiveWindow(const std::uint64_t streamId, const std::size_t consumedByteCount)
    {
        if (consumedByteCount == 0)
        {
            return;
        }
        auto incoming = m_incoming.find(streamId);
        if (incoming == m_incoming.end())
        {
            return;
        }
        IncomingStream &stream = incoming->second;
        stream.consumedByteCount += consumedByteCount;
        m_connectionConsumedBytes += consumedByteCount;
        raiseWindowAfterConsumption(stream.consumedByteCount, incomingInitialWindowOf(streamId), stream.streamLimit,
                                    stream.windowUpdatePending);
        raiseWindowAfterConsumption(m_connectionConsumedBytes, m_localParameters.initialMaximumData,
                                    m_connectionAdvertisedLimit, m_connectionWindowUpdatePending);
        raiseAdvertisedStreamLimits();
    }

    void QuicStreamLayer::raiseAdvertisedStreamLimits()
    {
        // 对端用掉一半已宣告的流数就把上限抬高，否则它会一直卡在 0x04 上（§4.6）
        const auto raise = [](const std::uint64_t usedCount, const std::uint64_t initialStreams, std::uint64_t &limit,
                              bool &isUpdatePending)
        {
            if (limit > usedCount && limit - usedCount > initialStreams / 2)
            {
                return;
            }
            const std::uint64_t refreshed = usedCount + initialStreams;
            if (refreshed <= limit)
            {
                return;
            }
            limit = refreshed;
            isUpdatePending = true;
        };
        raise(m_incomingBidirectionalCount, m_localParameters.initialMaximumBidirectionalStreams,
              m_advertisedBidirectionalStreams, m_streamsBidirectionalUpdatePending);
        raise(m_incomingUnidirectionalCount, m_localParameters.initialMaximumUnidirectionalStreams,
              m_advertisedUnidirectionalStreams, m_streamsUnidirectionalUpdatePending);
    }

    bool QuicStreamLayer::hasDeliveries() const noexcept
    {
        return !m_deliveries.empty();
    }

    std::optional<QuicStreamDelivery> QuicStreamLayer::takeDelivery()
    {
        return m_deliveries.takeFront();
    }

    bool QuicStreamLayer::hasAbortedStreams() const noexcept
    {
        return !m_abortedStreams.empty();
    }

    std::optional<std::uint64_t> QuicStreamLayer::takeAbortedStream()
    {
        return m_abortedStreams.takeFront();
    }

    std::size_t QuicStreamLayer::trackedStreamCount() const noexcept
    {
        return m_incoming.size() + m_outgoing.size();
    }

    void QuicStreamLayer::retireSettledStreams()
    {
        // 只摘对端发起的那两档：本端发起的流由上层自己掌握寿命，且它们的数量与连接寿命无关（控制流、
        // QPACK 流各一条），留着的账太小，不值得为它们再记一份「哪些流号已作废」
        for (auto entry = m_incoming.begin(); entry != m_incoming.end();)
        {
            if (isLocallyInitiated(entry->first) || !isIncomingSettled(entry->second))
            {
                ++entry;
                continue;
            }
            // 先抬边界再摘条目：同类型的流号只增不减（§2.1），边界之下的号都是「曾经有过、如今已作废」的，
            // 反过来做会让这一拍之后到达的迟到帧被当成一条新流
            noteStreamRetired(entry->first, true);
            entry = m_incoming.erase(entry);
        }
        for (auto entry = m_outgoing.begin(); entry != m_outgoing.end();)
        {
            if (isLocallyInitiated(entry->first) || !isOutgoingSettled(entry->second))
            {
                ++entry;
                continue;
            }
            noteStreamRetired(entry->first, false);
            entry = m_outgoing.erase(entry);
        }
    }

    bool QuicStreamLayer::isIncomingSettled(const IncomingStream &stream) const noexcept
    {
        // 接收侧的终局只有两条：FIN 收齐并交付完，或对端复位。乱序缓存已被复位清掉，之后不会再有交付
        if (stream.isFinished && !stream.isFinalDelivered)
        {
            return false;
        }
        if (!stream.isFinished && !stream.isReset)
        {
            return false;
        }
        // 上层报回来的加上作废掉的，要凑齐本层记过的每一个字节：还欠着就不能摘，否则
        // releaseReceiveWindow 找不到条目会把这份额度**丢掉**，连接级窗口就此不再前进，
        // 对端永远等不到 MAX_DATA
        if (stream.consumedByteCount + stream.discardedByteCount != stream.receivedHighWaterOffset)
        {
            return false;
        }
        // 欠对端的帧一条都不能欠：摘了就没人在 collectFrames 里把它编出去了
        return !stream.windowUpdatePending && (!stream.receiveStop.has_value() || stream.receiveStop->isAcknowledged);
    }

    bool QuicStreamLayer::isOutgoingSettled(const OutgoingStream &stream) const noexcept
    {
        if (!stream.pendingQueue.empty() || !stream.inFlight.empty())
        {
            return false;
        }
        // 还欠对端一条 RESET_STREAM：摘了记录，collectAbortAnnouncements 就找不到它要重发的那位
        if (stream.sendAbort.has_value() && !stream.sendAbort->isAcknowledged)
        {
            return false;
        }
        // 两条收尾之路：带 FIN 的那段已上线且没被判丢（判丢会把它退回未收尾），或本端已放弃发送且
        // 那份 RESET_STREAM 落了定
        return stream.isFinalSentToPeer || stream.sendAbort.has_value();
    }

    void QuicStreamLayer::noteStreamRetired(const std::uint64_t streamId, const bool isReceiveSide)
    {
        // 记的是「同类里的第几条」而不是流号本身：一个对端在同一档里只会往前走（§2.1 的流号单调），
        // 所以一个边界就够描述「这个号以下都可能已经作废」，不必留一张随连接时长增长的名单
        const std::size_t directionSlot = retiredBoundarySlotOf(streamId);
        std::uint64_t &boundary = m_retiredPeerStreamBoundaries[isReceiveSide ? 0U : 1U][directionSlot];
        boundary = std::max(boundary, streamIndexOfType(streamId) + 1);
    }

    bool QuicStreamLayer::isPeerStreamSideRetired(const std::uint64_t streamId, const bool isReceiveSide) const noexcept
    {
        if (isLocallyInitiated(streamId))
        {
            return false;
        }
        return streamIndexOfType(streamId) < m_retiredPeerStreamBoundaries[isReceiveSide ? 0U : 1U][retiredBoundarySlotOf(streamId)];
    }

    void QuicStreamLayer::onSendRangesAcknowledged(const std::vector<QuicStreamRange> &acknowledgedRanges)
    {
        for (const QuicStreamRange &range : acknowledgedRanges)
        {
            auto stream = m_outgoing.find(range.streamId);
            if (stream == m_outgoing.end())
            {
                continue;
            }
            stream->second.inFlight.erase(range.beginOffset);
        }
    }

    void QuicStreamLayer::onSendRangesLost(const std::vector<QuicStreamRange> &lostRanges)
    {
        for (const QuicStreamRange &range : lostRanges)
        {
            auto stream = m_outgoing.find(range.streamId);
            if (stream == m_outgoing.end())
            {
                continue;
            }
            auto inFlight = stream->second.inFlight.find(range.beginOffset);
            if (inFlight == stream->second.inFlight.end())
            {
                continue;
            }
            QuicStreamChunk lost = std::move(inFlight->second);
            stream->second.inFlight.erase(inFlight);
            if (range.isFinal)
            {
                // 带 FIN 的那段没了指望：发送侧还没收口，之后收到停发请求要按 §3.5 补一条复位
                stream->second.isFinalSentToPeer = false;
            }
            if (stream->second.isAborted)
            {
                // 已经被叫停：判丢的段不必再排回去
                continue;
            }
            lost.isFinal = range.isFinal;
            stream->second.pendingQueue.push_front(std::move(lost));
        }
        // 队首要按偏移递增；判丢的到达顺序不保证，重排一次比假设它有序稳妥
        for (auto &[streamId, stream] : m_outgoing)
        {
            std::ranges::sort(stream.pendingQueue, {}, &QuicStreamChunk::beginOffset);
        }
    }

    void QuicStreamLayer::onStreamAnnouncementsAcknowledged(const std::vector<QuicStreamAnnouncement> &acknowledgedAnnouncements)
    {
        for (const QuicStreamAnnouncement &announcement : acknowledgedAnnouncements)
        {
            AbortAnnouncement *target = abortAnnouncementOf(announcement);
            if (target == nullptr)
            {
                continue;
            }
            // 落定之后这一帧这辈子不再发第二遍（§13.3 只要「发到被确认为止」）
            target->isAcknowledged = true;
            target->isInFlight = false;
        }
    }

    void QuicStreamLayer::onStreamAnnouncementsLost(const std::vector<QuicStreamAnnouncement> &lostAnnouncements)
    {
        for (const QuicStreamAnnouncement &announcement : lostAnnouncements)
        {
            AbortAnnouncement *target = abortAnnouncementOf(announcement);
            if (target == nullptr || target->isAcknowledged)
            {
                // 已经有一份被确认过就不再补发：乱序里「先判丢后确认」的包不该复活一帧已落定的宣告
                continue;
            }
            target->isInFlight = false; // 下一包补发同一份内容（§13.3：内容不许变）
        }
    }

    QuicStreamLayer::AbortAnnouncement *QuicStreamLayer::abortAnnouncementOf(const QuicStreamAnnouncement &announcement)
    {
        if (announcement.isResetStream)
        {
            const auto outgoing = m_outgoing.find(announcement.streamId);
            if (outgoing != m_outgoing.end() && outgoing->second.sendAbort.has_value())
            {
                return std::addressof(*outgoing->second.sendAbort);
            }
            return nullptr;
        }
        const auto incoming = m_incoming.find(announcement.streamId);
        if (incoming != m_incoming.end() && incoming->second.receiveStop.has_value())
        {
            return std::addressof(*incoming->second.receiveStop);
        }
        return nullptr;
    }

    QuicStreamLayer::OutgoingStream &QuicStreamLayer::outgoingStream(const std::uint64_t streamId)
    {
        auto stream = m_outgoing.find(streamId);
        if (stream != m_outgoing.end())
        {
            return stream->second;
        }
        OutgoingStream &fresh = m_outgoing[streamId];
        fresh.streamLimit = initialSendWindowFor(streamId).value_or(0);
        return fresh;
    }

    std::optional<std::uint64_t> QuicStreamLayer::initialSendWindowFor(const std::uint64_t streamId) const noexcept
    {
        if (!m_hasPeerParameters)
        {
            return std::nullopt;
        }
        if (isUnidirectional(streamId))
        {
            // 单向流只有发起方会发数据：本端发起的（0x03）才对端参数里的 0x07 管得着
            return isLocallyInitiated(streamId) ? m_peerParameters.initialMaximumStreamDataUnidirectional
                                                : std::optional<std::uint64_t>{0};
        }
        // 对端参数里的 local/remote 是**它自己**的视角：本端发起的双向流在它那边算 remote（§18.2）
        return isLocallyInitiated(streamId) ? m_peerParameters.initialMaximumStreamDataBidirectionalRemote
                                            : m_peerParameters.initialMaximumStreamDataBidirectionalLocal;
    }

    std::uint64_t QuicStreamLayer::incomingInitialWindowOf(const std::uint64_t streamId) const noexcept
    {
        if (isUnidirectional(streamId))
        {
            return isLocallyInitiated(streamId) ? 0 : m_localParameters.initialMaximumStreamDataUnidirectional;
        }
        return isLocallyInitiated(streamId) ? m_localParameters.initialMaximumStreamDataBidirectionalLocal
                                            : m_localParameters.initialMaximumStreamDataBidirectionalRemote;
    }

    std::size_t QuicStreamLayer::sendCreditOf(const OutgoingStream &stream) const noexcept
    {
        const std::uint64_t streamRemaining = stream.streamLimit > stream.sentHighWater
                                                  ? stream.streamLimit - stream.sentHighWater
                                                  : 0;
        const std::uint64_t connectionRemaining = m_connectionSendLimit > m_connectionSentHighWater
                                                      ? m_connectionSendLimit - m_connectionSentHighWater
                                                      : 0;
        return static_cast<std::size_t>(std::min(streamRemaining, connectionRemaining));
    }
} // namespace AsynGyanis::Net
