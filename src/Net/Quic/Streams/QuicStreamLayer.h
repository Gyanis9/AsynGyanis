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
 * @note 流号低位按角色定（§2.1）：作服务端时本端发起的是低位为 1 的那两档（双向 0x01、单向 0x03），
 *       作客户端时是本端发起低位为 0 的那两档（双向 0x00、单向 0x02）。其余判据（额度、上限、
 *       按号数算的流数）两型对称。
 * @warning 不是线程安全的：一条连接一份，只能在所属事件循环线程上驱动。
 */

#pragma once

#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/QuicConnectionRole.h"
#include "Net/Quic/QuicReassemblyBuffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
         * @brief 一条出站流允许暂存的待发字节上界
         * @details 编帧只受对端窗口约束，因此这条队列的排空速度由对端决定：对端长期不授窗口
         *          （慢读者或恶意连接）时，上层每交一段正文就往本端内存里堆一段，没有上界就是
         *          让对端替本端决定占多少内存。超出上界即部分接收，余下的字节退回上层留着。
         *          量级与 h2 侧的待发闸门一致（`Http2Session` 的 1 MiB）。
         * @note 判丢重排的字节按原样回到队列里（重发不能改偏移，§13.3），因此队列可短暂越过此界；
         *       越界期间新写入一律不收，队列回到界内即恢复
         */
        static constexpr std::size_t kMaximumPendingSendByteCount = 1024U * 1024U;

        /**
         * @brief 一条连接上所有出站流合计允许暂存的待发字节上界
         * @details 单流上界挡不住「很多条流各卡一点」：本端宣告的流数上限乘以 1 MiB 是条与流数同增的
         *          乘法，几十条停滞的请求流就能替对端压住几十 MiB。连接级这道闸把那笔乘法收成一个常数。
         *          取单流上界的八倍：一条大文件下载照常全速跑，要八条流同时不排空才触顶。
         */
        static constexpr std::size_t kMaximumConnectionPendingSendByteCount = 8U * 1024U * 1024U;

        /**
         * @brief 用本端宣告的参数与角色建流层
         * @details 本端参数决定「愿意收多少」；「能发多少」要等对端参数到手，在那之前一条也不发。
         *          角色只定一件事：本端发起的流号取哪一档低位（§2.1）
         * @param localParameters 本端传输参数，取 initial_max_data 与三个 initial_max_stream_data_*
         * @param role 本端角色，见 `QuicConnectionRole`；缺省为服务端，与 `QuicConnectionCoreConfiguration`
         *        的缺省角色同一口径
         */
        explicit QuicStreamLayer(const QuicTransportParameters &localParameters,
                                 QuicConnectionRole role = QuicConnectionRole::Server);

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
         * @brief 编出这一轮要发的帧：收口宣告在前，窗口更新居中，排队数据在后
         * @param frames 目标缓冲，追写到末尾；二进制安全
         * @param byteBudget 本包还能装多少字节；用完即止，剩下的下一包再来
         * @param sentRanges 排进数据帧的字节区间，核心要照它登记进发包凭据，好让确认与判丢能对上
         * @param announcements 排进本包的收口宣告，同样由核心登记：这两类帧要发到被确认为止（§13.3）
         * @return true 至少编进了一帧
         */
        bool collectFrames(std::string &frames, std::size_t byteBudget, std::vector<QuicStreamRange> &sentRanges,
                           std::vector<QuicStreamAnnouncement> &announcements);

        /// @return true 还有数据、窗口更新或收口宣告等着发
        [[nodiscard]] bool hasOutgoingFrames() const noexcept;

        /**
         * @brief 上层要发出去的字节：先进本层队列，等额度与包预算允许才编帧
         * @param streamId 目标流号，必须是本端发起的流
         * @param bytes 数据本体，二进制安全；本层会拷进待发队列
         * @param isFinal 发完这段是否收尾
         * @return std::size_t 被接收的字节数。小于 `bytes.size()` 即本层只收下前一段（队列到
         *         `kMaximumPendingSendByteCount`，或流已收尾、被打断、超出对端给的流数上限时是 0）：
         *         **余下的字节仍归调用方持有**，调用方要留住它们并在队列排空后续交，否则正文被截断
         * @note 收尾只在整段收下时落定：只收一半时 FIN 跟着退回的那一段，不会提前告诉对端「发完了」
         */
        std::size_t writeStreamData(std::uint64_t streamId, std::span<const std::uint8_t> bytes, bool isFinal);

        /**
         * @brief 这条流的待发队列里还有多少字节（没编进包的那部分）
         * @param streamId 流号
         * @return std::size_t 没有这条流的出站记账时为 0
         * @note 在途（已上线未确认）的字节不算：那部分由对端的窗口与传输参数界定，不是本端能堆的
         */
        [[nodiscard]] std::size_t pendingSendByteCount(std::uint64_t streamId) const noexcept;

        /**
         * @brief 这条连接上所有出站流的待发字节合计
         * @return std::size_t 各流 `pendingQueue` 之和
         * @note 每次写入现算一遍而不另记总量：流本就少（受本端宣告的流数上限约束），
         *       而队列的增删点有六处，多一本账就多一处漏记
         */
        [[nodiscard]] std::size_t totalPendingSendByteCount() const noexcept;

        /**
         * @brief 取走并清零「自上次调用以来排进包的待发字节数」
         * @return std::size_t 本层队列被编帧掏空的字节数
         * @note 给上层一个「腾出了地方」的信号：上层因队列到界而留着的字节要等这个数非零时续交
         */
        [[nodiscard]] std::size_t takeDrainedSendByteCount() noexcept;

        /**
         * @brief 本端放弃这条流的发送侧：作废待发与在途，排一条 RESET_STREAM（§4.5、§19.4）
         * @details 收尾长度在调用这一刻定稿（取曾上线的最大结束偏移），之后重发不会变——§13.3
         *          明令内容不许改，因为对端要靠它把连接级额度算到同一个数上。上层已经交出过收尾
         *          （带 FIN 的那一段）或本端已经放弃过时是空操作：那条流迟早正常收齐。
         * @param streamId 流号
         * @param applicationErrorCode 应用协议的错误码（h3 用 0x0100 以上那一档，见 RFC 9114 §8.1）
         */
        void resetStreamSending(std::uint64_t streamId, std::uint64_t applicationErrorCode);

        /**
         * @brief 请对端别再往这条流上发：排一条 STOP_SENDING（§3.5、§19.5）
         * @details 对端收到之后应当回一条 RESET_STREAM 收尾；本端的接收记账照旧——停发之后的入站
         *          字节仍然占连接级额度（§3.5），所以这里不动窗口，也不提前丢掉已缓存的分片。
         * @param streamId 流号；本端没有它的入站记账时直接跳过（还不存在的流、或本端只能发的那一档）
         * @param applicationErrorCode 期望对端在 RESET_STREAM 里带回的错误码
         */
        void stopStreamReceiving(std::uint64_t streamId, std::uint64_t applicationErrorCode);

        /**
         * @brief 开一条本端发起的单向流：服务端侧 0x03、0x07……，客户端侧 0x02、0x06……
         * @details 调用即占用，计数器立刻推进，因此连续两次不会给出同一个流号；条目也当场建好，
         *          后续 `writeStreamData` 直接落在它上面
         * @return std::optional<std::uint64_t> 对端宣告的单向流数已用满时返回空
         */
        [[nodiscard]] std::optional<std::uint64_t> openUnidirectionalStream();

        /**
         * @brief 开一条本端发起的双向流：客户端侧 0x00、0x04……，服务端侧 0x01、0x05……
         * @details 与单向那条同一套记账：调用即占用、条目当场建好。出站 HTTP 请求走的就是这一条路
         *          （RFC 9000 §1.3 把客户端发起的双向流当作「请求流」），限制来自对端参数的
         *          `initial_max_streams_bidi`，参数没到手之前一条也开不出来（§4.6）
         * @return std::optional<std::uint64_t> 对端宣告的双向流数已用满时返回空
         */
        [[nodiscard]] std::optional<std::uint64_t> openBidirectionalStream();

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
         * @brief 本层此刻还记着多少条流的状态（收、发两侧相加）
         * @details 只作观测与用例判据：一条流两侧都收口、该发的字节也都落定之后就该忘掉它，
         *          否则这笔账会随连接上做过的请求数一直长
         * @return std::size_t 入站与出站两张表的条目数之和
         */
        [[nodiscard]] std::size_t trackedStreamCount() const noexcept;

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

        /**
         * @brief 确认一批包：里面带出的收口宣告就此落定，不再发第二遍
         * @param acknowledgedAnnouncements 这些包带出的宣告（按 `collectFrames` 交出去的原样还回来）
         */
        void onStreamAnnouncementsAcknowledged(const std::vector<QuicStreamAnnouncement> &acknowledgedAnnouncements);

        /**
         * @brief 判丢一批包：里面带出的收口宣告重新排队，下一包补发同一份内容
         * @param lostAnnouncements 判丢的包带出的宣告
         */
        void onStreamAnnouncementsLost(const std::vector<QuicStreamAnnouncement> &lostAnnouncements);

    private:
        /// 一段待发或在途的流数据；判丢后重发时偏移不变，同一批字节不会被算成两遍额度
        struct QuicStreamChunk
        {
            std::uint64_t beginOffset{0};      ///< 本段起始偏移
            std::vector<std::uint8_t> bytes{}; ///< 本段字节
            bool isFinal{false};               ///< 本段带着 FIN
        };

        /**
         * @brief 一条收口宣告的状态（RESET_STREAM 或 STOP_SENDING 各一份）
         * @details 内容一次定稿：错误码与收尾长度在决定收口的那一刻就冻结，此后每次重发都是同一份
         *          （§13.3 的「内容不许变」）。之后只剩两件事要记：有没有在途、有没有被确认。
         */
        struct AbortAnnouncement
        {
            std::uint64_t applicationErrorCode{0}; ///< 线上的 application error code，定稿后不再变
            std::uint64_t finalSize{0};            ///< RESET_STREAM 的收尾长度；STOP_SENDING 用不到
            bool isInFlight{false};                ///< 已排进某个包，还没确认也没判丢：这段时间不重复发
            bool isAcknowledged{false};            ///< 已被确认：这辈子不再发第二遍
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
            std::optional<AbortAnnouncement> sendAbort{};           ///< 本端放弃发送：欠对端一条 RESET_STREAM
            bool isFinalSentToPeer{false};                          ///< 带 FIN 的那段是否已经上过线（收齐了就无需再复位）
            bool isAborted{false};                                  ///< 被对端 STOP_SENDING 叫停，队列作废
        };

        /// 一条入站流的状态：交付点、上层消费点、乱序缓存与接收额度
        struct IncomingStream
        {
            QuicReassemblyBuffer reassembly{};                       ///< 未交付的段：重复与重叠的分片在这里并成覆盖区
            std::uint64_t consumedByteCount{0};                      ///< 上层报回来的「已经消化掉」的字节数，窗口按它抬
            std::uint64_t discardedByteCount{0};                     ///< 对端复位时作废的未交付字节，上层永远不会为它报回来
            std::uint64_t receivedHighWaterOffset{0};                ///< 见过的最大结束偏移，连接级额度按它算
            std::uint64_t streamLimit{0};                            ///< 本端给这条流的接收上限，也是已宣告出去的值
            std::optional<std::uint64_t> finalOffset{};              ///< 对端收尾后的总长度
            std::optional<AbortAnnouncement> receiveStop{};          ///< 已请对端停发：欠对端一条 STOP_SENDING
            bool isFinished{false};                                  ///< 收齐且已交付
            bool isFinalDelivered{false};                            ///< 带 FIN 的那段交付已经排进队列，不重复通知
            bool isReset{false};                                     ///< 被对端 RESET_STREAM 打断
            bool windowUpdatePending{false};                         ///< 欠这条流一条 MAX_STREAM_DATA
        };

        /**
         * @brief 只在流层内部用的先进先出队列：一条都没有的时候不占任何堆块
         * @details 这两条队列多数时刻是空的或只有一两项，而 std::deque 光默认构造就要一块索引表加
         *          一个 512 字节的节点——流层是每连接一份，这笔固定开销直接乘在连接数上。取走只推进
         *          读位置，读到过半才把前面那段一次性擦掉，因此既不逐条搬移也不为空队列留块。
         *          需要 push_front 的那条（出站待发队列）仍用 std::deque，不在这里勉强
         * @tparam ItemType 队列元素类型
         */
        template <typename ItemType>
        class PendingQueue
        {
        public:
            /**
             * @brief 追加到队尾
             * @param item 要入队的元素，按值收走后移交进队列
             */
            void pushBack(ItemType item)
            {
                m_items.push_back(std::move(item));
            }

            /// @return true 没有等着被取走的元素
            [[nodiscard]] bool empty() const noexcept
            {
                return m_readPosition == m_items.size();
            }

            /**
             * @brief 取走队首元素
             * @return std::optional<ItemType> 队首的一份内容（从队列里移出来）；队列已空时返回空
             */
            [[nodiscard]] std::optional<ItemType> takeFront()
            {
                if (empty())
                {
                    return std::nullopt;
                }
                std::optional<ItemType> taken = std::move(m_items[m_readPosition]);
                ++m_readPosition;
                // 攒够一半才真正回收前面那段：每取一个就 erase(begin()) 等于每回把整张表往前搬一遍
                if (m_readPosition * 2 >= m_items.size())
                {
                    m_items.erase(m_items.begin(),
                                  m_items.begin() + static_cast<std::vector<ItemType>::difference_type>(m_readPosition));
                    m_readPosition = 0;
                }
                return taken;
            }

        private:
            std::vector<ItemType> m_items{};      ///< 元素本体，前 m_readPosition 个已被取走
            std::size_t m_readPosition{0};        ///< 下一个待取元素的下标
        };

        /**
         * @brief 这条流是不是本端发起的（判据随角色翻转，§2.1）
         * @details 作服务端时是本端发起低位为 1 的那两档，作客户端时是低位为 0 的那两档。
         *          这一条判据被额度取用、流数上限、收口宣告等十来处共用，因此它必须是**带角色的成员**
         *          而不是文件内的自由函数——同一个流号在两型眼里归属相反
         * @param streamId 流号
         * @return true 本端发起
         */
        [[nodiscard]] bool isLocallyInitiated(std::uint64_t streamId) const noexcept;

        [[nodiscard]] OutgoingStream &outgoingStream(std::uint64_t streamId);

        /**
         * @brief 这条流的接收侧是否已再无事可做
         * @details 终局只有两条：FIN 收齐并交付完，或对端复位。此外「上层报回来的 + 复位作废的」要凑齐
         *          记过的每个字节，且不再欠对端任何一帧——缺一条都不能摘
         * @param stream 待判定的入站流状态
         * @return true 可以摘掉这条记录
         */
        [[nodiscard]] bool isIncomingSettled(const IncomingStream &stream) const noexcept;

        /**
         * @brief 这条流的发送侧是否已再无事可做
         * @details 带 FIN 的那段已确认（判丢会把它退回未收尾），或本端的 RESET_STREAM 已落定；
         *          待发与在途都要清空
         * @param stream 待判定的出站流状态
         * @return true 可以摘掉这条记录
         */
        [[nodiscard]] bool isOutgoingSettled(const OutgoingStream &stream) const noexcept;

        /**
         * @brief 摘掉两侧都再无事可做的对端流
         * @details 挂在每次编帧的入口：不摘的话这两张表会随连接上做过的请求数一直长
         */
        void retireSettledStreams();

        /**
         * @brief 记下「这个流号的某一侧已作废」
         * @details 取的是同类档位里的边界而非逐条名单
         * @param streamId 被摘掉记录的那条流
         * @param isReceiveSide 摘的是接收侧还是发送侧
         */
        void noteStreamRetired(std::uint64_t streamId, bool isReceiveSide);

        /**
         * @brief 这条对端流的某一侧记录是否已被摘掉
         * @details 同类型流号只增不减（§2.1），边界之下又不在表里的号只能是「有过、如今作废了」；
         *          按新流建一份就会带着全新的额度，把对端合法的重传判成 FLOW_CONTROL_ERROR（§4.5）。
         *          收与发各记一条边界：入站侧往往先结清，共用一条会把同一条流的响应也挡掉
         * @param streamId 流号
         * @param isReceiveSide 查接收侧还是发送侧
         * @return true 那一侧的记录已作废
         */
        [[nodiscard]] bool isPeerStreamSideRetired(std::uint64_t streamId, bool isReceiveSide) const noexcept;
        /// 按宣告描述找回它对应的那份状态；流已不存在或该方向没收口即返回空
        [[nodiscard]] AbortAnnouncement *abortAnnouncementOf(const QuicStreamAnnouncement &announcement);
        /// 按流的类别取对端给的初始发送窗口；对端参数没到手时返回空
        [[nodiscard]] std::optional<std::uint64_t> initialSendWindowFor(std::uint64_t streamId) const noexcept;
        /// 按流的类别取本端宣告的初始接收窗口
        [[nodiscard]] std::uint64_t incomingInitialWindowOf(std::uint64_t streamId) const noexcept;
        /// 排干重组缓存里连续的字节，攒成一段交付
        void drainContiguousBytes(IncomingStream &stream, std::uint64_t streamId);
        [[nodiscard]] std::size_t collectWindowUpdates(std::string &frames, std::size_t byteBudget);
        /// 收口宣告排在最前：一条帧只占十几字节，却决定对端要不要继续等下去
        [[nodiscard]] std::size_t collectAbortAnnouncements(std::string &frames, std::size_t byteBudget,
                                                            std::vector<QuicStreamAnnouncement> &announcements);
        [[nodiscard]] std::size_t collectStreamData(std::string &frames, std::size_t byteBudget,
                                                    std::vector<QuicStreamRange> &sentRanges);
        /// 对端用掉一半已宣告的流数就续上限，两处入口（新建流、消费数据）共用一段判据
        void raiseAdvertisedStreamLimits();
        /// 这条流现在还能发多少字节：流级与连接级额度取小
        [[nodiscard]] std::size_t sendCreditOf(const OutgoingStream &stream) const noexcept;
        /// 把这条流待发队列里各段的字节数加起来（队列短且只在写入时算，不值得另记一本账）
        [[nodiscard]] static std::size_t pendingQueueByteCount(const OutgoingStream &stream) noexcept;

        QuicTransportParameters m_localParameters{};       ///< 本端宣告的参数，决定接收侧额度
        QuicTransportParameters m_peerParameters{};        ///< 对端参数；没到手前所有发送额度都是 0
        bool m_hasPeerParameters{false};                   ///< 对端参数是否已经用上

        std::map<std::uint64_t, IncomingStream> m_incoming{}; ///< 对端发起或回写的流
        std::map<std::uint64_t, OutgoingStream> m_outgoing{}; ///< 本端发起的流
        /// 对端发起的流里「已作废」的流号边界，取的是同类型的第几条。两侧必须分开——一条请求的
        /// 入站侧往往先结清，共用一条边界会把同一条流的响应也挡掉
        std::array<std::array<std::uint64_t, 2>, 2> m_retiredPeerStreamBoundaries{}; ///< [收/发][双向/单向] 各一条边界
        PendingQueue<QuicStreamDelivery> m_deliveries{};        ///< 等着交给上层的数据
        PendingQueue<std::uint64_t> m_abortedStreams{};         ///< 被打断、等上层回收的流号
        std::size_t m_drainedSendByteCount{0};                ///< 自上层取数以来排进包的待发字节，上层据此续交留下的那段

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

        bool m_isLocalServer{true};                               ///< 本端是不是服务端：只用来定流号低位（§2.1）
        std::uint64_t m_nextBidirectionalStreamId{0x01};    ///< 本端下一条双向流（服务端发起的双向流低位是 0x01，客户端 0x00）
        std::uint64_t m_nextUnidirectionalStreamId{0x03};     ///< 本端下一条单向流（服务端发起的单向流低位是 0x03，客户端 0x02）
        std::uint64_t m_outgoingBidirectionalLimit{0};        ///< 对端允许本端发起的双向流数
        std::uint64_t m_outgoingUnidirectionalLimit{0};       ///< 对端允许本端发起的单向流数
    };
} // namespace AsynGyanis::Net
