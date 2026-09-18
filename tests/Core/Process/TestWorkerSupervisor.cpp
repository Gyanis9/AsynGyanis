/**
 * @file TestWorkerSupervisor.cpp
 * @brief WorkerSupervisor 单元测试：配置校验、补齐 worker、崩溃退避、收尾送走
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Process/WorkerSupervisor.h"

#include "Base/Exception/Exception.h"
#include "Platform/Platform.h"

#include "BaseTestSupport.h"
#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::kWaitTimeout;
        using TestSupport::waitForCondition;

        /// 假 worker 的行为：本类的编排逻辑要靠这些行为分别触发
        enum class WorkerBehaviour
        {
            CrashImmediately,       ///< 记一次启动后立刻以 1 退出：触发「起来就崩」与退避
            SleepUntilTerminated,   ///< 记一次启动后睡到被杀：正常 worker 的替身
            SleepIgnoringTerminate, ///< 忽略 SIGTERM 后睡：逼出「收尾到期强杀」那条路径
        };

        /**
         * @brief 一次「假 worker」的启动记录：每启动一次就往文件里追加一行
         *
         * @details 用外部可观察的痕迹代替给被测类加测试专用钩子：worker 真被起了几次，
         *          可以从文件行数里数出来，不必读被测对象的内部状态
         */
        class WorkerLaunchLog
        {
        public:
            WorkerLaunchLog() :
                m_directory("WorkerSupervisor")
            {
                m_path = m_directory.path() / "worker-launches.log";
            }

            WorkerLaunchLog(const WorkerLaunchLog &) = delete;

            WorkerLaunchLog &operator=(const WorkerLaunchLog &) = delete;

            /// 记录文件路径（交给假 worker 作为参数）
            [[nodiscard]] const std::filesystem::path &path() const noexcept
            {
                return m_path;
            }

            /// 数一数目前为止记录了几次启动
            [[nodiscard]] std::size_t launchCount() const
            {
                std::ifstream file(m_path, std::ios::in);
                std::size_t   lineCount = 0;
                std::string   line;
                while (std::getline(file, line))
                {
                    if (!line.empty())
                    {
                        ++lineCount;
                    }
                }
                return lineCount;
            }

        private:
            Base::TestSupport::TemporaryDirectory m_directory; ///< 本用例独占的临时目录（析构时递归删除）
            std::filesystem::path                 m_path;      ///< 记录文件
        };

        /**
         * @brief 构造一份指向「假 worker」的编排配置
         * @details 假 worker 是 shell 跑的一段脚本，与被测项目的可执行文件无关：编排只负责起进程与看护，
         *          用脚本当 worker 才能把「重启、退避、收尾」这几件事单独测出来
         * @param launchLog 启动记录
         * @param workerCount worker 个数
         * @param behaviour 假 worker 的行为
         * @return WorkerSupervisor::Configuration 编排参数
         */
        WorkerSupervisor::Configuration makeConfiguration(const WorkerLaunchLog &launchLog, const std::size_t workerCount,
                                                         const WorkerBehaviour behaviour)
        {
            std::string script = "echo worker >> \"" + launchLog.path().string() + "\"; ";
            switch (behaviour)
            {
                case WorkerBehaviour::CrashImmediately:
                    script += "exit 1";
                    break;
                case WorkerBehaviour::SleepUntilTerminated:
                    // exec：让 sleep 取代 shell 本体，于是「一个 worker = 一个进程」，SIGTERM 直接落在它身上
                    script += "exec sleep 5";
                    break;
                case WorkerBehaviour::SleepIgnoringTerminate:
                    // SIG_IGN 会被 exec 继承，因此 sleep 忽略 SIGTERM，只能等收尾期限到点被强杀
                    script += "trap '' TERM; exec sleep 5";
                    break;
            }

            WorkerSupervisor::Configuration configuration;
#if ASYN_PLATFORM_WIN32
            // Windows 上本类在构造时就拒绝多进程（没有 SO_REUSEPORT 共享端口），因此这些配置只用于
            // 「构造即拒绝」那条用例，脚本不会被执行
            configuration.executablePath  = "cmd.exe";
            configuration.workerArguments = {"/c", script};
#else
            configuration.executablePath  = "/bin/sh";
            configuration.workerArguments = {"-c", script};
#endif
            configuration.workerCount     = workerCount;
            configuration.pollInterval    = std::chrono::milliseconds{20};
            configuration.restartBackoff  = std::chrono::milliseconds{50};
            configuration.shutdownTimeout = std::chrono::milliseconds{2000};
            configuration.crashLoopWindow = std::chrono::milliseconds{300};
            configuration.crashLoopLimit  = 3;
            return configuration;
        }
    } // namespace

    /**
     * @brief worker 数小于 2 时当场拒绝：单进程不该走编排（多一层进程反而添乱）
     */
    TEST(WorkerSupervisor, RejectsWorkerCountBelowTwo)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 1;

        EXPECT_THROW(static_cast<void>(WorkerSupervisor(configuration)), Base::Exception);
    }

    /**
     * @brief 可执行文件路径为空时当场拒绝：起不来是必然的，配置错误要在构造期就暴露
     */
    TEST(WorkerSupervisor, RejectsEmptyExecutablePath)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.workerCount = 2;

        EXPECT_THROW(static_cast<void>(WorkerSupervisor(configuration)), Base::Exception);
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief Windows 上多进程被明确拒绝：没有 SO_REUSEPORT，多个进程绑不上同一个端口
     * @details 拒绝而不是静默降级成单进程：静默降级会让「配了 4 个 worker 却只有一个在干活」无从察觉
     */
    TEST(WorkerSupervisor, RejectsMultiProcessOnWindows)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 2;

        try
        {
            const WorkerSupervisor supervisor(configuration);
            FAIL() << "Windows 上构造多进程编排应当被拒绝";
        } catch (const Base::Exception &exception)
        {
            EXPECT_NE(std::string(exception.what()).find("SO_REUSEPORT"), std::string::npos)
                    << "拒绝原因应当说清缺的是端口共享能力：" << exception.what();
        }
    }
