/**
 * @file TestThreadPool.cpp
 * @brief ThreadPool 单元测试：线程数量、索引访问、启停与跨线程任务执行
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Coroutine/ThreadPool.h"
#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

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
         * @brief 测试协程：对原子计数器执行一次自增
         * @param counter 目标原子计数器
         * @return Task<int> 固定返回 0
         */
        Task<int> incrementCounter(std::atomic<int> &counter)
        {
            counter.fetch_add(1);
            co_return 0;
        }
    }

    TEST(ThreadPool, DefaultConstructionUsesAtLeastOneThread)
    {
        ThreadPool pool;

        EXPECT_GE(pool.threadCount(), 1u);
    }

    TEST(ThreadPool, ConstructionWithSpecificCountCreatesExactThreads)
    {
        ThreadPool pool(4);

        EXPECT_EQ(pool.threadCount(), 4u);
    }

    TEST(ThreadPool, ConstructionWithZeroUsesHardwareConcurrency)
    {
        ThreadPool pool(0);

        EXPECT_EQ(pool.threadCount(), std::thread::hardware_concurrency());
    }

    TEST(ThreadPool, EventLoopAccessValidatesIndexBounds)
    {
        ThreadPool pool(2);

        // 每个索引返回各自独立的 EventLoop，越界索引按 at() 的约定抛出
        EXPECT_NE(&pool.eventLoop(0), &pool.eventLoop(1));
        EXPECT_THROW(static_cast<void>(pool.eventLoop(2)), std::out_of_range);
    }

    TEST(ThreadPool, SchedulerAccessValidatesIndexBounds)
    {
        ThreadPool pool(2);

        EXPECT_NE(&pool.scheduler(0), &pool.scheduler(1));
        EXPECT_THROW(static_cast<void>(pool.scheduler(2)), std::out_of_range);
    }

    TEST(ThreadPool, StartRunsAllEventLoopsUntilStop)
    {
        ThreadPool pool(2);
        pool.start();

        // 轮询等待每个工作线程进入事件循环，替代固定 sleep
        for (size_t index = 0; index < pool.threadCount(); ++index)
        {
            ASSERT_TRUE(waitForCondition([&pool, &index]()
            {
                return pool.eventLoop(index).isRunning();
            }));
        }

        pool.stop();

        // stop() 会 join 全部工作线程，事件循环应已退出
        for (size_t index = 0; index < pool.threadCount(); ++index)
        {
            EXPECT_FALSE(pool.eventLoop(index).isRunning());
        }
    }

    TEST(ThreadPool, TaskScheduledBeforeStartExecutesAfterStart)
    {
        ThreadPool pool(2);
        std::atomic<int> counter{0};

        auto task = incrementCounter(counter);
        pool.scheduler(0).schedule(task.handle());

        pool.start();

        // 轮询等待协程被执行，替代固定 sleep
        const bool executed = waitForCondition([&]()
        {
            return counter.load() >= 1;
        });

        pool.stop();

        EXPECT_TRUE(executed);
        EXPECT_GE(counter.load(), 1);
    }

    TEST(ThreadPool, EventLoopsAndSchedulersAreDistinctPerThread)
    {
        ThreadPool pool(2);
        auto &firstLoop = pool.eventLoop(0);
        auto &secondLoop = pool.eventLoop(1);

        // 每个工作线程应持有独立的 epoll 实例与调度器
        EXPECT_NE(firstLoop.epoll().fileDescriptor(), secondLoop.epoll().fileDescriptor());
        EXPECT_NE(&pool.scheduler(0), &pool.scheduler(1));
    }

    TEST(ThreadPool, DoubleStopIsSafe)
    {
        ThreadPool pool(1);
        pool.start();
        pool.stop();

        EXPECT_NO_THROW(pool.stop());
    }
} // namespace AsynGyanis::Core
