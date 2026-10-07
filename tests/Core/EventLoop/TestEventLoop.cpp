// EventLoop 单元测试：启停状态、跨线程唤醒、调度器接入与协程执行

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// 关掉后端句柄这一步要直接摸平台 API：Linux 上是文件描述符，Windows 上是完成端口的 HANDLE
#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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
        Task<int> setValue(std::atomic<int> &value)
        {
            value.store(42);
            co_return 0;
        }

        /**
         * @brief 把事件后端的句柄关到内核那里去，让下一次 wait() 只能以系统错误抛出
         * @param loop 目标循环
         */
        void closeBackendHandle(EventLoop &loop)
        {
#if ASYN_PLATFORM_WIN32
            static_cast<void>(::CloseHandle(loop.epoll().fileDescriptor()));
#else
            static_cast<void>(::close(loop.epoll().fileDescriptor()));
#endif
        }
    } // namespace

    /**
     * @brief 钉住：后端抛过一次之后 run() 真的返回，而不是「读数说停了、线程还在派发」
     * @details run() 的两个 catch 置 `m_stoppedByFailure` 与 `m_running=false`，而这两个原子量是全仓
     *          唯一的「这条循环还在不在跑」判据（EventLoop.h 对 isRunning()/stoppedByFailure 的 @note
     *          就是这么写的）。修复前它们置完就落回 while 的开头继续跑：读数说已经停了，事件照派发、
     *          协程照恢复；而 ~EventLoop 按 `m_running==false` 跳过 stop()，紧接着在还在等事件的线程
     *          底下销毁后端与唤醒套接字；持续失败的那一格还会退化成每轮一条 ERROR 的满核空转。
     * @note 判据是「run() 有没有返回」，且**刻意不靠 stop() 收口**——旧代码被 stop() 也叫得醒，
     *       那样这条判据就是假的。触发方式：关掉后端句柄（Linux 上下一次 epoll_wait 直接 EBADF，
     *       Windows 上完成端口已关 → GetQueuedCompletionStatusEx 报错），再投一条空可调用体把
     *       正阻塞着的那一觉叫醒——不投这一下，阻塞中的等待不受 close 影响，抛点就永远到不了。
     *       派发级异常走不到这里（Scheduler::runGuarded 与 IoWatcher 逐条就地收下），所以落的正是
     *       「循环自身设施坏了」那一格。
     */
    TEST(EventLoop, RunReturnsAfterBackendThrows)
    {
        EventLoop         loop;
        std::atomic<bool> runReturned{false};
        std::thread       worker(
                [&loop, &runReturned]
                {
                    loop.run();
                    runReturned.store(true, std::memory_order_release);
                });

        ASSERT_TRUE(waitForCondition([&loop] { return loop.isRunning(); })) << "循环没进入 run()：本用例什么都没测";

        closeBackendHandle(loop);
        static_cast<void>(loop.scheduler().postRemote([] {})); // 叫醒那一觉，让抛点真的走到

        EXPECT_TRUE(waitForCondition([&runReturned] { return runReturned.load(std::memory_order_acquire); }))
                << "后端抛出之后 run() 没有退出：isRunning()/stoppedByFailure 已经在说谎，线程还在派发事件与恢复协程";

        // 断言之后仍要把线程收干净：万一判据是红的（旧代码），stop() 是它唯一的出路，不能让用例挂死
        loop.stop();
        worker.join();

        const EventLoopSnapshot snapshot = loop.snapshot();
        EXPECT_TRUE(snapshot.stoppedByFailure) << "异常没有落到循环的收口分支：本用例没测到它想测的那一格";
        EXPECT_FALSE(snapshot.isRunning);

        // 收尾一格自证「停了是真的不接单」：run() 已经返回，投进去的东西没有人会执行
        bool resumedAfterExit = false;
        static_cast<void>(loop.scheduler().postRemote([&resumedAfterExit] { resumedAfterExit = true; }));
        EXPECT_FALSE(resumedAfterExit) << "run() 返回之后仍有循环在派发投递进来的可调用体";
    }

    /**
     * @brief 构造即备好有效的 epoll 句柄，且此时并不处于运行态（run() 之前不误报 running）
     */
    TEST(EventLoop, ConstructionAllocatesValidEpoll)
    {
        EventLoop loop;

        EXPECT_TRUE(Platform::isEpollHandleValid(loop.epoll().fileDescriptor()));
        EXPECT_FALSE(loop.isRunning());
    }

    /**
     * @brief run()/stop() 的状态流转：进入循环后 isRunning() 为 true，stop() 并 join 后回到 false
     */
    TEST(EventLoop, RunAndStopTransitionsRunningState)
    {
        EventLoop         loop;
        std::atomic<bool> workerStarted{false};
        std::thread       worker(
                [&]()
                {
                    workerStarted.store(true);
                    loop.run();
                });

        // 轮询等待事件循环真正进入 run()
        ASSERT_TRUE(waitForCondition([&]() { return workerStarted.load() && loop.isRunning(); }));

        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    /**
     * @brief run() 之前就能拿到调度器（初始无待办）：允许先投递任务再启动循环
     */
    TEST(EventLoop, SchedulerIsAccessibleBeforeRun)
    {
        EventLoop loop;

        EXPECT_FALSE(loop.scheduler().hasWork());
    }

    /**
     * @brief wake() 能打断阻塞中的 epoll_wait：唤醒后 stop() 仍能正常退出，证明唤醒没有丢事件或卡住循环
     */
    TEST(EventLoop, WakeInterruptsBlockingWait)
    {
        EventLoop         loop;
        std::atomic<bool> workerStarted{false};
        std::thread       worker(
                [&]()
                {
                    workerStarted.store(true);
                    loop.run();
                });

        ASSERT_TRUE(waitForCondition([&]() { return workerStarted.load() && loop.isRunning(); }));

        // 先唤醒阻塞在 epoll_wait 中的事件循环，随后停止应能正常退出
        loop.wake();
        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    /**
     * @brief 投递到循环调度器的协程能被 runOne() 取出并执行，副作用落到调用方可见的变量上
     */
    TEST(EventLoop, SchedulerExecutesScheduledCoroutine)
    {
        EventLoop        loop;
        std::atomic<int> value{0};

        auto task = setValue(value);
        ASSERT_NE(task.handle(), nullptr);
        loop.scheduler().schedule(task.handle());

        EXPECT_TRUE(loop.scheduler().runOne());
        EXPECT_EQ(value.load(), 42);
    }

    /**
     * @brief runAll() 把已投递的多个协程全部执行（每个各自的结果都落位），不会只跑第一个
     */
    TEST(EventLoop, RunAllExecutesMultipleCoroutines)
    {
        EventLoop               loop;
        int                     executionCount = 0;
        int                     results[3]{};
        std::vector<Task<void>> tasks;

        // 三个协程各自记录执行结果。惰性 Task 的协程帧记住的是闭包对象的地址：闭包必须
        // 先落到具名变量上再调用，临时闭包在语句结束即销毁，恢复协程时读到的捕获已是死对象
        auto firstBody = [&]() -> Task<void>
        {
            ++executionCount;
            results[0] = 1;
            co_return;
        };
        auto secondBody = [&]() -> Task<void>
        {
            ++executionCount;
            results[1] = 2;
            co_return;
        };
        auto thirdBody = [&]() -> Task<void>
        {
            ++executionCount;
            results[2] = 3;
            co_return;
        };
        auto first  = firstBody();
        auto second = secondBody();
        auto third  = thirdBody();

        loop.scheduler().schedule(first.handle());
        loop.scheduler().schedule(second.handle());
        loop.scheduler().schedule(third.handle());
        tasks.push_back(std::move(first));
        tasks.push_back(std::move(second));
        tasks.push_back(std::move(third));

        loop.scheduler().runAll();

        EXPECT_EQ(executionCount, 3);
        EXPECT_EQ(results[0], 1);
        EXPECT_EQ(results[1], 2);
        EXPECT_EQ(results[2], 3);
    }

    /**
     * @brief 另一线程调用 stop() 能解除 run() 的阻塞并让工作线程 join 返回（stop() 线程安全）
     */
    TEST(EventLoop, StopFromAnotherThreadExitsLoop)
    {
        EventLoop   loop;
        std::thread worker([&]() { loop.run(); });

        // 轮询等待事件循环进入运行状态，替代固定 sleep
        ASSERT_TRUE(waitForCondition([&]() { return loop.isRunning(); }));

        loop.stop();
        worker.join();
        EXPECT_FALSE(loop.isRunning());
    }

    /**
     * @brief 重复 stop() 安全：多余的一次不抛异常、不影响退出（多路径收尾可叠加调用）
     */
    TEST(EventLoop, DoubleStopIsSafe)
    {
        EventLoop   loop;
        std::thread worker([&]() { loop.run(); });

        ASSERT_TRUE(waitForCondition([&]() { return loop.isRunning(); }));

        loop.stop();
        EXPECT_NO_THROW(loop.stop());
        worker.join();
    }

    /**
     * @brief 停止请求是粘性的且实例不支持重启：先 stop() 再 run() 立刻返回，绝不吞掉那次请求
     * @details 「先 stop 后 run」若被吞掉，工作线程会永远阻塞在 epoll_wait 上、join 随之卡死；
     *          代价是同一实例无法重启（需要重新运行请新建实例），这是刻意保留的取舍
     */
    TEST(EventLoop, StopBeforeRunIsStickyAndRunReturnsImmediately)
    {
        EventLoop loop;

        // 停止请求先于 run() 到达时必须被保留：start() 之后立刻 stop() 是常见写法，
        // 它会走到「标志已置位、工作线程还没进入 run()」这个时序。若 run() 开头清除该标志，
        // 这次停止请求就被吞掉，工作线程会永远阻塞在 epoll_wait 上、join 随之卡死
        // （这不是假设：按「可重启」改法实现后，ThreadPool.DoubleStopIsSafe 实测挂死）
        loop.stop();
        EXPECT_NO_THROW(loop.run());
        EXPECT_FALSE(loop.isRunning());

        // 停止请求是粘性的，同一实例不支持重启：再次 run() 同样立刻返回。
        // 这是为换取上面那条安全性质而刻意保留的取舍，需要重新运行请新建实例
        EXPECT_NO_THROW(loop.run());
        EXPECT_FALSE(loop.isRunning());
    }
} // namespace AsynGyanis::Core
