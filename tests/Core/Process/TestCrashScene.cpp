// CrashScene 测试：致命信号与 terminate 都要在死前留下现场，且死亡语义仍归默认处置。
// 覆盖场景：
// - FatalSignalLeavesSceneOnStandardErrorAndDiesBySignal（POSIX：fork 出子进程踩 SIGSEGV，
//   校验 stderr 上有现场、进程仍以该信号致死——WIFSIGNALED 与上层看护的口径都保住）
// - FatalLogLineAndEarlierRecordsSurviveSignalDeath（POSIX：LOG_FATAL 刷盘后信号致死，
//   文件里同时留有 FATAL 与更早的 INFO；崩溃现场本身不替日志系统补刷）
// - TerminateLeavesSceneWithEscapingExceptionOnStandardError（POSIX：子进程里抛出后调
//   std::terminate，现场里要带出逃逸异常原文）
// - InstallIsIdempotent（重复安装如实报「早就装过」）
#include "Core/Process/CrashScene.h"

#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/FileSink.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#if !ASYN_PLATFORM_WIN32
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

#include <string>

namespace AsynGyanis::Core
{
#if !ASYN_PLATFORM_WIN32
    namespace
    {
        using Base::FileSink;
        using Base::Logger;
        using Base::LogLevel;

        /// 子进程的记录：现场文本与退出状态从管道/状态码带回来
        struct ChildCrashOutcome
        {
            std::string standardErrorText; ///< 子进程 stderr 上的全部输出（现场的落点）
            int         status   = 0;      ///< waitpid 的原始状态字
            pid_t       childPid = -1;     ///< 子进程号
        };

        /**
         * @brief fork 一个子进程并把它的 stderr 接到管道，执行 crashAction，把现场与退出状态带回
         * @details 用 fork 而不是就地 raise：处理器会把默认处置恢复后重发信号，就地做等于把测试进程
         *          本身打死。子进程里不用 gtest 宏（那边不是测试上下文），判据全在父进程做
         * @tparam CrashAction 无参可调用体，负责把子进程弄死
         * @param crashAction 崩溃剧本
         * @return ChildCrashOutcome 现场文本与退出状态
         */
        template<typename CrashAction>
        ChildCrashOutcome runCrashInChildProcess(CrashAction crashAction)
        {
            int pipeDescriptorPair[2] = {-1, -1};
            EXPECT_EQ(::pipe(pipeDescriptorPair), 0);
            const pid_t childPid = ::fork();
            EXPECT_GE(childPid, 0);
            if (childPid == 0)
            {
                // 子进程：stderr 换成管道写端（现场只有写进这里，父进程才收得到），然后按剧本崩
                ::close(pipeDescriptorPair[0]);
                ::dup2(pipeDescriptorPair[1], 2);
                ::close(pipeDescriptorPair[1]);
                crashAction();
                // 剧本没能把子进程弄死（例如处理器根本没装）：用独立于被测判据的退出码区分这种局面
                std::_Exit(97);
            }

            ::close(pipeDescriptorPair[1]);
            ChildCrashOutcome outcome;
            outcome.childPid = childPid;
            char    buffer[4096];
            ssize_t readCount = 0;
            while ((readCount = ::read(pipeDescriptorPair[0], buffer, sizeof(buffer))) > 0)
            {
                outcome.standardErrorText.append(buffer, static_cast<std::size_t>(readCount));
            }
            ::close(pipeDescriptorPair[0]);
            [[maybe_unused]] const pid_t waitedPid = ::waitpid(childPid, &outcome.status, 0);
            return outcome;
        }
        /// 经 noexcept 边界抛：逃逸异常立刻走 terminate 且异常仍处活动态（handler 里取得到 what()），
        /// 也不会被 fork 进来的 gtest 测试体 catch 接走——直接 throw 会被它接住，测不到 terminate。
        /// throw 经可调用对象间接执行：noexcept 函数里直接写 throw 会吃到 GCC 的 -Wterminate
        /// （「必然 terminate」的静态告警），而这条正是要被测的真实形状
        void throwFromNoexceptBoundary(const std::function<void()> &thrower) noexcept
        {
            thrower();
        }
    } // namespace

    /**
     * @brief 致命信号：现场落在 stderr 上，进程仍以该信号致死
     * @details 后一条与 WorkerSupervisor 的补位口径同解：上层靠 WIFSIGNALED 区分「崩了」与
     *          「正常退出」，现场处理器不许把这条语义改掉（改掉的话看护会把崩溃读成正常停机）
     */
    TEST(CrashScene, FatalSignalLeavesSceneOnStandardErrorAndDiesBySignal)
    {
        ASSERT_TRUE(CrashScene::install()) << "本进程内首次安装应当完成";
        EXPECT_TRUE(CrashScene::isInstalled());

        const ChildCrashOutcome outcome = runCrashInChildProcess([] { std::raise(SIGSEGV); });

        ASSERT_TRUE(WIFSIGNALED(outcome.status)) << "崩溃现场处理器把「信号致死」的语义改掉了（状态字 " << outcome.status << "）";
        EXPECT_EQ(WTERMSIG(outcome.status), SIGSEGV) << "重发信号没有落到默认处置上";
        EXPECT_NE(outcome.standardErrorText.find("崩溃现场"), std::string::npos) << "stderr 上没有现场开头，收到：" << outcome.standardErrorText;
        EXPECT_NE(outcome.standardErrorText.find("SIGSEGV"), std::string::npos) << "现场没有交出致命信号名，收到：" << outcome.standardErrorText;
    }

