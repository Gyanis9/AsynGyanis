/**
 * @file QuicCongestionControl.h
 * @brief NewReno 拥塞控制（RFC 9002 §7.2–§7.6）：慢启动、恢复期、拥塞避免与持久拥塞重启
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 纯计算件，与恢复层同构：发出去的包按字节计入在途，被确认或判丢时销账，窗口随
 *          「确认了多少字节」长、随「丢了多少」减半。时刻由调用方注入，因此每个整数都能
 *          在单测里复算。
 *
 *          骨架（在途记账、恢复期闸、慢启动、读口）收在基类 `QuicCongestionAlgorithm`，
 *          本类只给三处虚点：降窗取半（§7.3.2）、拥塞避免期线性加一（§7.3.3）、
 *          持久拥塞重启后无事可清。另一家算法见 `QuicCubicCongestion`（RFC 9438），
 *          选择键落在 `QuicConnectionCoreConfiguration::congestionAlgorithm`。
 *
 * @note 探针不受本类阻塞（§7.5）：调用方自己决定发不发，本类只把它算进在途字节。
 *       只带 ACK 的包同样不计入在途（§B.2），否则两端会因为互相确认而把窗口吃光。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"

namespace AsynGyanis::Net
{
    /**
     * @brief NewReno 拥塞窗口
     *
     * @details 只有一个「在途字节数 + 窗口许可」的概念，不区分包号空间：拥塞窗口按整条连接算
     *          （RFC 9002 §7 的变量都是全局的），三个空间的包都记在同一本账上。
     */
    class ASYN_NET_API QuicCongestionControl : public QuicCongestionAlgorithm
    {
    public:
        /**
         * @brief 按本端的数据报上限建拥塞控制器
         * @details 初始窗口 = min(10×上限, max(2×上限, 14720))，最小窗口 = 2×上限（§7.2）
         * @param maximumDatagramByteLength 本端当前用的数据报净载荷上限，至少 1 字节
         */
        explicit QuicCongestionControl(std::size_t maximumDatagramByteLength) noexcept;

    protected:
        /**
         * @brief 降窗取半（§7.3.2）：阈值 = max(当前窗口 / 2, 最小窗)，窗口一并落到阈值
         * @details 已在恢复期就不再二次降窗的闸在基类 `startCongestionEvent()`——丢包与 ECN 两条路共用
         */
        void applyCongestionWindowDecrease() noexcept override;

        /**
         * @brief 拥塞避免期线性加一（§7.3.3）：每个「确认掉整个窗口」的时间涨一个数据报
         * @param acknowledgedByteCount 本次确认到的字节数
         * @note 整数除法向下取整、最少 1 字节；时刻与 srtt 都不参与 NewReno 的这条算式
         */
        void applyCongestionAvoidanceGrowth(std::size_t acknowledgedByteCount, QuicTime eventTime) override;
    };
} // namespace AsynGyanis::Net
