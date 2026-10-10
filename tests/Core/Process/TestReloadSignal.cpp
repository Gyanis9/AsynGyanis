// ReloadSignal 测试：SIGHUP 的接管、动作在循环线程上按序执行、注册表跨轮次保留，
// 以及 POSIX 上真信号送达这一条只在实信号下才验得出的路。
// 覆盖场景：
// - ProgrammaticTriggerRunsActionsOnLoopThread（requestReload 与真信号走同一条路，动作落在循环线程）
// - ActionsRunInRegistrationOrder（按注册序执行）
// - RegisteredActionsRunAgainOnEveryTrigger（重载不是一次性的：第二次触发还要跑同一批动作）
// - CancelRemovesRegisteredAction（句柄能摘掉动作）
// - ThrowingActionDoesNotStrandTheRest（一个动作抛异常，后面的照跑，计数照样记一次）
// - SecondObserverIsNotInstalled（POSIX：每进程只装一个观察者，第二个如实报没装上）
// - DeliveredSighupTriggersReload（POSIX：kill(getpid(), SIGHUP) 真走一遍屏蔽字 + 等待线程）
// - WindowsHasNoReloadChannelButProgrammaticTriggerStillRuns（Windows：平台事实 + 程序侧仍可驱动）
#include "Core/Process/ReloadSignal.h"
#include "Core/Process/GracefulShutdown.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
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
     * @brief 程序侧触发：动作落在绑定的循环线程上，并把触发次数记进 reloadCount()
     * @details 这一条钉的是「与真信号同一条路」的前半：投回循环那段。屏蔽字与等待线程由
     *          DeliveredSighupTriggersReload 那一条覆盖。
     */
    TEST(ReloadSignal, ProgrammaticTriggerRunsActionsOnLoopThread)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        ReloadSignal reload(runner.loop());

        std::thread::id loopThreadId;
        runner.loop().scheduler().postRemote([&loopThreadId] { recordThreadId(loopThreadId); });
        ASSERT_TRUE(waitForCondition([&loopThreadId] { return loopThreadId != std::thread::id{}; }));

        std::atomic<bool> isDone{false};
        std::thread::id   actionThreadId;
        reload.onReload(
                [&]
                {
                    recordThreadId(actionThreadId);
                    isDone.store(true, std::memory_order_release);
                });

        reload.requestReload();

        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); })) << "requestReload 之后动作没跑";
        EXPECT_EQ(actionThreadId, loopThreadId) << "重载动作没有落在事件循环线程上";
        EXPECT_EQ(reload.reloadCount(), 1U) << "一次触发该记一次";

        runner.join();
    }

    TEST(ReloadSignal, ActionsRunInRegistrationOrder)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        ReloadSignal      reload(runner.loop());
        std::atomic<bool> isDone{false};
        std::vector<int>  order;

        reload.onReload([&order] { order.push_back(1); });
        reload.onReload([&order] { order.push_back(2); });
        reload.onReload(
                [&]
                {
                    order.push_back(3);
                    isDone.store(true, std::memory_order_release);
                });

        reload.requestReload();
        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); }));
        EXPECT_EQ(order, (std::vector<int>{1, 2, 3})) << "注册顺序就是执行顺序，重排过的实现会把带依赖的两步换到彼此之后";

        runner.join();
    }

    /**
     * @brief 注册表跨轮次保留：第二次触发要把同一批动作再跑一遍
     * @details 这是重载与停机的核心差别。把 `fireReload()` 改成「取走并清空注册表」（照抄停机那一套）
     *          会让这条用例当场红：第一次 HUP 之后，这个进程的 reload 就再也不做任何事，
     *          而 `reloadCount()` 照样往上加——只有计数的运维面看出去是「一切正常」。
     */
    TEST(ReloadSignal, RegisteredActionsRunAgainOnEveryTrigger)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        ReloadSignal          reload(runner.loop());
        std::atomic<unsigned> runs{0};
        std::atomic<bool>     secondRoundDone{false};
        const std::size_t     token = reload.onReload(
                [&]
                {
                    ++runs;
                    if (runs.load(std::memory_order_acquire) >= 2U)
                    {
                        secondRoundDone.store(true, std::memory_order_release);
                    }
                });

        reload.requestReload();
        ASSERT_TRUE(waitForCondition([&runs] { return runs.load(std::memory_order_acquire) >= 1U; })) << "第一轮没跑动作";
        EXPECT_EQ(reload.reloadCount(), 1U);

        reload.requestReload();
        ASSERT_TRUE(waitForCondition([&secondRoundDone] { return secondRoundDone.load(std::memory_order_acquire); }))
                << "第二轮没再跑同一批动作：注册表被取走了，这等于 reload 只能用一次";
        EXPECT_EQ(reload.reloadCount(), 2U) << "两次触发该记两次——重载不是单向一次性事件";
        EXPECT_TRUE(reload.cancel(token));

        runner.join();
    }

    TEST(ReloadSignal, CancelRemovesRegisteredAction)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        ReloadSignal      reload(runner.loop());
        std::atomic<bool> keptRan{false};
        std::atomic<bool> cancelledRan{false};

        reload.onReload([&keptRan] { keptRan.store(true, std::memory_order_release); });
        const std::size_t doomed = reload.onReload([&cancelledRan] { cancelledRan.store(true, std::memory_order_release); });
        EXPECT_TRUE(reload.cancel(doomed));
        EXPECT_FALSE(reload.cancel(doomed)) << "同一个句柄摘第二次应当报「不存在」，否则调用方分不清是没注册上还是已经摘过";

        reload.requestReload();
        ASSERT_TRUE(waitForCondition([&keptRan] { return keptRan.load(std::memory_order_acquire); }));
        // 给被摘掉的那个一点时间：它若还挂在注册表上就会在同一个轮次里跑到
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        EXPECT_FALSE(cancelledRan.load(std::memory_order_acquire)) << "cancel() 没真的把动作摘掉";

        runner.join();
    }

    /**
     * @brief 一个动作抛异常，后面的照跑，且这一轮仍计一次触发
     * @details 与停机观察者同一条理由：重读失败不该把「把新配置接上」整段带走。计数口径也要一并钉住
     *          ——计数若按「跑完了几个动作」算，一次异常就会让它少记，运维面就看不出 HUP 到过。
     */
    TEST(ReloadSignal, ThrowingActionDoesNotStrandTheRest)
    {
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        ReloadSignal      reload(runner.loop());
        std::atomic<bool> laterRan{false};

        reload.onReload([] { throw std::runtime_error("重读失败注入"); });
        reload.onReload([&laterRan] { laterRan.store(true, std::memory_order_release); });

        reload.requestReload();
        ASSERT_TRUE(waitForCondition([&laterRan] { return laterRan.load(std::memory_order_acquire); })) << "前一个动作抛异常就把后面的整段带走了";
        EXPECT_EQ(reload.reloadCount(), 1U) << "触发计数应当按「触发了几次」算，不按「跑成了几个动作」算";

        runner.join();
    }

