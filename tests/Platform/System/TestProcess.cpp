// Process 单元测试：起进程、取退出码、观察存活、请求退出与强杀
#include "Platform/System/Process.h"

#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <sys/wait.h>
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

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 由测试自己把子进程回收掉，模拟「别处已经收走了这个孩子」（SIGCHLD 处理函数的作为）
         * @param processId 目标进程号
         * @return true 在时限内回收成功
         */
        bool reapExternally(const long processId)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{kWaitTimeoutMilliseconds};
            while (std::chrono::steady_clock::now() < deadline)
            {
                int         waitStatus = 0;
                const pid_t reaped     = ::waitpid(static_cast<pid_t>(processId), &waitStatus, WNOHANG);
                if (reaped > 0)
                {
                    return true;
                }
                if (reaped < 0)
                {
                    // 已经不可回收（ECHILD 等）：再等也不会有变化，如实报失败让用例亮出前提没成立
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return false;
        }
#endif
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

#if ASYN_PLATFORM_WIN32
    namespace
    {
        /// 标记「这一份子进程是被父侧以无控制台方式启出来的探针」：内层用例据此区分角色
        constexpr const char *kDetachedProbeEnvironmentVariable = "ASYN_DETACHED_PROBE";

        /// 内层探针跑的用例名，父侧按它筛出这一条（拼进宽字符命令行，故直接给宽字面量）
        constexpr const wchar_t *kDetachedProbeFilter = L"Process.SpawnsChildWithoutConsoleOnDetachedHost";

        /// 自身路径缓冲的长度：给足 4 倍 MAX_PATH，长路径前缀也放得下
        constexpr DWORD kExecutablePathBufferLength = 4 * MAX_PATH;
    } // namespace

    /**
     * @brief 钉住（Windows）：宿主没有控制台时也要起得来子进程
     * @details 服务、GUI 子系统与被 DETACHED_PROCESS 派出来的宿主都没有控制台，此时三个标准句柄
     *          全是 NULL。把 NULL 塞进 PROC_THREAD_ATTRIBUTE_HANDLE_LIST，CreateProcessW 当场报
     *          ERROR_INVALID_PARAMETER(87)，于是「派生 worker」这条路径在这种宿主上必然失败。
     *          本用例不假装自己没有控制台：它把同一枚二进制以 DETACHED_PROCESS 再启一份，由那一份
     *          走被测路径，父侧只等有界时限内的退出码（不赌时序）。
     */
    TEST(Process, SpawnsChildWhenHostHasNoConsole)
    {
        wchar_t executablePathText[kExecutablePathBufferLength] = {};
        const DWORD pathLength = ::GetModuleFileNameW(nullptr, executablePathText, kExecutablePathBufferLength);
        ASSERT_GT(pathLength, 0U) << "取不到自身路径，错误码 " << ::GetLastError();
        ASSERT_LT(pathLength, kExecutablePathBufferLength) << "自身路径被截断，本用例失去前提";

        static_cast<void>(::SetEnvironmentVariableA(kDetachedProbeEnvironmentVariable, "1"));

        std::wstring commandLine;
        commandLine += L'"';
        commandLine += executablePathText;
        commandLine += L"\" --gtest_filter=";
        commandLine += kDetachedProbeFilter;

        STARTUPINFOW    startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation{};
        const BOOL isCreated = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                                                nullptr, nullptr, &startupInfo, &processInformation);
        static_cast<void>(::SetEnvironmentVariableA(kDetachedProbeEnvironmentVariable, nullptr));
        ASSERT_TRUE(isCreated != 0) << "探针子进程没起来，错误码 " << ::GetLastError();

        // 句柄在断言之后统一释放：中途一律用 EXPECT 而不是 ASSERT，免得提前返回把句柄漏在那里
        const DWORD waitResult = ::WaitForSingleObject(processInformation.hProcess, 60000);
        DWORD      childExitCode = 0;
        static_cast<void>(::GetExitCodeProcess(processInformation.hProcess, &childExitCode));
        ::CloseHandle(processInformation.hProcess);
        ::CloseHandle(processInformation.hThread);

        EXPECT_EQ(waitResult, WAIT_OBJECT_0) << "无控制台的探针子进程没在时限内退出";
        EXPECT_EQ(static_cast<int>(childExitCode), 0)
                << "探针子进程非零退出，说明无控制台宿主里 Process::spawn 仍然失败（详见该用例输出）";
    }

    /**
     * @brief 无控制台那一侧的实际断言，只由上一条用例以 DETACHED_PROCESS 启起来执行
     * @details 角色靠环境变量区分，且前提写成硬断言：这一份若其实有控制台就当场红，不允许
     *          「悄悄跳过也算通过」把父侧的判据变成假证据。单独跑本用例时（全量清单会选到它）
     *          按 SKIP 处理，因为这条路径的成立条件由父侧负责构造。
     */
    TEST(Process, SpawnsChildWithoutConsoleOnDetachedHost)
    {
        const auto probeMark = ProcessInfo::environmentVariable(kDetachedProbeEnvironmentVariable);
        if (!probeMark.has_value())
        {
            GTEST_SKIP() << "本用例只在由上一条用例以 DETACHED_PROCESS 启起来时才有意义";
        }

        EXPECT_EQ(::GetConsoleWindow(), nullptr) << "本用例要在没有控制台的宿主里跑，否则测不到那条路径";

        const ExitCommand     command = makeExitCommand(3);
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid()) << "无控制台宿主里 spawn 失败，错误码 " << PlatformError::lastErrorCode()
                                      << "（87 即 ERROR_INVALID_PARAMETER，指向句柄清单里的空句柄）";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "子进程没在时限内退出";
        EXPECT_EQ(*exitCode, 3);
    }
#endif

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
     * @brief 子进程被别处回收之后，「已经结束了」这件事仍然要能问出来
     * @details 类的注释写着「回收过一次就记住，否则第二次问会拿到查不到」，而实现把有效性判定排在
     *          缓存之前、ECHILD 那条分支又回 nullopt：一旦被别的回收者（SIGCHLD 处理函数）收走，
     *          这句话永远兑不了现，按「取到值才算结束」轮询的编排者会一直等一个不会再来的值。
     *          -1 是本层给「已被别处回收」选的记号：POSIX 的退出码只有 0-255，它不会与真实退出码撞车。
     */
    TEST(Process, ExitCodeStaysReadableWhenAnotherReaperTookTheChild)
    {
        const ExitCommand     command = makeExitCommand(9);
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        ASSERT_TRUE(reapExternally(handle.processId())) << "测试自己没能回收这个子进程，本用例失去前提";

        const std::optional<int> exitCode = Process::pollExitCode(handle);
        ASSERT_TRUE(exitCode.has_value()) << "已被别处回收也要交出「已经结束」这个事实，而不是查不到";
        EXPECT_EQ(*exitCode, -1) << "已被别处回收时给不出真实退出码，只能记这个哨兵值";
        EXPECT_FALSE(Process::isRunning(handle)) << "已被回收的子进程不该报成还在运行";

        // 第二问必须有同一个答案：缓存排在有效性判定之前才做得到（进程号在上一问里已被作废）
        const std::optional<int> secondExitCode = Process::pollExitCode(handle);
        ASSERT_TRUE(secondExitCode.has_value()) << "第二次问把记住的结果弄丢了，轮询方会永远等下去";
        EXPECT_EQ(*secondExitCode, -1);
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
