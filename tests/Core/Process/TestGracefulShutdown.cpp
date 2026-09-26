// GracefulShutdown 测试：停机事件的接管、动作在循环线程上按序执行、单次触发的单向性，
// 以及 POSIX 上真信号送达这一条只在实信号下才验得出的路。
// 覆盖场景：
// - ProgrammaticTriggerRunsActionsOnLoopThread（requestShutdown 与真信号走同一条路，动作落在循环线程）
// - ActionsRunInRegistrationOrderAndOnlyOnce（按注册序执行，第二次触发不重跑）
// - CancelRemovesRegisteredAction（句柄能摘掉动作）
// - ThrowingActionDoesNotStrandTheRest（一个动作抛异常，后面的照跑）
// - LateRegistrationStillRuns（收尾已开始才注册的动作补投一次，不被静默丢掉）
// - SecondObserverIsNotInstalled（每进程只装一个观察者，第二个如实报没装上）
// - DeliveredSigtermTriggersShutdown（POSIX：raise(SIGTERM) 真走一遍屏蔽字 + 等待线程）
#include "Core/Process/GracefulShutdown.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <unistd.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::EventLoopThread;
        using TestSupport::kWaitTimeout;
        using TestSupport::waitForCondition;

        /// 记录当前线程号，供「动作落在哪条线程上」的断言用
        void recordThreadId(std::thread::id &target) noexcept
        {
            target = std::this_thread::get_id();
        }
    } // namespace

    /**
     * @brief 程序侧触发与真信号同路：动作必须在事件循环线程上执行，而不是在触发方
     * @details 收尾通常要碰服务器与会话状态，而本库的线程契约是「那些状态只在所属循环上读写」。
     *          动作跑在调用线程上看着更快，实际是把契约撕开一条口子。
     */
    TEST(GracefulShutdown, ProgrammaticTriggerRunsActionsOnLoopThread)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        GracefulShutdown shutdown(runner.loop());
        ASSERT_TRUE(shutdown.isInstalled());

        std::thread::id loopThreadId;
        runner.loop().scheduler().postRemote([&loopThreadId] { recordThreadId(loopThreadId); });
        ASSERT_TRUE(waitForCondition([&loopThreadId] { return loopThreadId != std::thread::id{}; })) << "取不到循环线程号，下面的归属断言就没有参照";

        std::thread::id   actionThreadId;
        std::atomic<bool> isDone{false};
        shutdown.onShutdown(
                [&actionThreadId, &isDone]
                {
                    recordThreadId(actionThreadId);
                    isDone.store(true, std::memory_order_release);
                });

        shutdown.requestShutdown(GracefulShutdown::Reason::Programmatic);
        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); })) << "触发了却没执行收尾动作";

        EXPECT_EQ(actionThreadId, loopThreadId) << "收尾动作没有落在事件循环线程上";
        EXPECT_NE(actionThreadId, std::this_thread::get_id()) << "收尾动作跑在触发线程上，等于把线程契约绕过去";
        EXPECT_TRUE(shutdown.isTriggered());
        ASSERT_TRUE(shutdown.triggeredBy().has_value());
        EXPECT_EQ(*shutdown.triggeredBy(), GracefulShutdown::Reason::Programmatic);

        runner.join();
    }

    /**
     * @brief 动作按注册顺序执行，且触发是单向的：第二次不重跑
     * @details 连按两次 Ctrl+C 是催命，不是把优雅退出再排一遍——重跑会看到已经析构一半的状态
     */
    TEST(GracefulShutdown, ActionsRunInRegistrationOrderAndOnlyOnce)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());
        GracefulShutdown shutdown(runner.loop());

        std::vector<int>  visitOrder;
        std::atomic<bool> isDone{false};
        shutdown.onShutdown([&visitOrder] { visitOrder.push_back(1); });
        shutdown.onShutdown([&visitOrder] { visitOrder.push_back(2); });
        shutdown.onShutdown(
                [&visitOrder, &isDone]
                {
                    visitOrder.push_back(3);
                    isDone.store(true, std::memory_order_release);
                });

        shutdown.requestShutdown();
        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); }));
        shutdown.requestShutdown();
        shutdown.requestShutdown();

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        ASSERT_EQ(visitOrder.size(), 3U) << "重复触发把收尾动作又排了一遍";
        EXPECT_EQ(visitOrder[0], 1);
        EXPECT_EQ(visitOrder[1], 2);
        EXPECT_EQ(visitOrder[2], 3);

        runner.join();
    }

    /**
     * @brief 句柄能摘掉还没跑的动作
     */
    TEST(GracefulShutdown, CancelRemovesRegisteredAction)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());
        GracefulShutdown shutdown(runner.loop());

        std::atomic<bool> cancelledRan{false};
        std::atomic<bool> keptRan{false};
        const std::size_t cancelled = shutdown.onShutdown([&cancelledRan] { cancelledRan.store(true); });
        shutdown.onShutdown([&keptRan] { keptRan.store(true); });
        EXPECT_TRUE(shutdown.cancel(cancelled));
        EXPECT_FALSE(shutdown.cancel(cancelled)) << "同一个句柄摘两次应当报没有这条";

        shutdown.requestShutdown();
        ASSERT_TRUE(waitForCondition([&keptRan] { return keptRan.load(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        EXPECT_FALSE(cancelledRan.load()) << "已经摘掉的动作还是跑了";

        runner.join();
    }

    /**
     * @brief 一个动作抛异常，后面的动作照跑
     * @details 否则「一条日志器刷不进盘」就能把「把连接体面关掉」一起带走，而这正是最需要它的时刻
     */
    TEST(GracefulShutdown, ThrowingActionDoesNotStrandTheRest)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());
        GracefulShutdown shutdown(runner.loop());

        std::atomic<bool> isSecondDone{false};
        shutdown.onShutdown([]() -> void { throw std::runtime_error("故意的收尾失败"); });
        shutdown.onShutdown([&isSecondDone] { isSecondDone.store(true, std::memory_order_release); });

        shutdown.requestShutdown();
        EXPECT_TRUE(waitForCondition([&isSecondDone] { return isSecondDone.load(std::memory_order_acquire); })) << "前一个动作抛异常就把后面的整串带走了";

        runner.join();
    }

    /**
     * @brief 收尾已经开始才注册的动作补投一次，而不是被静默丢掉
     */
    TEST(GracefulShutdown, LateRegistrationStillRuns)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());
        GracefulShutdown shutdown(runner.loop());

        shutdown.requestShutdown();
        std::atomic<bool> isLateDone{false};
        const std::size_t lateToken = shutdown.onShutdown([&isLateDone] { isLateDone.store(true, std::memory_order_release); });
        EXPECT_GT(lateToken, 0U);

        EXPECT_TRUE(waitForCondition([&isLateDone] { return isLateDone.load(std::memory_order_acquire); })) << "迟到注册被静默丢掉：调用方以为登记上了";

        runner.join();
    }

    /**
     * @brief 每进程只装一个观察者，第二个如实报「没装上」
     * @details 信号掩码与处理器都是进程级的东西，两个实例只会互相抢；静默让后装的赢是更难查的错法
     */
    TEST(GracefulShutdown, SecondObserverIsNotInstalled)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        GracefulShutdown first(runner.loop());
        GracefulShutdown second(runner.loop());

        EXPECT_TRUE(first.isInstalled());
        EXPECT_FALSE(second.isInstalled()) << "第二个观察者不该抢下信号接管";

        std::atomic<bool> isDone{false};
        second.onShutdown([&isDone] { isDone.store(true, std::memory_order_release); });
        second.requestShutdown();
        EXPECT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); })) << "没装上信号接管的实例仍要能程序触发自己的动作";
        EXPECT_FALSE(first.isTriggered()) << "第二个观察者的程序触发惊动了第一个";
        EXPECT_EQ(second.triggeredBy(), GracefulShutdown::Reason::Programmatic);

        runner.join();
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 真信号送达：屏蔽字 + 等待线程 + 投回循环这一整条路
     * @details 程序触发只覆盖到「投回循环」那一段，屏蔽字与等待线程要有一次实信号才算数。
     *          构造观察者必须早于循环线程：POSIX 的屏蔽字由子线程继承，先起的线程不会被挡。
     */
    TEST(GracefulShutdown, DeliveredSigtermTriggersShutdown)
    {
        EventLoop        loop;
        GracefulShutdown shutdown(loop);
        ASSERT_TRUE(shutdown.isInstalled()) << "屏蔽或安装失败，下面的实信号断言就没有意义";

        EventLoopThread runner(loop);
        ASSERT_TRUE(runner.waitUntilRunning());

        std::thread::id loopThreadId;
        loop.scheduler().postRemote([&loopThreadId] { recordThreadId(loopThreadId); });
        ASSERT_TRUE(waitForCondition([&loopThreadId] { return loopThreadId != std::thread::id{}; }));

        std::atomic<bool> isDone{false};
        std::thread::id   actionThreadId;
        shutdown.onShutdown(
                [&]
                {
                    recordThreadId(actionThreadId);
                    isDone.store(true, std::memory_order_release);
                });

        // 必须用 kill(getpid()) 而不是 raise()：raise 送的是**线程定向**的挂起信号，只有调用线程自己能收，
        // 而本类的等待线程要靠**进程定向**的挂起信号才能取到它（raise 的这颗会一直挂在主线程的掩码后面，
        // 析构解除屏蔽时才按缺省动作把进程干掉）。这与外部 `kill -TERM <pid>` 的实际形状一致
        ASSERT_EQ(::kill(::getpid(), SIGTERM), 0) << "kill 失败，这条用例就没有把信号送出去";

        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); })) << "SIGTERM 送达后收尾没有执行";
        EXPECT_TRUE(shutdown.isTriggered());
        ASSERT_TRUE(shutdown.triggeredBy().has_value());
        EXPECT_EQ(*shutdown.triggeredBy(), GracefulShutdown::Reason::Terminate);
        EXPECT_EQ(actionThreadId, loopThreadId) << "实信号触发的收尾没有落在事件循环线程上";

        runner.join();
    }
