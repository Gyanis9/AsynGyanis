// 本文件钉住已收包号区间集合的四件事：重复包号一定报 false、相邻的号并成一段（含把两段缝起来）、
// 额度从最老的一端截断而不动最新那些、以及连号流量下记账不再逐包碰堆。
// 判错任何一件都会直接改到线上的 ACK 帧：要么把重复包当新包二次交付，要么把对端已经确认到的区间报歪。

#include "Net/Quic/QuicReceivedPacketNumbers.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace AsynGyanis::Net
{
    using AsynGyanis::TestSupport::kMeasurementIterations;
    using AsynGyanis::TestSupport::measurePerOperation;

    /**
     * @brief 落进已记区间之内、或与两端相邻的包号各是什么结果
     */
    TEST(QuicReceivedPacketNumbers, TracksRunsAndRejectsRepeats)
    {
        QuicReceivedPacketNumbers tracked;
        EXPECT_TRUE(tracked.insert(10ULL));
        EXPECT_TRUE(tracked.insert(11ULL));
        EXPECT_TRUE(tracked.insert(12ULL));
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 3U) << "连号三个应当记成三个包号";
        EXPECT_EQ(tracked.ranges().size(), 1U) << "连号必须并成一段，否则滞留就是按包号线性涨的";

        // 区间之内的重复包号（含被并进去的那些）都必须报「已见过」：调用方靠它整包丢弃
        EXPECT_FALSE(tracked.insert(10ULL));
        EXPECT_FALSE(tracked.insert(11ULL));
        EXPECT_FALSE(tracked.insert(12ULL));
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 3U) << "重复包号不能把额度吃掉";
    }

    /**
     * @brief 补上中间那个空洞时，两段要被缝成一段
     */
    TEST(QuicReceivedPacketNumbers, BridgesTwoRangesWhenTheGapArrives)
    {
        QuicReceivedPacketNumbers tracked;
        ASSERT_TRUE(tracked.insert(10ULL));
        ASSERT_TRUE(tracked.insert(12ULL));
        ASSERT_EQ(tracked.ranges().size(), 2U) << "10 与 12 之间有空洞，此刻就是两段";

        ASSERT_TRUE(tracked.insert(11ULL));
        EXPECT_EQ(tracked.ranges().size(), 1U) << "缝上之后还留两段，就会把一段连号报成两个区间";
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 3U);
        ASSERT_EQ(tracked.ranges().size(), 1U);
        EXPECT_EQ(tracked.ranges().begin()->first, 10U);
        EXPECT_EQ(tracked.ranges().begin()->second, 12U);
    }

    /**
     * @brief 额度从最老的一端截断，最新的包号一个都不能少
     */
    TEST(QuicReceivedPacketNumbers, DropOldestUntilTrimsTheLowestEndOnly)
    {
        QuicReceivedPacketNumbers tracked;
        for (std::uint64_t packetNumber = 0; packetNumber < 10ULL; ++packetNumber)
        {
            ASSERT_TRUE(tracked.insert(packetNumber));
        }

        // 额度小于单段长度：截这段的开头，留下的仍是连续一段
        tracked.dropOldestUntil(4U);
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 4U);
        ASSERT_EQ(tracked.ranges().size(), 1U);
        EXPECT_EQ(tracked.ranges().begin()->first, 6U) << "该忘的是最老的那几个，留下的必须是 6..9";
        EXPECT_EQ(tracked.ranges().begin()->second, 9U);

        // 重复判定跟着截断走：被截掉的 5 号此刻算「没见过」，与逐个记号的老实现同一口径
        EXPECT_TRUE(tracked.insert(5ULL));
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 5U) << "5 号接回左端，不该新开一段";
        ASSERT_EQ(tracked.ranges().size(), 1U);
    }

    /**
     * @brief 超出最老一段的长度时整段丢弃，再往下一段继续裁
     */
    TEST(QuicReceivedPacketNumbers, DropOldestUntilErasesWholeRangesAsNeeded)
    {
        QuicReceivedPacketNumbers tracked;
        for (const std::uint64_t packetNumber: {0ULL, 1ULL, 5ULL, 6ULL, 7ULL, 20ULL})
        {
            ASSERT_TRUE(tracked.insert(packetNumber));
        }
        ASSERT_EQ(tracked.ranges().size(), 3U) << "[0,1] [5,7] [20,20] 三段";

        tracked.dropOldestUntil(4U);
        EXPECT_EQ(tracked.trackedPacketNumberCount(), 4U);
        // [0,1] 整段都在超额里，丢掉它正好降到 4，因此 [5,7] 一格都不该被牵连
        ASSERT_EQ(tracked.ranges().size(), 2U) << "应剩 [5,7] 与 [20,20]";
        EXPECT_EQ(tracked.ranges().begin()->first, 5U);
        EXPECT_EQ(tracked.ranges().begin()->second, 7U);
        EXPECT_EQ(tracked.ranges().rbegin()->second, 20U) << "最新那段必须原样留着";
    }

    /**
     * @brief 连号流量下，记一个包号还要不要碰堆
     * @details 这是收包路径上最后一笔逐包分配的去向：老写法每包一个树节点，连号也要几百 KB 滞留。
     *          区间表示下续接只改尾端的数，稳态一次都不申请。
     */
    TEST(QuicReceivedPacketNumbers, ConsecutiveInsertsDoNotAllocate)
    {
        QuicReceivedPacketNumbers tracked;
        ASSERT_TRUE(tracked.insert(0ULL));

        std::uint64_t nextPacketNumber = 1ULL;
        const auto    insertNext       = [&tracked, &nextPacketNumber]() -> std::size_t
        {
            const bool isNew = tracked.insert(nextPacketNumber);
            nextPacketNumber = nextPacketNumber + 1U < 1000ULL ? nextPacketNumber + 1U : 1ULL;
            return isNew ? 1U : 0U;
        };

        const auto profile = measurePerOperation(insertNext);
        std::printf("quic 记一个连号包号：每次 %llu 次分配 / %llu 字节\n", static_cast<unsigned long long>(profile.totalAllocations / kMeasurementIterations),
                    static_cast<unsigned long long>(profile.totalBytes / kMeasurementIterations));
        EXPECT_EQ(profile.totalAllocations, 0ULL) << "续接区间时又去申请节点了：每包还是要碰堆";
        // 999 次是新号（1..999 续进同一段）、最后那次绕回 1 是重复号：读数不满一千说明窗口跑的是这条路
        EXPECT_EQ(profile.resultSum, kMeasurementIterations - 1U) << "读数不是逐包记账本身，用例形状不对";
    }
} // namespace AsynGyanis::Net
