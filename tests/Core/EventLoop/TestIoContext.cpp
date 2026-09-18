// IoContext 单元测试：线程池配置、主调度器、启停阻塞与运行前投递任务

#include "Core/EventLoop/IoContext.h"
#include "Core/Coroutine/Task.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::waitForCondition;

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
    }

    /**
     * @brief 构造按参数建好线程池但不启动线程：线程数原样可查
     */
    TEST(IoContext, ConstructionCreatesConfiguredThreadPool)
    {
        IoContext context(2);

        EXPECT_EQ(context.threadPool().threadCount(), 2u);
    }

    /**
     * @brief threadPool() 返回的是内部那个池（同一对象）而不是副本，调用方才能借它投递任务
     */
    TEST(IoContext, ThreadPoolAccessorReturnsConfiguredPool)
    {
        IoContext context(2);

        auto &pool = context.threadPool();
        EXPECT_EQ(pool.threadCount(), 2u);
    }

    /**
     * @brief mainScheduler() 固定指向 0 号工作线程的调度器（地址相同），且初始无待办
     */
    TEST(IoContext, MainSchedulerReturnsFirstWorkerScheduler)
    {
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
        IoContext context(1);

        // 未调用 run() 时停止应当是安全的空操作
        EXPECT_NO_THROW(context.stop());
    }

    /**
     * @brief run() 阻塞调用线程直到 stop() 被请求，之后工作线程能正常退出并 join
     */
    TEST(IoContext, RunBlocksUntilStopIsRequested)
    {
        IoContext context(1);
        std::atomic<bool> workerStarted{false};

        std::thread worker([&]()
        {
            workerStarted.store(true);
            context.run();
        });

        // 轮询等待工作线程启动，stop() 应能解除 run() 的阻塞
        ASSERT_TRUE(waitForCondition([&]()
        {
            return workerStarted.load();
        }));

        context.stop();
        worker.join();
    }

    /**
     * @brief run() 之前投递到主调度器的任务不会丢：启动后被执行，且 run() 返回后仍可读到结果
     */
    TEST(IoContext, TaskScheduledBeforeRunExecutesAfterRun)
    {
        IoContext context(1);
        std::atomic<int> value{0};

        auto task = setIoValue(value);
        context.mainScheduler().schedule(task.handle());

        std::thread worker([&]()
        {
            context.run();
        });

        // 轮询等待协程被执行，替代固定 sleep
        const bool executed = waitForCondition([&]()
        {
            return value.load() == 99;
        });

        context.stop();
        worker.join();

        EXPECT_TRUE(executed);
        EXPECT_EQ(value.load(), 99);
    }

    /**
     * @brief 默认构造的线程数等于 hardware_concurrency()，不写死某个小常数
     */
    TEST(IoContext, DefaultConstructorUsesHardwareConcurrency)
    {
        IoContext context;

        EXPECT_EQ(context.threadPool().threadCount(), std::thread::hardware_concurrency());
    }
} // namespace AsynGyanis::Core