    /**
     * @brief std::terminate：现场里要带出逃逸异常的原文，死亡仍按默认处置（abort=信号致死）
     */
    TEST(CrashScene, TerminateLeavesSceneWithEscapingExceptionOnStandardError)
    {
        CrashScene::install();
        const ChildCrashOutcome outcome = runCrashInChildProcess([] { throwFromNoexceptBoundary([] { throw std::runtime_error("测试用的逃逸异常原文"); }); });

        ASSERT_TRUE(WIFSIGNALED(outcome.status)) << "terminate 之后没有按「信号致死」收尾（状态字 " << outcome.status << "）";
        EXPECT_NE(outcome.standardErrorText.find("terminate"), std::string::npos) << "现场没有点出 terminate，收到：" << outcome.standardErrorText;
        EXPECT_NE(outcome.standardErrorText.find("测试用的逃逸异常原文"), std::string::npos) << "现场没有带出逃逸异常的原文，收到：" << outcome.standardErrorText;
    }

    /**
     * @brief 多线程运行中一条线程崩：现场照旧、死亡语义不变（TSan 筛面含 Crash 后的观察点）
     * @details 处理器可能在任意线程上被投递：子进程里起一条空转线程，再由另一条线程 raise(SIGSEGV)
     *          ——最小现场与完整现场都要照常落出，进程仍以该信号致死
     */
    TEST(CrashScene, FatalSignalFromOneOfMultipleThreadsStillLeavesSceneAndDiesBySignal)
    {
        CrashScene::install();

        const ChildCrashOutcome outcome = runCrashInChildProcess(
                []
                {
                    std::atomic<bool> workerShouldStop{false};
                    std::thread       background(
                            [&workerShouldStop]
                            {
                                while (!workerShouldStop.load(std::memory_order_relaxed))
                                {
                                    std::this_thread::yield();
                                }
                            });
                    std::thread crasher([] { std::raise(SIGSEGV); });
                    crasher.join(); // 到不了这里：进程已随信号致死；留着只是让「没死成」的局面可控
                    workerShouldStop.store(true, std::memory_order_relaxed);
                    background.join();
                });

        ASSERT_TRUE(WIFSIGNALED(outcome.status)) << "多线程进程里崩溃处理器把死亡语义改掉了（状态字 " << outcome.status << "）";
        EXPECT_EQ(WTERMSIG(outcome.status), SIGSEGV);
        EXPECT_NE(outcome.standardErrorText.find("崩溃现场"), std::string::npos) << "stderr 上没有现场，收到：" << outcome.standardErrorText;
        EXPECT_NE(outcome.standardErrorText.find("线程指纹"), std::string::npos) << "现场没有交出线程指纹，收到：" << outcome.standardErrorText;
    }

    /**
     * @brief 钉住崩溃前的日志契约：LOG_FATAL 返回时记录已在盘上，信号随后致死也不丢
     * @details FileSink 默认不逐行同步，一条短记录留在流缓冲里；信号致死不会走析构，缓冲就随进程消失。
     *          Fatal 在分发后刷新每个收下它的 Sink，因此它自己与更早的普通记录应当一起落盘。
     *          崩溃前的普通等级不作保证（那要调用方自己 flush），故这里只断言 FATAL 拉齐的那一批。
     */
    TEST(CrashScene, FatalLogLineAndEarlierRecordsSurviveSignalDeath)
    {
        const std::filesystem::path logPath = std::filesystem::temp_directory_path() / ("asyn-crash-scene-fatal-" + std::to_string(static_cast<long long>(::getpid())) + ".log");
        std::filesystem::remove(logPath);

        CrashScene::install();
        const ChildCrashOutcome outcome = runCrashInChildProcess(
                [logPath]
                {
                    Logger logger("crash_scene_fatal");
                    logger.setLevel(LogLevel::Trace);
                    logger.addSink(std::make_shared<FileSink>(logPath, /*truncate*/ true));
                    logger.log(LogLevel::Info, "info-before-fatal");
                    logger.log(LogLevel::Fatal, "fatal-before-crash");
                    std::raise(SIGSEGV);
                });

        std::ifstream     logFile(logPath, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
        logFile.close();
        std::filesystem::remove(logPath);

        ASSERT_TRUE(WIFSIGNALED(outcome.status)) << "子进程没有按信号致死（状态字 " << outcome.status << "）";
        EXPECT_EQ(WTERMSIG(outcome.status), SIGSEGV);
        EXPECT_NE(contents.find("fatal-before-crash"), std::string::npos) << "LOG_FATAL 返回时那一条还没落盘：信号一死，缓冲区跟着进程没了。文件内容：" << contents;
        EXPECT_NE(contents.find("info-before-fatal"), std::string::npos) << "Fatal 只刷了自己那一条，之前同一 Sink 的普通记录仍留在缓冲里。文件内容：" << contents;
    }
#else
    TEST(CrashScene, FatalLogLineAndEarlierRecordsSurviveSignalDeath)
    {
        GTEST_SKIP() << "Windows 侧崩溃现场的完整路径未验证；本用例的 fork 剧本只在 POSIX 上可用";
    }
#endif

    TEST(CrashScene, InstallIsIdempotent)
    {
        CrashScene::install(); // 先确保处于已装状态（返回值取决于本进程此前有没有测过，不押它）
        EXPECT_FALSE(CrashScene::install()) << "重复安装应当如实报「早就装过」";
        EXPECT_TRUE(CrashScene::isInstalled());
    }
} // namespace AsynGyanis::Core
