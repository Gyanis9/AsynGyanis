/**
 * @file QuicRecovery.h
 * @brief QUIC 恢复层（RFC 9002 §5–§6）：RTT 估算、已发包记账、丢包判定与探测超时
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 纯计算件：发包时记一笔，收 ACK 时交进来，时刻一律由调用方注入，因此丢包与超时的
 *          判定可以在单测里按微秒精确复现，不需要真实网络或定时器。拥塞窗口与限速不在本文件，
 *          本层只把「在途字节数」算好交给它。
 *
 * @note 当前只服务服务端：§6.2.1 里「客户端不确定对端是否验证过地址」那一分支被简化成恒已验证，
 *       所以收到任何有效确认就把退避倍数归零。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Codec/QuicFrame.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// 恢复层的时钟单位，与连接状态机注入的时间戳同一套
    using QuicTime = std::chrono::microseconds;

    /// 包号空间；丢包与探测超时都是按空间各算各的（RFC 9002 §6 开头）
    enum class QuicRecoverySpace
    {
        Initial,
        Handshake,
        Application,
    };

    /// 一个包里握手字节的区间，判丢之后调用方按它重发对应的 CRYPTO 帧
    struct ASYN_NET_API QuicCryptoRange
    {
        std::uint64_t beginOffset{0}; ///< 该级别握手流上的起始偏移
        std::uint64_t endOffset{0};   ///< 结束偏移（不含）
    };

    /**
     * @brief 一个已发包在恢复层留下的凭据
     * @details `byteCount` 是拥塞控制的口径：算整个 UDP 数据报载荷，不算分帧后的明文长度
     *          （RFC 9002 §6.3 与 §7 把在途字节定义为计入拥塞窗口的数据报大小）。
     */
    struct ASYN_NET_API QuicSentPacketInfo
    {
        std::uint64_t                  packetNumber{0};       ///< 完整包号，不是线上截断的那个
        QuicTime                       timeSent{};            ///< 发出时刻，RTT 样本与时间阈值判定都靠它
        std::size_t                    byteCount{0};          ///< 计入在途的字节数；交 0 表示本包不计入（例如服务端在地址验证前的包）
        bool                           isAckEliciting{false}; ///< 是否触发确认：只有这种包才武装探测超时
        std::optional<QuicCryptoRange> cryptoRange{};         ///< 本包带的握手字节区间；没带就为空
        std::vector<QuicStreamRange>   streamRanges{};        ///< 本包带的流数据区间；确认与判丢都按它回收额度
        /// 本包带出的流收口宣告（RESET_STREAM / STOP_SENDING）：这两类帧要发到被确认为止（RFC 9000 §13.3），
        /// 所以和流数据一样得由发包方标出来，让上层按确认落定、按判丢补发
        std::vector<QuicStreamAnnouncement> streamAnnouncements{};
        /// 本包是否带了 HANDSHAKE_DONE：恢复层不理解帧语义，但这一帧要「重发到被确认为止」，
        /// 所以由发包方标出来，让上层判丢时知道该再补一次（RFC 9000 §19.20）
        bool carriesHandshakeDone{false};
    };

    /// 一次确认处理的结果
    struct ASYN_NET_API QuicAcknowledgementUpdate
    {
        std::vector<QuicSentPacketInfo> acknowledged{};            ///< 本次新确认的包，按包号递增
        std::vector<QuicSentPacketInfo> lost{};                    ///< 因此次确认而判丢的包
        bool                            isRoundTripSampled{false}; ///< 是否按 §5.1 的条件更新过 RTT 估算
        /**
         * @brief 本次确认是否把这条连接判进了**持久拥塞**（RFC 9002 §7.6.2）
         * @details 规范的三条并列条件都在这里核：两个**触发确认的**包被判丢、这两个包发出时刻之间发出的
         *          包一个都没被确认（跨三个空间一起看）、两者发出时刻之差超过持久拥塞时长；另外还要求
         *          这两个包发出时已经有一个先前的 RTT 样本。规范把「建立」这件事明确放在**收到确认之后**，
         *          所以这条信号只有这一个出口——定时器那条路只把新判丢并进这一段、不下判定（§7.6.1 末段
         *          还专门说了「不拿连续几次探测超时来建立持久拥塞」）。
         *          与普通判丢的差别在处置：普通判丢是窗口减半继续爬，持久拥塞是认定这条路已经不通，
         *          按 §7.6.2 把窗口与慢启动阈值一起落到最小窗重新慢启动。缺了这一格，链路真断了的话
         *          窗口只会一路减半贴着最小窗，重传堆在那里而读数上看不出「这条路径已经不行了」。
         */
        bool isPersistentCongestionDetected{false};
    };

    /// 定时器到期的处理结果
    /// @note 这里没有持久拥塞那一格：§7.6.2 明写「A sender establishes persistent congestion after the
    ///       receipt of an acknowledgment」，定时器这条出口只负责把新判丢并进那一段（`extendLossGap`）
    struct ASYN_NET_API QuicRecoveryTimeoutAction
    {
        std::vector<QuicSentPacketInfo> lost{};                                 ///< 按时间阈值新判丢的包
        QuicRecoverySpace               lostSpace{QuicRecoverySpace::Initial};  ///< 这些丢包属于哪个空间
        bool                            isProbeTimeout{false};                  ///< 是否需要发探测包
        QuicRecoverySpace               probeSpace{QuicRecoverySpace::Initial}; ///< 探测包该用哪个空间
    };

    /// RTT 估算的当前值，供用例断言与日志取用
    struct ASYN_NET_API QuicRoundTripTimeEstimate
    {
        QuicTime minimum{};      ///< min_rtt，不含对端报告的延迟
        QuicTime smoothed{};     ///< smoothed_rtt
        QuicTime variation{};    ///< rttvar
        QuicTime latest{};       ///< 最近一个样本
        QuicTime probeTimeout{}; ///< 当前退避倍数下的 PTO 周期
    };

    /**
     * @brief 一条连接的恢复状态：RTT + 已发包 + 丢包判定 + 探测超时
     *
     * @details 用法是三步一轮：发包调 `onPacketSent`，收 ACK 调 `onAcknowledgementReceived`，
     *          外层按 `nextDeadline` 定闹钟、到点调 `onDeadlineReached`。本类不持有任何时间源。
     * @warning 不是线程安全的，且刻意不锁：一个实例属于一条连接，只在所属循环线程上驱动。
     */
    class ASYN_NET_API QuicRecovery
    {
    public:
        /// 初始估算按 §5.3 落到 kInitialRtt，第一个样本到达时才换成真实观测
        QuicRecovery() noexcept;

        /**
         * @brief 记下一个刚发出的包
         * @param space 该包所在的包号空间
         * @param packet 发包凭据
         */
        void onPacketSent(QuicRecoverySpace space, QuicSentPacketInfo packet);

        /**
         * @brief 处理一个 ACK 帧：新确认的包移出在途、按 §6.1 判丢、必要时更新 RTT
         * @param space 该 ACK 属于哪个包号空间
         * @param acknowledgement 解好的 ACK 帧
         * @param acknowledgementTime 收到它的时刻
         * @param acknowledgementDelay 对端报告的 ACK 延迟，已由调用方按对端指数换算成时间（§19.3）
         * @return QuicAcknowledgementUpdate 确认与判丢的清单，以及是否采了 RTT 样本
         */
        [[nodiscard]] QuicAcknowledgementUpdate onAcknowledgementReceived(QuicRecoverySpace space, const QuicAcknowledgementFrame &acknowledgement, QuicTime acknowledgementTime,
                                                                          QuicTime acknowledgementDelay);

        /**
         * @brief 握手确认，并交入对端声明的 max_ack_delay
         * @details §5.3 要把报告延迟夹到这个值以内，§6.2.1 要把它加进入用空间的 PTO，
         *          两处的依据都是「握手已确认」，所以一次性给出
         * @param peerMaximumAcknowledgmentDelay 对端的 max_ack_delay
         */
        void onHandshakeConfirmed(QuicTime peerMaximumAcknowledgmentDelay) noexcept;

        /**
         * @brief 下一次该醒的时刻
         * @return 不需要定时器时返回空；否则返回「时间阈值丢包」与「探测超时」中更早的那个
         */
        [[nodiscard]] std::optional<QuicTime> nextDeadline() const noexcept;

        /**
         * @brief 定时器到期
         * @details 先按 §6.1.2 的时间阈值判丢；没有可判的才当作探测超时，并按 §6.2.1 把退避倍数翻倍
         * @param now 当前时刻
         * @return QuicRecoveryTimeoutAction 该重发什么、该往哪个空间探测
         */
        [[nodiscard]] QuicRecoveryTimeoutAction onDeadlineReached(QuicTime now);

        /**
         * @brief 丢弃一个空间的记账（Initial/Handshake 密钥退休时调）
         * @param space 要丢弃的空间；本方法不校验它是不是 Application
         */
        void discardSpace(QuicRecoverySpace space);

        /**
         * @brief 当前 RTT 估算
         * @return QuicRoundTripTimeEstimate 各统计量与当前 PTO 周期
         */
        [[nodiscard]] QuicRoundTripTimeEstimate roundTripTimeEstimate() const noexcept;

        /**
         * @brief 在途字节总数
         * @return std::size_t 三个空间里尚未确认、且计入拥塞窗口的包之和
         */
        [[nodiscard]] std::size_t inFlightByteCount() const noexcept;

        /**
         * @brief 某空间里仍未被确认的包
         * @details 重发时要靠这些记录的 `cryptoRange` 决定该重出哪几段握手字节：判丢与探测超时都走这条路
         * @param space 包号空间
         * @return std::vector<QuicSentPacketInfo> 按包号递增
         */
        [[nodiscard]] std::vector<QuicSentPacketInfo> unacknowledgedPackets(QuicRecoverySpace space) const;

    private:
        static constexpr std::size_t kSpaceCount = 3; ///< 包号空间个数

        /// 一个空间的记账
        struct SpaceState
        {
            std::map<std::uint64_t, QuicSentPacketInfo> unacknowledged{};      ///< 未确认的已发包，按包号有序
            std::optional<QuicTime>                     lossTime{};            ///< 最早可以按时间阈值判丢的时刻
            std::optional<std::uint64_t>                largestAcknowledged{}; ///< 本空间见过的最大确认值
        };

        [[nodiscard]] static std::size_t spaceIndex(QuicRecoverySpace space) noexcept;
        void                             updateRoundTripTime(QuicTime latestRoundTripTime, QuicTime acknowledgementDelay);
        /// 按 §6.1 的判据扫描一个空间：包号阈值或时间阈值命中即判丢，否则记下待判的时刻
        [[nodiscard]] std::vector<QuicSentPacketInfo>                       detectLostPackets(QuicRecoverySpace space, QuicTime now);
        [[nodiscard]] std::pair<std::optional<QuicTime>, QuicRecoverySpace> earliestLossTime() const noexcept;
        /// 该空间里还在途的、最后一个触发确认的包的发出时刻；没有就表示无需武装 PTO
        [[nodiscard]] std::optional<QuicTime>                               lastAckElicitingSentTime(const SpaceState &state) const noexcept;
        [[nodiscard]] std::pair<std::optional<QuicTime>, QuicRecoverySpace> probeTimeoutDeadline() const noexcept;
        /**
         * @brief 把这一趟新判丢的包并进 §7.6.2 那一段「没有确认打断的判丢」里
         * @details 只收**触发确认的**包：规范那句「These two packets MUST be ack-eliciting」的理由是
         *          对端只承诺在 max_ack_delay 内确认触发确认的包，只含 ACK 的包本来就可能没人答，
         *          把它算进「这条路死了」的那一段会误判。锚点跨三个包号空间共用（规范同样要求
         *          「across all packet number spaces」一起看）。
         * @param lost 这一趟新判丢的包（可为空）
         */
        void extendLossGap(const std::vector<QuicSentPacketInfo> &lost) noexcept;
        /**
         * @brief 按 §7.6.2 回答「这一帧确认是否建立了持久拥塞」
         * @details 先把本趟的判丢并进锚点，再把本帧新确认的包记进「晚于锚点起点的最早一次确认」，
         *          最后才比时长。这个顺序是有讲究的：段里的终点会随后续判丢继续往后长，此刻落在段外
         *          （本帧那条最新确认的包必然晚于本趟所有判丢）的确认，之后可能落进段内 ⇒ 那段不再
         *          「一个都没被确认」，必须扣掉。所以只留一个水位（晚于起点的最早一次确认），
         *          不必存下全部确认时刻。
         * @param acknowledged 本帧新确认的包：发出时刻落在锚点两段之间就打断这一段
         * @param lost 本趟新判丢的包
         * @return true 建立持久拥塞（调用方据此按 §7.6.2 重启窗口）
         */
        bool notePersistentCongestionSpan(const std::vector<QuicSentPacketInfo> &acknowledged, const std::vector<QuicSentPacketInfo> &lost) noexcept;
        /// 持久拥塞时长：(smoothed_rtt + max(4×rttvar, kGranularity) + 对端的 max_ack_delay) × 3。
        /// §7.6.1 明写这里的报告延迟**不论丢包在哪个空间都要算进去**（与 §6.2 的 PTO 相反），
        /// 所以没有「握手空间按 0 算」那一格；倍率取 §7.2 推荐的 3
        [[nodiscard]] QuicTime persistentCongestionDuration() const noexcept;

        std::array<SpaceState, kSpaceCount> m_spaces{};                    ///< 三个包号空间
        QuicRoundTripTimeEstimate           m_estimate{};                  ///< RTT 统计量
        bool                                m_hasRoundTripSample{false};   ///< 第一个样本走重置路径，之后才走加权
        std::size_t                         m_probeBackoffExponent{0};     ///< 退避倍数是 2 的几次方
        std::size_t                         m_inFlightByteCount{0};        ///< 在途字节总数，拥塞层与用例都要看
        bool                                m_isHandshakeConfirmed{false}; ///< §4.1.2 意义上的握手确认，决定延迟夹取与 PTO 空间
        /// 对端声明的 max_ack_delay：没拿到对端参数之前按 RFC 9000 §18.2 的默认 25ms 取。
        /// 这里宁可把时长算大：§7.6.1 明写「太小会让本端不必要地判成持久拥塞」，而误判的代价是把窗口按回最小窗
        QuicTime m_peerMaximumAcknowledgmentDelay{std::chrono::duration_cast<QuicTime>(std::chrono::milliseconds{25})};
        /// §7.6.2 的那两个判丢包：这一段没有确认的区间里最早 / 最近一个被判丢的触发确认包的发出时刻。
        /// 用 `std::optional` 而不是「0 表示没有」：0 在这里是合法的发出时刻（用例从 0 起算）
        std::optional<QuicTime> m_lossGapEarliestSentTime{}; ///< 这一段里最早被判丢的触发确认包的发出时刻
        std::optional<QuicTime> m_lossGapLatestSentTime{};   ///< 这一段里最近被判丢的触发确认包的发出时刻
        /// 这一段开着之后、发出时刻**晚于**锚点起点的被确认包里最早的那个：段里的终点会随后续判丢往后长，
        /// 所以留一个「最早的一次晚于起点的确认」就够判「这两个包之间有没有包被确认过」
        std::optional<QuicTime> m_lossGapAckedAfterStart{};
        /// 第一个 RTT 样本被收到的时刻：§7.6.2 要那两个包**发出时**就已有先前的样本，
        /// 只判「手上有没有样本」会把这一帧刚采到的样本算成先前的
        std::optional<QuicTime> m_firstRoundTripSampleTime{};
    };
} // namespace AsynGyanis::Net
