// CrashHandler 单元测试：重复安装的判定、致命信号下的回调顺序与「仍以信号致死」的语义
#include "Platform/System/CrashHandler.h"

#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        constexpr char kMinimalMarker[] = "minimal-scene-marker";
        constexpr char kFullMarker[]    = "full-scene-marker";

        /// 最小现场回调：信号上下文里只做一次无分配的描述符写
        void writeMinimalMarker(const CrashSignal, void *) noexcept
        {
            CrashHandler::writeStandardError(kMinimalMarker);
        }

        /// 完整现场回调：Linux 上它由 fork 出来的子进程执行
        void writeFullMarker(const CrashSignal, void *) noexcept
        {
            CrashHandler::writeStandardError(kFullMarker);
        }
    } // namespace

    TEST(CrashHandler, InstallIsIdempotent)
    {
        // 用例可能与前一条同进程执行（不走 ctest 的逐用例进程）：已装过就不再要求首次安装成功
        if (!CrashHandler::isInstalled())
        {
            EXPECT_TRUE(CrashHandler::install(&writeMinimalMarker, &writeFullMarker));
        }
        EXPECT_TRUE(CrashHandler::isInstalled());
        EXPECT_FALSE(CrashHandler::install(&writeMinimalMarker, &writeFullMarker)) << "重复安装应如实报「早就装过」";
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 致命信号：两个回调都跑到，进程仍以该信号致死
     * @details 子进程继承测试进程已装好的处置，stderr 接到管道上；父进程读到 EOF 时完整现场的子进程
     *          也已退出，两个标记必须都在。
     */
    TEST(CrashHandler, FatalSignalRunsBothReportersAndDiesBySignal)
    {
        if (!CrashHandler::isInstalled())
        {
            ASSERT_TRUE(CrashHandler::install(&writeMinimalMarker, &writeFullMarker));
        }

        int pipePair[2] = {-1, -1};
        ASSERT_EQ(::pipe(pipePair), 0);
        const pid_t childPid = ::fork();
        ASSERT_GE(childPid, 0);
        if (childPid == 0)
        {
            static_cast<void>(::close(pipePair[0]));
            static_cast<void>(::dup2(pipePair[1], 2));
            static_cast<void>(::close(pipePair[1]));
            static_cast<void>(std::raise(SIGSEGV));
            std::_Exit(97); // 处理器没装上时与「信号致死」区分开
        }

        static_cast<void>(::close(pipePair[1]));
        std::string standardErrorText;
        char        buffer[1024];
        ssize_t     readCount = 0;
        while ((readCount = ::read(pipePair[0], buffer, sizeof(buffer))) > 0)
        {
            standardErrorText.append(buffer, static_cast<std::size_t>(readCount));
        }
        static_cast<void>(::close(pipePair[0]));

        int status = 0;
        ASSERT_EQ(::waitpid(childPid, &status, 0), childPid);
        ASSERT_TRUE(WIFSIGNALED(status)) << "崩溃处置把「信号致死」改掉了（状态字 " << status << "）";
        EXPECT_EQ(WTERMSIG(status), SIGSEGV);
        EXPECT_NE(standardErrorText.find(kMinimalMarker), std::string::npos) << "最小现场回调没跑，收到：" << standardErrorText;
        EXPECT_NE(standardErrorText.find(kFullMarker), std::string::npos) << "完整现场回调没跑，收到：" << standardErrorText;
    }
#else
    TEST(CrashHandler, FatalSignalRunsInProcessOnWindows)
    {
        GTEST_SKIP() << "Windows 没有 fork，就地崩溃会带走测试进程；信号路径由 TestCrashScene 的子进程用例覆盖";
    }
#endif
} // namespace AsynGyanis::Platform
