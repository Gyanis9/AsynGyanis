/**
 * @file TestCancelable.cpp
 * @brief Cancelable 单元测试：停止请求、停止令牌、停止源与移动语义
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Coroutine/Cancelable.h"

#include <gtest/gtest.h>

namespace AsynGyanis::Core
{
    TEST(Cancelable, DefaultStateIsNotStopped)
    {
        const Cancelable cancelable;

        EXPECT_FALSE(cancelable.isStopRequested());
    }

    TEST(Cancelable, RequestStopSetsStopRequestedFlag)
    {
        Cancelable cancelable;

        EXPECT_TRUE(cancelable.requestStop());
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    TEST(Cancelable, StopTokenReflectsStopState)
    {
        Cancelable cancelable;
        const auto token = cancelable.stopToken();

        EXPECT_FALSE(token.stop_requested());

        // 令牌与停止源共享状态，请求停止后令牌同步可见
        cancelable.requestStop();
        EXPECT_TRUE(token.stop_requested());
    }

    TEST(Cancelable, MoveConstructionPreservesStopState)
    {
        Cancelable source;
        source.requestStop();

        const Cancelable target(std::move(source));

        EXPECT_TRUE(target.isStopRequested());
    }

    TEST(Cancelable, MoveAssignmentPreservesStopState)
    {
        Cancelable source;
        source.requestStop();

        Cancelable target;
        target = std::move(source);

        EXPECT_TRUE(target.isStopRequested());
    }

    TEST(Cancelable, StopSourceReturnsModifiableReference)
    {
        Cancelable cancelable;
        auto &source = cancelable.stopSource();

        EXPECT_FALSE(source.stop_requested());

        // 直接操作停止源应同步反映到 Cancelable 的状态查询
        source.request_stop();
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    TEST(Cancelable, RepeatedRequestStopIsIdempotent)
    {
        Cancelable cancelable;

        // 第一次调用返回 true（从未停止转为已停止）
        EXPECT_TRUE(cancelable.requestStop());
        // 第二次调用返回 false（已经处于停止状态）
        EXPECT_FALSE(cancelable.requestStop());
        // 状态查询保持为已停止
        EXPECT_TRUE(cancelable.isStopRequested());
    }
} // namespace AsynGyanis::Core
