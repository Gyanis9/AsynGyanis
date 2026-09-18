// 聚合发送游标单元测试：部分写之后的段内偏移、跨段推进与完成判定
//
// 这段推进逻辑在回环上很难被自然触发，但它一旦写错就是**静默的数据错位**：线上字节流
// 多一段、少一段或顺序乱了都不会有任何错误返回。因此把账目抽成游标，用确定性用例逐条钉住。

#include "Core/Socket/VectoredSendCursor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 三段示例数据：长度互不整除，跨段推进的边界才好观察
        const std::string kFirstSegment(10, 'A');
        const std::string kSecondSegment(4, 'B');
        const std::string kThirdSegment(7, 'C');
    } // namespace

    /**
     * @brief 初始快照按序交出全部段，且不改写段内容
     */
    TEST(VectoredSendCursor, InitialSnapshotCarriesEverySegmentInOrder)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        const detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        EXPECT_FALSE(cursor.isFinished());
        EXPECT_EQ(cursor.totalLength(), kFirstSegment.size() + kSecondSegment.size() + kThirdSegment.size());
        EXPECT_EQ(cursor.sentLength(), 0U);

        std::array<Platform::Socket::WriteBuffer, 3> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 3U);
        EXPECT_EQ(pending[0].data, kFirstSegment.data());
        EXPECT_EQ(pending[0].length, kFirstSegment.size());
        EXPECT_EQ(pending[1].data, kSecondSegment.data());
        EXPECT_EQ(pending[1].length, kSecondSegment.size());
        EXPECT_EQ(pending[2].data, kThirdSegment.data());
        EXPECT_EQ(pending[2].length, kThirdSegment.size());
    }

    /**
     * @brief 段内部分写：首段只切掉已发的一截，后续段原样带上
     */
    TEST(VectoredSendCursor, PartialWriteTrimsOnlyThePendingSegment)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        cursor.advance(3);
        EXPECT_EQ(cursor.sentLength(), 3U);

        std::array<Platform::Socket::WriteBuffer, 3> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 3U);
        EXPECT_EQ(pending[0].data, kFirstSegment.data() + 3) << "首段应当从已发位置继续，而不是从头重发";
        EXPECT_EQ(pending[0].length, kFirstSegment.size() - 3);
        EXPECT_EQ(pending[1].data, kSecondSegment.data());
        EXPECT_EQ(pending[1].length, kSecondSegment.size());
    }

    /**
     * @brief 一次写恰好跨过一整段：游标落到下一段开头，且不再交出已发完的那段
     */
    TEST(VectoredSendCursor, AdvanceAcrossWholeSegmentSkipsIt)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        cursor.advance(kFirstSegment.size());
        std::array<Platform::Socket::WriteBuffer, 3> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 2U);
        EXPECT_EQ(pending[0].data, kSecondSegment.data());
        EXPECT_EQ(pending[0].length, kSecondSegment.size());
        EXPECT_EQ(pending[1].data, kThirdSegment.data());
    }

    /**
     * @brief 一次写跨过若干整段并停在其中某段中间：游标同时推进段号与段内偏移
     */
    TEST(VectoredSendCursor, AdvanceAcrossSeveralSegmentsStopsInsideTheTarget)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        // 跨过第一段与第二段，再在第三段里发出 2 字节
        cursor.advance(kFirstSegment.size() + kSecondSegment.size() + 2);

        std::array<Platform::Socket::WriteBuffer, 3> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 1U);
        EXPECT_EQ(pending[0].data, kThirdSegment.data() + 2);
        EXPECT_EQ(pending[0].length, kThirdSegment.size() - 2);
    }

    /**
     * @brief 零长度段不被交出，也不影响其余段的顺序与长度
     */
    TEST(VectoredSendCursor, ZeroLengthSegmentsAreSkipped)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), 0},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        std::array<Platform::Socket::WriteBuffer, 3> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 2U) << "零长度段不该占掉一次提交的段位";
        EXPECT_EQ(pending[0].data, kSecondSegment.data());
        EXPECT_EQ(pending[1].data, kThirdSegment.data());

        // 发完数据段之后即完成：零长度段不该让游标停在「没发完」上
        cursor.advance(kSecondSegment.size() + kThirdSegment.size());
        EXPECT_TRUE(cursor.isFinished());
        EXPECT_EQ(cursor.sentLength(), kSecondSegment.size() + kThirdSegment.size());
    }

    /**
     * @brief 推进到末尾即完成，且推进量为 0 是空操作
     */
    TEST(VectoredSendCursor, ReachingTheEndFinishesAndZeroAdvanceDoesNothing)
    {
        const std::array<Platform::Socket::WriteBuffer, 2> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
        }};
        detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        cursor.advance(0);
        EXPECT_EQ(cursor.sentLength(), 0U);
        EXPECT_FALSE(cursor.isFinished());

        cursor.advance(kFirstSegment.size() + kSecondSegment.size());
        EXPECT_TRUE(cursor.isFinished());
        EXPECT_EQ(cursor.sentLength(), kFirstSegment.size() + kSecondSegment.size());

        // 完成之后再推进不会把已发长度算多（调用方拿到多算的长度会以为发得比实际多）
        cursor.advance(5);
        EXPECT_EQ(cursor.sentLength(), kFirstSegment.size() + kSecondSegment.size());
    }

    /**
     * @brief 快照容量不足时按序截断，不越界写
     */
    TEST(VectoredSendCursor, SnapshotRespectsCapacity)
    {
        const std::array<Platform::Socket::WriteBuffer, 3> buffers{{
                {kFirstSegment.data(), kFirstSegment.size()},
                {kSecondSegment.data(), kSecondSegment.size()},
                {kThirdSegment.data(), kThirdSegment.size()},
        }};
        const detail::VectoredSendCursor cursor(buffers.data(), buffers.size());

        std::array<Platform::Socket::WriteBuffer, 2> pending{};
        ASSERT_EQ(cursor.snapshotPending(pending.data(), pending.size()), 2U);
        EXPECT_EQ(pending[0].data, kFirstSegment.data());
        EXPECT_EQ(pending[1].data, kSecondSegment.data());
    }

} // namespace AsynGyanis::Core
