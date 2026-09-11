/**
 * @file TestTask.cpp
 * @brief Task 单元测试：返回值、异常传播、移动语义与等待器接口
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试协程：返回固定值 42
         * @return Task<int>
         */
        Task<int> simpleValueTask()
        {
            co_return 42;
        }

        /**
         * @brief 测试协程：无返回值直接结束
         * @return Task<void>
         */
        Task<void> simpleVoidTask()
        {
            co_return;
        }

        /**
         * @brief 测试协程：抛出运行时异常
         * @return Task<int>
         */
        Task<int> throwingTask()
        {
            throw std::runtime_error("test error");
            co_return 0;
        }
    }

    TEST(Task, SimpleValueReturnIsAvailableAfterResume)
    {
        auto task = simpleValueTask();
        ASSERT_FALSE(task.isReady());

        // 恢复协程至最终挂起点
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        const int result = task.handle().promise().result();
        EXPECT_EQ(result, 42);
    }

    TEST(Task, VoidTaskCompletesWithoutThrow)
    {
        auto task = simpleVoidTask();
        ASSERT_FALSE(task.isReady());

        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        // 正常结束的 void 协程取结果不应抛出异常
        EXPECT_NO_THROW(task.handle().promise().result());
    }

    TEST(Task, ExceptionIsCapturedAndRethrownByResult)
    {
        auto task = throwingTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        EXPECT_THROW(task.handle().promise().result(), std::runtime_error);
    }

    TEST(Task, MoveConstructionTransfersHandle)
    {
        auto first = simpleValueTask();
        const auto originalHandle = first.handle();

        Task<int> second(std::move(first));

        EXPECT_EQ(second.handle(), originalHandle);
    }

    TEST(Task, MoveAssignmentTransfersHandle)
    {
        auto first = simpleValueTask();
        auto second = simpleValueTask();

        const auto originalHandle = first.handle();
        second = std::move(first);

        EXPECT_EQ(second.handle(), originalHandle);
    }

    TEST(Task, IsReadyReturnsFalseBeforeResume)
    {
        // 惰性启动：协程创建后处于挂起状态，未恢复前未完成
        auto task = simpleValueTask();

        EXPECT_FALSE(task.isReady());
    }

    TEST(Task, IsReadyReturnsTrueAfterCompletion)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        EXPECT_TRUE(task.isReady());
    }

    TEST(Task, AwaitResumeReturnsCoroutineValue)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        const int value = task.await_resume();
        EXPECT_EQ(value, 42);
    }

    TEST(Task, AwaitReadyReflectsDoneState)
    {
        auto task = simpleValueTask();
        EXPECT_FALSE(task.await_ready());

        task.handle().resume();
        EXPECT_TRUE(task.await_ready());
    }

    TEST(Task, MovedFromTaskHasNullHandle)
    {
        auto first = simpleValueTask();
        Task<int> second(std::move(first));

        EXPECT_EQ(first.handle(), nullptr);
    }
} // namespace AsynGyanis::Core
