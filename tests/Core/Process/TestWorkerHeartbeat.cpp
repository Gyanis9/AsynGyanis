// WorkerHeartbeat 用例：POSIX 侧真造一条管道收拍（凑齐一轮写一个字节），拒绝面钉在 arm 的校验上；
// Windows 侧整条特性按平台事实拒绝（没有 systemd 看门狗），断言理由与「一条协程都不挂」。
//
// 跨循环「只有全部循环醒过才写一拍」这条判据的骨架由 RoundTicker 的用例钉住，这里钉的是
// 写端那一段：一条管道、一个池、字节真的到了对端。

#include "Core/Process/WorkerHeartbeat.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Core
{
    /**
     * @brief Windows：没有 systemd 看门狗这条通道，arm 直接拒绝且不挂任何协程
     */
    TEST(WorkerHeartbeat, WindowsRefusesToArmAsAPlatformFact)
    {
#if !ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "本用例判的是 Windows 分支";
#else
        ThreadPool pool{2};
        pool.start();
        WorkerHeartbeat heartbeat(pool, -1, std::chrono::milliseconds{100});
        EXPECT_FALSE(heartbeat.arm()) << "本平台该拒绝";
        EXPECT_FALSE(heartbeat.lastError().empty()) << "拒绝要给得出原因";
        EXPECT_EQ(heartbeat.armedLoopCount(), 0U);
        EXPECT_EQ(heartbeat.pingCount(), 0U);
        pool.stop();
#endif
    }

    /**
     * @brief POSIX：arm 之后心跳真的经管道到达对端，重复 arm 不会把一拍写两次
     */
    TEST(WorkerHeartbeat, WritesOnePingPerCompletedRound)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "本平台没有该通道";
#else
        int pipeDescriptors[2]{-1, -1};
        ASSERT_EQ(::pipe(pipeDescriptors), 0) << "管道都开不出来，用例失去前提";
        const int readFd  = pipeDescriptors[0];
        const int writeFd = pipeDescriptors[1];
        // 读端设非阻塞：有界轮询要有界——空管道上的阻塞读会让 deadline 永远轮不到（突变下这条
        // 用例会从「变红」变成「挂死」，那就失去了判据的意义）
        static_cast<void>(::fcntl(readFd, F_SETFL, O_NONBLOCK));

        ThreadPool pool{2};
        pool.start();
        ASSERT_TRUE(TestSupport::waitForCondition([&pool] { return pool.eventLoop(0).isRunning() && pool.eventLoop(1).isRunning(); }));

        WorkerHeartbeat heartbeat(pool, writeFd, std::chrono::milliseconds{10});
        ASSERT_TRUE(heartbeat.arm()) << heartbeat.lastError();
        EXPECT_TRUE(heartbeat.isArmed());
        EXPECT_EQ(heartbeat.armedLoopCount(), 2U);
        ASSERT_TRUE(heartbeat.arm()) << "重复 arm 是空操作且仍返回真";

        char       received      = 0;
        const auto deadline      = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        ssize_t    readByteCount = -1;
        while (std::chrono::steady_clock::now() < deadline)
        {
            readByteCount = ::read(readFd, &received, 1);
            if (readByteCount == 1)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(readByteCount, 1) << "两条循环都在跑，心跳该经管道到达";
        EXPECT_GE(heartbeat.pingCount(), 1U);

        pool.stop();
        static_cast<void>(::close(readFd));
        static_cast<void>(::close(writeFd));
#endif
    }

    /**
     * @brief POSIX：描述符非法或拍间隔非正时拒绝 arm，且不挂任何协程
     */
    TEST(WorkerHeartbeat, RejectsInvalidDescriptorAndInterval)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "本平台没有该通道";
#else
        ThreadPool pool{2};
        pool.start();

        WorkerHeartbeat badDescriptor(pool, -1, std::chrono::milliseconds{100});
        EXPECT_FALSE(badDescriptor.arm());
        EXPECT_NE(badDescriptor.lastError().find("描述符"), std::string::npos) << "拒绝原因没有点明是描述符：" << badDescriptor.lastError();
        EXPECT_EQ(badDescriptor.armedLoopCount(), 0U);

        int pipeDescriptors[2]{-1, -1};
        ASSERT_EQ(::pipe(pipeDescriptors), 0);
        WorkerHeartbeat badInterval(pool, pipeDescriptors[1], std::chrono::milliseconds{0});
        EXPECT_FALSE(badInterval.arm());
        EXPECT_NE(badInterval.lastError().find("间隔"), std::string::npos) << "拒绝原因没有点明是拍间隔：" << badInterval.lastError();
        EXPECT_NE(badInterval.lastError().find("0"), std::string::npos) << "拒绝原因没有带上实际取值：" << badInterval.lastError();
        EXPECT_EQ(badInterval.armedLoopCount(), 0U);

        pool.stop();
        static_cast<void>(::close(pipeDescriptors[0]));
        static_cast<void>(::close(pipeDescriptors[1]));
#endif
    }
} // namespace AsynGyanis::Core
