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

    /**
     * @brief 多次跨线程投递的任务全部保留在全局队列：逐个 runOne() 都能取到，结束后队列清空
     */
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
