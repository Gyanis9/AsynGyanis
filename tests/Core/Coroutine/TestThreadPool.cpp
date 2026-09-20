// ThreadPool 单元测试：线程数量、索引访问、启停、跨线程任务执行与可选的按线程绑核

#include "Core/Coroutine/ThreadPool.h"
#include "Core/Coroutine/Task.h"

#include "Platform/System/CpuAffinity.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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

    namespace
    {
        /**
         * @brief 投递一段代码到指定工作线程上执行，并等它跑完
         * @param pool 目标线程池
         * @param index 工作线程下标
         * @param body 要在该线程上执行的代码
         * @return true 代码在时限内于目标线程跑完
         */
        bool runOnPoolThread(ThreadPool &pool, const size_t index, const std::function<void()> &body)
        {
            std::atomic<bool> isFinished{false};
            pool.scheduler(index).postRemote([&body, &isFinished]
            {
                body();
                isFinished.store(true, std::memory_order_release);
            });
            return waitForCondition([&isFinished]
            {
                return isFinished.load(std::memory_order_acquire);
            });
        }

        /**
         * @brief 本机到底能不能设置线程亲和性：拿一条一次性线程试一次
         * @details 部分沙箱允许读亲和性却拒绝写。少了这道探测，绑核用例会把「环境不允许」
         *          当成实现缺陷报红；探测不过就整条用例跳过，结论才干净。
         * @return true 表示允许把线程绑到许可集合内的 0 号核上
         */
        bool canPinAnyThread()
        {
            std::atomic<bool> isPinned{false};
            std::thread probe([&isPinned]
            {
                const auto pinResult = Platform::CpuAffinity::pinCurrentThreadToCore(0);
                const std::uint64_t maskAfter = Platform::CpuAffinity::currentThreadCoreMask();
                isPinned.store(pinResult.has_value() && maskAfter == 1U, std::memory_order_release);
            });
            probe.join();
            return isPinned.load(std::memory_order_acquire);
        }
    } // namespace

    /**
     * @brief 打开绑核开关后，每个工作线程在自己的线程体里被绑到一枚核上（掩码只剩一位）
     * @details 判据取目标线程自己读回的掩码：亲和性是线程级属性，在投递线程上读不到结果。
     *          线程数多于可用核数时只有下标在核数以内的那部分被绑，本用例只用两条线程，
     *          在单核机器上退化为「两条都绑同一枚核」，断言的形状两种机器都成立。
     */
    TEST(ThreadPool, PinnedThreadsEachEndUpOnASingleCore)
    {
        if (Platform::CpuAffinity::availableCoreCount() == 0)
        {
            GTEST_SKIP() << "本机读不到进程可用的 CPU 集合，绑核无从验证";
        }
        if (!canPinAnyThread())
        {
            GTEST_SKIP() << "本环境不允许设置线程亲和性（沙箱常这样），跳过绑核断言";
        }

        ThreadPool pool(2);
        pool.setThreadsPinnedToCores(true);
        pool.start();

        std::atomic<std::uint64_t> firstMask{0};
        std::atomic<std::uint64_t> secondMask{0};
        const bool isFirstRead = runOnPoolThread(pool, 0, [&firstMask]
        {
            firstMask.store(Platform::CpuAffinity::currentThreadCoreMask(), std::memory_order_relaxed);
        });
        const bool isSecondRead = runOnPoolThread(pool, 1, [&secondMask]
        {
            secondMask.store(Platform::CpuAffinity::currentThreadCoreMask(), std::memory_order_relaxed);
        });
        pool.stop();

        ASSERT_TRUE(isFirstRead && isSecondRead) << "投递到工作线程的读数任务没跑完";
        for (const std::uint64_t coreMask: {firstMask.load(), secondMask.load()})
        {
            EXPECT_NE(coreMask, 0U);
            EXPECT_EQ(coreMask & (coreMask - 1), 0U) << "绑核后掩码应当只剩一枚核，实际 " << std::hex << coreMask;
        }
    }

    /**
     * @brief 对照：开关不动就谁都不绑，多核机器上工作线程仍保持可迁移
     * @details 绑核是可选行为，默认关闭必须是真的关闭——否则「线程多于核数时互相挤在同一枚核」
     *          这类问题就无从归因了。单核机器上掩码天然只剩一位，那条对照没有区分度，跳过。
     */
    TEST(ThreadPool, UnpinnedThreadsStayMigratableOnMultiCoreMachines)
    {
        if (Platform::CpuAffinity::availableCoreCount() <= 1)
        {
            GTEST_SKIP() << "本机只放行一枚核，掩码本来就只能是一位，无法区分「没绑」与「绑了」";
        }

        ThreadPool pool(1);
        pool.start();

        std::atomic<std::uint64_t> workerMask{0};
        const bool isRead = runOnPoolThread(pool, 0, [&workerMask]
        {
            workerMask.store(Platform::CpuAffinity::currentThreadCoreMask(), std::memory_order_relaxed);
        });
        pool.stop();

        ASSERT_TRUE(isRead) << "投递到工作线程的读数任务没跑完";
        EXPECT_NE(workerMask.load() & (workerMask.load() - 1), 0U)
                << "默认不开绑核时，工作线程的掩码不该被收窄到一枚核：" << std::hex << workerMask.load();
    }
} // namespace AsynGyanis::Core