#endif
    /**
     * @brief 没绑事件循环时动作就地执行：给「动作本来就与循环无关」的宿主用
     * @details 这类宿主（示例程序把一个原子量置假就是典型）在信号到达时往往还没起好任何循环，
     *          要求它先有一条跑着的循环才能接管停机，等于把库外的活又推回使用方
     */
    TEST(GracefulShutdown, WithoutLoopRunsActionsInline)
    {
        GracefulShutdown shutdown;
        ASSERT_TRUE(shutdown.isInstalled());

        std::thread::id actionThreadId;
        shutdown.onShutdown([&actionThreadId] { recordThreadId(actionThreadId); });

        shutdown.requestShutdown();
        EXPECT_EQ(actionThreadId, std::this_thread::get_id()) << "没绑循环却把动作投去了别处";
        EXPECT_TRUE(shutdown.isTriggered());
    }

    /**
     * @brief blockStopSignals() 可以早于构造调用，且重复调用不出错
     * @details 屏蔽字只对被屏蔽之后派生的线程生效：要在起工作线程之前先挡住，否则那些线程
     *          仍会按缺省动作把整个进程带走
     */
    TEST(GracefulShutdown, SignalsCanBeBlockedBeforeTheObserverIsBuilt)
    {
        EXPECT_TRUE(GracefulShutdown::blockStopSignals());
        EXPECT_TRUE(GracefulShutdown::blockStopSignals()) << "重复调用不该报错";

        GracefulShutdown shutdown;
        EXPECT_TRUE(shutdown.isInstalled());

        std::atomic<bool> isDone{false};
        shutdown.onShutdown([&isDone] { isDone.store(true, std::memory_order_release); });
        shutdown.requestShutdown();
        EXPECT_TRUE(isDone.load(std::memory_order_acquire));
    }
} // namespace AsynGyanis::Core