#if ASYN_PLATFORM_WIN32

    /**
     * @brief Windows：没有 SIGHUP 这条约定，接管装不上，但程序侧触发仍可驱动同一批动作
     * @details 「装不上」必须出声（isInstalled 为假）而不是静默成功——静默成功的表现是部署方以为
     *          `kill -HUP` 能重读配置，而 Windows 上根本没有这条路。
     */
    TEST(ReloadSignal, WindowsHasNoReloadChannelButProgrammaticTriggerStillRuns)
    {
        ReloadSignal      reload; // 不绑循环：动作就地跑
        std::atomic<bool> isDone{false};

        reload.onReload([&isDone] { isDone.store(true, std::memory_order_release); });

        EXPECT_FALSE(reload.isInstalled()) << "Windows 上没有 SIGHUP，接管不该报告装上了";
        reload.requestReload();
        EXPECT_TRUE(isDone.load(std::memory_order_acquire)) << "程序侧触发不该因为平台没有信号而一起废掉";
        EXPECT_EQ(reload.reloadCount(), 1U);
    }

#else

    TEST(ReloadSignal, SecondObserverIsNotInstalled)
    {
        EventLoop    first;
        ReloadSignal firstObserver(first);
        ASSERT_TRUE(firstObserver.isInstalled()) << "屏蔽或安装失败，后面的断言就没有意义";

        // 被绑定的循环得真在跑：动作是投回那条循环的，循环没跑就等于投进一个没人清的队列
        EventLoopThread runner(first);
        ASSERT_TRUE(runner.waitUntilRunning());

        EventLoop    secondLoop;
        ReloadSignal second(secondLoop);
        EXPECT_FALSE(second.isInstalled()) << "第二个观察者不应装上接管：屏蔽字与待取队列都是进程级的，两个实例只会互相抢";

        // 第一个仍然是唯一接管的：真信号只让它触发一次
        std::atomic<bool> firstRan{false};
        firstObserver.onReload([&firstRan] { firstRan.store(true, std::memory_order_release); });
        ASSERT_EQ(::kill(::getpid(), SIGHUP), 0) << "kill 失败，这条用例就没有把信号送出去";
        ASSERT_TRUE(waitForCondition([&firstRan] { return firstRan.load(std::memory_order_acquire); })) << "SIGHUP 没让第一个观察者动";
        EXPECT_EQ(second.reloadCount(), 0U) << "没装上接管的实例不该自己也记一次触发";

        runner.join();
    }

    /**
     * @brief 真信号送达：屏蔽字 + 等待线程 + 投回循环这一整条路
     * @details 程序触发只覆盖到「投回循环」那一段，屏蔽字与等待线程要有一次实信号才算数。
     *          构造观察者必须早于循环线程：POSIX 的屏蔽字由子线程继承，先起的线程不会被挡。
     * @note 用 kill(getpid(), SIGHUP) 而不是 raise()：raise 送的是**线程定向**的挂起信号，只有调用线程
     *       自己能收，等待线程要靠**进程定向**的那颗（与外部 `kill -HUP <pid>` 的实际形状一致）。
     *       顺带钉住一条契约变更：缺省的 SIGHUP 动作是「终止进程」，装上之后不再终止——本条能跑完本身就是证据。
     */
    /**
     * @brief 两个观察者并存且**先装重载、后装停机**时，两枚信号都要各自落到对的一方
     * @details 这条用例钉的是上一版本真犯的错：屏蔽字只被子线程继承，先起的等待线程没挡住后一路的号，
     *          于是进程定向的 SIGTERM 可能被那条「只等 SIGHUP」的线程接住，并按缺省动作把进程直接杀掉
     *          （端到端探针里的形状是 STOPPING=1 没发出去、退出码 -15）。
     *          两个观察者的等待线程现在互为对方补掩码，构造顺序因此不再有意义。
     */
    TEST(ReloadSignal, BothObserversWorkWhicheverOrderTheyAreInstalled)
    {
        EventLoop    loop;
        ReloadSignal reload(loop);
        ASSERT_TRUE(reload.isInstalled()) << "先装的重载观察者没装上，后面的断言就没有意义";

        GracefulShutdown shutdown(loop);
        ASSERT_TRUE(shutdown.isInstalled()) << "后装的停机观察者没装上，后面的断言就没有意义";

        EventLoopThread runner(loop);
        ASSERT_TRUE(runner.waitUntilRunning());

        std::atomic<bool> reloadRan{false};
        std::atomic<bool> shutdownRan{false};
        reload.onReload([&reloadRan] { reloadRan.store(true, std::memory_order_release); });
        shutdown.onShutdown([&shutdownRan] { shutdownRan.store(true, std::memory_order_release); });

        // 先一枚 SIGHUP：该触发重载，且不能把进程带走
        ASSERT_EQ(::kill(::getpid(), SIGHUP), 0) << "kill 失败，信号没送出去";
        ASSERT_TRUE(waitForCondition([&reloadRan] { return reloadRan.load(std::memory_order_acquire); })) << "SIGHUP 没触发重载";
        EXPECT_FALSE(shutdown.isTriggered()) << "重载信号被停机观察者接走了：两条通道混了";

        // 再一枚 SIGTERM：该触发收尾。这一枚若被那条「只等 SIGHUP」的线程接住，进程就已经按缺省动作死了
        ASSERT_EQ(::kill(::getpid(), SIGTERM), 0) << "kill 失败，信号没送出去";
        ASSERT_TRUE(waitForCondition([&shutdownRan] { return shutdownRan.load(std::memory_order_acquire); })) << "SIGTERM 没触发收尾——多半是被另一条等待线程按缺省动作处理掉了";
        EXPECT_TRUE(shutdown.isTriggered());
        EXPECT_EQ(reload.reloadCount(), 1U) << "重载的计数不该被停机那一路动过";

        runner.join();
    }

    TEST(ReloadSignal, DeliveredSighupTriggersReload)
    {
        EventLoop    loop;
        ReloadSignal reload(loop);
        ASSERT_TRUE(reload.isInstalled()) << "屏蔽或安装失败，下面的实信号断言就没有意义";

        EventLoopThread runner(loop);
        ASSERT_TRUE(runner.waitUntilRunning());

        std::thread::id loopThreadId;
        loop.scheduler().postRemote([&loopThreadId] { recordThreadId(loopThreadId); });
        ASSERT_TRUE(waitForCondition([&loopThreadId] { return loopThreadId != std::thread::id{}; }));

        std::atomic<bool> isDone{false};
        std::thread::id   actionThreadId;
        reload.onReload(
                [&]
                {
                    recordThreadId(actionThreadId);
                    isDone.store(true, std::memory_order_release);
                });

        ASSERT_EQ(::kill(::getpid(), SIGHUP), 0) << "kill 失败，这条用例就没有把信号送出去";

        ASSERT_TRUE(waitForCondition([&isDone] { return isDone.load(std::memory_order_acquire); })) << "SIGHUP 送达后重载动作没执行";
        EXPECT_EQ(actionThreadId, loopThreadId) << "实信号触发的重载没有落在事件循环线程上";
        EXPECT_EQ(reload.reloadCount(), 1U);

        runner.join();
    }

#endif

} // namespace AsynGyanis::Core
