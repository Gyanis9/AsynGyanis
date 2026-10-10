// ServiceWatchdog 测试：看门狗窗口的读法与拒绝面、节拍由循环 own 这条性质，
// 以及「凑齐一轮才喂」的换算本身。
// 覆盖场景：
// - CompletesRoundRequiresEveryLoopToReachThisRound（纯换算：谁走完最后一拍才发，落后与越界的格子）
// - ArmWithoutWatchdogEnvironmentLeavesThePoolUntouched（POSIX：没配窗口就不挂任何协程）
// - ArmRefusesWhenWatchdogPidNamesAnotherProcess（POSIX：被监督的是别的 pid，本进程不该喂）
// - ArmRefusesMalformedOrTooSmallWindow（POSIX：读不出的窗口与折不出 1 毫秒的窗口分别拒）
// - ArmRefusesWhenNotificationChannelIsClosed（POSIX：有窗口但通路没开）
// - PingsAtHalfTheWindowOnEveryLoopRound（POSIX：两条循环各一拍，收端逐条收到 WATCHDOG=1）
// - PingStopsWhileAnyLoopIsOccupied（POSIX：一条循环被占住时不再喂，放开后恢复——这条是整件事的意义）
// - RepeatedArmKeepsASingleTickerPerLoop（先后两次 arm 不会把一轮挂成两条）
// - WindowsRefusesToArmTheWatchdogAsAPlatformFact（Windows：平台事实，且一条协程都不挂）
#include "Core/Process/ServiceWatchdog.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"
#include "Platform/System/ServiceNotification.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    using AsynGyanis::Core::ServiceWatchdog;
    using AsynGyanis::Platform::ServiceNotification;

    constexpr const char *kWindowVariable = "WATCHDOG_USEC";
    constexpr const char *kPidVariable    = "WATCHDOG_PID";
    constexpr const char *kNotifyVariable = "NOTIFY_SOCKET";

    /// 用例用的窗口：400 毫秒，折出的节拍是 200 毫秒——判据要的是「几条循环凑齐一轮」而不是秒级等待
    constexpr const char *kWindowText = "400000";

    /// 收节拍的上限轮次：整轮最多三条，超出就是用例自己等错了对象
    constexpr std::size_t kPingSamples = 3;

    void setVariable(const char *name, const std::optional<std::string> &value)
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(::_putenv_s(name, value.has_value() ? value->c_str() : ""));
#else
        if (value.has_value())
        {
            static_cast<void>(::setenv(name, value->c_str(), 1));
        } else
        {
            static_cast<void>(::unsetenv(name));
        }
#endif
    }

#if !ASYN_PLATFORM_WIN32
    /// 抽象命名空间的名字按进程与序号唯一：并行跑的用例进程抢同一个名字会互相收到对方的数据报
    std::string makeAbstractName()
    {
        static std::atomic<std::uint64_t> sequence{0};
        return std::format("asyn-watchdog-{}-{}", static_cast<std::uint64_t>(::getpid()), sequence.fetch_add(1));
    }

    /**
     * @brief 建一个「假的服务管理器」：按抽象命名空间 bind 一个数据报套接字
     * @details 地址是**独立写的一份**，不复用被测代码里的 buildDestination：两边共用同一段代码时，
     *          一起写错同一处判据是抓不到的。
     * @param name 不带前导 '@' 的名字
     * @return int 已 bind 的描述符；失败返回 -1
     */
    int bindAbstractReceiver(const std::string &name)
    {
        const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0)
        {
            return -1;
        }

        sockaddr_un address{};
        address.sun_family  = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, name.data(), name.size());

        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), length) != 0)
        {
            static_cast<void>(::close(descriptor));
            return -1;
        }
        return descriptor;
    }

    /**
     * @brief 收一条数据报，带两秒上限：发方没送时用例要能红，而不是把作业挂在一次永久阻塞上
     * @param descriptor 收端描述符
     * @return std::string 收到的字节；超时或出错回空串
     */
    std::string receiveOneDatagram(const int descriptor)
    {
        timeval timeout{2, 0};
        static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));

        std::array<char, 256> buffer{};
        const auto            received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (received <= 0)
        {
            return {};
        }
        return std::string(buffer.data(), static_cast<std::size_t>(received));
    }
#endif
} // namespace

