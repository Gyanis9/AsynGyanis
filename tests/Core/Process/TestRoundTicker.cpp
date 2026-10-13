// RoundTicker 的装配用例：真起线程池、每条循环挂一拍、凑齐一轮才回调。
// 纯判据（completesRound）由 TestServiceWatchdog 的转发面钉住；这里钉的是「装到真循环上确实会转」
// 这一格——两条循环都在跑时轮数要持续增长，重复 arm 不把一轮挂成两条。
// 跨平台：RoundTicker 只用协程与定时器，没有平台专属部分。

#include "Core/Process/RoundTicker.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace AsynGyanis::Core
{
    /**
     * @brief 两条循环的真线程池上挂节拍：凑齐的轮数要随时间增长，回调次数与之一致
     */
    TEST(RoundTicker, CompletesRoundsWhenEveryLoopTicks)
    {
        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        std::atomic<std::uint64_t> callbackCount{0};
        RoundTicker                ticker(pool, [&callbackCount] { callbackCount.fetch_add(1, std::memory_order_relaxed); });
        ASSERT_TRUE(ticker.arm(std::chrono::milliseconds{10}));
        EXPECT_TRUE(ticker.isArmed());
        EXPECT_EQ(ticker.armedLoopCount(), 2U) << "两条循环各占一拍";

        EXPECT_TRUE(TestSupport::waitForCondition([&ticker] { return ticker.completedRoundCount() >= 3; })) << "两条循环都在跑，凑齐的轮数该持续增长";
        EXPECT_GE(callbackCount.load(std::memory_order_relaxed), 1U) << "每凑齐一轮回调一次";
        EXPECT_LE(callbackCount.load(std::memory_order_relaxed), ticker.completedRoundCount()) << "回调次数不该超过轮数";

        // 先停循环再让 ticker 出作用域：拍协程的帧只能在确定没人再恢复它们的时候销毁
        pool.stop();
    }

    /**
     * @brief 重复 arm 是空操作：一轮仍然只挂两条，不会把轮数翻倍
     */
    TEST(RoundTicker, RepeatedArmKeepsASingleTickerPerLoop)
    {
        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        std::atomic<std::uint64_t> callbackCount{0};
        RoundTicker                ticker(pool, [&callbackCount] { callbackCount.fetch_add(1, std::memory_order_relaxed); });
        ASSERT_TRUE(ticker.arm(std::chrono::milliseconds{10}));
        ASSERT_TRUE(ticker.arm(std::chrono::milliseconds{10}));
        EXPECT_EQ(ticker.armedLoopCount(), 2U) << "第二次 arm 不该再挂一批";

        EXPECT_TRUE(TestSupport::waitForCondition([&ticker] { return ticker.completedRoundCount() >= 2; })) << "重复 arm 之后节拍仍在转";
        EXPECT_GE(callbackCount.load(std::memory_order_relaxed), 1U);

        pool.stop();
    }
} // namespace AsynGyanis::Core
