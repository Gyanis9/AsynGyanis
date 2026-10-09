// TestQuicRecovery.cpp —— 恢复层（RFC 9002 §5–§6 与 §7.6）用例
//
// 时间全部注入，所以每条断言都是可精确复算的整数：RTT 的三个统计量、PTO 周期、按包号与按时间的
// 两种丢包判据、退避倍数、以及确认/判丢后在途字节的回收。
// 期望值都按 RFC 的公式手算并写成注释，整数除法一律**向零截断**（例如 41875/4 = 10468）；
// 若哪天改了运算次序，这些数字会先变红，而不是留下一个看着合理其实偏了的估算。
//
// 覆盖：
//   1) 没样本时的初值，以及「握手起步 PTO 约 1 秒」这条经验事实（§5.3 + §6.2.2）；
//   2) 第一个样本走重置、后续样本走 7/8 与 3/4 的加权（§5.3）；
//   3) ACK 延迟的夹取规则：不确认时不夹、确认后夹到 max_ack_delay、以及「减了会低于 min_rtt 就不减」（§5.3）；
//   4) 包号阈值判丢（kPacketThreshold=3）与时间阈值判丢（9/8 + 粒度下限）各自的边界（§6.1）；
//   5) 重复 ACK 不再采样本但照样判丢（§5.1 + §A.7 第 5 步）、PTO 到期翻倍退避并在收到确认时归零（§6.2.1）；
//   6) 进入用空间在握手确认前不武装 PTO（§6.2.1）、丢弃空间时清账（§A.11）。
//   7) 持久拥塞（§7.6）：端点条件各自的对照（段内出现过确认、只带 ACK 的包、时长不足、第一个样本之前
//      不开段）、定时器那一侧只并段不下结论、判成之后清账不许连着判第二次。

#include "Net/Quic/Recovery/QuicRecovery.h"

