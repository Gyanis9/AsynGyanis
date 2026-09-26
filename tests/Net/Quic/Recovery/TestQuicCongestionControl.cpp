// TestQuicCongestionControl.cpp —— NewReno 拥塞控制（RFC 9002 §7.2–§7.5）用例
//
// 时间、包号、字节数全部注入，所以每条断言都是能手算的整数。覆盖：
//   1) 初始窗口与最小窗口的取值（§7.2：min(10×上限, max(2×上限, 14720))，最小 2×上限）；
//   2) 慢启动按确认到的字节数涨，但一帧最多涨一个数据报（§7.3.1）；
//   3) 只带 ACK 的包不计在途（§B.2），探针计在途但不被窗口阻塞（§7.5）；
//   4) 一次丢包把阈值与窗口各降一半、并进入恢复期；恢复期内既不再降也不涨（§7.3.2）；
//   5) 恢复期在「恢复期之后发出的包被确认」时结束，之后走 AIMD 的加法增长（§7.3.3）；
//   6) 降窗不会跌破最小窗口；在途超过窗口时余额收成 0 而不是回绕。
// 纯计算，不起网络也不依赖外部服务。

#include "Net/Quic/Recovery/QuicCongestionControl.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端目前唯一使用的数据报上限（没做 PMTU 探测，见 RFC 9000 §14.1）
        constexpr std::size_t kDatagramSize = 1200;

        /// §7.2 算出来的初始窗口：min(10×1200, max(2×1200, 14720)) = 12000
        constexpr std::size_t kInitialWindow = 12000;

        /**
         * @brief 造一个已发包记录
         * @param packetNumber 包号
         * @param timeSent 发出时刻
         * @param byteCount 字节数
         * @param isAckEliciting 是否触发确认（只带 ACK 的包不计在途）
         * @return QuicSentPacketInfo 交给拥塞控制器的凭据
         */
        QuicSentPacketInfo makePacket(const std::uint64_t packetNumber, const std::int64_t timeSent, const std::size_t byteCount, const bool isAckEliciting = true)
        {
            QuicSentPacketInfo packet;
            packet.packetNumber   = packetNumber;
            packet.timeSent       = QuicTime{timeSent};
            packet.byteCount      = byteCount;
            packet.isAckEliciting = isAckEliciting;
            return packet;
        }
    } // namespace

    /**
     * @brief 建好就是慢启动、窗口取 §7.2 的初始值，阈值则是「无穷大」
     */
    TEST(QuicCongestionControl, StartsInSlowStartWithTheInitialWindow)
    {
        const QuicCongestionControl congestion{kDatagramSize};

        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow);
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), std::numeric_limits<std::size_t>::max());
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::SlowStart);
        EXPECT_EQ(congestion.bytesInFlight(), 0U);
        EXPECT_EQ(congestion.remainingByteBudget(), kInitialWindow);
        EXPECT_TRUE(congestion.maySend(kDatagramSize));
    }

    /**
     * @brief 在途字节只算触发确认的包，超额时余额收成 0
     */
    TEST(QuicCongestionControl, CountsOnlyAckElicitingPacketsAsInFlight)
    {
        QuicCongestionControl congestion{kDatagramSize};
        congestion.onPacketSent(makePacket(0, 0, kDatagramSize, false));
        EXPECT_EQ(congestion.bytesInFlight(), 0U) << "只带 ACK 的包不该吃窗口（§B.2）";

        congestion.onPacketSent(makePacket(1, 1000, kDatagramSize));
        congestion.onPacketSent(makePacket(2, 2000, kDatagramSize));
        EXPECT_EQ(congestion.bytesInFlight(), 2 * kDatagramSize);
        EXPECT_EQ(congestion.remainingByteBudget(), kInitialWindow - 2 * kDatagramSize);

        // 探针可以在窗口满之后照发（§7.5），于是确实会出现超额：这时候不能有符号翻转
        for (std::uint64_t packetNumber = 3; packetNumber <= 12; ++packetNumber)
        {
            congestion.onPacketSent(makePacket(packetNumber, 3000, kDatagramSize));
        }
        EXPECT_EQ(congestion.bytesInFlight(), 12 * kDatagramSize);
        EXPECT_FALSE(congestion.maySend(1U)) << "已经压了 12 个数据报，一个都不该再许可";

        const auto drained = std::vector<QuicSentPacketInfo>{makePacket(3, 3000, kDatagramSize), makePacket(4, 3000, kDatagramSize)};
        congestion.onCongestionUpdate(drained, {}, QuicTime{9000});
        EXPECT_EQ(congestion.bytesInFlight(), 10 * kDatagramSize) << "确认掉的包该从在途里销账";
    }

    /**
     * @brief 慢启动一帧最多涨一个数据报，否则同一帧确认多个包会被算成好几倍
     */
    TEST(QuicCongestionControl, GrowsSlowStartByAtMostOneDatagramPerAcknowledgement)
    {
        QuicCongestionControl congestion{kDatagramSize};
        congestion.onPacketSent(makePacket(0, 0, 300));
        congestion.onPacketSent(makePacket(1, 0, 500));

        congestion.onCongestionUpdate({makePacket(0, 0, 300)}, {}, QuicTime{10000});
        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow + 300) << "确认多少涨多少";

        const std::vector<QuicSentPacketInfo> coalesced{makePacket(1, 0, 500)};
        congestion.onCongestionUpdate(coalesced, {}, QuicTime{11000});
        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow + 800);

        // 一帧里确认掉 5 个数据报：只该涨一个数据报的量
        std::vector<QuicSentPacketInfo> burst;
        for (std::uint64_t packetNumber = 2; packetNumber <= 6; ++packetNumber)
        {
            burst.push_back(makePacket(packetNumber, 20000, kDatagramSize));
            congestion.onPacketSent(makePacket(packetNumber, 20000, kDatagramSize));
        }
        congestion.onCongestionUpdate(burst, {}, QuicTime{30000});
        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow + 800 + kDatagramSize);
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::SlowStart);
    }

    /**
     * @brief 丢包把阈值与窗口各降一半并进恢复期，恢复期内不再降也不涨
     */
    TEST(QuicCongestionControl, HalvesWindowOncePerRecoveryPeriod)
    {
        QuicCongestionControl congestion{kDatagramSize};
        for (std::uint64_t packetNumber = 0; packetNumber < 5; ++packetNumber)
        {
            congestion.onPacketSent(makePacket(packetNumber, 1000 * static_cast<std::int64_t>(packetNumber), kDatagramSize));
        }

        const std::vector<QuicSentPacketInfo> lost{makePacket(0, 0, kDatagramSize)};
        congestion.onCongestionUpdate({makePacket(4, 4000, kDatagramSize)}, lost, QuicTime{9000});
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), kInitialWindow / 2) << "阈值取降窗前的一半（§7.3.2）";
        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow / 2);
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::Recovery);
        EXPECT_EQ(congestion.bytesInFlight(), 3 * kDatagramSize) << "确认与判丢都不再算在途";

        // 恢复期里又丢一包：不该二次降窗，也不该涨（§7.3.2 一轮拥塞只降一次）
        const std::vector<QuicSentPacketInfo> moreLost{makePacket(1, 1000, kDatagramSize)};
        congestion.onCongestionUpdate({makePacket(2, 2000, kDatagramSize)}, moreLost, QuicTime{12000});
        EXPECT_EQ(congestion.congestionWindowByteLength(), kInitialWindow / 2);
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), kInitialWindow / 2);
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::Recovery);
    }

    /**
     * @brief 恢复期靠「恢复期之后发出的包被确认」结束，之后按 AIMD 加法增长
     */
    TEST(QuicCongestionControl, LeavesRecoveryOnPacketSentAfterItAndGrowsAdditively)
    {
        QuicCongestionControl congestion{kDatagramSize};
        congestion.onPacketSent(makePacket(0, 0, kDatagramSize));
        congestion.onCongestionUpdate({}, {makePacket(0, 0, kDatagramSize)}, QuicTime{9000});
        ASSERT_EQ(congestion.congestionWindowByteLength(), kInitialWindow / 2);
        const std::size_t windowInRecovery = congestion.congestionWindowByteLength();

        // 恢复期之前发出的包被确认：状态不变，窗口也不涨
        congestion.onCongestionUpdate({makePacket(1, 5000, kDatagramSize)}, {}, QuicTime{10000});
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::Recovery);
        EXPECT_EQ(congestion.congestionWindowByteLength(), windowInRecovery);

        // 恢复期之后发出的包被确认：出圈，走拥塞避免。1200×1200/6000 = 240
        congestion.onCongestionUpdate({makePacket(2, 10000, kDatagramSize)}, {}, QuicTime{11000});
        EXPECT_EQ(congestion.phase(), QuicCongestionPhase::CongestionAvoidance);
        EXPECT_EQ(congestion.congestionWindowByteLength(), windowInRecovery + 240);
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), windowInRecovery);
    }

    /**
     * @brief 反复降窗不跌破最小窗口：2×数据报上限是地板
     */
    TEST(QuicCongestionControl, KeepsTheMinimumWindowWhenReducing)
    {
        QuicCongestionControl congestion{kDatagramSize};
        std::uint64_t         nextPacketNumber = 0;
        std::int64_t          now              = 0;
        // 一轮「丢包 → 恢复期之后发出的包被确认」：降半、出圈、AIMD 再涨一格，下一轮才继续降
        const auto endureOneRound = [&congestion, &nextPacketNumber, &now]
        {
            const QuicSentPacketInfo lost = makePacket(nextPacketNumber++, now, kDatagramSize);
            congestion.onPacketSent(lost);
            now += 1000;
            congestion.onCongestionUpdate({}, {lost}, QuicTime{now});
            now += 1000;
            const QuicSentPacketInfo later = makePacket(nextPacketNumber++, now, kDatagramSize);
            congestion.onPacketSent(later);
            now += 1000;
            congestion.onCongestionUpdate({later}, {}, QuicTime{now});
        };

        endureOneRound();
        EXPECT_EQ(congestion.congestionWindowByteLength(), 6240) << "12000 降半后出圈，AIMD 又涨了 1200×1200/6000";
        endureOneRound();
        EXPECT_EQ(congestion.congestionWindowByteLength(), 3581) << "6240 降半到 3120，再涨 1200×1200/3120 = 461";
        endureOneRound();
        EXPECT_EQ(congestion.slowStartThresholdByteLength(), 2 * kDatagramSize) << "3581 的一半只有 1790，被最小窗口 2400 抬住（§7.2）";
        EXPECT_EQ(congestion.congestionWindowByteLength(), 2 * kDatagramSize + 600);
        EXPECT_EQ(congestion.bytesInFlight(), 0U) << "每轮的包都在同一轮里销了账";
    }

    /**
     * @brief 数据报上限换小之后（学到对端的 max_udp_payload_size），初始窗口跟着换算法
     */
    TEST(QuicCongestionControl, DerivesTheInitialWindowFromTheDatagramSize)
    {
        const QuicCongestionControl small{1000};
        EXPECT_EQ(small.congestionWindowByteLength(), 10000) << "min(10×1000, max(2×1000, 14720)) = 10000";

        const QuicCongestionControl large{9000};
        EXPECT_EQ(large.congestionWindowByteLength(), 18000) << "两倍的 9000 已经大过 14720，取较大者（§7.2）";
    }
} // namespace AsynGyanis::Net
