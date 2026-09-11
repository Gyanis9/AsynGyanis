/**
 * @file TestScheduler.cpp
 * @brief Scheduler 单元测试：本地/跨线程调度、队列查询与工作窃取
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试协程：对原子计数器执行一次自增
         * @param counter 目标原子计数器
         * @return Task<int> 固定返回 0
         */
        Task<int> incrementTask(std::atomic<int> &counter)
        {
            counter.fetch_add(1);
            co_return 0;
        }
    }

    TEST(Scheduler, ScheduleAndRunOneExecutesTask)
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};

        auto task = incrementTask(counter);
        scheduler.schedule(task.handle());

        EXPECT_TRUE(scheduler.hasWork());
        ASSERT_TRUE(scheduler.runOne());
        EXPECT_EQ(counter.load(), 1);
    }

    TEST(Scheduler, RunAllProcessesAllScheduledTasks)
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};
        std::vector<Task<int>> tasks;

        for (int round = 0; round < 10; ++round)
        {
            auto task = incrementTask(counter);
            scheduler.schedule(task.handle());
            tasks.push_back(std::move(task));
        }

        scheduler.runAll();

        EXPECT_EQ(counter.load(), 10);
        EXPECT_FALSE(scheduler.hasWork());
    }

    TEST(Scheduler, ScheduleRemoteFromAnotherThreadExecutesTask)
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};

        auto task = incrementTask(counter);
        std::thread remote([&]()
        {
            // scheduleRemote 线程安全，可在任意线程投递到全局队列
            scheduler.scheduleRemote(task.handle());
        });
        remote.join();

        ASSERT_TRUE(scheduler.runOne());
        EXPECT_EQ(counter.load(), 1);
    }

    TEST(Scheduler, HasWorkReturnsFalseWhenEmpty)
    {
        Scheduler scheduler;

        EXPECT_FALSE(scheduler.hasWork());
    }

    TEST(Scheduler, LocalQueueSizeReflectsPendingTasks)
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};

        EXPECT_EQ(scheduler.localQueueSize(), 0u);

        auto task = incrementTask(counter);
        scheduler.schedule(task.handle());

        EXPECT_EQ(scheduler.localQueueSize(), 1u);

        scheduler.runOne();
        EXPECT_EQ(scheduler.localQueueSize(), 0u);
    }

    TEST(Scheduler, ScheduleNullHandleIsIgnored)
    {
        Scheduler scheduler;

        // 空句柄应被静默忽略，不进入任何队列
        scheduler.schedule(nullptr);
        EXPECT_FALSE(scheduler.hasWork());
    }

    TEST(Scheduler, ScheduleRemoteNullHandleIsIgnored)
    {
        Scheduler scheduler;

        scheduler.scheduleRemote(nullptr);
        EXPECT_FALSE(scheduler.runOne());
    }

    TEST(Scheduler, RunOneReturnsFalseWhenEmpty)
    {
        Scheduler scheduler;

        EXPECT_FALSE(scheduler.runOne());
    }

    TEST(Scheduler, StealFromTakesOnlyGlobalQueueTasks)
    {
        Scheduler source;
        Scheduler thief;
        std::atomic<int> counter{0};

        // 本地队列任务对窃取者不可见
        auto localTask = incrementTask(counter);
        source.schedule(localTask.handle());

        const auto unstealable = Scheduler::stealFrom(source);
        EXPECT_FALSE(unstealable);

        // 全局队列中的跨线程任务可以被窃取并由窃取者执行
        auto remoteTask = incrementTask(counter);
        source.scheduleRemote(remoteTask.handle());

        auto stolen = Scheduler::stealFrom(source);
        ASSERT_TRUE(stolen);
        stolen.resume();
        EXPECT_EQ(counter.load(), 1);

        // 本地任务仍留在源调度器，由其自行执行
        EXPECT_TRUE(source.runOne());
        EXPECT_EQ(counter.load(), 2);
        EXPECT_FALSE(source.hasWork());
    }

    TEST(Scheduler, MultipleScheduleRemoteCallsAreProcessed)
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};
        std::vector<Task<int>> tasks;

        // 投递 5 个任务进入全局队列，逐个执行
        for (int round = 0; round < 5; ++round)
        {
            auto task = incrementTask(counter);
            scheduler.scheduleRemote(task.handle());
            tasks.push_back(std::move(task));
        }

        for (int round = 0; round < 5; ++round)
        {
            ASSERT_TRUE(scheduler.runOne());
        }
        EXPECT_EQ(counter.load(), 5);
        EXPECT_FALSE(scheduler.hasWork());
    }
} // namespace AsynGyanis::Core
