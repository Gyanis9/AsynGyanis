/**
 * @file QuicCubicCongestion.h
 * @brief CUBIC 拥塞控制（RFC 9438）：乘性减 0.7、三次曲线增长、Reno 友好区与快收敛
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 骨架（在途记账、恢复期闸、慢启动、读口）收在基类 `QuicCongestionAlgorithm`，
 *          本类给三处虚点：降窗按 β=0.7 并把降窗前的窗口记进 W_max（§4.6 + §4.7 快收敛）、
 *          拥塞避免期按三次曲线增长并与 Reno 友好区取大（§4.2–§4.5）、
 *          持久拥塞重启后清掉 epoch（§4.8 的精神：重启后从当前窗口重定 K）。
 *
 *          窗口在曲线计算里按「段」（字节 ÷ 数据报上限）走浮点，C 与 β 两个常量才不用为
 *          字节制另配一套刻度（RFC 9438 §4.1.1 允许字节实现，代价是自己折算 SMSS）；
 *          读写口与落进窗口的增量仍回到字节，与另一家算法同一把尺子。
 *
 * @note 刻意不做、以及它们各自的触发条件：
 *       @li HyStart++ 慢启动（§4.10 的 SHOULD）：本端沿 Reno 慢启动（同段 MAY），
 *           要的是「先有第二家算法」这一步；慢启动退出策略另有门面（出口路径见 §7 节拍），
 *           触发条件：高丢包链路上慢启动过冲成为实测瓶颈时再引 RFC 9406；
 *       @li 与 RFC 9002 §7.3.2 字面并存的「降窗用 flight_size」：QUIC 侧按该节允许的 cwnd
 *           比例式降窗（β 折进比例），两端都做同一件事就不用在恢复期另传 flight_size。
 * @see QuicCongestionAlgorithm, makeQuicCongestionAlgorithm
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"

#include <cstddef>
#include <optional>

namespace AsynGyanis::Net
{
    /**
     * @brief CUBIC 拥塞窗口（RFC 9438）
     *
     * @details 与 NewReno 同骨架、同读口；差异全部落在三处虚点与几个算法私有量上：
     *          W_max（降窗前的窗口）、epoch 起点、K（从 cwnd_epoch 长回 W_max 的时间）、
     *          W_est（Reno 友好区的估算窗口）。
     */
    class ASYN_NET_API QuicCubicCongestion final : public QuicCongestionAlgorithm
    {
    public:
        /**
         * @brief 按本端的数据报上限建拥塞控制器
         * @details 初始窗口 = min(10×上限, max(2×上限, 14720))，最小窗口 = 2×上限（RFC 9002 §7.2）
         * @param maximumDatagramByteLength 本端当前用的数据报净载荷上限，至少 1 字节
         */
        explicit QuicCubicCongestion(std::size_t maximumDatagramByteLength) noexcept;

        /**
         * @brief 降窗前的窗口（W_max），单位字节；还没有过拥塞事件时为空
         * @details 快收敛与 K 的算法都记在它上面，读出来给用例与应用层观测用
         */
        [[nodiscard]] std::optional<std::size_t> maximumWindowBeforeDecreaseByteLength() const noexcept;

        /// 当前拥塞避免阶段是否已经开钟（epoch 建立、K 已定）；给用例观测用
        [[nodiscard]] bool hasActiveCongestionAvoidanceEpoch() const noexcept;

    protected:
        /**
         * @brief 降窗（§4.6 图 5）：ssthresh = max(cwnd × β, 最小窗)，cwnd 落到 ssthresh
         * @details 降窗前按 §4.7 的快收敛记 W_max：当前窗口低于上一次的 W_max 时再压低一档
         *          （× (1+β)/2），把带宽更快让给新流；否则就记当前窗口。
         *          随后清掉 epoch 与 W_est——新的一轮拥塞避免在下一次增长时重开（K 由新 W_max 定）。
         */
        void applyCongestionWindowDecrease() noexcept override;

        /**
         * @brief 拥塞避免期的增长（§4.2–§4.5）：三次曲线目标与 Reno 友好区取大
         * @param acknowledgedByteCount 本次确认到的字节数
         * @param eventTime 本次更新的时刻（epoch 计时用；空闲期不算进曲线时间）
         */
        void applyCongestionAvoidanceGrowth(std::size_t acknowledgedByteCount, QuicTime eventTime) override;

        /// 窗口被重置回基准（§7.6.2 的持久拥塞重启或 §9.4 的迁移重置，§4.8 的精神）：
        /// 清掉 epoch 与 W_max 记录，下一次增长从当前窗口重定 K=0 起步
        void onWindowReset() noexcept override;

    private:
        /// 拥塞避免阶段的时钟与曲线参数（epoch 建立那一刻定下，降窗或重启时作废）
        struct CongestionAvoidanceEpoch
        {
            QuicTime startTime{};       ///< t_epoch：本阶段起点
            double   kSeconds{0.0};     ///< K：从 cwnd_epoch 长回 W_max 需要的时间（秒）
            double   wMaxSegments{0.0}; ///< 本阶段固定的 W_max（段）：阶段内不再变，重启时随 epoch 一并作废
        };

        /// 按当前窗口与 W_max 建立（或作为空闲近似重定）一轮拥塞避免的时钟
        void beginCongestionAvoidanceEpoch(QuicTime eventTime) noexcept;

        std::optional<std::size_t>              m_maximumWindowBeforeDecrease{}; ///< W_max（字节）；快收敛会把它再压低
        std::optional<std::size_t>              m_windowAtDecrease{};            ///< cwnd_prior：降窗前的窗口（W_est 的 α 切换判据）
        std::optional<CongestionAvoidanceEpoch> m_epoch{};                       ///< 当前拥塞避免阶段；降窗/重启后为空
        std::optional<double>                   m_renoFriendlyWindow{};          ///< W_est（段，浮点）；随 epoch 一起作废
    };
} // namespace AsynGyanis::Net
