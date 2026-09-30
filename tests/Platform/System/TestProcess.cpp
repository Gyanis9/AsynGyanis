// Process 单元测试：起进程、取退出码、观察存活、请求退出与强杀
#include "Platform/System/Process.h"

#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"
#include "Platform/System/TextEncoding.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <cerrno>
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
        const ExitCommand     command = makeExitCommand(3);
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
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
        const ExitCommand     command = makeExitCommand(3);
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
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
        wchar_t     executablePathText[kExecutablePathBufferLength] = {};
        const DWORD pathLength                                      = ::GetModuleFileNameW(nullptr, executablePathText, kExecutablePathBufferLength);
        ASSERT_GT(pathLength, 0U) << "取不到自身路径，错误码 " << ::GetLastError();
        ASSERT_LT(pathLength, kExecutablePathBufferLength) << "自身路径被截断，本用例失去前提";

        static_cast<void>(::SetEnvironmentVariableA(kDetachedProbeEnvironmentVariable, "1"));

        std::wstring commandLine;
        commandLine += L'"';
        commandLine += executablePathText;
        commandLine += L"\" --gtest_filter=";
        commandLine += kDetachedProbeFilter;

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation{};
        const BOOL isCreated = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startupInfo, &processInformation);
        static_cast<void>(::SetEnvironmentVariableA(kDetachedProbeEnvironmentVariable, nullptr));
        ASSERT_TRUE(isCreated != 0) << "探针子进程没起来，错误码 " << ::GetLastError();

        // 句柄在断言之后统一释放：中途一律用 EXPECT 而不是 ASSERT，免得提前返回把句柄漏在那里
        const DWORD waitResult    = ::WaitForSingleObject(processInformation.hProcess, 60000);
        DWORD       childExitCode = 0;
        static_cast<void>(::GetExitCodeProcess(processInformation.hProcess, &childExitCode));
        ::CloseHandle(processInformation.hProcess);
        ::CloseHandle(processInformation.hThread);

        EXPECT_EQ(waitResult, WAIT_OBJECT_0) << "无控制台的探针子进程没在时限内退出";
        EXPECT_EQ(static_cast<int>(childExitCode), 0) << "探针子进程非零退出，说明无控制台宿主里 Process::spawn 仍然失败（详见该用例输出）";
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
        ASSERT_TRUE(handle.isValid()) << "无控制台宿主里 spawn 失败，错误码 " << PlatformError::lastErrorCode() << "（87 即 ERROR_INVALID_PARAMETER，指向句柄清单里的空句柄）";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "子进程没在时限内退出";
        EXPECT_EQ(*exitCode, 3);
    }

    namespace
    {
        /// 标记「这一份是被父侧以带控制台方式启出来的探针」：内层用例据此区分角色
        constexpr const char *kConsoleProbeEnvironmentVariable = "ASYN_CONSOLE_PROBE";

        /// 内层探针跑的三条用例，父侧按这个前缀一次选中。刻意不与外层用例名同前缀，否则子进程会再启一份自己
        constexpr const wchar_t *kConsoleProbeFilter = L"Process.GracefulStopInConsoleChild*";

        /**
         * @brief 拼一条「睡到被人叫醒」的子进程命令，给请求退出类用例当靶子
         * @details 睡足 60 秒而不是 makeSleepCommand() 的 6 秒：用例只在 2 秒的窗口里等它消失，
         *          睡得过短会让「信号没到、自己到点退了」冒充成功
         * @return ExitCommand 可执行文件与参数
         */
        ExitCommand makeLongSleepCommand()
        {
            return ExitCommand{"cmd.exe", std::vector<std::string>{"/c", "ping -n 60 127.0.0.1 > nul"}};
        }

        /// 请求体面退出后等子进程消失的窗口：远小于 makeLongSleepCommand() 的 60 秒，短到不可能是自己睡醒
        constexpr int kTerminationWaitMilliseconds = 2000;

        /// 等探针子进程跑完的时限：它自己只跑到秒级，这里给的是「它挂了/没人收」的兜底
        constexpr DWORD kConsoleProbeWaitMilliseconds = 60000;

        /// 内层探针「这条真的跑过」的标记文件名
        constexpr const wchar_t *kConsoleProbeMarkerStopsTarget = L"asyn-console-probe-stops-target.mark";
        constexpr const wchar_t *kConsoleProbeMarkerKeepsGroup  = L"asyn-console-probe-keeps-group.mark";
        constexpr const wchar_t *kConsoleProbeMarkerRefuses     = L"asyn-console-probe-refuses-without-group.mark";

        /**
         * @brief 拼出标记文件在临时目录里的完整路径
         * @param markerName 标记文件名
         * @return std::filesystem::path 可直接用于写与读
         */
        std::filesystem::path consoleProbeMarkerPath(const wchar_t *const markerName)
        {
            return std::filesystem::temp_directory_path() / markerName;
        }

        /**
         * @brief 由内层探针写下「我跑过」的标记
         * @param markerName 标记文件名
         */
        void writeConsoleProbeMarker(const wchar_t *const markerName)
        {
            // 用 ofstream 而不是 std::filesystem::write_file：后者是 C++23 设施，本机 MSVC 的 <filesystem>
            // 里还没有它（实测 C2039），而这份标记要的只是「文件确实存在」
            std::ofstream markerFile(consoleProbeMarkerPath(markerName));
            markerFile << "ran";
        }

        /**
         * @brief 父侧用：标记是否存在
         * @param markerName 标记文件名
         * @return true 内层探针写过这一条
         */
        bool consoleProbeMarkerExists(const wchar_t *const markerName)
        {
            std::error_code ignored;
            return std::filesystem::exists(consoleProbeMarkerPath(markerName), ignored);
        }

        /**
         * @brief 父侧用：清掉上一次留下的标记，免得把旧证据当成本轮的
         * @param markerName 标记文件名
         */
        void removeConsoleProbeMarker(const wchar_t *const markerName)
        {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(consoleProbeMarkerPath(markerName), ignored));
        }
    } // namespace

    /**
     * @brief 内层探针之一：不给独立进程组就不许发控制台事件
     * @details CTRL_BREAK 的投递单位是「进程组」，没有独立组时唯一的目标就是本进程所在的组——
     *          那一下会打断宿主自己的键盘输入与服务循环（探针没装控制台处理函数，会被系统直接终止）。
     *          本用例因此钉的是「不发」：返回 false，且靶子进程照旧在跑，随后由用例自己收掉。
     *          必须在带控制台的宿主里测：没有控制台时事件本来就发不出去，去掉实现里那道进程组判据也照样
     *          返回 false，用例就变成一条恒真的空判——所以它跟着另外两条一起进控制台探针。
     */
    TEST(Process, GracefulStopInConsoleChildRefusesWithoutGroup)
    {
        const auto probeMark = ProcessInfo::environmentVariable(kConsoleProbeEnvironmentVariable);
        if (!probeMark.has_value())
        {
            GTEST_SKIP() << "本用例只在由控制台探针的父侧启起来时才有意义";
        }
        ASSERT_NE(::GetConsoleWindow(), nullptr) << "探针本该带着控制台起来，没有就测不到这条路";
        writeConsoleProbeMarker(kConsoleProbeMarkerRefuses);

        const ExitCommand     command = makeLongSleepCommand();
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid()) << "子进程没起来，错误码 " << PlatformError::lastErrorCode();

        EXPECT_FALSE(Process::requestTermination(handle)) << "没给独立进程组却发了控制台事件，那会打断本进程所在的组";
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_TRUE(Process::isRunning(handle)) << "请求被拒的同时子进程也不该消失";

        static_cast<void>(Process::forceTermination(handle));
        static_cast<void>(waitForExit(handle, kWaitTimeoutMilliseconds));
    }

    /**
     * @brief 钉住（Windows）：控制台事件这条收尾通道真的叫得停子进程——前提由本用例自己造
     * @details 宿主没有控制台时（stdout 被管道接管的服务、非交互拉起的测试进程）CTRL_BREAK 无处投递，
     *          三条内层探针在有控制台的宿主里才测得到。本进程不 AllocConsole：那会把三个标准句柄换成控制台
     *          缓冲区，同一进程里其余用例的输出就漂了。做法沿用本文件的探针形状——把同一枚二进制以
     *          CREATE_NEW_CONSOLE 再启一份，由那一份跑三条内层探针，父侧既等退出码也数标记文件。
     *          父侧这条在任意宿主下都会跑，因此「跳过」不会把结论冒充成通过。
     */
    TEST(Process, RequestTerminationStopsChildViaConsoleHarness)
    {
        wchar_t     executablePathText[kExecutablePathBufferLength] = {};
        const DWORD pathLength                                      = ::GetModuleFileNameW(nullptr, executablePathText, kExecutablePathBufferLength);
        ASSERT_GT(pathLength, 0U) << "取不到自身路径，错误码 " << ::GetLastError();
        ASSERT_LT(pathLength, kExecutablePathBufferLength) << "自身路径被截断，本用例失去前提";

        static_cast<void>(::SetEnvironmentVariableA(kConsoleProbeEnvironmentVariable, "1"));

        // 先把标记清掉：父侧要靠它们确认「探针真的上场了」——过滤器一条也没选中时 gtest 同样回 0，
        // 只看退出码会把「裁判没上场」读成「裁判判了通过」
        removeConsoleProbeMarker(kConsoleProbeMarkerStopsTarget);
        removeConsoleProbeMarker(kConsoleProbeMarkerKeepsGroup);
        removeConsoleProbeMarker(kConsoleProbeMarkerRefuses);

        std::wstring commandLine;
        commandLine += L'"';
        commandLine += executablePathText;
        commandLine += L"\" --gtest_filter=";
        commandLine += kConsoleProbeFilter;

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        // 控制台窗口藏起来：这条路径一轮门禁要跑好几次，弹一个黑窗出来只是干扰
        startupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION processInformation{};
        const BOOL isCreated = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startupInfo, &processInformation);
        static_cast<void>(::SetEnvironmentVariableA(kConsoleProbeEnvironmentVariable, nullptr));
        ASSERT_TRUE(isCreated != 0) << "带控制台的探针子进程没起来，错误码 " << ::GetLastError();

        // 句柄在断言之前统一释放：中途一律用 EXPECT 而不是 ASSERT，免得提前返回把句柄漏在那里
        const DWORD waitResult    = ::WaitForSingleObject(processInformation.hProcess, kConsoleProbeWaitMilliseconds);
        DWORD       childExitCode = 0;
        static_cast<void>(::GetExitCodeProcess(processInformation.hProcess, &childExitCode));
        ::CloseHandle(processInformation.hProcess);
        ::CloseHandle(processInformation.hThread);

        EXPECT_EQ(waitResult, WAIT_OBJECT_0) << "带控制台的探针子进程没在时限内退出";
        EXPECT_EQ(static_cast<int>(childExitCode), 0) << "探针子进程非零退出：CTRL_BREAK 没能叫停子进程，或移动后进程组归属权丢了。"
                                                         "想看细节就在一个控制台窗口里跑：TestPlatform.exe --gtest_filter=Process.GracefulStopInConsoleChild*";
        // 退出码之外还要问「这两条到底跑没跑」：0 也可能是过滤器没选中任何东西
        EXPECT_TRUE(consoleProbeMarkerExists(kConsoleProbeMarkerRefuses)) << "探针没跑「无组不发」那条，本用例因此没有证据";
        EXPECT_TRUE(consoleProbeMarkerExists(kConsoleProbeMarkerStopsTarget)) << "探针没跑「叫得停」那条，本用例因此没有证据";
        EXPECT_TRUE(consoleProbeMarkerExists(kConsoleProbeMarkerKeepsGroup)) << "探针没跑「移动后仍叫得停」那条，本用例因此没有证据";
    }

    /**
     * @brief 内层探针之一：给了独立进程组，请求体面退出就真的能把子进程叫停
     * @details 这是 worker 在 Windows 上的收尾通道：编排者派生时给每个 worker 一份独立进程组，停机时
     *          逐个发 CTRL_BREAK，装了控制台处理函数的 worker 就能自己走完 stop()/drain()，而不是被强杀。
     *          靶子进程要睡 60 秒，而这里只等 2 秒——它消失了只能是事件到了，不可能是自己睡醒。
     *          角色靠环境变量区分：单独跑这一条时（全量清单会选到它）按 SKIP 处理，因为前提由父侧负责构造
     */
    TEST(Process, GracefulStopInConsoleChildStopsTarget)
    {
        const auto probeMark = ProcessInfo::environmentVariable(kConsoleProbeEnvironmentVariable);
        if (!probeMark.has_value())
        {
            GTEST_SKIP() << "本用例只在由上一条用例以 CREATE_NEW_CONSOLE 启起来时才有意义";
        }
        ASSERT_NE(::GetConsoleWindow(), nullptr) << "探针本该带着控制台起来，没有就测不到这条路";
        writeConsoleProbeMarker(kConsoleProbeMarkerStopsTarget);

        const ExitCommand     command = makeLongSleepCommand();
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments, true});
        ASSERT_TRUE(handle.isValid()) << "子进程没起来，错误码 " << PlatformError::lastErrorCode();

        ASSERT_TRUE(Process::requestTermination(handle)) << "事件没发出去，错误码 " << PlatformError::lastErrorCode();

        const std::optional<int> exitCode = waitForExit(handle, kTerminationWaitMilliseconds);
        if (!exitCode.has_value())
        {
            static_cast<void>(Process::forceTermination(handle));
            FAIL() << "子进程在 " << kTerminationWaitMilliseconds << " 毫秒内没退出：CTRL_BREAK 没落到它名下的进程组（它本来要睡 60 秒）";
        }
        EXPECT_FALSE(Process::isRunning(handle));
    }

    /**
     * @brief 内层探针之二：进程组归属权跟着句柄一起移动
     * @details 「有没有独立进程组」只有派生那一次知道，事后无从向平台追问，所以它是句柄状态的一部分：
     *          移动时丢了它，新句柄就再也发不出 CTRL_BREAK，而表现是「worker 每次都被强杀」——
     *          看着像超时给短了，没人会往移动构造上查。源句柄同时作废，两侧各钉一次
     */
    TEST(Process, GracefulStopInConsoleChildKeepsGroupOwnershipAfterMove)
    {
        const auto probeMark = ProcessInfo::environmentVariable(kConsoleProbeEnvironmentVariable);
        if (!probeMark.has_value())
        {
            GTEST_SKIP() << "本用例只在由控制台探针的父侧启起来时才有意义";
        }
        ASSERT_NE(::GetConsoleWindow(), nullptr) << "探针本该带着控制台起来，没有就测不到这条路";
        writeConsoleProbeMarker(kConsoleProbeMarkerKeepsGroup);

        const ExitCommand command = makeLongSleepCommand();
        Process::Handle   handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments, true});
        ASSERT_TRUE(handle.isValid()) << "子进程没起来，错误码 " << PlatformError::lastErrorCode();

        const Process::Handle movedHandle = std::move(handle);
        EXPECT_FALSE(Process::requestTermination(handle)) << "移空的源句柄仍能发事件，说明移动后留下了两个主人";
        ASSERT_TRUE(Process::requestTermination(movedHandle)) << "移动后进程组归属权没了，新句柄发不出 CTRL_BREAK，错误码 " << PlatformError::lastErrorCode();

        const std::optional<int> exitCode = waitForExit(movedHandle, kTerminationWaitMilliseconds);
        if (!exitCode.has_value())
        {
            static_cast<void>(Process::forceTermination(movedHandle));
            FAIL() << "移动后的句柄没能叫停子进程";
        }
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
        const ExitCommand     command = makeSleepCommand();
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
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
        const ExitCommand     command = makeSleepCommand();
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
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
        const ExitCommand     command = makeSleepCommand();
        const Process::Handle handle  = Process::spawn(Process::LaunchOptions{command.executablePath, command.arguments});
        ASSERT_TRUE(handle.isValid());

        ASSERT_TRUE(Process::forceTermination(handle)) << "SIGKILL 没有发出去";

        const std::optional<int> exitCode = waitForExit(handle, kWaitTimeoutMilliseconds);
        ASSERT_TRUE(exitCode.has_value()) << "被强杀的进程没有退出";
        EXPECT_EQ(*exitCode, 128 + SIGKILL);
        EXPECT_FALSE(Process::isRunning(handle));
    }