#include "Net/Quic/Codec/QuicFrame.h"
#include "Platform/IO/DatagramSocket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// @return QuicTime 毫秒换成本层的微秒时间
        QuicTime milliseconds(const std::int64_t value)
        {
            return std::chrono::duration_cast<QuicTime>(std::chrono::milliseconds{value});
        }

        /**
         * @brief 造一个已发包记录
         * @param packetNumber 包号
         * @param timeSent 发出时刻
         * @param byteCount 计入在途的字节数
         * @return QuicSentPacketInfo 触发确认的包（本层几乎所有路径都以它为准）
         */
        QuicSentPacketInfo makeSentPacket(const std::uint64_t packetNumber, const QuicTime timeSent, const std::size_t byteCount = 100)
        {
            QuicSentPacketInfo packet;
            packet.packetNumber   = packetNumber;
            packet.timeSent       = timeSent;
            packet.byteCount      = byteCount;
            packet.isAckEliciting = true;
            return packet;
        }

        /**
         * @brief 造一个 ACK 帧
         * @param largestAcknowledged 最大确认包号
         * @param ranges 区间列表（递减、闭区间）
         * @param ecnCounts 交空表示这一帧**不带**三个计数（帧类型 0x02）；给出则置 `hasEcnCounts`
         *                  并按 ECT(0)、ECT(1)、ECN-CE 的顺序填进去（§19.3.2）
         * @return QuicAcknowledgementFrame 交给恢复层的帧
         */
        QuicAcknowledgementFrame makeAcknowledgement(const std::uint64_t largestAcknowledged, const std::vector<QuicAcknowledgementRange> &ranges,
                                                     const std::optional<std::array<std::uint64_t, 3>> &ecnCounts = std::nullopt)
        {
            QuicAcknowledgementFrame acknowledgement;
            acknowledgement.largestAcknowledgedPacketNumber = largestAcknowledged;
            acknowledgement.ranges                          = ranges;
            if (ecnCounts.has_value())
            {
                acknowledgement.ecnCounts    = *ecnCounts;
                acknowledgement.hasEcnCounts = true;
            }
            return acknowledgement;
        }

        /// 造一条本端**标了 ECT(0)** 的已发包（ECN 试探期就长这样）
        QuicSentPacketInfo makeMarkedSentPacket(const std::uint64_t packetNumber, const QuicTime timeSent)
        {
            QuicSentPacketInfo packet = makeSentPacket(packetNumber, timeSent);
            packet.ecnCodepoint       = Platform::kEcnCodepointEctZero;
            return packet;
        }

        /// 只确认单个包号的 ACK
        QuicAcknowledgementFrame acknowledgementOf(const std::uint64_t packetNumber)
        {
            return makeAcknowledgement(packetNumber, {{packetNumber, packetNumber}});
        }
    } // namespace

    /**
     * @brief 初值就是 kInitialRtt，第一个包武装出来的 PTO 约 1 秒（§6.2.2 的经验值）
     */
    TEST(QuicRecovery, InitializesEstimateAndArmsOneSecondProbeAtStart)
    {
        QuicRecovery                    recovery;
        const QuicRoundTripTimeEstimate initial = recovery.roundTripTimeEstimate();
        EXPECT_EQ(initial.smoothed, milliseconds(333));
        EXPECT_EQ(initial.variation, milliseconds(166) + QuicTime{500});
        EXPECT_EQ(initial.minimum, QuicTime{0});
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "还没发包就不该有定时器";

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        // PTO = smoothed + max(4*rttvar, kGranularity) = 333000 + 666000 = 999000（§6.2.1，
        // Initial/Handshake 空间不加 max_ack_delay）
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(999));
    }

    /**
     * @brief 第一个样本直接重置估算，不留初值权重
     */
    TEST(QuicRecovery, FirstSampleResetsTheEstimator)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(50), QuicTime{0});

        const QuicRoundTripTimeEstimate estimate = recovery.roundTripTimeEstimate();
        EXPECT_EQ(estimate.latest, milliseconds(50));
        EXPECT_EQ(estimate.minimum, milliseconds(50));
        EXPECT_EQ(estimate.smoothed, milliseconds(50));
        EXPECT_EQ(estimate.variation, milliseconds(25));
    }

    /**
     * @brief 第二个样本起走 7/8 与 3/4 的加权，且 rttvar 用的是更新前的 smoothed
     */
    TEST(QuicRecovery, AppliesWeightedUpdateOnLaterSamples)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(100), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(110)));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), milliseconds(250), QuicTime{0});

        // 样本 latest=140ms（250-110）、延迟 0 → adjusted=140ms
        // rttvar = (3*50000 + |100000-140000|)/4 = 190000/4 = 47500
        // smoothed = 100000 + (140000-100000)/8 = 105000
        const QuicRoundTripTimeEstimate estimate = recovery.roundTripTimeEstimate();
        EXPECT_EQ(estimate.latest, milliseconds(140));
        EXPECT_EQ(estimate.minimum, milliseconds(100)) << "min_rtt 不被更大的样本抬高（§5.2）";
        EXPECT_EQ(estimate.variation, milliseconds(47) + QuicTime{500});
        EXPECT_EQ(estimate.smoothed, milliseconds(105));
    }

    /**
     * @brief 延迟只在「减了不会低于 min_rtt」时才减，超过 max_ack_delay 的部分确认后不再算延迟
     */
    TEST(QuicRecovery, SubtractsAcknowledgementDelayOnlyWhenPlausible)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        // 第一个样本 20ms 定下 min_rtt
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(20), QuicTime{0});

        // 样本 2：latest=50ms、延迟 15ms → 50 >= 20+15，减完 adjusted=35ms
        // rttvar = (3*10000 + |20000-35000|)/4 = 11250；smoothed = 20000 + 15000/8 = 21875
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(50)));
        std::ignore                           = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), milliseconds(100), milliseconds(15));
        QuicRoundTripTimeEstimate afterSecond = recovery.roundTripTimeEstimate();
        EXPECT_EQ(afterSecond.smoothed, milliseconds(21) + QuicTime{875});
        EXPECT_EQ(afterSecond.variation, milliseconds(11) + QuicTime{250});

        // 样本 3：latest=30ms、延迟 15ms → 30 < 20+15，不减（否则等于承认 RTT 比观测下界还小）
        // adjusted=30000；rttvar = (3*11250 + |21875-30000|)/4 = 41875/4 = 10468（向零截断）
        // smoothed = 21875 + 8125/8 = 21875 + 1015 = 22890
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(110)));
        std::ignore                                = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(2), milliseconds(140), milliseconds(15));
        const QuicRoundTripTimeEstimate afterThird = recovery.roundTripTimeEstimate();
        EXPECT_EQ(afterThird.smoothed, milliseconds(22) + QuicTime{890});
        EXPECT_EQ(afterThird.variation, milliseconds(10) + QuicTime{468});
    }

    /**
     * @brief 握手确认之后，对端吹的延迟被夹到 max_ack_delay 以内
     */
    TEST(QuicRecovery, ClampsAcknowledgementDelayAfterHandshakeConfirmation)
    {
        QuicRecovery unconfirmed;
        QuicRecovery confirmed;
        for (QuicRecovery *recovery: {&unconfirmed, &confirmed})
        {
            recovery->onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
            std::ignore = recovery->onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(20), QuicTime{0});
        }
        confirmed.onHandshakeConfirmed(milliseconds(25));

        // 对端报告 1000ms 延迟：确认前全信（减不动，因为 60 < 20+1000），确认后夹到 25ms（60 >= 45 → adjusted=35ms）
        for (QuicRecovery *recovery: {&unconfirmed, &confirmed})
        {
            recovery->onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(50)));
            std::ignore = recovery->onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), milliseconds(110), milliseconds(1000));
        }
        // 确认后的那条：延迟夹到 25ms → adjusted=35ms → smoothed = 20000 + 15000/8 = 21875
        EXPECT_EQ(confirmed.roundTripTimeEstimate().smoothed, milliseconds(21) + QuicTime{875});
        // 确认前的那条：1000ms 的延迟减不动（60 < 20+1000）→ adjusted=60ms → smoothed = 20000 + 40000/8 = 25000
        EXPECT_EQ(unconfirmed.roundTripTimeEstimate().smoothed, milliseconds(25));
    }

    /**
     * @brief 包号阈值：确认值比它大 3 个包以上即判丢，不足阈值的留到时间判据
     */
    TEST(QuicRecovery, DeclaresPacketsLostByPacketThreshold)
    {
        QuicRecovery recovery;
        for (std::uint64_t packetNumber = 0; packetNumber <= 4; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(packetNumber, QuicTime{0}));
        }
        const auto update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(4), milliseconds(10), QuicTime{0});

        // 4 >= 0+3 与 4 >= 1+3 成立；2、3 还差一点，交给时间阈值
        ASSERT_EQ(update.lost.size(), 2U);
        EXPECT_EQ(update.lost[0].packetNumber, 0U);
        EXPECT_EQ(update.lost[1].packetNumber, 1U);
        ASSERT_EQ(update.acknowledged.size(), 1U);
        EXPECT_EQ(update.acknowledged[0].packetNumber, 4U);
        EXPECT_TRUE(recovery.nextDeadline().has_value()) << "还有包没到判丢条件，必须留一个定时器";
        EXPECT_EQ(recovery.inFlightByteCount(), 200U) << "确认与判丢都要把在途字节还回来";
    }

    /**
     * @brief 时间阈值：判丢时刻是 send_time + max(9/8*rtt, kGranularity)
     */
    TEST(QuicRecovery, DeclaresPacketsLostByTimeThreshold)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, QuicTime{0}));
        const auto update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), milliseconds(10), QuicTime{0});
        // 样本 10ms → smoothed=10ms；loss_delay = 9/8*10ms = 11250us，包号阈值又不够（1 < 0+3）
        EXPECT_TRUE(update.lost.empty());
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(11) + QuicTime{250});

        // 提前到点：什么都不做，也不许把包判丢
        const QuicRecoveryTimeoutAction early = recovery.onDeadlineReached(milliseconds(11));
        EXPECT_TRUE(early.lost.empty());
        EXPECT_FALSE(early.isProbeTimeout);

        const QuicRecoveryTimeoutAction action = recovery.onDeadlineReached(milliseconds(11) + QuicTime{250});
        ASSERT_EQ(action.lost.size(), 1U);
        EXPECT_EQ(action.lost[0].packetNumber, 0U);
        EXPECT_FALSE(action.isProbeTimeout) << "时间阈值判丢优先于探测超时，两者不该同时报（§6.2.1）";
    }

    /**
     * @brief 两个被判丢的包之间没有任何确认、且相隔超过持久拥塞时长时判成持久拥塞（§7.6.2）
     * @details 时长这条判据要能算出来才算数：先采一个 10ms 的样本（首样本直接重置估算 →
     *          smoothed=10ms、rttvar=5ms），§7.6.1 的时长一律带上对端的 max_ack_delay（没交进来就按
     *          RFC 9000 §18.2 的默认 25ms），于是本帧之前的基准 = 10ms + max(4×5ms, 1ms) + 25ms = 60ms，
     *          时长 = 60ms × 3 = 180ms；本帧又先采了一个 10ms 样本（rttvar 折到 3.75ms），
     *          判据按更新后的估算比 = (10 + 15 + 25) × 3 = 150ms。
     *          包 1 在 20ms 发出、包 4 在 220ms 发出 ⇒ 相隔 200ms，越过两个数中的哪一个都够。
     */
    TEST(QuicRecovery, DeclaresPersistentCongestionWhenLossesSpanMoreThanThreeProbes)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(200)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(210)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(4, milliseconds(220)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(5, milliseconds(230)));

        const QuicAcknowledgementUpdate update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(5, {{5, 5}}), milliseconds(240), QuicTime{0});

        ASSERT_GE(update.lost.size(), 2U) << "至少要判掉包 1 与包 2 这两个触发确认的包，实判 " << update.lost.size() << " 个";
        EXPECT_TRUE(update.isPersistentCongestionDetected) << "两次判丢相隔 200ms、持久拥塞时长 150ms，这一帧就该判成持久拥塞";

        // 判成之后那一段必须清账：同一对包不许连着判第二次，否则每帧都把窗口按回最小窗
        const QuicAcknowledgementUpdate followUp = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(5), milliseconds(250), QuicTime{0});
        EXPECT_FALSE(followUp.isPersistentCongestionDetected) << "上一次判成之后锚点没清，同一对判丢被重复计入持久拥塞";
    }

    /**
     * @brief 相隔不足持久拥塞时长时不判：普通判丢的「减半」处置就够（§7.6.1 的时长那一格）
     * @details 与对照那条只差一个数——最后一个被判丢的包在 80ms 发出而不是 220ms ⇒ 相隔 60ms，
     *          而时长是 150ms（同一套估算，见对照那条的算法）。
     *          这一格是「时长判据不是摆设」的对照：把倍率写成 1、或把比较写成 `>=` 都会在这里露出来。
     */
    TEST(QuicRecovery, WithholdsPersistentCongestionInsideTheDuration)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(60)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(70)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(4, milliseconds(80)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(5, milliseconds(90)));

        const QuicAcknowledgementUpdate update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(5, {{5, 5}}), milliseconds(100), QuicTime{0});

        ASSERT_GE(update.lost.size(), 2U) << "判丢集合应当与对照那条一致（至少两个）";
        EXPECT_FALSE(update.isPersistentCongestionDetected) << "相隔 60ms 没到 150ms 的时长，不该把窗口按回最小窗";
    }

    /**
     * @brief 段里出现过一次确认就不判：那说明这条路在段内通过过（§7.6.2 的第一条并列条件）
     * @details 这一格盯的是「先并段、再记确认、最后比时长」这个顺序。按旧写法（拿**上一趟**的段终点筛本帧
     *          的确认）会是另一副样子：第一趟判掉包 1 与包 4（20ms 与 50ms），本帧确认到的包 5 在 60ms
     *          发出、落在段外 ⇒ 水位记成 60ms；第二趟判掉包 6 与包 8（100ms 与 210ms）把段终点推到 210ms，
     *          于是 60ms 这个确认就落进了 (20ms, 210ms] 这段里 ⇒ 规范要的答案是「不判」。
     *          时长这边是够的（相隔 190ms > 150ms），所以这条红的只能怪那格「段内有没有确认」。
     */
    TEST(QuicRecovery, WithholdsPersistentCongestionWhenAPacketInsideTheGapWasAcknowledged)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(30)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(40)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(4, milliseconds(50)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(5, milliseconds(60)));
        const QuicAcknowledgementUpdate first = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(5, {{5, 5}}), milliseconds(70), QuicTime{0});
        ASSERT_GE(first.lost.size(), 2U) << "第一趟就要判掉两个包把这一段开起来，否则后面那格无从谈起";
        EXPECT_FALSE(first.isPersistentCongestionDetected) << "相隔 30ms 远没到时长，也不该在这趟判成";

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(6, milliseconds(100)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(7, milliseconds(200)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(8, milliseconds(210)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(9, milliseconds(220)));
        const QuicAcknowledgementUpdate second = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(9, {{9, 9}}), milliseconds(230), QuicTime{0});

        ASSERT_GE(second.lost.size(), 2U) << "第二趟该把包 6 到包 8 判丢，实判 " << second.lost.size() << " 个";
        EXPECT_FALSE(second.isPersistentCongestionDetected) << "段内 (20ms, 210ms] 出现过一次 60ms 发出的确认，这条路通过过，不该按持久拥塞重启";
    }

    /**
     * @brief 定时器那条路只把新判丢并进这一段，结论要等下一帧确认（§7.6.2 的「收到确认之后」）
     * @details 包 1（20ms）由第一帧判丢并把段开起来；包 2（200ms）当时还没到时间阈值，恢复层把
     *          211.25ms 那个时刻记成待判丢的闹钟（200ms + 9/8×10ms）。定时器到点把它判掉，段的跨度
     *          推到 180ms——这一趟**不给**结论（`QuicRecoveryTimeoutAction` 里根本没有那一格）；
     *          紧接着的一帧重复确认什么新包都没确认到，但规范那一格「收到确认之后」满足了 ⇒ 就在这里建立。
     *          两格各自都要：漏了定时器那一侧的并进锚点，这条要等下一次真判丢才判得出；
     *          在定时器那一侧就下结论，红的是「到点了却没重启」那一格。
     */
    TEST(QuicRecovery, EstablishesPersistentCongestionOnTheAcknowledgementAfterTheLossTimerExtendedTheGap)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(200)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(201)));
        const QuicAcknowledgementUpdate first = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(3, {{3, 3}}), milliseconds(210), QuicTime{0});
        ASSERT_EQ(first.lost.size(), 1U) << "这一帧只该判掉包 1：包 2 在 200ms 发出，还没到时间阈值";
        EXPECT_FALSE(first.isPersistentCongestionDetected) << "只有一个判丢的包，构不成「两个相隔超过时长」";

        const std::optional<QuicTime> lossDeadline = recovery.nextDeadline();
        ASSERT_TRUE(lossDeadline.has_value()) << "包 2 还悬着，必须留下按时间阈值判丢的闹钟";
        const QuicRecoveryTimeoutAction timer = recovery.onDeadlineReached(*lossDeadline);
        ASSERT_EQ(timer.lost.size(), 1U) << "到点该把包 2 判丢";

        const QuicAcknowledgementUpdate afterTimer = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(3), milliseconds(220), QuicTime{0});
        EXPECT_TRUE(afterTimer.acknowledged.empty()) << "这条重复确认不该再产出样本，也不该有新判丢";
        EXPECT_TRUE(afterTimer.isPersistentCongestionDetected) << "定时器把段推到 180ms（>150ms 的时长），下一帧确认就该建立持久拥塞";
    }

    /**
     * @brief 只带 ACK 的包不算那两个端点：对端本来就不必按时确认它（§7.6.2「MUST be ack-eliciting」）
     * @details 包 2 在 200ms 发出但不触发确认。它被判丢之后段的跨度会到 180ms，越过时长——
     *          把这种包当端点就等于拿「对端本来就没答应要确认」的那段空白去判死这条路。
     */
    TEST(QuicRecovery, IgnoresNonAckElicitingPacketsWhenAnchoringTheLossGap)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(20)));
        QuicSentPacketInfo acknowledgementOnly = makeSentPacket(2, milliseconds(200));
        acknowledgementOnly.isAckEliciting     = false;
        recovery.onPacketSent(QuicRecoverySpace::Initial, acknowledgementOnly);
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(201)));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(3, {{3, 3}}), milliseconds(210), QuicTime{0});

        const std::optional<QuicTime> lossDeadline = recovery.nextDeadline();
        ASSERT_TRUE(lossDeadline.has_value()) << "包 2 还悬着，必须留下按时间阈值判丢的闹钟";
        ASSERT_EQ(recovery.onDeadlineReached(*lossDeadline).lost.size(), 1U) << "到点该把包 2 判丢";

        const QuicAcknowledgementUpdate afterTimer = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(3), milliseconds(220), QuicTime{0});
        EXPECT_FALSE(afterTimer.isPersistentCongestionDetected) << "段里只有一个触发确认的包（包 1），不该拿只带 ACK 的那一条当第二个端点";
    }

    /**
     * @brief 路径 MTU 探针不算那两个端点：按尺寸被丢是预期结局，不是「这条路不通」（RFC 9000 §14.4）
     * @details 形状与上面那条「定时器把段推开之后建立持久拥塞」逐字节相同——包 1 在 20ms、包 2 在 200ms，
     *          跨度 180ms 越过 150ms 的时长——只把那两个端点换成探针。探针就是拿去撞一个更大尺寸的，
     *          路由器照尺寸丢正是它的工作方式；把它算进「两个包之间一个确认都没有」的那一段，一条
     *          8952 字节的探针就足以把一条好路判死并按最小窗重启。对照：同一形状换成正常包会建立
     *          （见 `EstablishesPersistentCongestionOnTheAcknowledgementAfterTheLossTimerExtendedTheGap`）。
     */
    TEST(QuicRecovery, IgnoresPathMtuProbesWhenAnchoringThePersistentCongestionGap)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(10), QuicTime{0});

        QuicSentPacketInfo firstProbe = makeSentPacket(1, milliseconds(20));
        firstProbe.isPathMtuProbe     = true;
        recovery.onPacketSent(QuicRecoverySpace::Initial, firstProbe);
        QuicSentPacketInfo secondProbe = makeSentPacket(2, milliseconds(200));
        secondProbe.isPathMtuProbe     = true;
        recovery.onPacketSent(QuicRecoverySpace::Initial, secondProbe);
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(201)));

        const QuicAcknowledgementUpdate first = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(3, {{3, 3}}), milliseconds(210), QuicTime{0});
        ASSERT_EQ(first.lost.size(), 1U) << "这一帧该按时间阈值把包 1 判掉，实判 " << first.lost.size() << " 个";
        EXPECT_FALSE(first.isPersistentCongestionDetected) << "只有一个判丢的包，构不成「两个相隔超过时长」";

        const std::optional<QuicTime> lossDeadline = recovery.nextDeadline();
        ASSERT_TRUE(lossDeadline.has_value()) << "包 2 还悬着，必须留下按时间阈值判丢的闹钟";
        ASSERT_EQ(recovery.onDeadlineReached(*lossDeadline).lost.size(), 1U) << "到点该把包 2 判丢";

        const QuicAcknowledgementUpdate afterTimer = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(3), milliseconds(220), QuicTime{0});
        EXPECT_TRUE(afterTimer.acknowledged.empty()) << "这条重复确认不该再产出样本，也不该有新判丢";
        EXPECT_FALSE(afterTimer.isPersistentCongestionDetected) << "段的两个端点都是探针：尺寸过不去不等于这条路不通";
    }

    /**
     * @brief 落下第一个 RTT 样本的那一帧不建立持久拥塞，哪怕这一帧判丢的包相隔 1010ms（§7.6.2 的样本那一格）
     * @details 段里的两个端点必须是「发出时已有一个先前的样本」的那些：第一样本之前用的是 kInitialRtt
     *          （333ms），拿它算出来的时长去比这段空白，等于用一把还没校准的尺判死这条路——规范那句
     *          「The persistent congestion period SHOULD NOT start until there is at least one RTT sample」
     *          说的就是这件事，理由是那样可能只用上太少几条探针。
     *          这一格钉的是「早于第一样本的判丢根本不开段」：时长那一格在这里是拦不住的
     *          （跨度 1010ms，而按本帧新估算算出的时长只有 (10 + 4×5 + 25) × 3 = 150ms）。
     */
    TEST(QuicRecovery, DoesNotStartThePersistentCongestionPeriodBeforeTheFirstRoundTripSample)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, QuicTime{0}));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(1000)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(1010)));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(4, milliseconds(1020)));

        // 这一帧既是本连接第一个 RTT 样本（1030 − 1020 = 10ms），又把包 1 到包 3 一起判丢
        const QuicAcknowledgementUpdate update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(4, {{4, 4}}), milliseconds(1030), QuicTime{0});

        ASSERT_GE(update.lost.size(), 2U) << "这一帧该把包 1 与包 2 都判丢，实判 " << update.lost.size() << " 个";
        EXPECT_TRUE(update.isRoundTripSampled) << "这一帧应当落下本连接第一个 RTT 样本";
        EXPECT_FALSE(update.isPersistentCongestionDetected) << "段里这两个包的发出时刻都早于第一个 RTT 样本，不该建立持久拥塞";
    }


    /**
     * @brief 标够 §13.4.2 建议的十条也接着标，直到某个失败探测器出结论（本仓对附录 A.4 的有意偏离）
     * @details 规范那一格「unknown」的作用是「试探窗用完先停标、等已标出去那些包的回话」，代价是
     *          **握手本身就常常吃掉十条**：等不到应用数据的包被标过，ECN 对真正要保护的那段流量等于没做。
     *          §13.4.2 明写「Endpoints can implement different methods」，这里改成一路标到有结论，
     *          靠两条失败判据收口（见下面两个用例）。这一格钉的就是「不按包数停标」。
     */
    TEST(QuicRecovery, KeepsMarkingEcnUntilAVerdictIsReached)
    {
        QuicRecovery recovery;
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointEctZero) << "开局在 Testing，第一条就该标 ECT(0)";

        for (std::uint64_t packetNumber = 0; packetNumber < 20; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(packetNumber, milliseconds(packetNumber)));
            EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointEctZero) << "第 " << packetNumber + 1 << " 条已发，没有结论就不该提前停标";
            EXPECT_EQ(recovery.ecnState(), QuicEcnState::Testing) << "只发包没收确认，验证还没走到任何一格";
        }
    }

    /**
     * @brief 标过的包全被判丢且够 §13.4.2 建议的十条，判验证失败并从此不标
     * @details 这是「本端不停标」的另一半代价：得有个探测器把「中间节点直接丢掉带标的包」这种路认出来。
     *          单条带标的包丢了不算证据（本来就会丢包），要的是「全丢且够多条」这个形状。
     *          12 条带标的包（0..11）之后再发一条没标的并确认它，判丢会把 12 条一起带走——越过十条这一格。
     */
    TEST(QuicRecovery, FailsEcnWhenEveryMarkedPacketIsDeclaredLost)
    {
        QuicRecovery recovery;
        for (std::uint64_t packetNumber = 0; packetNumber < 12; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(packetNumber, milliseconds(packetNumber)));
        }
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(12, milliseconds(12)));

        // 确认的是那条没标的包，所以这一帧不带计数不会走 §13.4.2.1 第一条；判丢把 12 条带标的包全带走
        const QuicAcknowledgementUpdate update =
                recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(12, {{12, 12}}), milliseconds(13), QuicTime{0});
        ASSERT_EQ(update.lost.size(), 12U) << "这一帧该把 12 条带标的包全判丢，实判 " << update.lost.size() << " 个";

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed) << "带标的包全被判丢，且条数越过 §13.4.2 建议的十条";
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointNotCapable) << "§13.4.2.2：验证失败必须停标";
    }

    /**
     * @brief 有条带标的包活着被确认过，其余全丢也不关 ECN：那条包已经证明这条路传得出带标的包
     * @details 这一格钉的是判据里的「相等」而不是「够十条」：只留条数那一格的话，一次普通丢包突发
     *          就把 ECN 关了，而那正是 ECN 最该起作用的时候（拥塞丢包与「路不支持标记」是两件事）。
     */
    TEST(QuicRecovery, KeepsEcnEnabledWhenOneMarkedPacketSurvivesTheRest)
    {
        QuicRecovery recovery;
        for (std::uint64_t packetNumber = 0; packetNumber < 12; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(packetNumber, milliseconds(packetNumber)));
        }
        // 包 0 被确认，且这一帧带着计数（ECT(0) 累计 1）：校验通过，转入 Capable
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(0, {{0, 0}}, std::array<std::uint64_t, 3>{1, 0, 0}), milliseconds(13),
                                                         QuicTime{0});
        ASSERT_EQ(recovery.ecnState(), QuicEcnState::Capable);

        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(12, milliseconds(30)));
        const QuicAcknowledgementUpdate update =
                recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(12, {{12, 12}}), milliseconds(50), QuicTime{0});
        ASSERT_EQ(update.lost.size(), 11U) << "剩下的 11 条带标的包全被判丢，实判 " << update.lost.size() << " 个";

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Capable) << "已有带标的包被确认，全丢判据的分母与分子不再相等，不该关 ECN";
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointEctZero) << "还开着就该继续标";
    }

    /**
     * @brief 外壳说本平台用不了 ECN 之后，恢复层从此不标，对端报回来的计数再漂亮也不改结论（disableEcn 的直测）
     * @details 这一格钉的是「关掉之后不会再被打开」：`processEcnFeedback` 转入 Capable 的条件里带着
     *          「本端确实标过包并被确认」，而关掉之后一条带标的包都没发出去，那条前提永远不成立。
     *          CE 那一格同理——没标过就没有认证过的带标包，§7.1 的反应不该发生。
     */
    TEST(QuicRecovery, KeepsEcnDisabledOnceTheShellTurnedItOff)
    {
        QuicRecovery recovery;
        recovery.disableEcn();
        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed);
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointNotCapable);

        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(0, QuicTime{0}));
        QuicAcknowledgementUpdate update;
        {
            // 交回一份「ECT(0) 1 条、CE 1 条」的计数：换作没关掉的本端，这会同时点亮 Capable 与拥塞事件
            const QuicAcknowledgementFrame acknowledgement = makeAcknowledgement(0, {{0, 0}}, std::array<std::uint64_t, 3>{1, 0, 1});
            update                                         = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, acknowledgement, milliseconds(10), QuicTime{0});
        }
        ASSERT_EQ(update.acknowledged.size(), 1U);
        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed) << "本端从没标过包，对端报什么都不能把它拉回 Capable";
        EXPECT_FALSE(update.ecnCongestionEventTime.has_value()) << "没标过就没有认证过的带标包，§7.1 的反应不该发生";
    }

    /**
     * @brief 本端标过的包被一帧不带计数的确认收走，判验证失败并从此不标（§13.4.2.1 第一条）
     * @details 这一格抓的是「路上有元件把 ECN 字段抹成 0」与「对端压根不报标记」——两种都在规范列出的
     *          检测对象里。另一臂同样要紧：确认的是**没标过**的包而不带计数，什么都不许发生，
     *          否则任何普通 ACK 都会把 ECN 判死。
     */
    TEST(QuicRecovery, FailsEcnValidationWhenMarkedPacketsAreAcknowledgedWithoutCounts)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(0, QuicTime{0}));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(1, QuicTime{0}));

        // 区间要盖住包 0 与包 1：只确认包 1 的话本帧没确认到任何带标的包，那一格反而不该触发
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(1, {{0, 1}}), milliseconds(10), QuicTime{0});

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed) << "包 0 是标着 ECT(0) 发出去的，这一帧却没带计数";
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointNotCapable) << "§13.4.2.2：验证失败必须停标";
    }

    /**
     * @brief 计数校验通过且确有带标的包被确认，才转入 Capable 并据 CE 增量触发一次拥塞事件（§7.1 + §B.7）
     * @details 拥塞信号到得比丢包早：CE 出现时那条包还被确认着，等判丢再降窗就把这条路的容量用完了。
     *          时刻取本帧最大确认值那条包的发出时刻（§B.7 的那一行），§7.3.2 的「一轮只降一次」据此判。
     */
    TEST(QuicRecovery, ReportsEcnCongestionEventWhenTheCeCountIncreases)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(0, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(1, milliseconds(30)));

        // ECT(0) 累计 2、CE 累计 1：增量盖得住本帧新确认的两条带标包，校验通过
        const QuicAcknowledgementUpdate first = recovery.onAcknowledgementReceived(
                QuicRecoverySpace::Application, makeAcknowledgement(1, {{1, 1}}, std::array<std::uint64_t, 3>{2, 0, 1}), milliseconds(40), QuicTime{0});

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Capable) << "校验通过且确有带标的包被确认，就该转入 Capable";
        ASSERT_TRUE(first.ecnCongestionEventTime.has_value()) << "CE 从 0 涨到 1，这一帧要触发一次拥塞事件";
        EXPECT_EQ(*first.ecnCongestionEventTime, milliseconds(30)) << "§B.7 取的是最大确认值那条包（包 1）的发出时刻";

        // 抬高最大确认值、再报一份 CE 没涨的计数：CE 不涨就不该再降一次窗（否则一条信号能被反复兑现）
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(2, milliseconds(50)));
        const QuicAcknowledgementUpdate second = recovery.onAcknowledgementReceived(
                QuicRecoverySpace::Application, makeAcknowledgement(2, {{2, 2}}, std::array<std::uint64_t, 3>{3, 0, 1}), milliseconds(60), QuicTime{0});
        EXPECT_FALSE(second.ecnCongestionEventTime.has_value()) << "CE 计数没涨，不该重复触发拥塞事件";
    }

    /**
     * @brief 计数盖不住本帧新确认的带标包数时判验证失败，且失败的那一份不许污染基准（§13.4.2.1 的增量判据）
     * @details 三条带标的包被这一帧一起确认（区间 {{0, 2}}），而对端只报「ECT(0) + CE 一共 1 条」——
     *          中间节点改写过标记。
     *          顺带钉住「失败即停标」：这一格红的是状态而不是时刻。
     */
    TEST(QuicRecovery, FailsEcnValidationWhenTheReportedIncreaseCannotCoverMarkedPackets)
    {
        QuicRecovery recovery;
        for (std::uint64_t packetNumber = 0; packetNumber < 3; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(packetNumber, milliseconds(packetNumber)));
        }

        const QuicAcknowledgementUpdate update = recovery.onAcknowledgementReceived(
                QuicRecoverySpace::Application, makeAcknowledgement(2, {{0, 2}}, std::array<std::uint64_t, 3>{1, 0, 0}), milliseconds(10), QuicTime{0});

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed) << "三条带标的包被确认，报上来的 ECT(0)+CE 只有 1 条";
        EXPECT_FALSE(update.ecnCongestionEventTime.has_value()) << "校验没过就不该拿这份计数去做拥塞反应";
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointNotCapable);
    }

    /**
     * @brief 没抬高最大确认值的那一帧不许用来判 ECN 验证失败（§13.4.2.1 的 MUST NOT）
     * @details 乱序后到的旧 ACK 报了一份比现在小的计数：拿它比增量会判出一个假失败，从此停标。
     *          要让先到的那一帧先把基准立起来，再让旧帧撞上去——这一格红的是「状态被假失败改掉」。
     */
    TEST(QuicRecovery, DoesNotFailEcnValidationFromAReorderedAcknowledgement)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(0, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(1, milliseconds(30)));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(2, milliseconds(40)));
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(2, {{2, 2}}, std::array<std::uint64_t, 3>{3, 0, 0}), milliseconds(50),
                                                         QuicTime{0});
        ASSERT_EQ(recovery.ecnState(), QuicEcnState::Capable) << "先让校验通过的那一帧把基准立起来";

        // 同一空间再来一帧只确认到包 1 的旧 ACK：计数比基准小，但它没抬高最大确认值
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(1, {{1, 1}}, std::array<std::uint64_t, 3>{1, 0, 0}), milliseconds(60),
                                                         QuicTime{0});

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Capable) << "乱序后到的旧帧把 ECN 判成失败了，规范明写不许";
        EXPECT_EQ(recovery.outgoingEcnCodepoint(), Platform::kEcnCodepointEctZero) << "状态没被改坏，标也就没停";
    }

    /**
     * @brief 报上来的 ECT 总量超过本端发出去带过这一格的包总数，判验证失败（§13.4.2.1 末段）
     * @details 这一格抓的是「中间节点凭空把包改成 ECT(0)」：本端一条都没标，对端却报了 ECT(0) 计数。
     *          规范同时明写计数**允许大于**本帧新确认的包数（确认帧可能丢过），所以只有超总量才失败——
     *          另一臂（未确认的包带标，报得比本帧确认数大仍然算通过）一并钉在这里。
     */
    TEST(QuicRecovery, FailsEcnValidationWhenReportedEctTotalExceedsWhatThisEndSent)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(0, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(1, milliseconds(30)));

        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Application, makeAcknowledgement(1, {{1, 1}}, std::array<std::uint64_t, 3>{5, 0, 0}), milliseconds(40),
                                                         QuicTime{0});

        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Failed) << "本端一条都没标 ECT(0)，对端却报了 5 条";
    }

    /**
     * @brief 对端一条都没报过时不触发 CE 反应，即使 CE 计数在涨（§7.1 的第一句限定）
     * @details 规范那句「只有路径已验证支持 ECN 才把 CE 当拥塞信号」挡的就是这一形：包都没被确认过，
     *          涨上来的 CE 无从校验，据此降窗等于让一份来历不明的计数决定拥塞。
     */
    TEST(QuicRecovery, WithholdsEcnCongestionReactionBeforeAnyMarkedPacketIsAcknowledged)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(0, milliseconds(20)));
        recovery.onPacketSent(QuicRecoverySpace::Application, makeMarkedSentPacket(1, milliseconds(30)));

        // 这一帧只确认到没标过的包 0，带标的那条还悬着；CE 却已经报上来了
        const QuicAcknowledgementUpdate update = recovery.onAcknowledgementReceived(
                QuicRecoverySpace::Application, makeAcknowledgement(0, {{0, 0}}, std::array<std::uint64_t, 3>{0, 0, 4}), milliseconds(40), QuicTime{0});

        EXPECT_FALSE(update.ecnCongestionEventTime.has_value()) << "一条带标的包都还没被确认，不该据这份来路不明的 CE 降窗";
        EXPECT_EQ(recovery.ecnState(), QuicEcnState::Testing) << "没确认到带标的包就不该转入 Capable（§A.4 的那一除句）";
    }

    /**
     * @brief 粒度下限：RTT 极小时也不能把 loss_delay 算成 0
     */
    TEST(QuicRecovery, KeepsTimeThresholdAtLeastOneTimerTick)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, QuicTime{0}));
        // 0 微秒的样本：9/8*0 = 0，但 kGranularity 兜住（§6.1.2 的 MUST）
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), QuicTime{0}, QuicTime{0});
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(1));
    }

    /**
     * @brief 后到的确认清空了积压，之前算出的判丢时刻必须一起作废
     * @details 留着旧时刻会有两个害处：定时器白醒，而且按 §6.2.1 的优先级它还会压住探测超时，
     *          于是真正在途的包再也等不到探针
     */
    TEST(QuicRecovery, DropsStaleLossTimeWhenLaterAcknowledgementEmptiesTheBacklog)
    {
        QuicRecovery recovery;
        for (std::uint64_t packetNumber = 0; packetNumber <= 2; ++packetNumber)
        {
            recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(packetNumber, QuicTime{0}));
        }
        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(2), milliseconds(10), QuicTime{0});
        // 包 0、1 还不够包号阈值（2 < 0+3），于是留下一个按时间判丢的定时点
        ASSERT_EQ(recovery.nextDeadline(), milliseconds(11) + QuicTime{250});

        std::ignore = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(1, {{0, 1}}), milliseconds(11), QuicTime{0});
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "积压已清空，旧的判丢时刻不该继续武装定时器";

        // 之后新发的包要按新的时刻重新算，而不是沿用已作废的那条。
        // 第二个样本 11ms → smoothed=10000+1000/8=10125、rttvar=(15000+1000)/4=4000
        // → PTO 基准 = 10125 + max(4*4000, 1000) = 26125
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(3, milliseconds(20)));
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(46) + QuicTime{125});
    }

    /**
     * @brief 重复确认同一批包不再采样本（§5.1）
     */
    TEST(QuicRecovery, DoesNotSampleTwiceForTheSameAcknowledgement)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        const auto first = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(30), QuicTime{0});
        ASSERT_TRUE(first.isRoundTripSampled);

        // 同一个 ACK 再来一遍（对端重发确认是常态）：没有新确认，也就不该有样本
        const auto second = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(0), milliseconds(9000), QuicTime{0});
        EXPECT_TRUE(second.acknowledged.empty());
        EXPECT_FALSE(second.isRoundTripSampled);
        EXPECT_EQ(recovery.roundTripTimeEstimate().smoothed, milliseconds(30));
    }

    /**
     * @brief 探测超时到期把等待时间翻倍，收到确认后归零
     */
    TEST(QuicRecovery, DoublesProbeTimeoutOnExpiryAndResetsOnAcknowledgement)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        ASSERT_EQ(recovery.nextDeadline(), milliseconds(999));

        const QuicRecoveryTimeoutAction expired = recovery.onDeadlineReached(milliseconds(999));
        EXPECT_TRUE(expired.isProbeTimeout);
        EXPECT_EQ(expired.probeSpace, QuicRecoverySpace::Initial);
        EXPECT_TRUE(expired.lost.empty()) << "探测超时不代表丢包（§6.2）";

        // 退避一次：还是那个包的发送时刻起算，但周期翻倍
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(1998));

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(1000)));
        const auto update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, acknowledgementOf(1), milliseconds(1010), QuicTime{0});
        // 包号 0 离确认值不到 3 个包，但发出时刻早已超出 loss_delay，于是被判丢
        ASSERT_EQ(update.lost.size(), 1U);
        EXPECT_EQ(update.lost[0].packetNumber, 0U);
        // 样本 10ms → smoothed=10ms、rttvar=5ms → PTO = 10000 + max(20000,1000) = 30000，退避已归零
        EXPECT_EQ(recovery.roundTripTimeEstimate().probeTimeout, milliseconds(30));
        // 在途的触发确认包全清了，定时器就该撤掉：留着它会空转出一堆没根据的 PTO
        EXPECT_FALSE(recovery.nextDeadline().has_value());

        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(2, milliseconds(1020)));
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(1020) + milliseconds(30));
    }

    /**
     * @brief 进入用空间在握手确认前不武装探测超时（§6.2.1）
     */
    TEST(QuicRecovery, WithholdsApplicationSpaceProbeUntilHandshakeConfirmed)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Application, makeSentPacket(0, QuicTime{0}));
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "确认前重发对端还解不开，不该武装 PTO";

        recovery.onHandshakeConfirmed(milliseconds(25));
        // 确认后进入用空间要加 max_ack_delay：999000 + 25000
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(1024));
    }

    /**
     * @brief 丢弃一个空间要连账一起清掉，定时器只反映剩下的空间
     */
    TEST(QuicRecovery, DiscardSpaceReleasesItsAccounting)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{0}));
        recovery.onPacketSent(QuicRecoverySpace::Handshake, makeSentPacket(0, milliseconds(500)));
        ASSERT_EQ(recovery.inFlightByteCount(), 200U);
        ASSERT_EQ(recovery.nextDeadline(), milliseconds(999));

        recovery.discardSpace(QuicRecoverySpace::Initial);
        EXPECT_EQ(recovery.inFlightByteCount(), 100U);
        // 剩下 Handshake 的那条：500ms + 999ms
        EXPECT_EQ(recovery.nextDeadline(), milliseconds(1499));

        recovery.discardSpace(QuicRecoverySpace::Handshake);
        EXPECT_FALSE(recovery.nextDeadline().has_value());
        EXPECT_EQ(recovery.inFlightByteCount(), 0U);
    }

    /**
     * @brief 非触发确认的包不武装探测超时，但确认它时照样回收在途字节
     */
    TEST(QuicRecovery, DoesNotArmProbeForNonAckElicitingPackets)
    {
        QuicRecovery       recovery;
        QuicSentPacketInfo paddingOnly = makeSentPacket(0, QuicTime{0});
        paddingOnly.isAckEliciting     = false;
        recovery.onPacketSent(QuicRecoverySpace::Initial, paddingOnly);
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "只发 PADDING/ACK 的包不该指望对端确认（§13.2）";

        // 跟着一条触发确认的包进来，两包都在途；ACK 一次收掉两个
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, milliseconds(10)));
        ASSERT_TRUE(recovery.nextDeadline().has_value());
        const auto update = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(1, {{0, 1}}), milliseconds(20), QuicTime{0});
        EXPECT_EQ(update.acknowledged.size(), 2U);
        EXPECT_TRUE(update.isRoundTripSampled);
        EXPECT_FALSE(recovery.nextDeadline().has_value());
        EXPECT_EQ(recovery.inFlightByteCount(), 0U);

        // §5.1 的第二条：只确认了非触发确认的包时不许再采样本——这种 ACK 的延迟字段可以任意大
        QuicSentPacketInfo paddingPacket = makeSentPacket(2, milliseconds(500), 60);
        paddingPacket.isAckEliciting     = false; // 只有 PADDING：对端不会为它发确认，报告延迟也就无从解释
        recovery.onPacketSent(QuicRecoverySpace::Initial, paddingPacket);
        const auto paddingAcknowledgement = makeAcknowledgement(2, {{2, 2}});
        const auto third                  = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, paddingAcknowledgement, milliseconds(900), QuicTime{0});
        EXPECT_EQ(third.acknowledged.size(), 1U);
        EXPECT_FALSE(third.isRoundTripSampled);
        EXPECT_EQ(recovery.roundTripTimeEstimate().smoothed, milliseconds(10)) << "没触发确认的包不该抬高估算";
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "在途的只有不触发确认的包，不该武装任何定时器";
    }

    /**
     * @brief 重复的 ACK 不再采样本，但判丢照做：§A.7 的 OnAckReceived 第 5 步是无条件的
     * @details 只看「有新确认才判丢」会把补发推迟到定时器，白慢一个粒度
     */
    TEST(QuicRecovery, DetectsLossOnRedundantAcknowledgement)
    {
        QuicRecovery recovery;
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(0, QuicTime{10500}));
        recovery.onPacketSent(QuicRecoverySpace::Initial, makeSentPacket(1, QuicTime{10800}));

        // 第一帧只认到 1 号：样本 200 微秒，时间阈值被 1 毫秒的粒度抬着，10500 那包还差一点
        const auto first = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(1, {{1, 1}}), QuicTime{11000}, QuicTime{0});
        ASSERT_TRUE(first.isRoundTripSampled);
        EXPECT_TRUE(first.lost.empty()) << "1 毫秒的窗口还没到，此时判丢太急";
        ASSERT_TRUE(recovery.nextDeadline().has_value());
        EXPECT_EQ(*recovery.nextDeadline(), QuicTime{11500}) << "待判丢时刻 = 发出时刻 + loss_delay";

        // 同一帧再来一次：没有新确认，也就不该再采样本，但空洞到这会儿已经过线
        const auto second = recovery.onAcknowledgementReceived(QuicRecoverySpace::Initial, makeAcknowledgement(1, {{1, 1}}), QuicTime{12000}, QuicTime{0});
        EXPECT_TRUE(second.acknowledged.empty());
        EXPECT_FALSE(second.isRoundTripSampled) << "§5.1：重复的 ACK 不许再采一次样本";
        ASSERT_EQ(second.lost.size(), 1U) << "重复的 ACK 也要把空洞判丢";
        EXPECT_EQ(second.lost.front().packetNumber, 0U);
        EXPECT_EQ(recovery.roundTripTimeEstimate().smoothed, QuicTime{200}) << "没有新样本时估算不该动";
        EXPECT_FALSE(recovery.nextDeadline().has_value()) << "在途清空后不该留着定时器";
    }
} // namespace AsynGyanis::Net
