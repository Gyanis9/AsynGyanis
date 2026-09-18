// Scheduler 单元测试：本地就绪队列、跨线程投递与队列查询

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
     * @brief 本地投递的任务构成待办：runOne() 能取出并执行一次，副作用落到实处
     */
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

    /**
     * @brief runAll() 一次跑完队列里的全部任务（不止第一个），跑完 hasWork() 归 false
     */
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

    /**
     * @brief scheduleRemote() 可从其它线程投递：跨线程到达的任务最终由本调度器执行，不丢不裂
     */
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

    /**
     * @brief 空调度器报「无工作」：事件循环据此改用无限阻塞而不是 0 超时空转
     */
    TEST(Scheduler, HasWorkReturnsFalseWhenEmpty)
    {
        Scheduler scheduler;

        EXPECT_FALSE(scheduler.hasWork());
    }

    /**
     * @brief localQueueSize() 如实反映本地队列长度：投递后为 1，被取出执行后回到 0
     */
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

    /**
     * @brief 投递空句柄被静默忽略（不进任何队列）：空句柄不构成待办，也不该被当成有效任务执行
     */
    TEST(Scheduler, ScheduleNullHandleIsIgnored)
    {
        Scheduler scheduler;

        // 空句柄应被静默忽略，不进入任何队列
        scheduler.schedule(nullptr);
        EXPECT_FALSE(scheduler.hasWork());
    }

    /**
     * @brief 远端（跨线程）投递空句柄同样被忽略：runOne() 无任务可跑，返回 false
     */
    TEST(Scheduler, ScheduleRemoteNullHandleIsIgnored)
    {
        Scheduler scheduler;

        scheduler.scheduleRemote(nullptr);
        EXPECT_FALSE(scheduler.runOne());
    }

    /**
     * @brief 队列为空时 runOne() 返回 false，调用方据此区分「没有跑到任务」与「跑到了但无副作用」
     */
    TEST(Scheduler, RunOneReturnsFalseWhenEmpty)
    {
        Scheduler scheduler;

        EXPECT_FALSE(scheduler.runOne());
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
    /**
     * @brief postRemote() 投递的代码在目标循环线程上执行，而不是投递线程上
     */
    TEST(Scheduler, PostRemoteRunsCallableOnTheLoopThread)
    {
        Scheduler       scheduler;
        std::thread::id executedOn;
        std::thread::id posterThreadId;

        std::thread remote([&scheduler, &executedOn, &posterThreadId]()
        {
            posterThreadId = std::this_thread::get_id();
            scheduler.postRemote([&executedOn]()
            {
                executedOn = std::this_thread::get_id();
            });
        });
        remote.join();

        ASSERT_TRUE(scheduler.runOne());
        // 关键断言是两条：跑在目标循环（本线程）而不是投递线程上，跨循环移交才有意义
        EXPECT_EQ(executedOn, std::this_thread::get_id()) << "回调没有跑在目标循环线程上";
        EXPECT_NE(executedOn, posterThreadId) << "回调跑在了投递线程上";
    }

    /**
     * @brief 多条回调按投递顺序（FIFO）执行
     */
    TEST(Scheduler, PostRemoteKeepsFifoOrder)
    {
        Scheduler       scheduler;
        std::vector<int> executedOrder;

        for (int index = 0; index < 3; ++index)
        {
            scheduler.postRemote([&executedOrder, index]()
            {
                executedOrder.push_back(index);
            });
        }

        for (int round = 0; round < 3; ++round)
        {
            ASSERT_TRUE(scheduler.runOne());
        }
        EXPECT_EQ(executedOrder, (std::vector<int>{0, 1, 2}));
        EXPECT_FALSE(scheduler.hasWork());
    }

    /**
     * @brief 空的回调对象被忽略：不会占位，也不会让 runOne() 空转
     */
    TEST(Scheduler, PostRemoteEmptyCallableIsIgnored)
    {
        Scheduler scheduler;
        scheduler.postRemote({});

        EXPECT_FALSE(scheduler.hasWork());
        EXPECT_FALSE(scheduler.runOne());
    }

    /**
     * @brief 待执行的回调计入待办：hasWork() 能看见它
     */
    TEST(Scheduler, HasWorkSeesPendingRemoteCallables)
    {
        Scheduler scheduler;
        std::thread remote([&scheduler]()
        {
            scheduler.postRemote([]()
            {
            });
        });
        remote.join();

        EXPECT_TRUE(scheduler.hasWork());
        ASSERT_TRUE(scheduler.runOne());
        EXPECT_FALSE(scheduler.hasWork());
    }

    /**
     * @brief runAll() 会把积压的回调一次跑完
     */
    TEST(Scheduler, RunAllDrainsRemoteCallables)
    {
        Scheduler        scheduler;
        std::atomic<int> counter{0};

        for (int index = 0; index < 4; ++index)
        {
            scheduler.postRemote([&counter]()
            {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        scheduler.runAll();
        EXPECT_EQ(counter.load(), 4);
        EXPECT_FALSE(scheduler.hasWork());
    }

} // namespace AsynGyanis::Core