#endif

    // ---------------------------------------------------------------- 随父终止（killWithParent）
    //
    // 这条保护要三个进程才判得了：父侧（用例）→ 中间层（同一枚二进制的探针角色）→ 孙进程。
    // 中间层必须「不退干净」地结束（ExitProcess / _exit），因为正常析构会由 Handle 关掉作业句柄，
    // 那就测的是主动收口而不是被硬杀。孙进程的存在与消失由父侧独立核对，判据不靠句柄。

    namespace
    {
        /// 探针的角色标记：父侧据此把同一枚二进制启成中间层；中间层没有它就按普通用例走（不递归）
        constexpr const char *kKillProbeRoleVariable = "ASYN_KILL_PROBE";

        /// 中间层把孙进程号与保护状态写进这个文件，父侧按它判
        constexpr const char *kKillProbeReportVariable = "ASYN_KILL_PROBE_FILE";

        /// 探针报告的落地文件名（临时目录里）
        constexpr const char *kKillProbeReportName = "asyn-kill-probe.report";

        /// 父侧等孙进程消失的窗口：保护生效时是毫秒级，给到 4 秒已经把「要睡满 60 秒」的靶子区分开了
        constexpr int kKillProbeWaitMilliseconds = 4000;

        /// 对照组里「保护没开时孙进程该活着」的观察窗口
        constexpr int kKillProbeSurvivalWindowMilliseconds = 1000;

        /**
         * @brief 探针用的长睡靶子：两端都要睡到远超观察窗口
         * @details Windows 走 ping（无外部依赖），POSIX 走 sleep；刻意不用 makeSleepCommand()——
         *          它 POSIX 侧只睡 5 秒，与这里的 4 秒窗口挨得太近，会留下「自己睡醒了」的假绿空间
         * @return ExitCommand 可执行文件与参数
         */
        ExitCommand makeGuardProbeTargetCommand()
        {
#if ASYN_PLATFORM_WIN32
            return ExitCommand{"cmd.exe", std::vector<std::string>{"/c", "ping -n 60 127.0.0.1 > nul"}};
#else
            return ExitCommand{"/bin/sh", std::vector<std::string>{"-c", "sleep 60"}};
#endif
        }

        /// 中间层的「被硬杀」：跳过析构与 atexit，句柄交还给系统，这正是 master 被 Taskkill /F 带走的形状
        void exitTheProbeProcessAbruptly()
        {
#if ASYN_PLATFORM_WIN32
            static_cast<void>(::ExitProcess(0));
#else
            ::_exit(0);
#endif
        }

        /**
         * @brief 给探针子进程设/清环境变量（两端的调用形状不同，收在一处）
         * @param name 变量名
         * @param value 空指针表示清掉这个变量
         */
        void setProbeEnvironmentVariable(const char *const name, const char *const value)
        {
#if ASYN_PLATFORM_WIN32
            static_cast<void>(::SetEnvironmentVariableA(name, value));
#else
            if (value != nullptr)
            {
                static_cast<void>(::setenv(name, value, 1));
            }
            else
            {
                static_cast<void>(::unsetenv(name));
            }
#endif
        }

        /**
         * @brief 取本测试二进制自己的路径，父侧用它把中间层启起来
         * @return std::string 可直接交给 spawn 的可执行文件路径；取不到时为空
         */
        std::string currentExecutablePath()
        {
#if ASYN_PLATFORM_WIN32
            std::vector<wchar_t> buffer(4 * MAX_PATH);
            const DWORD          length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size())
            {
                return {};
            }
            return TextEncoding::toUtf8String(std::wstring(buffer.data(), length));
#else
            std::vector<char> buffer(4096);
            const ssize_t     length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
            if (length <= 0)
            {
                return {};
            }
            return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
        }

        /**
         * @brief 进程号此刻是否还活着（父侧手里没有它的句柄，只能这样问）
         * @details POSIX 上「僵尸态」按不在处理算：它不占端口、不占描述符也不占内存，而容器里
         *          PID 1 不带 reap 时，被 PDEATHSIG 打死的那个孩子会一直以 Z 态挂在那里
         * @param processId 目标进程号
         * @return true 还在
         */
        bool isProcessAlive(const long processId)
        {
            if (processId <= 0)
            {
                return false;
            }
#if ASYN_PLATFORM_WIN32
            const HANDLE handle = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
            if (handle == nullptr)
            {
                return false;
            }
            const DWORD  waitResult = ::WaitForSingleObject(handle, 0);
            static_cast<void>(::CloseHandle(handle));
            return waitResult == WAIT_TIMEOUT;
#else
            // kill(pid, 0) 对僵尸进程也回成功，直接拿它当「还在跑」会把「保护没生效」与
            // 「保护生效了但没人收尸」判成同一件事。孙进程不是本进程的 child（waitpid 只回 ECHILD），
            // 所以只能读 /proc 里的状态字段
            if (std::ifstream statFile{std::format("/proc/{}/stat", processId)}; statFile)
            {
                std::string statLine;
                if (std::getline(statFile, statLine))
                {
                    // comm 里可以有空格与括号，状态字段必须按最后一个 ')' 之后定位，不能按空白切列
                    const std::size_t statePosition = statLine.rfind(')');
                    if (statePosition != std::string::npos && statePosition + 2U < statLine.size())
                    {
                        return statLine[statePosition + 2U] != 'Z';
                    }
                }
            }
            // 读不到 /proc（非 Linux 的 POSIX 环境、或那条进程属于别人）才退回信号判据
            if (::kill(static_cast<pid_t>(processId), 0) == 0)
            {
                return true;
            }
            // 存在但不属于我：EPERM 仍算活着，ESRCH 才是「没了」
            return errno == EPERM;
#endif
        }

        /**
         * @brief 收尾：把探针留下的孙进程收掉，绝不让它带着 60 秒的睡眠跑进别的用例
         * @param processId 目标进程号
         */
        void terminateProbeTarget(const long processId)
        {
            if (processId <= 0)
            {
                return;
            }
#if ASYN_PLATFORM_WIN32
            const HANDLE handle = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
            if (handle != nullptr)
            {
                static_cast<void>(::TerminateProcess(handle, 2));
                static_cast<void>(::WaitForSingleObject(handle, kWaitTimeoutMilliseconds));
                static_cast<void>(::CloseHandle(handle));
            }
#else
            static_cast<void>(::kill(static_cast<pid_t>(processId), SIGKILL));
#endif
        }

        /// 中间层报回来的三件事：孙进程号、保护是否真挂上、它在中间层还活着时是否确实在跑
        struct KillProbeReport
        {
            long processId{0};     ///< 孙进程号
            bool isGuardActive{false}; ///< 作业是否真挂上了（本进程已在禁止嵌套的作业里时为 false）
            bool wasRunning{false};    ///< 中间层退出前它确实在运行，排除「根本没起来」这种假绿
        };

        /**
         * @brief 父侧：按角色启一个中间层，等它硬退，再把探针报告读回来
         * @param role "on" 表示中间层要带保护起孙进程，"off" 表示不带
         * @return std::optional<KillProbeReport> 探针留下的事实；没留证据时为空
         */
        std::optional<KillProbeReport> runKillProbeAndReadReport(const std::string_view role)
        {
            const std::filesystem::path reportPath = std::filesystem::temp_directory_path() / kKillProbeReportName;
            std::error_code             ignored;
            static_cast<void>(std::filesystem::remove(reportPath, ignored));

            setProbeEnvironmentVariable(kKillProbeRoleVariable, std::string(role).c_str());
            setProbeEnvironmentVariable(kKillProbeReportVariable, reportPath.string().c_str());

            const std::string executablePath = currentExecutablePath();
            EXPECT_FALSE(executablePath.empty()) << "取不到自身路径，中间层启不起来";

            const Process::Handle middleHandle = Process::spawn(Process::LaunchOptions{
                .executablePath = executablePath,
                .arguments      = std::vector<std::string>{"--gtest_filter=Process.KillWithParentProbeMiddle", "--gtest_brief=1"},
            });
            setProbeEnvironmentVariable(kKillProbeRoleVariable, nullptr);
            setProbeEnvironmentVariable(kKillProbeReportVariable, nullptr);
            if (!middleHandle.isValid())
            {
                ADD_FAILURE() << "中间层探针没起来，平台错误码 " << PlatformError::lastErrorCode();
                return std::nullopt;
            }

            // 中间层写完报告就硬退，因此这里只等有界时限内的退出码；等不到说明探针卡在别处，判据不成立
            if (!waitForExit(middleHandle, kWaitTimeoutMilliseconds).has_value())
            {
                static_cast<void>(Process::forceTermination(middleHandle));
                ADD_FAILURE() << "中间层探针没在时限内退出";
                return std::nullopt;
            }

            std::ifstream report(reportPath);
            if (!report)
            {
                ADD_FAILURE() << "探针没留下报告文件：" << reportPath.string() << "（中间层可能在写下孙进程号之前就死了）";
                return std::nullopt;
            }
            long processId        = 0;
            int  guardAsDigit     = 0;
            int  wasRunningAsDigit = 0;
            report >> processId >> guardAsDigit >> wasRunningAsDigit;
            if (!report || processId <= 0)
            {
                ADD_FAILURE() << "探针报告读不成形，内容不可信";
                return std::nullopt;
            }
            return KillProbeReport{processId, guardAsDigit != 0, wasRunningAsDigit != 0};
        }
    } // namespace

    /**
     * @brief 中间层角色：起一个（按要求带或不带「随父终止」的）孙进程，报下进程号与保护状态后硬退
     * @details 报告必须先落盘再硬退：父侧看不到报告就当场红，而不是把「孙进程不见了」当成保护生效的证据
     */
    TEST(Process, KillWithParentProbeMiddle)
    {
        const auto role = ProcessInfo::environmentVariable(kKillProbeRoleVariable);
        if (!role.has_value())
        {
            GTEST_SKIP() << "本用例只在被父侧以探针角色启起来时执行";
        }
        const auto reportPathText = ProcessInfo::environmentVariable(kKillProbeReportVariable);
        ASSERT_TRUE(reportPathText.has_value()) << "父侧没交代报告落在哪";

        const ExitCommand     command = makeGuardProbeTargetCommand();
        const Process::Handle grandChild = Process::spawn(Process::LaunchOptions{
            .executablePath   = command.executablePath,
            .arguments        = command.arguments,
            .ownProcessGroup  = false,
            .killWithParent   = *role == "on",
        });
        ASSERT_TRUE(grandChild.isValid()) << "孙进程起不来，平台错误码 " << PlatformError::lastErrorCode();

        // 保护状态与「此刻它确实在跑」都要报回来：父侧看到孙进程不见了，得能分清那是保护带走的，
        // 还是它根本没起来
        const bool isGuardActive = grandChild.killWithParentGuardActive();
        const bool wasRunning    = Process::isRunning(grandChild);
        std::ofstream report(reportPathText->c_str());
        report << grandChild.processId() << ' ' << (isGuardActive ? 1 : 0) << ' ' << (wasRunning ? 1 : 0) << '\n';
        ASSERT_TRUE(static_cast<bool>(report)) << "探针报告写不出去，父侧的判据会失去前提";
        report.flush();

        // 硬退：正常返回会走 Handle 析构关作业句柄，那测的是「主动收口」而不是「被硬杀」
        exitTheProbeProcessAbruptly();
    }

    /**
     * @brief 钉住：给了 killWithParent 时，父侧被硬杀后子进程一并消失（不留占着端口的孤儿）
     * @details POSIX 靠 spawn 里那条 prctl(PR_SET_PDEATHSIG)（这条是它第一次被直接判），
     *          Windows 靠作业句柄。主机不给嵌套作业时保护挂不上，探针会把这件事写进报告，
     *          父侧据此 SKIP——那条路在这台机器上本来就测不到，而不是实现有问题
     */
    TEST(Process, KillWithParentGuardTakesTheChildDown)
    {
        const auto probe = runKillProbeAndReadReport("on");
        ASSERT_TRUE(probe.has_value());
        const long childProcessId = probe->processId;
        if (!probe->isGuardActive)
        {
            terminateProbeTarget(childProcessId);
            GTEST_SKIP() << "这台主机不给子进程挂作业（多为已处在一个禁止嵌套的作业里），保护本来就不可用";
        }
        // 「起来过」由中间层在退出前确认：父侧看到它不见了，得能分清那是保护带走的还是它根本没起来
        ASSERT_TRUE(probe->wasRunning) << "孙进程在中间层手里就没跑起来，后面的判定没有对象";

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{kKillProbeWaitMilliseconds};
        while (std::chrono::steady_clock::now() < deadline && isProcessAlive(childProcessId))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        const bool isGone = !isProcessAlive(childProcessId);
        terminateProbeTarget(childProcessId);
        EXPECT_TRUE(isGone) << "父进程硬退后，带保护的子进程还在跑：它正占着端口却没有编排者";
    }

    /**
     * @brief 钉住（Windows）：没给 killWithParent 时，父侧硬杀不该带走子进程
     * @details 换代交棒要的正是这一侧——新一代必须活过交棒的那一代，默认开保护会把它做成静默自杀。
     *          POSIX 没有「不要这条保护」的形状（spawn 一律装 PDEATHSIG），因此对照组只在 Windows 跑
     */
#if ASYN_PLATFORM_WIN32
    TEST(Process, WithoutKillWithParentTheChildOutlivesTheProbe)
    {
        const auto probe = runKillProbeAndReadReport("off");
        ASSERT_TRUE(probe.has_value());
        const long childProcessId = probe->processId;
        EXPECT_FALSE(probe->isGuardActive) << "没要保护却挂上了作业，说明开关没被消费";
        ASSERT_TRUE(probe->wasRunning) << "对照组里孙进程也没起来，那「它活着」这条判据就是空的";

        // 保护生效时消失是毫秒级，这一秒里还活着就等于「没被带走」
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{kKillProbeSurvivalWindowMilliseconds};
        bool       isAlive  = true;
        while (std::chrono::steady_clock::now() < deadline)
        {
            isAlive = isProcessAlive(childProcessId);
            if (!isAlive)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        EXPECT_TRUE(isAlive) << "没给 killWithParent 的子进程也被带走了，交棒那条路会被做成静默自杀";
        terminateProbeTarget(childProcessId);
    }
#endif
} // namespace AsynGyanis::Platform