#else
    /**
     * @brief 起来就崩的 worker 会被补若干次，超过上限后放弃并让编排结束（不空转刷日志）
     */
    TEST(WorkerSupervisor, GivesUpAfterRepeatedCrashLoop)
    {
        const WorkerLaunchLog launchLog;
        WorkerSupervisor      supervisor(makeConfiguration(launchLog, 2, WorkerBehaviour::CrashImmediately));

        // 两个槽位各崩 crashLoopLimit(3) 次后放弃，编排随即返回
        const auto runStartTime = std::chrono::steady_clock::now();
        supervisor.run();

        EXPECT_LT(std::chrono::steady_clock::now() - runStartTime, kWaitTimeout) << "崩溃循环之后编排没有自己结束";
        EXPECT_EQ(launchLog.launchCount(), 6U) << "两个槽位各应被补到上限为止（各 3 次）";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U);
    }

    /**
     * @brief 正常 worker 只起一次就稳定运行；收到停止请求后编排把它送走并返回
     */
    TEST(WorkerSupervisor, StartsWorkersOnceAndStopsThemOnRequest)
    {
        const WorkerLaunchLog launchLog;
        WorkerSupervisor      supervisor(makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated));

        // run() 阻塞，因此编排跑在另一个线程上；停止请求从本线程发起
        std::thread supervisorThread([&supervisor]
        {
            supervisor.run();
        });

        ASSERT_TRUE(waitForCondition(
                [&launchLog]
                {
                    return launchLog.launchCount() >= 2;
                },
                kWaitTimeout)) << "两个 worker 没有都起来";
        EXPECT_EQ(supervisor.runningWorkerCount(), 2U) << "两个 worker 都应当在运行";

        // 稳定运行之后再等一小会儿：不该出现「明明活着却被重复补位」的情况
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_EQ(launchLog.launchCount(), 2U) << "worker 还活着却被重复启动";

        supervisor.requestStop();
        supervisorThread.join();

        EXPECT_EQ(launchLog.launchCount(), 2U) << "收尾不该再起新 worker";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U) << "收尾之后不该还有 worker 在跑";
    }

    /**
     * @brief 收尾有期限：worker 不理会 SIGTERM 时，编排到点强杀并按时返回
     */
    TEST(WorkerSupervisor, ShutdownTimeoutForcesTermination)
    {
        const WorkerLaunchLog launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepIgnoringTerminate);
        configuration.shutdownTimeout = std::chrono::milliseconds{300};

        WorkerSupervisor supervisor(configuration);
        std::thread      supervisorThread([&supervisor]
        {
            supervisor.run();
        });

        ASSERT_TRUE(waitForCondition(
                [&launchLog]
                {
                    return launchLog.launchCount() >= 2;
                },
                kWaitTimeout)) << "两个 worker 没有都起来";

        const auto stopStartTime = std::chrono::steady_clock::now();
        supervisor.requestStop();
        supervisorThread.join();
        const auto stopElapsedTime = std::chrono::steady_clock::now() - stopStartTime;

        EXPECT_GE(stopElapsedTime, configuration.shutdownTimeout) << "worker 不理会 SIGTERM，收尾却早于期限返回了";
        EXPECT_LT(stopElapsedTime, kWaitTimeout) << "收尾超出了期限还在等：强杀那一步没有兜住";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U);
    }
#endif
} // namespace AsynGyanis::Core
