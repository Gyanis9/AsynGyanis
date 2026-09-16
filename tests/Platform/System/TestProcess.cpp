/**
 * @file TestProcess.cpp
 * @brief Process 单元测试：起进程、取退出码、观察存活、请求退出与强杀
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/System/Process.h"

#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 有界重试统一使用的最长等待毫秒数，避免任何一步无限阻塞
        constexpr int kWaitTimeoutMilliseconds = 5000;

        /// 子进程要用的「立即以指定码退出」命令：两端都用系统自带的 shell，不依赖被测项目
        struct ExitCommand
        {
            std::string              executablePath; ///< 可执行文件
            std::vector<std::string> arguments;      ///< 参数表（不含 argv[0]：本层会自己补上）
        };

        /**
         * @brief 拼一条「立刻以 exitCode 退出」的子进程命令
         * @param exitCode 期望的退出码
         * @return ExitCommand 可执行文件与参数
         */
        ExitCommand makeExitCommand(const int exitCode)
        {
            const std::string codeText = std::to_string(exitCode);
#if ASYN_PLATFORM_WIN32
            return ExitCommand{"cmd.exe", std::vector<std::string>{"/c", "exit " + codeText}};
#else
            return ExitCommand{"/bin/sh", std::vector<std::string>{"-c", "exit " + codeText}};
#endif
        }

        /**
         * @brief 拼一条「一直运行到被杀」的子进程命令
         * @details 用 sleep 而不是死循环：被强杀前不该占用 CPU，否则这条用例会干扰同机并行的其它用例
         * @return ExitCommand 可执行文件与参数
         */
        ExitCommand makeSleepCommand()
        {
#if ASYN_PLATFORM_WIN32
            // Windows 上 sleep 由 ping 的间隔凑：无外部依赖且能睡够
            return ExitCommand{"cmd.exe", std::vector<std::string>{"/c", "ping -n 6 127.0.0.1 > nul"}};
#else
            return ExitCommand{"/bin/sh", std::vector<std::string>{"-c", "sleep 5"}};
#endif
        }

        /**
         * @brief 在时限内轮询等待子进程退出，并给出退出码
         * @param handle 目标句柄
         * @param timeoutMilliseconds 等待上限
         * @return std::optional<int> 已退出时的退出码，超时未退出时为空
         */
        std::optional<int> waitForExit(const Process::Handle &handle, const int timeoutMilliseconds)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (const std::optional<int> exitCode = Process::pollExitCode(handle); exitCode.has_value())
                {
                    return exitCode;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return std::nullopt;
        }
    } // namespace

    /**
     * @brief 起一个子进程并取回它自己的退出码
     */
    TEST(Process, SpawnsChildAndReportsItsExitCode)
    {
        const ExitCommand command = makeExitCommand(7);

        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid()) << "子进程没有起来，errno/错误码 " << PlatformError::lastErrorCode();
        EXPECT_GT(handle.processId(), 0L) << "有效句柄应当能给出进程号";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "子进程没有在时限内退出";
        EXPECT_EQ(*exitCode, 7) << "取回的退出码不是子进程自己的";
        EXPECT_FALSE(Process::isRunning(handle)) << "已退出的子进程不该被报成还在运行";
    }

    /**
     * @brief 退出码会被记住：反复问都得到同一个值，不会被第二次问成「查不到」
     */
    TEST(Process, ExitCodeSurvivesRepeatedPolling)
    {
        const ExitCommand command = makeExitCommand(3);
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        const std::optional<int> firstExitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(firstExitCode.has_value());
        EXPECT_EQ(*firstExitCode, 3);

        // 第二次问：回收已经发生过，仍然要给同一个退出码
        const std::optional<int> secondExitCode = Process::pollExitCode(handle);
        ASSERT_TRUE(secondExitCode.has_value()) << "退出码在第二次取用时丢了";
        EXPECT_EQ(*secondExitCode, *firstExitCode);
    }

    /**
     * @brief 空的可执行文件路径当场判错：返回无效句柄并置参数非法，不去猜也不去试
     */
    TEST(Process, RejectsEmptyExecutablePath)
    {
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{});
        EXPECT_FALSE(handle.isValid()) << "空路径不该起出进程";
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);
    }

    /**
     * @brief 已经退出并被回收的子进程不能再被终止：回收那一刻 pid 就交还系统了
     * @details 拿已回收的 pid 去 kill 可能命中一个复用了同一 pid 的无关进程——
     *          Linux 上 SIGKILL 必中，而且是「杀掉别人」这种最恶劣的串扰
     */
    TEST(Process, TerminationOnReapedChildIsRejected)
    {
        const ExitCommand command = makeExitCommand(3);
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        // 先回收：退出码被记下，pid 从此不再属于本进程
        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "子进程没有在时限内退出";

        EXPECT_FALSE(Process::requestTermination(handle)) << "对已回收的 pid 发出了 SIGTERM";
        EXPECT_FALSE(Process::forceTermination(handle)) << "对已回收的 pid 发出了 SIGKILL";
    }

    /**
     * @brief 无效句柄上的观察与终止一律安全失败，不崩不猜
     */
    TEST(Process, InvalidHandleIsSafeToObserveAndTerminate)
    {
        Process::Handle invalidHandle;

        EXPECT_FALSE(invalidHandle.isValid());
        EXPECT_EQ(invalidHandle.processId(), 0L);
        EXPECT_FALSE(Process::isRunning(invalidHandle));
        EXPECT_FALSE(Process::pollExitCode(invalidHandle).has_value());
        EXPECT_FALSE(Process::requestTermination(invalidHandle));
        EXPECT_FALSE(Process::forceTermination(invalidHandle));
        invalidHandle.close(); // 幂等：析构还会再调一次
        EXPECT_FALSE(invalidHandle.isValid());
    }

    /**
     * @brief 句柄移动之后所有权易主：源侧变无效，目标侧照常观察
     */
    TEST(Process, HandleMoveTransfersOwnership)
    {
        const ExitCommand command = makeExitCommand(5);

        Process::Handle source = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(source.isValid());
        const long childProcessId = source.processId();

        Process::Handle target = std::move(source);
        EXPECT_FALSE(source.isValid()) << "移动之后源句柄仍自称有效";
        ASSERT_TRUE(target.isValid());
        EXPECT_EQ(target.processId(), childProcessId);

        const std::optional<int> exitCode = waitForExit(target, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "移动之后观察不到子进程退出";
        EXPECT_EQ(*exitCode, 5);
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 仍在运行的子进程被如实报成「在运行」，且拿不到退出码
     * @note 本条与下面两条只在 POSIX 上跑：本切片里多进程 worker 模型本身也只落 POSIX
     *       （Windows 没有 SO_REUSEPORT，`workers>1` 会被 supervisor 明确拒绝）
     */
    TEST(Process, ReportsRunningChildAsRunning)
    {
        const ExitCommand command = makeSleepCommand();
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        EXPECT_TRUE(Process::isRunning(handle)) << "还在睡的子进程被报成已退出";
        EXPECT_FALSE(Process::pollExitCode(handle).has_value()) << "还在运行却给出了退出码";

        // 收尾：用例自己起的进程自己收掉，不给后续用例留垃圾
        EXPECT_TRUE(Process::forceTermination(handle));
        EXPECT_TRUE(waitForExit(handle, kWaitTimeoutMilliseconds).has_value());
    }

    /**
     * @brief 请求体面退出（SIGTERM）能把进程停下来，且退出码按 128+信号号折算
     */
    TEST(Process, RequestTerminationStopsChild)
    {
        const ExitCommand command = makeSleepCommand();
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        ASSERT_TRUE(Process::requestTermination(handle)) << "SIGTERM 没有发出去";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "收到 SIGTERM 的子进程没有退出";
        EXPECT_EQ(*exitCode, 128 + SIGTERM) << "被信号终止的退出码应当按 128+信号号折算（shell 惯例）";
    }

    /**
     * @brief 强杀能停掉不理会请求的进程，且退出码与信号终止不同
     */
    TEST(Process, ForceTerminationStopsChild)
    {
        const ExitCommand command = makeSleepCommand();
        const Process::Handle handle = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        ASSERT_TRUE(Process::forceTermination(handle)) << "SIGKILL 没有发出去";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "被强杀的进程没有退出";
        EXPECT_EQ(*exitCode, 128 + SIGKILL);
        EXPECT_FALSE(Process::isRunning(handle));
    }
#endif
} // namespace AsynGyanis::Platform
