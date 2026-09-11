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
    TEST(Timer, ConstructionSucceeds)
    {
        EventLoop loop;

        EXPECT_NO_THROW(
        {
            Timer timer(loop);
        });
    }

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