namespace AsynGyanis::Core
{
    /**
     * @brief 进出都把三枚看门狗相关变量恢复原样
     * @details 本组用例会改它们；不复原的话后跑的用例读到的是上一个用例留下的窗口，
     *          表现为「arm() 突然成功」这种随执行顺序而变的形态。
     */
    class ServiceWatchdogTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_savedWindow = Platform::ProcessInfo::environmentVariable(kWindowVariable);
            m_savedPid    = Platform::ProcessInfo::environmentVariable(kPidVariable);
            m_savedNotify = Platform::ProcessInfo::environmentVariable(kNotifyVariable);
            setVariable(kWindowVariable, std::nullopt);
            setVariable(kPidVariable, std::nullopt);
            setVariable(kNotifyVariable, std::nullopt);
        }

        void TearDown() override
        {
            setVariable(kWindowVariable, m_savedWindow);
            setVariable(kPidVariable, m_savedPid);
            setVariable(kNotifyVariable, m_savedNotify);
        }

        std::optional<std::string> m_savedWindow{};
        std::optional<std::string> m_savedPid{};
        std::optional<std::string> m_savedNotify{};
    };

    /**
     * @brief 纯换算：谁把最后一拍补上谁发，落后的那条循环补上时也算一轮齐了
     * @details 这条通道全部的判据就是「什么时候该发、什么时候不该发」，而按真实时钟去等
     *          「某条循环没醒」那一格在测试里只能靠睡，故把它单列成不读状态的函数钉住。
     */
    TEST_F(ServiceWatchdogTest, CompletesRoundRequiresEveryLoopToReachThisRound)
    {
        // 一条循环时它自己就是整轮
        EXPECT_TRUE(ServiceWatchdog::completesRound({5}, 0));

        // 两条都走到第 3 拍：无论谁报到 3，这一轮都齐了
        EXPECT_TRUE(ServiceWatchdog::completesRound({3, 3}, 0));
        EXPECT_TRUE(ServiceWatchdog::completesRound({3, 3}, 1));

        // 下标 0 报到第 3 拍而 1 还在第 2 拍：本轮缺一条，不喂
        EXPECT_FALSE(ServiceWatchdog::completesRound({3, 2}, 0));
        // 落后那条随后补到第 2 拍时，两条都已至少走完 2 拍——按「至少走到这一拍」的口径该喂
        EXPECT_TRUE(ServiceWatchdog::completesRound({3, 2}, 1));

        // 差得再多也一样的两条出口
        EXPECT_FALSE(ServiceWatchdog::completesRound({6, 1}, 0));
        EXPECT_TRUE(ServiceWatchdog::completesRound({6, 1}, 1));

        // 调用者总是先把自那一格加过一再进来，所以本格恒 ≥ 1；表是空的或下标越界都不喂——
        // 在认不出自己有几条循环的状态下把狗喂了，报出去的是假证
        EXPECT_FALSE(ServiceWatchdog::completesRound({}, 0));
        EXPECT_FALSE(ServiceWatchdog::completesRound({3}, 5));
    }

    /**
     * @brief 没配窗口时一条协程都不挂
     * @details 挂一条永远不会发东西的常驻协程比不挂更容易让人以为这条通道是通的，
     *          所以 lastError() 要有原因而 armedLoopCount() 必须是 0。
     */
    TEST_F(ServiceWatchdogTest, ArmWithoutWatchdogEnvironmentLeavesThePoolUntouched)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "Windows 侧的拒绝原因由 WindowsRefusesToArmTheWatchdogAsAPlatformFact 钉，这一条判的是 POSIX 分支";
#else
        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        ServiceNotification notification;
        ServiceWatchdog     watchdog(pool, notification);
        EXPECT_FALSE(watchdog.arm()) << "没有 WATCHDOG_USEC 却挂上了节拍";
        EXPECT_FALSE(watchdog.isArmed());
        EXPECT_EQ(watchdog.armedLoopCount(), 0U) << "被拒时不该在线程池上留下任何常驻协程";
        EXPECT_EQ(watchdog.pingCount(), 0U);
        EXPECT_NE(watchdog.lastError().find("WATCHDOG_USEC"), std::string::npos) << "原因里要点名是哪一枚变量没配：" << watchdog.lastError();

        pool.stop();
#endif
    }

    /**
     * @brief 窗口是给别的进程的就不喂
     * @details 多 worker 形态下子进程继承父进程的环境，按父进程那枚窗口各喂各的，
     *          被监督的那个 pid 反而一直没声——WATCHDOG_PID 就是用来认出这一格的。
     */
    TEST_F(ServiceWatchdogTest, ArmRefusesWhenWatchdogPidNamesAnotherProcess)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这条判据读的是 POSIX 分支的环境变量";
#else
        setVariable(kWindowVariable, kWindowText);
        setVariable(kPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid()) + 1U));

        ThreadPool pool{1};
        pool.start();
        ServiceNotification notification;
        ServiceWatchdog     watchdog(pool, notification);
        EXPECT_FALSE(watchdog.arm());
        EXPECT_EQ(watchdog.armedLoopCount(), 0U);
        EXPECT_NE(watchdog.lastError().find("不由本进程喂"), std::string::npos) << "原因要写清这条看门狗归谁：" << watchdog.lastError();

        pool.stop();
