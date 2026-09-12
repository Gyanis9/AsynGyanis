/**
 * @file TestEventLoop.cpp
 * @brief EventLoop 单元测试：启停状态、跨线程唤醒、调度器接入与协程执行
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/EventLoop/EventLoop.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 等待类断言的轮询上限，避免固定 sleep 硬等，同时防止用例卡死
        constexpr auto kConditionTimeout = std::chrono::milliseconds(2000);

        /**
         * @brief 在超时上限内逐毫秒轮询等待条件成立
         * @tparam Predicate 可调用对象，返回 bool
         * @param predicate 待轮询的条件
         * @param timeout 超时上限，默认 2 秒
         * @return true 条件在时限内成立
         */
        template<typename Predicate>
        bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout = kConditionTimeout)
        {
            // 以 steady_clock 计算截止时间，轮询而非固定 sleep
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 测试协程：向原子变量写入标记值
         * @param value 目标原子变量
         * @return Task<int> 固定返回 0
         */
        Task<int> setValue(std::atomic<int> &value)
        {
            value.store(42);
            co_return 0;
        }
    }

    TEST(EventLoop, ConstructionAllocatesValidEpoll)
    {
        EventLoop loop;

        EXPECT_TRUE(Platform::isEpollHandleValid(loop.epoll().fileDescriptor()));
        EXPECT_FALSE(loop.isRunning());
    }

    TEST(EventLoop, RunAndStopTransitionsRunningState)
    {
        EventLoop loop;
        std::atomic<bool> workerStarted{false};
        std::thread worker([&]()
        {
            workerStarted.store(true);
            loop.run();
        });

        // 轮询等待事件循环真正进入 run()
        ASSERT_TRUE(waitForCondition([&]()
        {
            return workerStarted.load() && loop.isRunning();
        }));

        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    TEST(EventLoop, SchedulerIsAccessibleBeforeRun)
    {
        EventLoop loop;

        EXPECT_FALSE(loop.scheduler().hasWork());
    }

    TEST(EventLoop, WakeInterruptsBlockingWait)
    {
        EventLoop loop;
        std::atomic<bool> workerStarted{false};
        std::thread worker([&]()
        {
            workerStarted.store(true);
            loop.run();
        });

        ASSERT_TRUE(waitForCondition([&]()
        {
            return workerStarted.load() && loop.isRunning();
        }));

        // 先唤醒阻塞在 epoll_wait 中的事件循环，随后停止应能正常退出
        loop.wake();
        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    TEST(EventLoop, SchedulerExecutesScheduledCoroutine)
    {
        EventLoop loop;
        std::atomic<int> value{0};

        auto task = setValue(value);
        ASSERT_NE(task.handle(), nullptr);
        loop.scheduler().schedule(task.handle());

        EXPECT_TRUE(loop.scheduler().runOne());
        EXPECT_EQ(value.load(), 42);
    }

    TEST(EventLoop, RunAllExecutesMultipleCoroutines)
    {
        EventLoop loop;
        int executionCount = 0;
        int results[3]{};
        std::vector<Task<void>> tasks;

        // 三个协程各自记录执行结果
        auto first = [&]() -> Task<void>
        {
            ++executionCount;
            results[0] = 1;
            co_return;
        }();
        auto second = [&]() -> Task<void>
        {
            ++executionCount;
            results[1] = 2;
            co_return;
        }();
        auto third = [&]() -> Task<void>
        {
            ++executionCount;
            results[2] = 3;
            co_return;
        }();

        loop.scheduler().schedule(first.handle());
        loop.scheduler().schedule(second.handle());
        loop.scheduler().schedule(third.handle());
        tasks.push_back(std::move(first));
        tasks.push_back(std::move(second));
        tasks.push_back(std::move(third));

        loop.scheduler().runAll();

        EXPECT_EQ(executionCount, 3);
        EXPECT_EQ(results[0], 1);
        EXPECT_EQ(results[1], 2);
        EXPECT_EQ(results[2], 3);
    }

    TEST(EventLoop, StopFromAnotherThreadExitsLoop)
    {
        EventLoop loop;
        std::thread worker([&]()
        {
            loop.run();
        });

        // 轮询等待事件循环进入运行状态，替代固定 sleep
        ASSERT_TRUE(waitForCondition([&]()
        {
            return loop.isRunning();
        }));

        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    TEST(EventLoop, DoubleStopIsSafe)
    {
        EventLoop loop;
        std::thread worker([&]()
        {
            loop.run();
        });

        ASSERT_TRUE(waitForCondition([&]()
        {
            return loop.isRunning();
        }));

        loop.stop();
        EXPECT_NO_THROW(loop.stop());
        worker.join();
    }

    TEST(EventLoop, StopBeforeRunIsStickyAndRunReturnsImmediately)
    {
        EventLoop loop;

        // 停止请求先于 run() 到达时必须被保留：start() 之后立刻 stop() 是常见写法，
        // 它会走到「标志已置位、工作线程还没进入 run()」这个时序。若 run() 开头清除该标志，
        // 这次停止请求就被吞掉，工作线程会永远阻塞在 epoll_wait 上、join 随之卡死
        //（这不是假设：按「可重启」改法实现后，ThreadPool.DoubleStopIsSafe 实测挂死）
        loop.stop();
        EXPECT_NO_THROW(loop.run());
        EXPECT_FALSE(loop.isRunning());

        // 停止请求是粘性的，同一实例不支持重启：再次 run() 同样立刻返回。
        // 这是为换取上面那条安全性质而刻意保留的取舍，需要重新运行请新建实例
        EXPECT_NO_THROW(loop.run());
        EXPECT_FALSE(loop.isRunning());
    }
} // namespace AsynGyanis::Core
