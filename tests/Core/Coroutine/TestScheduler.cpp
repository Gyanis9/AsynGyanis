// Scheduler 单元测试：本地就绪队列、跨线程投递与队列查询
//
// 末尾的 RemotePostAllocationProfile 是跨线程投递的分配画像台账（口径与
// tests/Net/Http/TestHotPathAllocations.cpp 一致，共用 AllocationProbe）：
//   · 孤立投递（投一条立刻取走）一千次：libstdc++ 62 次 / 31744 字节，MSVC 2 次 / 128 字节；
//   · 成批投递（投 64 条再整批取完）一千轮共六万四千条：libstdc++ 4001 次，MSVC 67 次。
// 两家都不到「每投一条各要一块」，因此判据取「每 8 条至多一块」的上界而不是钉某个块大小；
// 分配判据只在 Release 下钉，Debug 仍跑同样的形状并打出读数供对照

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "CoreTestSupport.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
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

        /**
         * @brief 测试协程：以 release 语义置位标记后结束
         * @param flag 目标标记
         * @return Task<void> 无返回值
         */
        Task<void> markTask(std::atomic<bool> &flag)
        {
            flag.store(true, std::memory_order_release);
            co_return;
        }
    } // namespace

    /**
     * @brief 本地投递的任务构成待办：runOne() 能取出并执行一次，副作用落到实处
     */
    TEST(Scheduler, ScheduleAndRunOneExecutesTask)
    {
        Scheduler        scheduler;
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
        Scheduler              scheduler;
        std::atomic<int>       counter{0};
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
        Scheduler        scheduler;
        std::atomic<int> counter{0};

        auto        task = incrementTask(counter);
        std::thread remote(
                [&]()
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
        Scheduler        scheduler;
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
        Scheduler              scheduler;
        std::atomic<int>       counter{0};
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

        std::thread remote(
                [&scheduler, &executedOn, &posterThreadId]()
                {
                    posterThreadId = std::this_thread::get_id();
                    scheduler.postRemote([&executedOn]() { executedOn = std::this_thread::get_id(); });
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
        Scheduler        scheduler;
        std::vector<int> executedOrder;

        for (int index = 0; index < 3; ++index)
        {
            scheduler.postRemote([&executedOrder, index]() { executedOrder.push_back(index); });
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
        Scheduler   scheduler;
        std::thread remote([&scheduler]() { scheduler.postRemote([]() {}); });
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
            scheduler.postRemote([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
        }

        scheduler.runAll();
        EXPECT_EQ(counter.load(), 4);
        EXPECT_FALSE(scheduler.hasWork());
    }

    /**
     * @brief 一趟 runAll() 不把跨线程投递吃到见底：做满上限就把控制权交回调用方，剩下的分趟取完且一条不丢
     * @details 钉住的是公平性上界——投递方（执行器完成回调、别的循环移交的连接）可以长期不断流，
     *          没有上界的一趟会让事件循环再也回不到 epoll_wait，同循环上的套接字一个事件都收不到。
     *          同时钉住「剩余仍算待办」：漏记账会让循环带着积压睡在 epoll 上，那是比慢更糟的挂死
     */
    TEST(Scheduler, SingleRunAllPassIsBoundedOnRemoteQueue)
    {
        Scheduler        scheduler;
        std::atomic<int> executed{0};

        constexpr int kPostCount = static_cast<int>(Scheduler::kMaximumRemoteItemsPerPass) * 3;
        for (int index = 0; index < kPostCount; ++index)
        {
            scheduler.postRemote([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
        }

        scheduler.runAll();

        EXPECT_LE(executed.load(), static_cast<int>(Scheduler::kMaximumRemoteItemsPerPass)) << "一趟 runAll() 吃掉了超过上限的跨线程投递：调用方拿不回控制权，IO 事件会被饿死";
        EXPECT_TRUE(scheduler.hasWork()) << "剩下的投递没被算成待办：循环会带着积压睡在 epoll 上";

        // 分趟取用必须最终把所有投递做完：上界不能变成丢任务或取不完
        for (int pass = 0; pass < kPostCount && scheduler.hasWork(); ++pass)
        {
            scheduler.runAll();
        }
        EXPECT_EQ(executed.load(), kPostCount) << "分趟取用把投递弄丢了或没有取完";
        EXPECT_FALSE(scheduler.hasWork());
    }

    namespace
    {
        /**
         * @brief 让一条已在跑的循环确实睡进无限阻塞的那一步
         * @details run() 只在 `hasWork()` 为假时把超时取成 -1，因此投递前必须确认队列已空并留出
         *          一轮空档。一轮循环是微秒级，这里的余量放到 50 毫秒：宁可放宽前置条件也不放宽
         *          断言——前置条件宽了最坏是这次没测到唤醒路径（绿灯偏乐观），而断言宽了会把真缺陷
         *          报成失败。跨线程投递能不能被"不需要唤醒"地取走，正是要被这条用例证伪的东西。
         * @param runner 承载循环的运行器
         */
        void settleLoopIntoBlockingWait(TestSupport::EventLoopThread &runner)
        {
            ASSERT_TRUE(runner.waitUntilRunning()) << "后台循环没能进入 run()，谈不上睡在阻塞等待里";
            ASSERT_FALSE(runner.loop().scheduler().hasWork()) << "循环还没跑空，等不出「睡在阻塞等待里」这个前提";
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    } // namespace

    /**
     * @brief postRemote 能把睡在无限阻塞里的循环叫醒，并由那条循环自己执行回调
     * @details 钉住的是整条跨线程唤醒链：投递 → 全局队列 → 写唤醒器 → 目标循环从 epoll_wait/
     *          GetQueuedCompletionStatus 醒来完成派发。其余投递用例都由测试线程自己 runOne()，
     *          因此把唤醒那一段整个绕开了——唤醒接线一旦被摘掉，那些用例照绿，而线上表现是
     *          「跨循环移交的连接永不生效、stop() 之后 join 永久挂住」。
     *          新线程刚起时循环可能还不在阻塞等待里，所以先用运行器确认它已进入 run()
     */
    TEST(Scheduler, PostRemoteWakesALoopBlockedInPoll)
    {
        TestSupport::EventLoopThread runner;
        settleLoopIntoBlockingWait(runner);

        std::thread::id   executedOn{};
        std::atomic<bool> isExecuted{false};
        runner.loop().scheduler().postRemote(
                [&executedOn, &isExecuted]()
                {
                    // 载荷先写、标记最后以 release 发布：读侧用 acquire 配对才看得到线程 id
                    executedOn = std::this_thread::get_id();
                    isExecuted.store(true, std::memory_order_release);
                });

        EXPECT_TRUE(TestSupport::waitForCondition([&isExecuted] { return isExecuted.load(std::memory_order_acquire); }))
                << "睡在阻塞等待里的循环没有被 postRemote 唤醒：回调永远不会执行";
        EXPECT_EQ(executedOn, runner.threadId()) << "回调没有跑在目标循环线程上";
    }

    /**
     * @brief scheduleRemote 的协程恢复同样能叫醒睡住的循环，且恢复发生在循环线程上
     * @details 与上一条同一链路，只是载荷从 std::function 换成协程句柄：执行器完成回调、跨循环
     *          移交的连接走的都是这一支
     */
    TEST(Scheduler, ScheduleRemoteWakesALoopBlockedInPoll)
    {
        TestSupport::EventLoopThread runner;
        settleLoopIntoBlockingWait(runner);

        std::atomic<bool> isResumed{false};
        Task<void>        task = markTask(isResumed);
        runner.loop().scheduler().scheduleRemote(task.handle());
        // 帧交给运行器保管：即便这次没被恢复（用例失败），也不会在「恢复已投递、尚未执行」的
        // 窗口里被销毁，读栈时看到的就是断言失败而不是又叠一个悬垂帧
        runner.parkDriver(std::move(task));

        EXPECT_TRUE(TestSupport::waitForCondition([&isResumed] { return isResumed.load(std::memory_order_acquire); }))
                << "睡在阻塞等待里的循环没有被 scheduleRemote 唤醒：协程永远得不到恢复";
    }

    /**
     * @brief 跨线程投递的分配画像：孤立的一条与成批的六十四条各碰几次堆
     * @details 分两种形状量是因为队列容器按「块」要内存：一条投完就取走时，块会被交还，
     *          下一次投递又要一块；攒够一批再取则摊薄。分不清这两种读数就会把「每投一条一块」
     *          误判成「几乎没有分配」。口径与 tests/Net/Http/TestHotPathAllocations.cpp 一致，
     *          判据只在 Release 下钉原值，Debug 仍跑同样的形状并把读数打出来供对照
     */
    TEST(Scheduler, RemotePostAllocationProfile)
    {
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        Scheduler        scheduler;
        std::atomic<int> executed{0};

        // 形状①「一条在途」：投一条马上被 runOne() 取走——跨循环移交与执行器回调都是这种孤立投递
        const auto single = measurePerOperation(
                [&]
                {
                    scheduler.postRemote([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
                    return scheduler.runOne() ? 1U : 0U;
                });

        // 形状②「成批」：一次操作投 64 条再整批取完，看摊薄之后的读数
        constexpr std::uint64_t kBatchSize = 64U;
        const auto              batch      = measurePerOperation(
                [&]
                {
                    for (std::uint64_t index = 0; index < kBatchSize; ++index)
                    {
                        scheduler.postRemote([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
                    }
                    std::uint64_t drainedCount = 0;
                    while (scheduler.runOne())
                    {
                        ++drainedCount;
                    }
                    return drainedCount;
                });

        std::printf("scheduler-remote-post single total=%llu bytes=%llu | batch total=%llu bytes=%llu\n", static_cast<unsigned long long>(single.totalAllocations),
                    static_cast<unsigned long long>(single.totalBytes), static_cast<unsigned long long>(batch.totalAllocations), static_cast<unsigned long long>(batch.totalBytes));

        // 两条与配置无关的结构判据：被测体确实跑满了，且取用一条不丢
        EXPECT_EQ(single.resultSum, kMeasurementIterations) << "单次投递根本没被执行，读数没有意义";
        EXPECT_EQ(batch.resultSum, kMeasurementIterations * kBatchSize) << "整批取用漏了投递，读数没有意义";

#ifdef NDEBUG
        // 判据钉的是「不许退化成每投一条各要一块堆」，而不是钉某个 STL 的块大小：
        // 实测同一形状 libstdc++ 是每 16 条要一块 512 字节（一千次 62 / 六万四千条 4001），
        // MSVC 是每 512 条左右要一块（一千次 2 / 六万四千条 67），两家差两个量级但都远不到「每条一块」。
        // 取「每 8 条至多一块」当上界：既容得下换 STL 与改块大小，又能在真退化成每投一条一块时立刻报红
        constexpr std::uint64_t kMaximumBlocksPerThousandPosts = kMeasurementIterations / 8U;
        EXPECT_LE(single.totalAllocations, kMaximumBlocksPerThousandPosts) << "孤立投递的堆块数越界：队列快退化成每投一条各要一块了";
        EXPECT_LE(batch.totalAllocations, kMaximumBlocksPerThousandPosts * kBatchSize) << "成批投递的堆块数越界：同上，这条量的是六万四千条投递摊到多少块上";
#endif
    }

} // namespace AsynGyanis::Core