#endif
    }

    /**
     * @brief 三种「读不出可用窗口」的形状各自给出自己的原因
     * @details 混成一句会让部署方以为改哪一半都一样：值是垃圾、窗口是 0、窗口小到折不出 1 毫秒，
     *          处置完全不同（分别是改单元、关掉看门狗、把 WatchdogSec 调到秒级以上）。
     */
    TEST_F(ServiceWatchdogTest, ArmRefusesMalformedOrTooSmallWindow)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这条判据读的是 POSIX 分支的环境变量";
#else
        struct Case
        {
            const char *windowText;
            const char *expectedFragment;
        };
        const std::array<Case, 4> cases{Case{"abc", "读不出"}, Case{" 400000", "读不出"}, Case{"0", "写的是 0"}, Case{"1500", "不足 1 毫秒"}};

        for (const auto &testCase: cases)
        {
            setVariable(kWindowVariable, testCase.windowText);

            ThreadPool pool{1};
            pool.start();
            ServiceNotification notification;
            ServiceWatchdog     watchdog(pool, notification);
            EXPECT_FALSE(watchdog.arm()) << "窗口「" << testCase.windowText << "」不该被当成可用的节拍";
            EXPECT_EQ(watchdog.armedLoopCount(), 0U);
            EXPECT_NE(watchdog.lastError().find(testCase.expectedFragment), std::string::npos)
                    << "窗口「" << testCase.windowText << "」的原因里要点名这一格：" << watchdog.lastError();
            pool.stop();
        }
#endif
    }

    /**
     * @brief 有窗口但通知通路没开：拒绝并说明先 open()
     * @details 这两半在 systemd 那边是同一件事，分开出现就是部署方自己拼的启动环境；
     *          不报出来的话，监督者等的是一个永远不会有的回执。
     */
    TEST_F(ServiceWatchdogTest, ArmRefusesWhenNotificationChannelIsClosed)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这条判据读的是 POSIX 分支的环境变量";
#else
        setVariable(kWindowVariable, kWindowText);
        setVariable(kPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setVariable(kNotifyVariable, std::string("@") + makeAbstractName());

        ThreadPool pool{1};
        pool.start();
        ServiceNotification notification;
        ServiceWatchdog     watchdog(pool, notification);
        EXPECT_FALSE(watchdog.arm()) << "通路没开却挂上了节拍";
        EXPECT_NE(watchdog.lastError().find("通知通路还没打开"), std::string::npos) << watchdog.lastError();

        pool.stop();
#endif
    }

    /**
     * @brief 端到端：两条循环各占一拍，收端逐条收到 WATCHDOG=1
     * @details 钉的是「节拍真的由循环驱动」与「发出去的是那一串字节」两件事；
     *          间隔是否恰好是窗口的一半由 pingInterval() 那条读数核对，不靠掐秒表。
     */
    TEST_F(ServiceWatchdogTest, PingsAtHalfTheWindowOnEveryLoopRound)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这条要真发数据报，Windows 侧没有这条通路";
#else
        const std::string socketName = makeAbstractName();
        // 收端先挂上：数据报没人收时内核会把后面的发送算成失败，那种红与这条通道本身无关
        const int listener = bindAbstractReceiver(socketName);
        ASSERT_GE(listener, 0) << "假的服务管理器 bind 不上，这条用例没法判";

        setVariable(kWindowVariable, kWindowText);
        setVariable(kPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setVariable(kNotifyVariable, std::string("@") + socketName);

        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        ServiceWatchdog watchdog(pool, notification);
        ASSERT_TRUE(watchdog.arm()) << watchdog.lastError();
        EXPECT_TRUE(watchdog.isArmed());
        EXPECT_EQ(watchdog.armedLoopCount(), 2U) << "两条循环各占一拍";
        EXPECT_EQ(watchdog.pingInterval(), std::chrono::milliseconds(200)) << "节拍该是窗口折半";

        for (std::size_t index = 0; index < kPingSamples; ++index)
        {
            EXPECT_EQ(receiveOneDatagram(listener), "WATCHDOG=1") << "第 " << index << " 条节拍发的不是那串字节";
        }
        EXPECT_GE(watchdog.pingCount(), kPingSamples);

        static_cast<void>(::close(listener));
        // 先停循环再让 watchdog 出作用域：拍协程的帧只能在确定没人再恢复它们的时候销毁
        pool.stop();
#endif
    }

    /**
     * @brief 一条循环被占住时不再喂，放开后恢复——这条通道的全部意义在这一格
     * @details 若喂狗由一条独立线程做，这一条照样绿不了：那条线程睡醒就发，看不见循环有没有在转。
     *          「没有新节拍」是判「缺席」，等待窗口取得比节拍宽得多，慢机器只会让它更容易成立。
     */
    TEST_F(ServiceWatchdogTest, PingStopsWhileAnyLoopIsOccupied)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这条要真发数据报，Windows 侧没有这条通路";
