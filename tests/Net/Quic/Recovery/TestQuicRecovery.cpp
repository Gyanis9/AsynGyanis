// TestQuicRecovery.cpp —— 恢复层（RFC 9002 §5–§6）用例
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

#include "Net/Quic/Recovery/QuicRecovery.h"

#include "Net/Quic/Codec/QuicFrame.h"

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
         * @return QuicAcknowledgementFrame 交给恢复层的帧
         */
        QuicAcknowledgementFrame makeAcknowledgement(const std::uint64_t largestAcknowledged, const std::vector<QuicAcknowledgementRange> &ranges)
        {
            QuicAcknowledgementFrame acknowledgement;
            acknowledgement.largestAcknowledgedPacketNumber = largestAcknowledged;
            acknowledgement.ranges                          = ranges;
            return acknowledgement;
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
