// ThreadPool 单元测试：线程数量、索引访问、启停与跨线程任务执行

#include "Core/Coroutine/ThreadPool.h"
#include "Core/Coroutine/Task.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::waitForCondition;

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

    /**
     * @brief 默认构造不产生空池：至少一个工作线程，任务永远有地方可跑
     */
    TEST(ThreadPool, DefaultConstructionUsesAtLeastOneThread)
    {
        ThreadPool pool;

        EXPECT_GE(pool.threadCount(), 1u);
    }

    /**
     * @brief 显式线程数按原值生效（4 就是 4），不做取整或补足
     */
    TEST(ThreadPool, ConstructionWithSpecificCountCreatesExactThreads)
    {
        ThreadPool pool(4);

        EXPECT_EQ(pool.threadCount(), 4u);
    }

    /**
     * @brief 线程数为 0 时回落到 hardware_concurrency()，与构造参数的文档约定一致
     */
    TEST(ThreadPool, ConstructionWithZeroUsesHardwareConcurrency)
    {
        ThreadPool pool(0);

        EXPECT_EQ(pool.threadCount(), std::thread::hardware_concurrency());
    }

    /**
     * @brief eventLoop() 一索引一实例（不同线程的事件循环不共享），越界索引按 at() 约定抛 out_of_range
     */
    TEST(ThreadPool, EventLoopAccessValidatesIndexBounds)
    {
        ThreadPool pool(2);

        // 每个索引返回各自独立的 EventLoop，越界索引按 at() 的约定抛出
        EXPECT_NE(&pool.eventLoop(0), &pool.eventLoop(1));
        EXPECT_THROW(static_cast<void>(pool.eventLoop(2)), std::out_of_range);
    }

    /**
     * @brief scheduler() 同样一索引一实例，越界索引抛 out_of_range（拒绝面与 eventLoop() 一致）
     */
    TEST(ThreadPool, SchedulerAccessValidatesIndexBounds)
    {
        ThreadPool pool(2);

        EXPECT_NE(&pool.scheduler(0), &pool.scheduler(1));
        EXPECT_THROW(static_cast<void>(pool.scheduler(2)), std::out_of_range);
    }

    /**
     * @brief start() 让每个工作线程都进入事件循环；stop() 负责停止并 join，返回时所有循环都已退出
     */
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

    /**
     * @brief start() 之前投递的任务不会丢：启动后由工作线程补跑（投递先于启动是常见写法）
     */
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

    /**
     * @brief 每线程持有独立的 epoll 实例与调度器：描述符与对象地址两两不重合
     */
    TEST(ThreadPool, EventLoopsAndSchedulersAreDistinctPerThread)
    {
        ThreadPool pool(2);
        auto &firstLoop = pool.eventLoop(0);
        auto &secondLoop = pool.eventLoop(1);

        // 每个工作线程应持有独立的 epoll 实例与调度器
        EXPECT_NE(firstLoop.epoll().fileDescriptor(), secondLoop.epoll().fileDescriptor());
        EXPECT_NE(&pool.scheduler(0), &pool.scheduler(1));
    }

    /**
     * @brief stop() 幂等：重复停止既不再有线程可停，也不抛异常（析构与显式停止可叠加）
     */
    TEST(ThreadPool, DoubleStopIsSafe)
    {
        ThreadPool pool(1);
        pool.start();
        pool.stop();

        EXPECT_NO_THROW(pool.stop());
    }
} // namespace AsynGyanis::Core
