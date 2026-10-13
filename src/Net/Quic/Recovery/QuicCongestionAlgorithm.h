/**
 * @file QuicCongestionAlgorithm.h
 * @brief 拥塞控制算法的策略接口与共用记账：RFC 9002 §7 的窗口许可，算法部分可插拔
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 两家算法（NewReno、CUBIC）在 QUIC 里共用同一套骨架：发出去的包按字节计入在途、
 *          被确认或判丢时销账，恢复期「一轮拥塞只降一次」的闸，§7.3.1 的慢启动增长，
 *          以及最小窗/上界这些读口——全部收在基类。分叉只在三处虚点：
 *          @li 降窗算式（NewReno 取半、CUBIC 乘 0.7 并把降窗前的窗口记进 W_max）；
 *          @li 拥塞避免期的增长律（NewReno 线性加一、CUBIC 三次曲线 + Reno 友好区）；
 *          @li 持久拥塞重启后的算法状态清理（NewReno 无事、CUBIC 清掉 epoch 与 W_est）。
 *
 * @note 时刻与 RTT 都由调用方注入（与恢复层同构的纯计算件）：`setSmoothedRoundTripTime()`
 *       在每帧确认落账前把恢复层的 srtt 刷进来，CUBIC 的 Reno 友好区与「下一个 RTT 的目标窗」
 *       都要用它；NewReno 不读这一格。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Recovery/QuicRecovery.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace AsynGyanis::Net
{
    /// 拥塞控制算法的选择键：落在 `QuicConnectionCoreConfiguration::congestionAlgorithm`
    enum class QuicCongestionAlgorithmKind
    {
        NewReno, ///< RFC 9002 §7 的默认形态：丢包减半、线性加一（缺省）
        Cubic,   ///< RFC 9438：乘性减 0.7、三次曲线增长、Reno 友好区与快收敛
    };

    /// 三个状态：恢复期由基类判，慢启动/拥塞避免之分两家同形（窗口与阈值谁大）
    enum class QuicCongestionPhase
    {
        SlowStart,           ///< 窗口低于慢启动阈值：每个确认按确认到的字节数增长
        Recovery,            ///< 刚判丢：窗口不动，直到一个恢复期之后发出的包被确认
        CongestionAvoidance, ///< 窗口到顶：由算法自己的增长律接手（两家都只在圈外涨）
    };

    /**
     * @brief 拥塞控制算法的共有骨架（见文件头）：子类只实现三处虚点
     *
     * @details 窗口按整条连接算（RFC 9002 §7 的变量都是全局的），三个包号空间的包记同一本账。
     *          探针不受本类阻塞（§7.5）：调用方自己决定发不发，本类只把它算进在途字节。
     *          只带 ACK 的包同样不计入在途（§B.2），否则两端会因为互相确认而把窗口吃光。
     * @note ECN 的拥塞信号从 `noteEcnCongestionEvent()` 进来（RFC 9002 §7.1 + §B.7）：与丢包
     *       共用「一轮只降一次」那道闸（§7.3.2），具体降窗算式由算法自己定。
     *       持久拥塞（§7.6.2）已接上：恢复层判成之后调 `restartAfterPersistentCongestion()`，
     *       窗口与阈值一起落到最小窗并退出恢复期，再由子类清自己的算法状态。
     */
    class ASYN_NET_API QuicCongestionAlgorithm
    {
    public:
        virtual ~QuicCongestionAlgorithm() = default;

        QuicCongestionAlgorithm(const QuicCongestionAlgorithm &)            = delete;
        QuicCongestionAlgorithm &operator=(const QuicCongestionAlgorithm &) = delete;
        QuicCongestionAlgorithm(QuicCongestionAlgorithm &&)                 = delete;
        QuicCongestionAlgorithm &operator=(QuicCongestionAlgorithm &&)      = delete;

        /**
         * @brief 换一条数据报尺寸：§B.2 的 max_datagram_size 是窗口增减与最小窗的计量单位
         * @details 路径 MTU 探到更大的尺寸之后必须跟着换，否则慢启动按 1200 一格格长、实际每条报文
         *          有 1452 字节，窗口涨的速度就比线上花的速度快。0 一律不收——那是个会把最小窗
         *          折成 0 的取值，宁可留着旧尺寸
         * @param maximumDatagramByteLength 本端当前允许的数据报净载荷上限
         */
        void setMaximumDatagramByteLength(std::size_t maximumDatagramByteLength) noexcept;

        /**
         * @brief 记下一个刚发出去的包
         * @details 只有触发确认的包计入在途；探针也计（§7.5：探针不被窗口阻塞，但要算进负荷）
         * @param packet 发包凭据，取其中的字节数与是否触发确认
         */
        void onPacketSent(const QuicSentPacketInfo &packet) noexcept;

        /**
         * @brief 把恢复层当前的 srtt 刷进来：CUBIC 的友好区与目标窗要用，NewReno 不读
         * @param smoothed 平滑往返时间（RFC 9002 §5.3 的估计；调用方保证非负）
         */
        void setSmoothedRoundTripTime(QuicTime smoothed) noexcept;

        /**
         * @brief 一次确认的处理框架落到窗口上：销账、必要时进入恢复期、再按状态分派增长
         * @details 销账与「恢复期是否结束」两家完全同形，慢启动增长也同为 §7.3.1 那一格；
         *          拥塞避免期才把增长交给算法自己的虚点。
         * @param acknowledged 本次新确认的包，按包号递增
         * @param lost 因此次确认（或定时器）判丢的包，可为空
         * @param eventTime 触发本次更新的时刻：收到确认或定时器到期，进入恢复期时记的就是它
         */
        void onCongestionUpdate(const std::vector<QuicSentPacketInfo> &acknowledged, const std::vector<QuicSentPacketInfo> &lost, QuicTime eventTime);

        /// 降窗并进入恢复期（§7.3.2 的「已在恢复期就不二次降窗」由这一处统一判），丢包与 ECN 两条路共用
        void startCongestionEvent(QuicTime eventTime) noexcept;

        /**
         * @brief 对端报上来的 ECN-CE 计数涨了：按一次拥塞事件处置（RFC 9002 §7.1 + §B.7）
         * @details 与丢包那条路同一个动作、同一道闸：CE 是「网络还没丢包但已经堵」的早到信号，
         *          等到判丢再降窗就把这条路的容量用完了。共用 §7.3.2 的「一轮拥塞只降一次」判据，
         *          所以同一帧里 CE 与丢包同时出现也只降一次——这就是调用方要把本方法排在
         *          `onCongestionUpdate` **之前**的原因（与 §A.7 里 ECN 那一步排在判丢之前同形）。
         * @param eventTime 拥塞事件的时刻：规范取的是本帧最大确认值那条包的发出时刻（§B.7）
         */
        void noteEcnCongestionEvent(QuicTime eventTime) noexcept;

        /**
         * @brief 持久拥塞成立后按 §7.6.2 重启：窗口与慢启动阈值都落到最小窗
         * @details 与「降窗」的普通丢包反应不是一回事：停滞足够久说明这条路径大概已经不通，
         *          继续按缩小后的窗口爬只会让重传一直堆在那里，规范给的解法是回到最小窗重新慢启动。
         *          在途字节不动（那些包该销的账由确认与判丢那两条路去销）；子类的算法状态
         *          （如 CUBIC 的 epoch）由 `onPersistentCongestionRestart()` 清。
         */
        void restartAfterPersistentCongestion() noexcept;

        /**
         * @brief 某个包号空间的密钥退休了：把其中的在途字节销账，但不动窗口
         * @details 这些包既不会被确认也不会再判丢（RFC 9001 §4.9.1）。恢复层本就清了自己的在途表，
         *          拥塞层若不同步就永久背着这笔账——Initial 空间通常在握手期就退休，几十 KB 的旧包
         *          会把这条连接之后所有 `remainingByteBudget` 都扣掉那么多人。
         * @param discardedPackets 该空间里尚未确认的包
         */
        void onPacketsDiscarded(const std::vector<QuicSentPacketInfo> &discardedPackets) noexcept;

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
        [[nodiscard]] std::size_t         congestionWindowByteLength() const noexcept;
        [[nodiscard]] std::size_t         slowStartThresholdByteLength() const noexcept;
        [[nodiscard]] std::size_t         bytesInFlight() const noexcept;

    protected:
        /**
         * @brief 按本端的数据报上限建骨架
         * @details 初始窗口 = min(10×上限, max(2×上限, 14720))，最小窗口 = 2×上限（§7.2）
         * @param maximumDatagramByteLength 本端当前用的数据报净载荷上限，至少 1 字节
         */
        explicit QuicCongestionAlgorithm(std::size_t maximumDatagramByteLength) noexcept;

        /// 虚点一：降窗算式（减半 / 乘 0.7 并记 W_max）。调用时机由 `startCongestionEvent` 的闸统一给
        virtual void applyCongestionWindowDecrease() noexcept = 0;

        /**
         * @brief 虚点二：拥塞避免期的增长律（慢启动那一段由基类直接做）
         * @param acknowledgedByteCount 本次确认到的字节数（触发确认的包合计）
         * @param eventTime 本次更新的时刻
         * @note srtt 从成员 `m_smoothedRoundTripTime` 读（`setSmoothedRoundTripTime()` 刷进来的那份）
         */
        virtual void applyCongestionAvoidanceGrowth(std::size_t acknowledgedByteCount, QuicTime eventTime) = 0;

        /// 虚点三：持久拥塞重启后的算法状态清理；默认无事
        virtual void onPersistentCongestionRestart() noexcept
        {
        }

        /// 恢复期什么时候算结束：一个在 recoveryStartTime 之后发出的包被确认（§7.3.2）
        [[nodiscard]] bool isInRecovery() const noexcept;

        /// §7.2 的最小窗口（2×数据报上限）：降窗与持久拥塞重启的地板
        [[nodiscard]] std::size_t minimumWindowByteLength() const noexcept;

        std::size_t             m_maximumDatagramByteLength{0};    ///< §B.2 的 max_datagram_size，窗口增减都以它为单位
        std::size_t             m_congestionWindowByteLength{0};   ///< 拥塞窗口
        std::size_t             m_slowStartThresholdByteLength{0}; ///< 慢启动阈值；初始为「无穷大」
        std::size_t             m_bytesInFlight{0};                ///< 在途字节数
        std::optional<QuicTime> m_recoveryStartTime{};             ///< 有值即在本轮恢复期内
        QuicTime                m_smoothedRoundTripTime{};         ///< 最近一次注入的 srtt；CUBIC 读、NewReno 不读
    };

    /**
     * @brief 按选择键造一台拥塞控制器（RFC 9002 §7 的算法可插拔点）
     * @param kind 算法选择（自 `QuicConnectionCoreConfiguration::congestionAlgorithm`）
     * @param maximumDatagramByteLength 本端当前用的数据报净载荷上限
     * @return std::unique_ptr<QuicCongestionAlgorithm> 对应算法的实例；拒绝一切空指针契约由返回的非空保证
     */
    [[nodiscard]] ASYN_NET_API std::unique_ptr<QuicCongestionAlgorithm> makeQuicCongestionAlgorithm(QuicCongestionAlgorithmKind kind, std::size_t maximumDatagramByteLength);
} // namespace AsynGyanis::Net