#else
        const std::string socketName = makeAbstractName();
        const int         listener   = bindAbstractReceiver(socketName);
        ASSERT_GE(listener, 0) << "假的服务管理器 bind 不上，这条用例没法判";

        setVariable(kWindowVariable, kWindowText);
        setVariable(kPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setVariable(kNotifyVariable, std::string("@") + socketName);

        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();
        ServiceWatchdog watchdog(pool, notification);
        ASSERT_TRUE(watchdog.arm()) << watchdog.lastError();

        // 先等到第一条节拍：它证明这条通道此刻是通的，后面的「不发」才有内容
        ASSERT_EQ(receiveOneDatagram(listener), "WATCHDOG=1") << "没等到第一条节拍，无法判它后来停没停";

        // 占住第二条循环，并等到「确实占上了」这个事实再观察（不是睡一拍赌它已经进去）
        std::atomic<bool> isOccupied{false};
        std::atomic<bool> isReleased{false};
        pool.scheduler(1).postRemote(
                [&isOccupied, &isReleased]
                {
                    isOccupied.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds{1200});
                    isReleased.store(true, std::memory_order_release);
                });
        ASSERT_TRUE(TestSupport::waitForCondition([&isOccupied] { return isOccupied.load(std::memory_order_acquire); })) << "占用任务没进到第二条循环";

        const std::uint64_t frozenCount = watchdog.pingCount();
        std::this_thread::sleep_for(std::chrono::milliseconds{900});
        EXPECT_EQ(watchdog.pingCount(), frozenCount) << "有一条循环被占住期间还在喂狗，这条读数就证明不了循环在转";

        ASSERT_TRUE(TestSupport::waitForCondition([&isReleased] { return isReleased.load(std::memory_order_acquire); })) << "占用任务没跑完";
        ASSERT_EQ(receiveOneDatagram(listener), "WATCHDOG=1") << "循环放开之后节拍没有恢复：一轮凑齐就该重新发";

        static_cast<void>(::close(listener));
        pool.stop();
#endif
    }

    /**
     * @brief 先后两次 arm 不会把同一轮挂出两条节拍
     * @details 两条节拍会把每轮的 WATCHDOG=1 喂成两条，而「这条通道发过几次」正是运维要看的数。
     */
    TEST_F(ServiceWatchdogTest, RepeatedArmKeepsASingleTickerPerLoop)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "这一条要有可用的窗口，Windows 侧恒被拒";
#else
        const std::string socketName = makeAbstractName();
        // 收端照样要挂着：这一条不判发出去的内容，但没人收时每条节拍都会换回一条 ECONNREFUSED 的 WARN
        const int listener = bindAbstractReceiver(socketName);
        ASSERT_GE(listener, 0) << "假的服务管理器 bind 不上，这条用例没法判";

        setVariable(kWindowVariable, kWindowText);
        setVariable(kPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setVariable(kNotifyVariable, std::string("@") + socketName);

        ThreadPool pool{2};
        pool.start();
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        ServiceWatchdog watchdog(pool, notification);
        ASSERT_TRUE(watchdog.arm()) << watchdog.lastError();
        ASSERT_TRUE(watchdog.arm()) << "第二次 arm 该如实报「已挂上」而不是失败";
        EXPECT_EQ(watchdog.armedLoopCount(), 2U) << "重复挂会把一轮变成两条节拍协程";

        static_cast<void>(::close(listener));
        pool.stop();
#endif
    }

    /**
     * @brief Windows 上没有这条通路，且一条协程都不该挂
     * @details 平台事实而不是待办：那一侧没有 systemd，也就没有按超时重启本进程的监督者。
     */
    TEST_F(ServiceWatchdogTest, WindowsRefusesToArmTheWatchdogAsAPlatformFact)
    {
#if ASYN_PLATFORM_WIN32
        setVariable(kWindowVariable, kWindowText);
        setVariable(kNotifyVariable, "0");

        ThreadPool pool{1};
        pool.start();
        ServiceNotification notification;
        ServiceWatchdog     watchdog(pool, notification);
        EXPECT_FALSE(watchdog.arm()) << "Windows 侧不该挂上看门狗节拍";
        EXPECT_EQ(watchdog.armedLoopCount(), 0U);
        EXPECT_EQ(watchdog.pingCount(), 0U);
        EXPECT_NE(watchdog.lastError().find("当前平台"), std::string::npos) << watchdog.lastError();

        pool.stop();
#else
        GTEST_SKIP() << "这条钉的是 Windows 分支";
#endif
    }
} // namespace AsynGyanis::Core
