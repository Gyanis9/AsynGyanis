// IoContext 单元测试：线程池配置、主调度器、启停阻塞、运行前投递任务，以及与另一线程 stop() 撞车的收尾

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/IoContext.h"

#include "CoreTestSupport.h"
#include "Platform/System/CpuAffinity.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::waitForCondition;

#if ASYN_WITH_IO_URING
        /// 拿不到环时写进 SKIP 说明的那句话：本文件每条用例都要构造 IoContext，而它按档建出后端，
        /// 环起不出来时这些断言判的都不是实现而是环境
        constexpr const char *kNoIoUringRingReason = "本环境起不出 io_uring 环（内核禁用或被沙箱挡下），IoContext 在这一档下无从构造";

        /**
         * @brief 本环境能不能建出「用例真正要建的那个」IoContext 后端
         * @details 判据取实际构造本身，且形状要与用例一致：只探一个环是不够的——CI runner 上一个环给
         *          得起、两条循环就回 ENOMEM（实测），内核开关与 /proc 那些信号都读不出这种「够用但不
         *          够这一档」。0 表示按默认档自动定线程数，与 `IoContext{}` 同形
         * @param threadCount 本用例要起的循环条数，0 为默认档
         * @return true 建得出来，本用例的断言才有可判的对象
         */
        [[nodiscard]] bool canCreateIoUringBackend(const std::size_t threadCount)
        {
            try
            {
                static_cast<void>(IoContext{threadCount});
                return true;
            } catch (const std::exception &)
            {
                return false;
            }
        }
#endif

        /**
         * @brief 测试协程：向原子变量写入标记值
         * @param value 目标原子变量
         * @return Task<int> 固定返回 0
         */
        Task<int> setIoValue(std::atomic<int> &value)
        {
            value.store(99);
            co_return 0;
        }
    } // namespace

    /**
     * @brief 构造按参数建好线程池但不启动线程：线程数原样可查
     */
    TEST(IoContext, ConstructionCreatesConfiguredThreadPool)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(2U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext context(2);

        EXPECT_EQ(context.threadPool().threadCount(), 2u);
    }

    /**
     * @brief threadPool() 返回的是内部那个池（同一对象）而不是副本，调用方才能借它投递任务
     */
    TEST(IoContext, ThreadPoolAccessorReturnsConfiguredPool)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(2U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext context(2);

        auto &pool = context.threadPool();
        EXPECT_EQ(pool.threadCount(), 2u);
    }

    /**
     * @brief mainScheduler() 固定指向 0 号工作线程的调度器（地址相同），且初始无待办
     */
    TEST(IoContext, MainSchedulerReturnsFirstWorkerScheduler)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(2U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext context(2);

        auto &scheduler = context.mainScheduler();
        EXPECT_FALSE(scheduler.hasWork());
        EXPECT_EQ(&scheduler, &context.threadPool().scheduler(0));
    }

    /**
     * @brief 从未 run() 就 stop() 是安全的空操作：不会死锁、不抛异常（收尾路径可以无条件调用）
     */
    TEST(IoContext, StopWithoutRunDoesNotDeadlock)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(1U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext context(1);

        // 未调用 run() 时停止应当是安全的空操作
        EXPECT_NO_THROW(context.stop());
    }

    /**
     * @brief run() 阻塞调用线程直到 stop() 被请求，之后工作线程能正常退出并 join
     */
    TEST(IoContext, RunBlocksUntilStopIsRequested)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(1U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext         context(1);
        std::atomic<bool> workerStarted{false};

        std::thread worker(
                [&]()
                {
                    workerStarted.store(true);
                    context.run();
                });

        // 轮询等待工作线程启动，stop() 应能解除 run() 的阻塞
        ASSERT_TRUE(waitForCondition([&]() { return workerStarted.load(); }));

        context.stop();
        worker.join();
    }

    /**
     * @brief run() 之前投递到主调度器的任务不会丢：启动后被执行，且 run() 返回后仍可读到结果
     */
    TEST(IoContext, TaskScheduledBeforeRunExecutesAfterRun)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(1U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext        context(1);
        std::atomic<int> value{0};

        auto task = setIoValue(value);
        context.mainScheduler().schedule(task.handle());

        std::thread worker([&]() { context.run(); });

        // 轮询等待协程被执行，替代固定 sleep
        const bool executed = waitForCondition([&]() { return value.load() == 99; });

        context.stop();
        worker.join();

        EXPECT_TRUE(executed);
        EXPECT_EQ(value.load(), 99);
    }

    /**
     * @brief 默认构造的线程数取「本进程实际可用的核数」，不写死某个小常数
     * @details 旧断言是 threadCount() == hardware_concurrency()。新语义改成与
     *          CpuAffinity::recommendedWorkerCount() 相等：容器里按宿主核数起循环，每条都自带一份
     *          epoll 与定时器描述符，白占内存与文件描述符。裸机上两者仍相等，因此这条在两种环境下都成立
     */
    TEST(IoContext, DefaultConstructorUsesPermittedCoreCount)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(0U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        IoContext context;

        EXPECT_EQ(context.threadPool().threadCount(), Platform::CpuAffinity::recommendedWorkerCount()) << "自动档没有走进程可用核数的统一口径";
        EXPECT_GE(context.threadPool().threadCount(), 1U) << "0 条循环的运行时没有人推进事件";
    }
    /**
     * @brief run() 与 stop() 撞在一起时，两边都返回后不得留下还在跑的线程池
     * @details 一条线程 run()、另一条 stop() 是本类文档写明的正常用法（信号处理与看门狗都这么收尾）。
     *          一道 barrier 把两次调用放在同一瞬间放行，两百轮压下来钉的是收尾结果这条不变式。
     *          它兜住的是「把停止标志检查与 start() 拆成两步」这类改法——那会起出一个没人收尾的池子；
     *          旧实现两步合在同一把锁里，实测 TSan 下两百轮报不出竞争，故这条不是那件事的证伪
     */
    TEST(IoContext, ConcurrentRunAndStopNeverLeavesARunningPool)
    {
#if ASYN_WITH_IO_URING
        if (!canCreateIoUringBackend(2U))
        {
            GTEST_SKIP() << kNoIoUringRingReason;
        }
#endif
        constexpr int kRoundCount = 200;
        for (int round = 0; round < kRoundCount; ++round)
        {
            IoContext         context(2);
            std::barrier      releasePoint(2);
            std::atomic<bool> isRunReturned{false};

            std::thread runner(
                    [&context, &releasePoint, &isRunReturned]
                    {
                        releasePoint.arrive_and_wait();
                        context.run();
                        isRunReturned.store(true, std::memory_order_release);
                    });
            std::thread stopper(
                    [&context, &releasePoint]
                    {
                        releasePoint.arrive_and_wait();
                        context.stop();
                    });

            runner.join();
            stopper.join();
            EXPECT_TRUE(isRunReturned.load(std::memory_order_acquire)) << "第 " << round << " 轮 run() 没有返回";

            for (size_t index = 0; index < context.threadPool().threadCount(); ++index)
            {
                EXPECT_FALSE(context.threadPool().eventLoop(index).isRunning()) << "第 " << round << " 轮收尾后第 " << index << " 条循环仍在运行";
            }
        }
    }
} // namespace AsynGyanis::Core
