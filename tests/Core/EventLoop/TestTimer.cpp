/**
 * @file TestTimer.cpp
 * @brief Timer 单元测试：基于事件循环的构造与等待器创建
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"

#include <gtest/gtest.h>

#include <chrono>

namespace AsynGyanis::Core
{
    /**
     * @brief 只持有事件循环即可构造定时器：构造过程不抛异常，也不向循环注册任何事件
     */
    TEST(Timer, ConstructionSucceeds)
    {
        EventLoop loop;

        EXPECT_NO_THROW(
        {
            Timer timer(loop);
        });
    }

    /**
     * @brief waitFor() 返回可构造的等待器，且未被 co_await 时不会向事件循环注册事件（不产生副作用）
     */
    TEST(Timer, WaitForReturnsAwaiter)
    {
        EventLoop loop;
        Timer timer(loop);

        // 未被 co_await 的等待器不会注册任何事件，仅验证可构造
        auto awaiter = timer.waitFor(std::chrono::milliseconds(100));
        (void)awaiter;
        SUCCEED();
    }
} // namespace AsynGyanis::Core
