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
    struct QuicCryptoRange
    {
        std::uint64_t beginOffset{0}; ///< 该级别握手流上的起始偏移
        std::uint64_t endOffset{0};   ///< 结束偏移（不含）
    };

    /**
     * @brief 一个已发包在恢复层留下的凭据
     * @details `byteCount` 是拥塞控制的口径：算整个 UDP 数据报载荷，不算分帧后的明文长度
     *          （RFC 9002 §6.3 与 §7 把在途字节定义为计入拥塞窗口的数据报大小）。
     */
    struct QuicSentPacketInfo
    {
        std::uint64_t packetNumber{0};      ///< 完整包号，不是线上截断的那个
        QuicTime timeSent{};                ///< 发出时刻，RTT 样本与时间阈值判定都靠它
        std::size_t byteCount{0};           ///< 计入在途的字节数；交 0 表示本包不计入（例如服务端在地址验证前的包）
        bool isAckEliciting{false};         ///< 是否触发确认：只有这种包才武装探测超时
        std::optional<QuicCryptoRange> cryptoRange{}; ///< 本包带的握手字节区间；没带就为空
    };

    /// 一次确认处理的结果
    struct QuicAcknowledgementUpdate
    {
        std::vector<QuicSentPacketInfo> acknowledged{}; ///< 本次新确认的包，按包号递增
        std::vector<QuicSentPacketInfo> lost{};         ///< 因此次确认而判丢的包
        bool isRoundTripSampled{false};                 ///< 是否按 §5.1 的条件更新过 RTT 估算
    };

    /// 定时器到期的处理结果
    struct QuicRecoveryTimeoutAction
    {
        std::vector<QuicSentPacketInfo> lost{};         ///< 按时间阈值新判丢的包
        bool isProbeTimeout{false};                     ///< 是否需要发探测包
        QuicRecoverySpace probeSpace{QuicRecoverySpace::Initial}; ///< 探测包该用哪个空间
    };

    /// RTT 估算的当前值，供用例断言与日志取用
    struct QuicRoundTripTimeEstimate
    {
        QuicTime minimum{};            ///< min_rtt，不含对端报告的延迟
        QuicTime smoothed{};           ///< smoothed_rtt
        QuicTime variation{};          ///< rttvar
        QuicTime latest{};             ///< 最近一个样本
        QuicTime probeTimeout{};       ///< 当前退避倍数下的 PTO 周期
    };

    /**
     * @brief 一条连接的恢复状态：RTT + 已发包 + 丢包判定 + 探测超时
     *
     * @details 用法是三步一轮：发包调 `onPacketSent`，收 ACK 调 `onAcknowledgementReceived`，
     *          外层按 `nextDeadline` 定闹钟、到点调 `onDeadlineReached`。本类不持有任何时间源。
     * @warning 不是线程安全的，且刻意不锁：一个实例属于一条连接，只在所属循环线程上驱动。
     */
    class QuicRecovery
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
        [[nodiscard]] QuicAcknowledgementUpdate onAcknowledgementReceived(QuicRecoverySpace space,
                                                                          const QuicAcknowledgementFrame &acknowledgement,
                                                                          QuicTime acknowledgementTime,
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

    private:
        static constexpr std::size_t kSpaceCount = 3; ///< 包号空间个数

        /// 一个空间的记账
        struct SpaceState
        {
            std::map<std::uint64_t, QuicSentPacketInfo> unacknowledged{}; ///< 未确认的已发包，按包号有序
            std::optional<QuicTime> lossTime{};                           ///< 最早可以按时间阈值判丢的时刻
            std::optional<std::uint64_t> largestAcknowledged{};            ///< 本空间见过的最大确认值
        };

        [[nodiscard]] static std::size_t spaceIndex(QuicRecoverySpace space) noexcept;
        void updateRoundTripTime(QuicTime latestRoundTripTime, QuicTime acknowledgementDelay);
        /// 按 §6.1 的判据扫描一个空间：包号阈值或时间阈值命中即判丢，否则记下待判的时刻
        [[nodiscard]] std::vector<QuicSentPacketInfo> detectLostPackets(QuicRecoverySpace space, QuicTime now);
        [[nodiscard]] std::pair<std::optional<QuicTime>, QuicRecoverySpace> earliestLossTime() const noexcept;
        /// 该空间里还在途的、最后一个触发确认的包的发出时刻；没有就表示无需武装 PTO
        [[nodiscard]] std::optional<QuicTime> lastAckElicitingSentTime(const SpaceState &state) const noexcept;
        [[nodiscard]] std::pair<std::optional<QuicTime>, QuicRecoverySpace> probeTimeoutDeadline() const noexcept;

        std::array<SpaceState, kSpaceCount> m_spaces{};      ///< 三个包号空间
        QuicRoundTripTimeEstimate m_estimate{};    ///< RTT 统计量
        bool m_hasRoundTripSample{false};          ///< 第一个样本走重置路径，之后才走加权
        std::size_t m_probeBackoffExponent{0};     ///< 退避倍数是 2 的几次方
        std::size_t m_inFlightByteCount{0};        ///< 在途字节总数，拥塞层与用例都要看
        bool m_isHandshakeConfirmed{false};        ///< §4.1.2 意义上的握手确认，决定延迟夹取与 PTO 空间
        QuicTime m_peerMaximumAcknowledgmentDelay{}; ///< 对端声明的 max_ack_delay
    };
} // namespace AsynGyanis::Net
