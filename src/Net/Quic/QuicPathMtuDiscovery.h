/**
 * @file QuicPathMtuDiscovery.h
 * @brief DPLPMTUD 的探测状态机：为这条路径维护「本端单个数据报最多能写多少字节」
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 纯计算件，与恢复层同构：时刻一律由调用方注入，因此四格状态机、探针计时与尺寸阶梯都能在
 *          单测里按微秒精确复现，不需要真实网络或定时器。本类不碰套接字也不发任何东西，它只回答三件事：
 *          「现在最大的数据报净载荷是多少」「下一次该醒的时刻」「该发探针了的话发多大」。
 *
 * @note 规范的 DPLPMTUD（RFC 8899）有 DISABLED / BASE / SEARCHING / SEARCH_COMPLETE / ERROR 五格，
 *       本仓把其中两格并掉了，理由都写在对应的转移处：
 *       ①BASE 那一格用「握手已完成」当确认——RFC 9000 §14.1 要求 Initial 的净载荷补足到不小于
 *         1200 字节，而 §14.3.1 又说 QUIC 作为被确认的 PL 在握手完成后才进 BASE，因此 BASE_PLPMTU
 *         在这条路上已经被真实确认过了，不必再为它单发一条探针；
 *       ②ERROR 那一格折进「贴着 BASE 不动、等 PMTU_RAISE_TIMER 再试」——RFC 8899 允许在这一格启用
 *         端点分片，而 RFC 9000 §14 明写数据报不许在 IP 层分片、IPv4 的 DF 位要设，QUIC 没有比
 *         1200 更小的退路，所以那一格对本协议只剩「停在 BASE 重试」这一种动作。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Quic/Recovery/QuicRecovery.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace AsynGyanis::Net
{
    /// 探测走到哪一格（RFC 8899 图 5 的那四格，ERROR 见文件头②）
    enum class QuicPathMtuPhase
    {
        Disabled,       ///< 握手还没完成：不探测，尺寸停在 BASE（RFC 9000 §14.3.1）
        Base,           ///< 已确认 BASE_PLPMTU 可用，等第一条更大的探针
        Searching,      ///< 正在往更大尺寸探测
        SearchComplete, ///< 探到头了（或同一尺寸连丢 MAX_PROBES 条收口），只等抬升计时器
    };

    /**
     * @brief 一条路径的数据报包层 PMTU 探测（DPLPMTUD）
     * @details 探针是**触发确认的**包：只有被确认的尺寸才敢立为 PLPMTU，所以「发得出去」与「收得回来」
     *          这一对才是证据，路由器的 ICMP 一律不参与（RFC 9000 §14.3.3 要求先验证再用，本仓不接
     *          ICMP 那条通路）。
     */
    class ASYN_NET_API QuicPathMtuDiscovery
    {
    public:
        /// BASE_PLPMTU：与 QUIC 最小的允许最大数据报尺寸同一个数（RFC 9000 §14.3、RFC 8899 §5.1.2）
        static constexpr std::size_t kBaseDatagramPayloadByteLength = 1200;

        /// MIN_PLPMTU：RFC 9000 §14.3 明写在本协议里与 BASE_PLPMTU 同一个数（QUIC 不用更小的数据报）
        static constexpr std::size_t kMinimumDatagramPayloadByteLength = kBaseDatagramPayloadByteLength;

        /// MAX_PROBES：同一尺寸允许的连续失败探针条数，RFC 8899 §5.1.2 给的默认值 3
        static constexpr std::size_t kMaximumFailedProbeCount = 3;

        /**
         * @brief 探测尺寸的阶梯，全部按「两端可能更小的那一侧」算，不猜本机接口 MTU（RFC 8899 §5.3.2）
         * @details 1232 = IPv6 最小 PMTU 1280 减 40 的 IP 头与 8 的 UDP 头；1452 = 以太网 1500 按 IPv6
         *          扣头（IPv4 同链路是 1472，取小的那个）；8952 = 巨型帧 9000 按 IPv6 扣头。
         *          阶梯之外的尺寸不去探：每多一格就多一组探针包，而常见的链路 MTU 就这几档。
         */
        static constexpr std::array<std::size_t, 3> kProbeSizeTable{1232U, 1452U, 8952U};

        /// 阶梯顶格，也就是本类愿意探测的最大尺寸（对端上限再高也不越它）
        static constexpr std::size_t kLargestProbedDatagramPayloadByteLength = kProbeSizeTable.back();

        /**
         * @brief 建状态机并定下两个计时器的周期
         * @param probeTimeout PROBE_TIMER 的周期：RFC 8899 §5.1.1 要求「不得小于 1 秒，且 SHOULD 大于
         *        15 秒」，缺省 16 秒即同时满足两条；用例里可以拿毫秒级的手表把整台机器跑完
         * @param raiseTimeout PMTU_RAISE_TIMER 的周期：规范在 §5.1.1 直接给了 600 秒这个数，
         *        作用是「用久了要重新探一遍，看路变宽了没有」
         */
        explicit QuicPathMtuDiscovery(QuicTime probeTimeout = QuicTime{16000000}, QuicTime raiseTimeout = QuicTime{600000000}) noexcept;

        /**
         * @brief 握手完成：从 Disabled 进 Base，并让第一条更大的探针随时可发
         * @details RFC 9000 §14.3.1 的那一句「QUIC 连接握手完成后就可以进 BASE 态」。Base 这一格在本仓
         *          不单独发探针（理由见文件头①），因此这一调用直接把本端尺寸停在 BASE、把待探尺寸推到阶梯
         *          的第一格
         * @param now 当前时刻
         */
        void onHandshakeConfirmed(QuicTime now) noexcept;

        /**
         * @brief 交入对端宣告的 max_udp_payload_size，作为 MAX_PLPMTU 的另一半
         * @details RFC 8899 §5.3.1：MAX_PLPMTU 取「本地 MTU 与对端 EMTU_R 的较小者」。本仓不猜本地 MTU，
         *          所以天花板就是对端的值与阶梯顶格的较小那一个。对端把值改小时本端立刻跟着降
         *          （正在探的尺寸与本端尺寸都会被折回来）
         * @param peerMaximumPayloadByteLength 对端宣告的最大 UDP 载荷。天花板不低于 MIN_PLPMTU：
         *        §18.2 让比它小的宣告进不了连接，而本类对 `maximumDatagramPayloadByteLength()` 的承诺是
         *        「不小于 BASE」——把那道校验的结果再信一次，这条不变式就成了别人的义务
         */
        void onPeerMaximumPayload(std::uint64_t peerMaximumPayloadByteLength) noexcept;

        /**
         * @brief 本端当前允许的最大数据报净载荷，也就是 PLPMTU
         * @return std::size_t 不小于 `kBaseDatagramPayloadByteLength`；握手没完成时就是 BASE
         */
        [[nodiscard]] std::size_t maximumDatagramPayloadByteLength() const noexcept;

        /**
         * @brief 该发探针了就交出这一条的尺寸，并把 PROBE_TIMER 武装起来
         * @details 同一条探针没收尾之前不会再发第二条（RFC 8899 §4.1「一次只探一个尺寸」），
         *          所以本方法在「有探针在途」时一律交空。探针照样要吃拥塞窗口（RFC 9000 §14.4 明写
         *          「PMTU probes consume congestion window」），因此窗口腾不出待探尺寸时也不算到点——
         *          不这样就会把「窗口小」记成「这个尺寸走不通」，连记三次就把 PLPMTU 白收口了
         * @param now 当前时刻
         * @param availableByteBudget 本端此刻愿意交给一条数据报的字节数（拥塞窗口的余量）
         * @return std::optional<std::size_t> 有待发探针时返回它该有多大，否则为空
         */
        [[nodiscard]] std::optional<std::size_t> probeByteLengthIfDue(QuicTime now, std::size_t availableByteBudget) noexcept;

        /**
         * @brief 在途探针被确认了：把待探尺寸立为本端尺寸，并把阶梯往上推一格
         * @param now 收到那条确认的时刻
         */
        void onProbeAcknowledged(QuicTime now) noexcept;

        /**
         * @brief PROBE_TIMER 或 PMTU_RAISE_TIMER 到点
         * @details 探针到点没被确认只算「这一尺寸的这一条没走过去」，连丢满 `kMaximumFailedProbeCount`
         *          条才收口（RFC 8899 §5.3.1）。到点时按在途的那格决定动作：
         *          有探针在途就记一次失败，否则是抬升计时器到点——重新从当前尺寸往上找
         * @param now 到点时刻
         */
        void onDeadlineReached(QuicTime now) noexcept;

        /**
         * @brief 路径上「发出去的东西一概没人答」：回到 BASE 重新探测
         * @details 调用方把这条接到 QUIC-RECOVERY §7.6.2 的持久拥塞上——那正是 RFC 8899 图 5 里
         *          「PL indicates loss of connectivity」那一格在 QUIC 里的对应物。掉回 BASE 是必要的：
         *          路断了的时候最可能的形状就是 MTU 变了，继续按原来的大尺寸发就一直在黑洞里发
         * @param now 判定时刻
         */
        void onConnectivityLost(QuicTime now) noexcept;

        /// @return QuicPathMtuPhase 当前那一格，供读数与用例判转移
        [[nodiscard]] QuicPathMtuPhase phase() const noexcept;

        /// @return std::size_t 当前待探的尺寸（也就是下一条探针该有多大）
        [[nodiscard]] std::size_t probedDatagramPayloadByteLength() const noexcept;

        /// @return std::size_t 当前尺寸的连续失败条数
        [[nodiscard]] std::size_t failedProbeCount() const noexcept;

        /// @return 下一次该醒的时刻；Disabled 或不需要定时器时为空
        [[nodiscard]] std::optional<QuicTime> nextDeadline() const noexcept;

    private:
        /// 把待探尺寸推到阶梯里下一个不超过天花板的那一格；推不动即返回 false
        [[nodiscard]] bool advanceProbedByteLength() noexcept;

        /// 天花板：对端宣告值与阶梯顶格的较小那一个
        [[nodiscard]] std::size_t ceilingByteLength() const noexcept;

        QuicTime                m_probeTimeout{};                                                        ///< PROBE_TIMER 周期
        QuicTime                m_raiseTimeout{};                                                        ///< PMTU_RAISE_TIMER 周期
        std::size_t             m_datagramPayloadByteLength{kMinimumDatagramPayloadByteLength};          ///< PLPMTU：已被确认的最大尺寸
        std::size_t             m_probedByteLength{kMinimumDatagramPayloadByteLength};                   ///< PROBED_SIZE：当前待探尺寸
        std::size_t             m_peerMaximumPayloadByteLength{kLargestProbedDatagramPayloadByteLength}; ///< 对端 EMTU_R
        std::size_t             m_failedProbeCount{0};                                                   ///< PROBE_COUNT：当前尺寸的连续失败条数
        QuicPathMtuPhase        m_phase{QuicPathMtuPhase::Disabled};                                     ///< 当前那一格
        bool                    m_isProbeOutstanding{false};                                             ///< 有一条探针在途（等它的 PROBE_TIMER）
        std::optional<QuicTime> m_deadline{};                                                            ///< 下一个到点时刻（探针或抬升，随在途与否而定）
    };
} // namespace AsynGyanis::Net
