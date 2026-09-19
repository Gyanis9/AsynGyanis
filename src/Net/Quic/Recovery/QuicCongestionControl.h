/**
 * @file QuicCongestionControl.h
 * @brief QUIC 拥塞控制（RFC 9002 §7.2–§7.5）：NewReno 的慢启动、恢复期与拥塞避免
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 纯计算件，与恢复层同构：发出去的包按字节计入在途，被确认或判丢时销账，窗口随
 *          「确认了多少字节」长、随「丢了多少」减半。时刻由调用方注入，因此每个整数都能
 *          在单测里复算。
 *
 * @note 探针不受本类阻塞（§7.5）：调用方自己决定发不发，本类只把它算进在途字节。
 *       只带 ACK 的包同样不计入在途（§B.2），否则两端会因为互相确认而把窗口吃光。
 * @warning 不处理 ECN-CE 计数与持久拥塞（§7.6），二者由后续里程碑接。
 */

#pragma once

#include "Net/Quic/Recovery/QuicRecovery.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    /// NewReno 的三个状态，判据按 §7.3 的图 1：是否已过恢复期、窗口与慢启动阈值谁大
    enum class QuicCongestionPhase
    {
        SlowStart,          ///< 窗口低于慢启动阈值：每个确认按确认到的字节数增长
        Recovery,           ///< 刚判丢：窗口不动，直到一个恢复期之后发出的包被确认
        CongestionAvoidance,///< 窗口到顶：按每个窗口最多涨一个数据报的加法增长
    };

    /**
     * @brief NewReno 拥塞窗口
     *
     * @details 只有一个「在途字节数 + 窗口许可」的概念，不区分包号空间：拥塞窗口按整条连接算
     *          （RFC 9002 §7 的变量都是全局的），三个空间的包都记在同一本账上。
     */
    class QuicCongestionControl
    {
    public:
        /**
         * @brief 按本端的数据报上限建拥塞控制器
         * @details 初始窗口 = min(10×上限, max(2×上限, 14720))，最小窗口 = 2×上限（§7.2）
         * @param maximumDatagramByteLength 本端当前用的数据报净载荷上限，至少 1 字节
         */
        explicit QuicCongestionControl(std::size_t maximumDatagramByteLength) noexcept;

        /**
         * @brief 记下一个刚发出去的包
         * @details 只有触发确认的包计入在途；探针也计（§7.5：探针不被窗口阻塞，但要算进负荷）
         * @param packet 发包凭据，取其中的字节数与是否触发确认
         */
        void onPacketSent(const QuicSentPacketInfo &packet) noexcept;

        /**
         * @brief 一次确认的处理结果落到窗口上：销账、必要时进入恢复期、再按状态涨窗口
         * @param acknowledged 本次新确认的包，按包号递增
         * @param lost 因此次确认（或定时器）判丢的包，可为空
         * @param eventTime 触发本次更新的时刻：收到确认或定时器到期，进入恢复期时记的就是它
         */
        void onCongestionUpdate(const std::vector<QuicSentPacketInfo> &acknowledged,
                                const std::vector<QuicSentPacketInfo> &lost,
                                QuicTime eventTime);

        /**
         * @brief 当前还能往网络上压多少字节
         * @return std::size_t 窗口减去在途；在途已超窗时返回 0 而不是回绕
         */
        [[nodiscard]] std::size_t remainingByteBudget() const noexcept;

        /**
         * @brief 是否还许可发一个指定大小的包
         * @param byteCount 这个包连头带尾的字节数
         * @return true 许可；false 表示该等下一个确认
         */
        [[nodiscard]] bool maySend(std::size_t byteCount) const noexcept;

        [[nodiscard]] QuicCongestionPhase phase() const noexcept;
        [[nodiscard]] std::size_t congestionWindowByteLength() const noexcept;
        [[nodiscard]] std::size_t slowStartThresholdByteLength() const noexcept;
        [[nodiscard]] std::size_t bytesInFlight() const noexcept;

    private:
        /// 恢复期什么时候算结束：一个在 recoveryStartTime 之后发出的包被确认（§7.3.2）
        [[nodiscard]] bool isInRecovery() const noexcept;

        std::size_t m_maximumDatagramByteLength{0};   ///< §B.2 的 max_datagram_size，窗口增减都以它为单位
        std::size_t m_congestionWindowByteLength{0};  ///< 拥塞窗口
        std::size_t m_slowStartThresholdByteLength{0};///< 慢启动阈值；初始为「无穷大」
        std::size_t m_bytesInFlight{0};               ///< 在途字节数
        std::optional<QuicTime> m_recoveryStartTime{}; ///< 有值即在本轮恢复期内
    };
} // namespace AsynGyanis::Net
