/**
 * @file QuicStreamLayer.h
 * @brief QUIC 流层（RFC 9000 §2–§4.6、§19.4–§19.11）：流状态、连接级与流级流量控制、收发排队
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 纯计算件，与恢复层同构：入站帧交进来，出站帧由 `collectFrames` 编出来，不碰 socket
 *          也不碰事件循环。连接级额度管的是各流「最大偏移之和」（§4.1），流级额度只管单条流，
 *          两套各自记账；越界的判定归本层，传输错误码交给连接核心收口。
 *
 * @note 只有服务端视角：流号低位 0x00/0x02 是对端发起的，0x01/0x03 是本端发起的（§2.1）。
 * @warning 不是线程安全的：一条连接一份，只能在所属事件循环线程上驱动。
 */

#pragma once

#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/QuicReassemblyBuffer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /// 交给上层的一段流数据：已经按序、也已经拷走，上层可以安心持有
    struct QuicStreamDelivery
    {
        std::uint64_t streamId{0};         ///< 流号
        std::vector<std::uint8_t> bytes{}; ///< 数据本体（拷贝，不指向收包缓冲）
        bool isFinal{false};               ///< 对端在本段之后收尾；纯 FIN 时本段长度为 0
    };

    /**
     * @brief 流层报给连接核心的违规：带 §11.1 的传输错误码与可直接发出的中文文案
     * @details 本层不碰 socket，也就没法自己收口；核心拿到这个结构后调 beginClose。
     */
    struct QuicStreamViolation
    {
        std::uint64_t errorCode{0}; ///< FLOW_CONTROL_ERROR、STREAM_LIMIT_ERROR 等
        std::string reasonPhrase{}; ///< 进 CONNECTION_CLOSE 的原因文案
    };

    /**
     * @brief 一条连接上的全部流与流量控制额度
     *
     * @details 流按 §2 的「隐式创建」处理：收到一条合法的新流号即视为对端已发起该流，前提是不超出
     *          本端宣告的流数上限。上层消费多少，接收窗口才抬多少，所以 `releaseReceiveWindow` 是
     *          这条链上唯一的额度入口；不调它，对端迟早卡在 §4.1 的连接级上限上。
     */
    class QuicStreamLayer
    {
    public:
        /**
         * @brief 用本端宣告的参数建流层
         * @details 本端参数决定「愿意收多少」；「能发多少」要等对端参数到手，在那之前一条也不发
         * @param localParameters 本端传输参数，取 initial_max_data 与三个 initial_max_stream_data_*
         */
        explicit QuicStreamLayer(const QuicTransportParameters &localParameters);

        /**
         * @brief 对端参数到手：把发送侧的额度从 0 换成对端宣告的值
         * @param peerParameters 已验过的对端传输参数
         */
        void adoptPeerParameters(const QuicTransportParameters &peerParameters);

        /// @name 入站帧：返回错误即由核心按错误码收口
        /// @{
        [[nodiscard]] std::expected<void, QuicStreamViolation> onStreamFrame(const QuicStreamFrame &frame);
        [[nodiscard]] std::expected<void, QuicStreamViolation> onMaxDataFrame(const QuicMaxDataFrame &frame);
        [[nodiscard]] std::expected<void, QuicStreamViolation> onMaxStreamDataFrame(const QuicMaxStreamDataFrame &frame);
        [[nodiscard]] std::expected<void, QuicStreamViolation> onMaxStreamsFrame(const QuicMaxStreamsFrame &frame);
        [[nodiscard]] std::expected<void, QuicStreamViolation> onResetStreamFrame(const QuicResetStreamFrame &frame);
        [[nodiscard]] std::expected<void, QuicStreamViolation> onStopSendingFrame(const QuicStopSendingFrame &frame);
        ///@}

        /**
         * @brief 编出这一轮要发的帧：窗口更新在前，排队数据在后
         * @param frames 目标缓冲，追写到末尾；二进制安全
         * @param byteBudget 本包还能装多少字节；用完即止，剩下的下一包再来
         * @param sentRanges 排进数据帧的字节区间，核心要照它登记进发包凭据，好让确认与判丢能对上
         * @return true 至少编进了一帧
         */
        bool collectFrames(std::string &frames, std::size_t byteBudget, std::vector<QuicStreamRange> &sentRanges);

        /// @return true 还有数据或窗口更新等着发
        [[nodiscard]] bool hasOutgoingFrames() const noexcept;

        /**
         * @brief 上层要发出去的字节：先进本层队列，等额度与包预算允许才编帧
         * @param streamId 目标流号，必须是本端发起的流
         * @param bytes 数据本体，二进制安全；本层会拷进待发队列
         * @param isFinal 发完这段是否收尾
         * @return std::size_t 被接收的字节数；流已收尾、被打断或超出流数上限时是 0
         */
        std::size_t writeStreamData(std::uint64_t streamId, std::span<const std::uint8_t> bytes, bool isFinal);

        /**
         * @brief 开一条本端发起的单向流：0x03、0x07、0x0b……
         * @details 调用即占用，计数器立刻推进，因此连续两次不会给出同一个流号；条目也当场建好，
         *          后续 `writeStreamData` 直接落在它上面
         * @return std::optional<std::uint64_t> 对端宣告的单向流数已用满时返回空
         */
        [[nodiscard]] std::optional<std::uint64_t> openUnidirectionalStream();

        /**
         * @brief 上层消费掉数据后归还额度：流级与连接级一起抬
         * @param streamId 流号
         * @param consumedByteCount 这次消化掉的字节数
         */
        void releaseReceiveWindow(std::uint64_t streamId, std::size_t consumedByteCount);

        /// @return true 有按序到达的数据等着上层取
        [[nodiscard]] bool hasDeliveries() const noexcept;
        /// @return 一段交付；队列空时返回空
        [[nodiscard]] std::optional<QuicStreamDelivery> takeDelivery();

        /**
         * @brief 是否被打断
         * @details 只收「异常收场」的流：对端 RESET_STREAM，或对端用 STOP_SENDING 叫停本端的发送。
         *          正常收尾走交付的 `isFinal`，不走这里，上层因此不必区分「两路都说是结束」
         */
        /// @return true 有被打断的流等着上层回收
        [[nodiscard]] bool hasAbortedStreams() const noexcept;
        /// @return 一条被打断的流号；队列空时返回空
        [[nodiscard]] std::optional<std::uint64_t> takeAbortedStream();

        /**
         * @brief 确认一批包：把里面已送达的流数据从在途账上销掉
         * @param acknowledgedRanges 这些包里排出去的字节区间
         */
        void onSendRangesAcknowledged(const std::vector<QuicStreamRange> &acknowledgedRanges);

        /**
         * @brief 判丢一批包：把里面的流数据重新排到队首，偏移保持不变
         * @param lostRanges 判丢的包排出去的字节区间
         */
        void onSendRangesLost(const std::vector<QuicStreamRange> &lostRanges);

    private:
        /// 一段待发或在途的流数据；判丢后重发时偏移不变，同一批字节不会被算成两遍额度
        struct QuicStreamChunk
        {
            std::uint64_t beginOffset{0};      ///< 本段起始偏移
            std::vector<std::uint8_t> bytes{}; ///< 本段字节
            bool isFinal{false};               ///< 本段带着 FIN
        };

        /// 一条出站流的状态：待发队列 + 在途账 + 发送额度
        struct OutgoingStream
        {
            std::deque<QuicStreamChunk> pendingQueue{};             ///< 待发的段，按偏移递增
            std::map<std::uint64_t, QuicStreamChunk> inFlight{};    ///< 按起始偏移索引的在途段
            std::uint64_t nextWriteOffset{0};                       ///< 上层下一次写入的偏移
            std::uint64_t sentHighWater{0};                         ///< 曾经上线的最大结束偏移，额度按它算
            std::uint64_t streamLimit{0};                           ///< 对端给的这条流的发送上限
            std::optional<std::uint64_t> finalOffset{};             ///< 本端收尾后的总长度
            bool isAborted{false};                                  ///< 被对端 STOP_SENDING 叫停，队列作废
        };

        /// 一条入站流的状态：交付点、上层消费点、乱序缓存与接收额度
        struct IncomingStream
        {
            QuicReassemblyBuffer reassembly{};                       ///< 未交付的段：重复与重叠的分片在这里并成覆盖区
            std::uint64_t consumedByteCount{0};                      ///< 上层报回来的「已经消化掉」的字节数，窗口按它抬
            std::uint64_t receivedHighWaterOffset{0};                ///< 见过的最大结束偏移，连接级额度按它算
            std::uint64_t streamLimit{0};                            ///< 本端给这条流的接收上限，也是已宣告出去的值
            std::optional<std::uint64_t> finalOffset{};              ///< 对端收尾后的总长度
            bool isFinished{false};                                  ///< 收齐且已交付
            bool isFinalDelivered{false};                            ///< 带 FIN 的那段交付已经排进队列，不重复通知
            bool isReset{false};                                     ///< 被对端 RESET_STREAM 打断
            bool windowUpdatePending{false};                         ///< 欠这条流一条 MAX_STREAM_DATA
        };

        [[nodiscard]] OutgoingStream &outgoingStream(std::uint64_t streamId);
        /// 按流的类别取对端给的初始发送窗口；对端参数没到手时返回空
        [[nodiscard]] std::optional<std::uint64_t> initialSendWindowFor(std::uint64_t streamId) const noexcept;
        /// 按流的类别取本端宣告的初始接收窗口
        [[nodiscard]] std::uint64_t incomingInitialWindowOf(std::uint64_t streamId) const noexcept;
        /// 排干重组缓存里连续的字节，攒成一段交付
        void drainContiguousBytes(IncomingStream &stream, std::uint64_t streamId);
        [[nodiscard]] std::size_t collectWindowUpdates(std::string &frames, std::size_t byteBudget);
        [[nodiscard]] std::size_t collectStreamData(std::string &frames, std::size_t byteBudget,
                                                    std::vector<QuicStreamRange> &sentRanges);
        /// 对端用掉一半已宣告的流数就续上限，两处入口（新建流、消费数据）共用一段判据
        void raiseAdvertisedStreamLimits();
        /// 这条流现在还能发多少字节：流级与连接级额度取小
        [[nodiscard]] std::size_t sendCreditOf(const OutgoingStream &stream) const noexcept;

        QuicTransportParameters m_localParameters{};       ///< 本端宣告的参数，决定接收侧额度
        QuicTransportParameters m_peerParameters{};        ///< 对端参数；没到手前所有发送额度都是 0
        bool m_hasPeerParameters{false};                   ///< 对端参数是否已经用上

        std::map<std::uint64_t, IncomingStream> m_incoming{}; ///< 对端发起或回写的流
        std::map<std::uint64_t, OutgoingStream> m_outgoing{}; ///< 本端发起的流
        std::deque<QuicStreamDelivery> m_deliveries{};        ///< 等着交给上层的数据
        std::deque<std::uint64_t> m_abortedStreams{};         ///< 被打断、等上层回收的流号

        std::uint64_t m_connectionReceivedBytes{0};           ///< 各入站流最大结束偏移之和，§4.1 的连接级账
        std::uint64_t m_connectionConsumedBytes{0};           ///< 上层消化掉的字节总数，连接级窗口按它抬
        std::uint64_t m_connectionAdvertisedLimit{0};         ///< 已宣告的连接级接收上限
        bool m_connectionWindowUpdatePending{false};          ///< 欠一条 MAX_DATA

        std::uint64_t m_connectionSentHighWater{0};           ///< 各出站流曾上线的最大结束偏移之和
        std::uint64_t m_connectionSendLimit{0};               ///< 对端给的连接级发送上限，参数没到之前是 0

        std::uint64_t m_incomingBidirectionalCount{0};        ///< 对端发起的双向流数（按号数算，不按存活数）
        std::uint64_t m_incomingUnidirectionalCount{0};       ///< 对端发起的单向流数
        std::uint64_t m_advertisedBidirectionalStreams{0};    ///< 已经给过对端的双向流数上限
        std::uint64_t m_advertisedUnidirectionalStreams{0};   ///< 已经给过的单向流数上限
        bool m_streamsBidirectionalUpdatePending{false};      ///< 欠一条 MAX_STREAMS（双向）
        bool m_streamsUnidirectionalUpdatePending{false};     ///< 欠一条 MAX_STREAMS（单向）

        std::uint64_t m_nextUnidirectionalStreamId{0x03};     ///< 本端下一条单向流（服务端发起的单向流低位是 0x03）
        std::uint64_t m_outgoingBidirectionalLimit{0};        ///< 对端允许本端发起的双向流数
        std::uint64_t m_outgoingUnidirectionalLimit{0};       ///< 对端允许本端发起的单向流数
    };
} // namespace AsynGyanis::Net
