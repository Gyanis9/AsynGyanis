// 阻塞任务执行器测试 —— 恢复落在哪个线程，以及调用方提前丢弃 Task 时的收口形态。
// 盯住一条只在真并发下才暴露的性质：协程挂起期间工作线程手里只有堆上的共享状态，
// 一旦它把当初记下的**裸句柄**投回事件循环，而调用方此刻已经销毁 Task，循环就会 resume
// 一块已释放的帧。恢复因此必须经「取走即作废」的通道，取到空就当无事发生。
// 覆盖场景：
// - ResumesContinuationOnGivenEventLoop（恢复线程 == submit() 给定的循环线程，且不是调用线程）
// - DiscardingSuspendedTaskDoesNotResumeFreedFrame（任务开工后、交付前销毁 Task，恢复必须是空操作）
// - SubmissionQueueIsBoundedAndRejectsOverCapacity（排队有上限，超出的提交如实失败并说清原因）
// - AbandonedSubmissionDoesNotStrandTheWorker（作废的那次提交不占住工作线程，后续提交照常完成）

#include "Core/Coroutine/AsyncExecutor.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/System/CpuAffinity.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using AsynGyanis::Core::TestSupport::EventLoopThread;
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /// 一次提交的观测结果。就绪标记最后发布：finished 为真后另两个字段才可读
        struct SubmitProbe
        {
            std::atomic<int>             value{0};        ///< co_await 拿到的任务返回值
            std::atomic<std::thread::id> resumeThread{};  ///< 恢复点所在线程
            std::atomic<bool>            finished{false}; ///< 「恢复之后的代码跑完了」：以 release 发布的就绪标记
        };

        /**
         * @brief 驱动协程：提交一个被门禁卡住的阻塞任务，恢复后把结果记进探针
         * @param executor 目标执行器
         * @param completionLoop 恢复用的事件循环
         * @param probe 观测结果
         * @param workStarted 工作线程进入任务时置真
         * @param workDone 工作线程跑完任务时置真
         * @param releaseGate 工作线程在此等待，由用例决定何时放行
         */
        Core::Task<void> probeSubmit(AsyncExecutor &executor, Core::EventLoop &completionLoop, SubmitProbe &probe, std::atomic<bool> &workStarted, std::atomic<bool> &workDone,
                                     const std::shared_future<void> &releaseGate)
        {
            const int result = co_await executor.submit<int>(completionLoop,
                                                             [&workStarted, &workDone, releaseGate]()
                                                             {
                                                                 workStarted.store(true, std::memory_order_release);
                                                                 releaseGate.wait();
                                                                 workDone.store(true, std::memory_order_release);
                                                                 return 42;
                                                             });

            // 载荷先写、就绪标记最后以 release 发布：等待方以 acquire 配对后才能读到上面的值
            probe.value.store(result, std::memory_order_relaxed);
            probe.resumeThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
            probe.finished.store(true, std::memory_order_release);
        }

        /**
         * @brief 有界轮询等到标记到位，等不到即失败
         * @param predicate 标记读取
         * @param failureText 失败时打印的中文说明
         */
        template<typename Predicate>
        void requireReached(Predicate predicate, const char *const failureText)
        {
            EXPECT_TRUE(waitForCondition(std::move(predicate))) << failureText;
        }

        /**
         * @brief 恢复落在 submit() 指定的事件循环线程上，而不是工作线程或调用线程
         * @details 工作线程只跑阻塞任务；若哪天改成在工作线程上就地恢复，调用方「协程后续代码与
         *          循环同线程」的假设就全错了。驱动帧交给 EventLoopThread::parkDriver 保管：
         *          帧在循环手里时从别的线程销毁它是数据竞争，因此销毁必须晚于循环线程 join
         */
        TEST(AsyncExecutor, ResumesContinuationOnGivenEventLoop)
        {
            AsyncExecutor executor(1);

            SubmitProbe     probe;
            EventLoopThread loopThread; // 声明在探针之后：销毁顺序因此是先 join 循环、后销毁探针

            std::atomic<bool>              workStarted{false};
            std::atomic<bool>              workDone{false};
            std::promise<void>             releaseSignal;
            const std::shared_future<void> releaseGate = releaseSignal.get_future().share();

            Core::Task<void> driver = probeSubmit(executor, loopThread.loop(), probe, workStarted, workDone, releaseGate);
            // 内联启动：await_suspend 只做入队、不阻塞，控制权立刻回到调用线程
            driver.handle().resume();
            loopThread.parkDriver(std::move(driver));

            requireReached([&workStarted]() { return workStarted.load(std::memory_order_acquire); }, "工作线程没接到任务");
            releaseSignal.set_value();

            requireReached([&probe]() { return probe.finished.load(std::memory_order_acquire); }, "协程没被恢复");
            EXPECT_EQ(probe.value.load(std::memory_order_relaxed), 42);
            EXPECT_EQ(probe.resumeThread.load(std::memory_order_relaxed), loopThread.threadId()) << "恢复没落在指定事件循环的线程上";
            EXPECT_NE(probe.resumeThread.load(std::memory_order_relaxed), std::this_thread::get_id()) << "恢复不该发生在调用线程上";
        }

        /**
         * @brief 任务开工后、交付前丢弃 Task：工作线程之后那次恢复必须是空操作
         * @details 协程挂起期间调用方销毁 Task，帧连同等待体一起析构。工作线程若仍按裸句柄投递恢复，
         *          事件循环就会 resume 一块已释放的帧——释放后使用。
         *          循环刻意不启动、由本线程 runAll() 才处理恢复，这样「帧已销毁」与「恢复被处理」两个
         *          时刻被拉成确定的先后，不依赖任何调度运气。
         * @note 摘掉等待体析构里的作废动作后，本用例在 ASan 下报 heap-use-after-free（无 ASan 时以
         *       0xc0000005 崩在 resume 上）
         */
        TEST(AsyncExecutor, DiscardingSuspendedTaskDoesNotResumeFreedFrame)
        {
            // 循环声明在执行器之前：成员逆序析构因此是先 join 全部工作线程、后销毁循环，
            // 不会留下「工作线程正往已析构的循环里投递恢复」的窗口
            Core::EventLoop loop; // 不启动：恢复只排进队列，不会有人执行
            AsyncExecutor   executor(1);

            SubmitProbe                    probe;
            std::atomic<bool>              workStarted{false};
            std::atomic<bool>              workDone{false};
            std::promise<void>             releaseSignal;
            const std::shared_future<void> releaseGate = releaseSignal.get_future().share();

            {
                Core::Task<void> driver = probeSubmit(executor, loop, probe, workStarted, workDone, releaseGate);
                driver.handle().resume(); // 提交：任务入队、协程挂起
                requireReached([&workStarted]() { return workStarted.load(std::memory_order_acquire); }, "工作线程没接到任务");
                // driver 在本作用域末尾析构 → 协程帧连同等待体一起销毁，堆状态里的句柄随之作废
            }

            releaseSignal.set_value(); // 工作线程此刻才交付结果：它取到的必须是空句柄
            requireReached([&workDone]() { return workDone.load(std::memory_order_acquire); }, "工作线程没跑完任务");

            // 队列里若真留着那次恢复，runAll() 这一刻就会踩已释放的帧；空操作则什么都不会发生
            loop.scheduler().runAll();
            EXPECT_FALSE(probe.finished.load(std::memory_order_acquire)) << "帧已销毁，这次恢复不该跑任何代码";
        }

        /**
         * @brief 作废一次提交不会占住工作线程：后续提交照样跑完
         * @details 执行器只有 1 个工作线程，若被丢弃的那次提交把线程卡在恢复投递上、或把循环的
         *          就绪队列弄坏，第二条提交就永远跑不到——那是比多恢复一次更难查的故障。
         *          两条提交共用同一个未启动的循环，恢复一律由本线程在轮询里 runAll() 处理，
         *          因此帧始终在被销毁之前就已经停在 final_suspend 上
         */
        TEST(AsyncExecutor, AbandonedSubmissionDoesNotStrandTheWorker)
        {
            Core::EventLoop loop; // 同上：先 join 工作线程再销毁循环
            AsyncExecutor   executor(1);

            SubmitProbe                    abandonedProbe;
            std::atomic<bool>              abandonedStarted{false};
            std::atomic<bool>              abandonedDone{false};
            std::promise<void>             abandonGate;
            const std::shared_future<void> abandonedReleaseGate = abandonGate.get_future().share();

            {
                Core::Task<void> driver = probeSubmit(executor, loop, abandonedProbe, abandonedStarted, abandonedDone, abandonedReleaseGate);
                driver.handle().resume();
                requireReached([&abandonedStarted]() { return abandonedStarted.load(std::memory_order_acquire); }, "工作线程没接到被丢弃的任务");
                // 出作用域即销毁帧，句柄留在堆状态里作废——与下一条用例同一收口形态
            }
            abandonGate.set_value();
            requireReached([&abandonedDone]() { return abandonedDone.load(std::memory_order_acquire); }, "工作线程没跑完被丢弃的任务");

            // 第二条的门禁在提交前就打开：这条只需要证明「还能跑完」
            std::promise<void> followUpSignal;
            followUpSignal.set_value();
            const std::shared_future<void> followUpGate = followUpSignal.get_future().share();

            SubmitProbe       followUpProbe;
            std::atomic<bool> followUpStarted{false};
            std::atomic<bool> followUpDone{false};
            Core::Task<void>  followUp = probeSubmit(executor, loop, followUpProbe, followUpStarted, followUpDone, followUpGate);
            followUp.handle().resume();

            // 工作线程先置 workDone 再投递恢复，因此必须在轮询里反复 runAll() 把恢复取出来，
            // 不能等到 followUpDone 就断言已经恢复完（那是赌两个线程恰好重叠）
            const bool isFollowUpResumed = waitForCondition(
                    [&loop, &followUpProbe]()
                    {
                        loop.scheduler().runAll();
                        return followUpProbe.finished.load(std::memory_order_acquire);
                    });
            ASSERT_TRUE(isFollowUpResumed) << "被丢弃的提交占住了工作线程，后续提交没能完成";
            EXPECT_EQ(followUpProbe.value.load(std::memory_order_relaxed), 42);
            EXPECT_FALSE(abandonedProbe.finished.load(std::memory_order_acquire)) << "被丢弃的那次提交不该产出结果";
        }
    } // namespace

    /**
     * @brief 自动档的工作线程数不得越过本进程的 CPU 约束（与线程池同一口径）
     * @details 工作线程干的是压缩这类纯 CPU 活：在配额受限的容器里按宿主核数起线程，
     *          会把配额内那点 CPU 时间从事件循环手里抢走，外派压缩反而拖垮了它要保护的一方
     */
    TEST(AsyncExecutor, AutoWorkerCountStaysWithinProcessCpuLimits)
    {
        AsyncExecutor executor;

        EXPECT_GE(executor.workerCount(), 1U) << "自动档起出 0 个工作线程：提交的任务永远不会有结果";

        if (const std::size_t allowedCoreCount = Platform::CpuAffinity::availableCoreCount(); allowedCoreCount > 0)
        {
            EXPECT_LE(executor.workerCount(), allowedCoreCount) << "工作线程数超出了本进程被允许的核集合，配额内的 CPU 会被从事件循环手里抢走";
        }
    }
    /**
     * @brief 排队有上限：工作线程被占住时，超出的提交如实失败而不是把队列无限撑大
     * @details 无界队列把「下游比提交方慢」从延迟问题变成内存问题：每个在途请求都往队列里
     *          留下一份闭包加一份堆上的共享状态，进程先被自己的排队撑死。这里要求上限生效，
     *          且拒绝的理由写的是「排队已满」（该降并发），不是「执行器已停止」（那是生命周期用错了）。
     *          重叠条件由用例自己造：一条任务卡在门闩上，等它**已被工作线程取走**才开始灌队列，
     *          于是排队数只可能来自后面这批提交，判据可以写成精确值
     */
    TEST(AsyncExecutor, SubmissionQueueIsBoundedAndRejectsOverCapacity)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        AsyncExecutor     executor(1);
        std::atomic<bool> isGateOpen{false};
        std::atomic<bool> isGateKeeperRunning{false};

        Task<int> gateKeeper = executor.submit<int>(runner.loop(),
                                                    [&isGateOpen, &isGateKeeperRunning]()
                                                    {
                                                        isGateKeeperRunning.store(true, std::memory_order_release);
                                                        while (!isGateOpen.load(std::memory_order_acquire))
                                                        {
                                                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                                        }
                                                        return 0;
                                                    });
        gateKeeper.handle().resume();
        ASSERT_TRUE(waitForCondition([&isGateKeeperRunning] { return isGateKeeperRunning.load(std::memory_order_acquire); }))
                << "门闩任务没被那唯一的工作线程取走，后面的排队数就说不清是谁占的";

        constexpr std::size_t  kCapacity = AsyncExecutor::kMaximumPendingTasksPerWorker;
        std::vector<Task<int>> heldTasks;
        heldTasks.reserve(kCapacity + 8);
        std::size_t rejectedCount = 0;
        std::string firstRejectionText;

        for (std::size_t index = 0; index < kCapacity + 8; ++index)
        {
            Task<int> task = executor.submit<int>(runner.loop(), []() { return 1; });
            task.handle().resume();
            // 被拒的提交在 await_suspend 里就不挂起了：协程当场跑到 await_resume 抛出并结束。
            // 此刻工作线程仍被门闩占着，因此这里读帧不与之相撞（已排上的任务不可能被恢复）
            if (task.isReady())
            {
                ++rejectedCount;
                if (firstRejectionText.empty())
                {
                    try
                    {
                        static_cast<void>(task.handle().promise().result());
                    } catch (const std::exception &rejection)
                    {
                        firstRejectionText = rejection.what();
                    }
                }
            }
            heldTasks.push_back(std::move(task));
        }

        EXPECT_EQ(executor.pendingTaskCount(), kCapacity) << "队列长度越过了每线程上限：排队仍然是无界的";
        EXPECT_EQ(rejectedCount, 8U) << "超出上限的提交数应当全部被拒";
        EXPECT_NE(firstRejectionText.find("排队已满"), std::string::npos) << "拒绝原因要说清是排队满了（该降并发），文案是：" + firstRejectionText;

        isGateOpen.store(true, std::memory_order_release);
        ASSERT_TRUE(waitForCondition([&executor] { return executor.pendingTaskCount() == 0; })) << "放行之后排队的任务没有做完";
        // 帧的销毁必须晚于循环线程收手（见 EventLoopThread 的销毁纪律）：先显式 join 再让上面
        // 那批 Task 出作用域，否则收尾里可能有人在 resume 已经消亡的帧
        runner.join();
    }


} // namespace AsynGyanis::Core
