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

        /**
         * @brief 手动放行的挂起点：把恢复权完全交给测试代码
         * @details 用来制造「任务已启动、挂在内部等待上」这一中间状态——这正是
         *          「co_await 一个已经在跑的任务」要面对的场景，定时器之类的真实等待点
         *          无法在单测里精确控制放行时刻。
         */
        class ManualGate
        {
        public:
            [[nodiscard]] bool await_ready() const noexcept { return false; }

            void await_suspend(const std::coroutine_handle<> handle) noexcept { m_handle = handle; }

            void await_resume() const noexcept {}

            /// 放行：恢复挂在门上的协程（门未被等待时为空操作）
            void open() const noexcept
            {
                if (m_handle)
                {
                    m_handle.resume();
                }
            }

        private:
            std::coroutine_handle<> m_handle{}; ///< 挂在门上的协程
        };
    }

    /**
     * @brief 惰性启动：resume 之前不算完成，resume 到终结点后 result() 交出 co_return 的值
     */
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

    /**
     * @brief Task<void> 正常跑到终结点时 isReady() 为 true，且 result() 不抛异常
     */
    TEST(Task, VoidTaskCompletesWithoutThrow)
    {
        auto task = simpleVoidTask();
        ASSERT_FALSE(task.isReady());

        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        // 正常结束的 void 协程取结果不应抛出异常
        EXPECT_NO_THROW(task.handle().promise().result());
    }

    /**
     * @brief 协程体抛出的异常被 promise 捕获存下，在 result() 处按原类型重新抛出，不会丢失在协程帧里
     */
    TEST(Task, ExceptionIsCapturedAndRethrownByResult)
    {
        auto task = throwingTask();
        task.handle().resume();
        ASSERT_TRUE(task.isReady());

        EXPECT_THROW(task.handle().promise().result(), std::runtime_error);
    }

    /**
     * @brief 移动构造把句柄整体交给新对象：新对象持有的句柄与源原先持有的完全相同
     */
    TEST(Task, MoveConstructionTransfersHandle)
    {
        auto first = simpleValueTask();
        const auto originalHandle = first.handle();

        Task<int> second(std::move(first));

        EXPECT_EQ(second.handle(), originalHandle);
    }

    /**
     * @brief 移动赋值同样转移句柄所有权：目标接管新帧并销毁旧帧，不会出现两个 Task 共用一个帧
     */
    TEST(Task, MoveAssignmentTransfersHandle)
    {
        auto first = simpleValueTask();
        auto second = simpleValueTask();

        const auto originalHandle = first.handle();
        second = std::move(first);

        EXPECT_EQ(second.handle(), originalHandle);
    }

    /**
     * @brief isReady() 以协程是否到达终结点为准：未 resume 过的惰性协程不算完成
     */
    TEST(Task, IsReadyReturnsFalseBeforeResume)
    {
        // 惰性启动：协程创建后处于挂起状态，未恢复前未完成
        auto task = simpleValueTask();

        EXPECT_FALSE(task.isReady());
    }

    /**
     * @brief 跑到终结点后 isReady() 翻转为 true，调用方据此决定「直接取结果」还是「继续等待」
     */
    TEST(Task, IsReadyReturnsTrueAfterCompletion)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        EXPECT_TRUE(task.isReady());
    }

    /**
     * @brief await_resume() 本身就能取出协程结果，等待器接口可脱离 co_await 单独使用
     */
    TEST(Task, AwaitResumeReturnsCoroutineValue)
    {
        auto task = simpleValueTask();
        task.handle().resume();

        const int value = task.await_resume();
        EXPECT_EQ(value, 42);
    }

    /**
     * @brief await_ready() 与完成状态一致：未完成返回 false（需挂起等待），完成后返回 true（可直接取结果）
     */
    TEST(Task, AwaitReadyReflectsDoneState)
    {
        auto task = simpleValueTask();
        EXPECT_FALSE(task.await_ready());

        task.handle().resume();
        EXPECT_TRUE(task.await_ready());
    }

    /**
     * @brief 被移动走的 Task 句柄为空：源对象不再持有协程帧，其析构不会重复销毁
     */
    TEST(Task, MovedFromTaskHasNullHandle)
    {
        auto first = simpleValueTask();
        Task<int> second(std::move(first));

        EXPECT_EQ(first.handle(), nullptr);
    }

    /**
     * @brief co_await 一个已启动的任务是「等它结束」，不是「把它叫醒」
     *
     * @details 任务停在自身内部的等待上时，恢复它等于谎报「你等的那个操作完成了」：
     *          等待方会立刻越过 co_await 继续执行，而任务里的子等待依旧悬着，随后
     *          随帧析构一起被销毁。这里用一道手动门把这个差异钉死——等待方在子任务
     *          真正放行之前，一步都不许前进。
     */
    TEST(Task, AwaitStartedTaskWaitsInsteadOfResumingIt)
    {
        ManualGate gate;
        bool       isChildFinished = false;

        auto child = [&gate, &isChildFinished]() -> Task<>
        {
            co_await gate;
            isChildFinished = true;
        }();

        // 先把子任务启动到内部等待点上：从这一刻起它已经「在跑」
        child.handle().resume();
        ASSERT_FALSE(isChildFinished);
        ASSERT_FALSE(child.isReady());

        int parentProgress = 0;
        auto parent        = [&child, &parentProgress]() -> Task<>
        {
            parentProgress = 1;
            co_await child;
            parentProgress = 2;
        }();

        parent.handle().resume();
        EXPECT_EQ(parentProgress, 1) << "等待方越过 co_await 前进了：说明它把子任务的内部等待当成了已完成";
        EXPECT_FALSE(isChildFinished) << "子任务被等待方从自己的等待点上叫醒了";

        // 真正放行子任务：它跑到终结点后应当把等待方唤醒
        gate.open();
        EXPECT_TRUE(isChildFinished);
        EXPECT_EQ(parentProgress, 2) << "子任务结束后没有把等待方唤醒";
        EXPECT_TRUE(child.isReady());
        EXPECT_TRUE(parent.isReady());
    }

    /**
     * @brief co_await 一个惰性任务会就地启动它，而不是空等一个没人启动的协程
     */
    TEST(Task, AwaitLazyTaskStartsIt)
    {
        bool isChildFinished = false;

        auto child = [&isChildFinished]() -> Task<>
        {
            isChildFinished = true;
            co_return;
        }();

        auto parent = [&child]() -> Task<>
        {
            co_await child;
        }();

        parent.handle().resume();
        EXPECT_TRUE(isChildFinished) << "co_await 没有启动惰性任务";
        EXPECT_TRUE(child.isReady());
        EXPECT_TRUE(parent.isReady());
    }
} // namespace AsynGyanis::Core
