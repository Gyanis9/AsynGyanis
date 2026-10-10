// WorkerSupervisor 单元测试：配置校验、补齐 worker、崩溃退避、收尾送走

#include "Core/Process/WorkerSupervisor.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/LogicException.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include "CommonTestSupport.h"
#include "CoreTestSupport.h"
#include "MetricsTestSupport.h"

#if ASYN_PLATFORM_WIN32
// 控制台事件那两条用例要的是 console/process API（winsock 的传递包含给不到）
#include <windows.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using AsynGyanis::TestSupport::hasRegistrySample;
        using AsynGyanis::TestSupport::registryValue;
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
            WorkerLaunchLog() : m_directory("WorkerSupervisor")
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
            AsynGyanis::TestSupport::TemporaryDirectory m_directory; ///< 本用例独占的临时目录（析构时递归删除）
            std::filesystem::path                       m_path;      ///< 记录文件
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
        WorkerSupervisor::Configuration makeConfiguration(const WorkerLaunchLog &launchLog, const std::size_t workerCount, const WorkerBehaviour behaviour)
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
            // 移交模式要求 worker 自己连回通道，shell 脚本不会做这件事，因此这份假 worker 在 Windows
            // 上只服务「构造期就拒绝」那几条用例，脚本不会被执行；真的跨进程编排由下面那条用例拿
            // tests/Tools 的夹具（handoff_worker）跑
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

#if ASYN_PLATFORM_WIN32
        /**
         * @brief 连一次端口，把夹具写回的那行文本读回来
         * @param port master 那边监听的实际端口
         * @return std::string 读到的内容（去掉行尾换行）；连不上或读不到时为空串
         * @details 判据必须落在「读到内容」上：回环上 connect 能建立只说明 backlog 收了这条请求，
         *          并不证明真有人在 accept——移交只做到「端口还听着」而 worker 没接手时，
         *          只看连接成功的用例会是假绿。读操作带期限，超时就交回空串由用例判红。
         */
        std::string askWorker(const std::uint16_t port)
        {
            const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (client < 0)
            {
                return {};
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = htons(port);
            if (::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
            {
                static_cast<void>(::closesocket(client));
                return {};
            }

            DWORD receiveTimeout{5000U};
            static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receiveTimeout), sizeof(receiveTimeout)));
            char      buffer[32]{};
            const int received = ::recv(client, buffer, static_cast<int>(sizeof(buffer)) - 1, 0);
            static_cast<void>(::closesocket(client));
            if (received <= 0)
            {
                return {};
            }

            std::string text(buffer, static_cast<std::size_t>(received));
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            {
                text.pop_back();
            }
            return text;
        }
#endif
    } // namespace

    /**
     * @brief worker 数小于 2 时当场拒绝：单进程不该走编排（多一层进程反而添乱）
     */
    TEST(WorkerSupervisor, RejectsWorkerCountBelowTwo)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 1;

        EXPECT_THROW(static_cast<void>(WorkerSupervisor(configuration)), Base::LogicException);
    }

    /**
     * @brief 可执行文件路径为空时当场拒绝：起不来是必然的，配置错误要在构造期就暴露
     */
    TEST(WorkerSupervisor, RejectsEmptyExecutablePath)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.workerCount = 2;

        EXPECT_THROW(static_cast<void>(WorkerSupervisor(configuration)), Base::LogicException);
    }

    /**
     * @brief 拒绝面：崩溃判据非正会让编排变成满速重启循环，配置错误要在构造期就被挡下
     * @details 存活窗口取 0 或负数时，「存活不足窗口才算一次起来就崩」这条比较恒不成立，崩溃计数
     *          走的是「稳定运行后退出」那条清零分支：既不涨计数也就永不放弃、永不退避，秒退型
     *          worker 会被按轮询间隔满速 fork/exec，run() 永不返回。上限取 0 是反方向的写错：
     *          崩一次就放弃整池。
     *          断言盯的是异常文本而不是异常类型：两个平台的构造期校验条数不同（Windows 还多一条
     *          「必须给移交配置」），只判异常类型会看不出这条校验到底有没有生效。
     */
    TEST(WorkerSupervisor, RejectsNonPositiveCrashLoopJudgement)
    {
        // 返回构造抛出的异常文本；构造成功则返回空串
        auto rejectionTextOf = [](const WorkerSupervisor::Configuration &configuration)
        {
            try
            {
                static_cast<void>(WorkerSupervisor(configuration));
            } catch (const Base::LogicException &exception)
            {
                return std::string(exception.what());
            }
            return std::string{};
        };

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 2;
        configuration.crashLoopLimit = 3;

        for (const long long windowMilliseconds: {0LL, -1LL})
        {
            configuration.crashLoopWindow   = std::chrono::milliseconds{windowMilliseconds};
            const std::string rejectionText = rejectionTextOf(configuration);
            EXPECT_FALSE(rejectionText.empty()) << "窗口 " << windowMilliseconds << " 毫秒该被拒绝";
            EXPECT_NE(rejectionText.find("crashLoopWindow"), std::string::npos) << rejectionText;
        }

        configuration.crashLoopWindow        = std::chrono::milliseconds{3000};
        configuration.crashLoopLimit         = 0;
        const std::string limitRejectionText = rejectionTextOf(configuration);
        EXPECT_FALSE(limitRejectionText.empty()) << "上限 0 意味着崩一次就放弃整池，该被拒绝";
        EXPECT_NE(limitRejectionText.find("crashLoopLimit"), std::string::npos) << limitRejectionText;

        // 对照：合法判据不许被这条校验挡掉，否则上面的断言就成了「什么都拒」
        configuration.crashLoopLimit            = 3;
        const std::string acceptedRejectionText = rejectionTextOf(configuration);
        EXPECT_EQ(acceptedRejectionText.find("崩溃判据"), std::string::npos) << acceptedRejectionText;
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief Windows 上没有移交配置就拒绝：多个进程各自 bind 同一端口不分摊
     * @details 拒绝而不是静默降级成单进程：静默降级会让「配了 4 个 worker 却只有一个在干活」无从
     *          察觉——Windows 的内核把全部连接交给最后绑上的那一个，其余进程一个错都不报却永远
     *          收不到连接。替代形状现在有两个，拒绝文案都要点名：要么把 workers 设为 1（单进程 +
     *          多工作循环 + 接受分发），要么把 master 已监听的描述符交进 handoff 走移交。
     */
    TEST(WorkerSupervisor, RequiresHandoffConfigurationOnWindows)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 2;

        try
        {
            const WorkerSupervisor supervisor(configuration);
            FAIL() << "Windows 上没给移交配置的多进程编排应当被拒绝";
        } catch (const Base::LogicException &exception)
        {
            const std::string text{exception.what()};
            EXPECT_NE(text.find("SO_REUSEPORT"), std::string::npos) << "拒绝原因应当说清缺的是端口共享能力：" << text;
            // 光说「不支持」不够：拒绝的同时要把当下能用的形状交出来，否则调用方只能去翻代码
            EXPECT_NE(text.find("workers 设为 1"), std::string::npos) << "拒绝信息应当给出可落地的替代配置：" << text;
            EXPECT_NE(text.find("handoff"), std::string::npos) << "拒绝信息应当指出移交那条形状叫什么：" << text;
        }

        // 移交配置的每一句都得自己站得住：描述符负数与预算非正都会把编排变成看不出来的等待
        WorkerSupervisor::Configuration badDescriptor = configuration;
        badDescriptor.handoff                         = WorkerSupervisor::Handoff{};
        badDescriptor.handoff->listeningDescriptor    = -1;
        EXPECT_THROW(static_cast<void>(WorkerSupervisor(badDescriptor)), Base::LogicException);

        WorkerSupervisor::Configuration badBudget = configuration;
        badBudget.handoff                         = WorkerSupervisor::Handoff{1, std::chrono::milliseconds::zero()};
        EXPECT_THROW(static_cast<void>(WorkerSupervisor(badBudget)), Base::LogicException) << "预算 0 意味着等 worker 连上通道没有期限";
    }

    /**
     * @brief Windows 的移交编排：master 绑一次，两个 worker 各自接手同一份监听并真的应答
     * @details 这条路只能这样钉：WSADuplicateSocketW 按**目标进程号**发凭证，同一个进程里试不出
     *          「另一个进程拿到监听引用后能不能替这个端口接活」。判据也不留在被测类里——连接由本
     *          用例发起、回话由 tests/Tools 的夹具写，两边都不读编排器的内部状态。
     * @note run() 放在一条独立线程上：Windows 的派生是 CreateProcess，没有 POSIX 那套 fork 前必须
     *       单线程的约束（夹具也不碰 SIGTERM 之外的信号）。
     */
    TEST(WorkerSupervisor, HandsTheListeningSocketToEachWorker)
    {
        const Platform::Socket::Initialization network;

        // master 侧的监听套接字：端口交给内核挑，本用例只按拿到的端口去连
        const int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(listener, 0) << "造不出监听套接字，错误码 " << WSAGetLastError();
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0) << "绑定失败，错误码 " << WSAGetLastError();
        ASSERT_EQ(::listen(listener, 16), 0) << "进入监听失败，错误码 " << WSAGetLastError();
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
        ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength), 0) << "取不回实际端口";
        const std::uint16_t port = ntohs(address.sin_port);

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath  = ASYN_HANDOFF_WORKER_TOOL;
        configuration.workerCount     = 2;
        configuration.handoff         = WorkerSupervisor::Handoff{listener, std::chrono::seconds{10}};
        configuration.pollInterval    = std::chrono::milliseconds{20};
        configuration.restartBackoff  = std::chrono::milliseconds{50};
        configuration.shutdownTimeout = std::chrono::seconds{5};
        WorkerSupervisor supervisor(configuration);

        std::atomic<bool> isRunFinished{false};
        std::atomic<bool> isStoppedAsRequested{false};
        std::thread       supervisorThread(
                [&supervisor, &isRunFinished, &isStoppedAsRequested]
                {
                    isStoppedAsRequested.store(supervisor.run(), std::memory_order_release);
                    isRunFinished.store(true, std::memory_order_release);
                });

        // 移交没做成的槽位会被收掉重开，因此「两个都在跑」这个数字本身就说明两条通道都交到了对方手里
        const bool isPoolUp = waitForCondition([&supervisor] { return supervisor.runningWorkerCount() == 2U; }, kWaitTimeout);

        // 两次独立的问答：落哪个 worker 上都算，两次都拿到回话就证明接手过移交的进程真在服务这个端口
        std::vector<std::string> answers;
        if (isPoolUp)
        {
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                answers.push_back(askWorker(port));
            }
        }

        supervisor.requestStop();
        const bool isThreadFinished = waitForCondition([&isRunFinished] { return isRunFinished.load(std::memory_order_acquire); }, kWaitTimeout);
        if (isThreadFinished)
        {
            supervisorThread.join();
        }
        static_cast<void>(::closesocket(listener));

        ASSERT_TRUE(isThreadFinished) << "请求停止后编排没有返回";
        EXPECT_TRUE(isStoppedAsRequested.load(std::memory_order_acquire)) << "按请求收口却报了「整池被放弃」";
        ASSERT_TRUE(isPoolUp) << "两个 worker 没在预算内都接手监听套接字";
        ASSERT_EQ(answers.size(), 2U);
        EXPECT_EQ(answers[0], "handoff-ok") << "第一次问答没拿到夹具的回话：那份监听引用在 worker 手里不可用";
        EXPECT_EQ(answers[1], "handoff-ok") << "第二次问答没拿到回话";
    }

    /// 角色标记：内层探针只在由父侧以 CREATE_NEW_CONSOLE 启起来时才有控制台前提
    constexpr const char *kConsoleProbeEnvironmentVariable = "ASYN_WORKER_SUPERVISOR_CONSOLE_PROBE";

    /// 父侧等内层探针跑完的上限：里面要起两个 worker、发事件、再走完收尾
    constexpr DWORD kConsoleProbeWaitMilliseconds = 60000;

    /// 标记文件按名字落在临时目录里：父侧靠它们把「探针上场了」与「裁判判了通过」分开
    std::filesystem::path consoleProbeMarkerPath(const std::string &markerName)
    {
        return std::filesystem::temp_directory_path() / ("asyn-supervisor-console-" + markerName + ".marker");
    }

    void writeConsoleProbeMarker(const std::string &markerName)
    {
        std::ofstream stream(consoleProbeMarkerPath(markerName));
        stream << "1";
    }

    bool consoleProbeMarkerExists(const std::string &markerName)
    {
        return std::filesystem::exists(consoleProbeMarkerPath(markerName));
    }

    void removeConsoleProbeMarker(const std::string &markerName)
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(consoleProbeMarkerPath(markerName), ignored));
    }

    /**
     * @brief 内层探针逐里程碑留痕：父侧只拿得到退出码与标记文件，红一条时要看得见「走到哪一步」
     * @param step 步骤名
     */
    void appendConsoleProbeTrace(const std::string &step)
    {
        std::ofstream stream(consoleProbeMarkerPath("trace"), std::ios::app);
        stream << step << "\n";
    }

    /// @brief 读出探针留下的痕迹；父侧把它附在失败消息里
    std::string readConsoleProbeTrace()
    {
        std::ifstream      stream(consoleProbeMarkerPath("trace"));
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

    /**
     * @brief 编排线程的收尾守卫：用例从任何出口离开都先把那个线程收干
     * @details `ASSERT_*` 失败是从用例体里直接 return，此时留着一个还能 join 的 `std::thread` 走出作用域
     *          就是 `std::terminate`（实测退出码 3）——「判据红」会变成「整个测试进程 abort」，现场与原因
     *          都读不到。本守卫在早退路上兜一次显式叫停 + join，正常路径上那次 requestStop() 是幂等的。
     */
    class SupervisorThreadCleanup
    {
    public:
        SupervisorThreadCleanup(WorkerSupervisor &supervisor, std::thread &thread) noexcept : m_supervisor(supervisor), m_thread(thread)
        {
        }

        SupervisorThreadCleanup(const SupervisorThreadCleanup &)            = delete;
        SupervisorThreadCleanup &operator=(const SupervisorThreadCleanup &) = delete;

        /// 叫停并 join：run() 已返回时这一次即刻完成
        ~SupervisorThreadCleanup()
        {
            m_supervisor.requestStop();
            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }

    private:
        WorkerSupervisor &m_supervisor; ///< 被收尾的编排器
        std::thread      &m_thread;     ///< 跑 run() 的那个线程
    };

    /**
     * @brief 内层探针：控制台停机事件落到 master 上，走的是「逐个送走 worker」而不是「当场暴死」
     * @details 这是 Windows 上多进程停机的真实入口：宿主按 Ctrl+C 时，控制台事件只送到 master
     *          （每个 worker 有自己的进程组，CTRL_C 对它们无效，只能由编排者按组发 CTRL_BREAK）。
     *          没装处理函数时默认处置会直接把 master 打死，而每个 worker 都挂在带
     *          `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的作业里，作业句柄随之关闭——worker 是被硬杀的，
     *          在途请求不收尾，而启动日志正承诺着「Ctrl+C 会让 worker 各自体面退出」。
     *          本条只有带着控制台才有前提：单独跑（全量清单会选到）按 SKIP 处理，前提由父侧构造。
     * @note 这里不设「只记不发」的差分处理器。上一版装了这样一枚并假定它先跑，实测它一个字都没记到，
     *       而编排却确确实实停下来了——链条里谁先跑、以及「有处理器返回 TRUE 之后还剩不剩下后面的」
     *       都没实测过，拿未证的东西当判据就会把一次真实通过读成投递失败。两种红靠痕迹文件分：
     *       ① 停在 `event-posted` 而用例走满 20 秒（记 `run-timeout`）⇒ 事件没送到，或送到了没人转成停止请求；
     *       ② 停在 `event-posted` 而子进程当场没了、退出码 0xC000013A ⇒ 默认处置把 master 打死了，
     *       也就是编排器那枚处理器没装上（把产品侧改动撤掉时应看到的正是这一类）。
     */
    TEST(WorkerSupervisor, ConsoleCtrlCStopsTheOrchestrationInsteadOfTheProcess)
    {
        if (!Platform::ProcessInfo::environmentVariable(kConsoleProbeEnvironmentVariable).has_value())
        {
            GTEST_SKIP() << "本用例只在由 WorkerSupervisor.ConsoleCtrlCStopsWorkersViaConsoleHarness 以 CREATE_NEW_CONSOLE 启起来时才有前提";
        }
        ASSERT_NE(::GetConsoleWindow(), nullptr) << "探针本该带着控制台起来，没有就测不到这条路";
        appendConsoleProbeTrace("has-console");

        const Platform::Socket::Initialization network;
        const int                              listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(listener, 0) << "造不出监听套接字，错误码 " << WSAGetLastError();
        appendConsoleProbeTrace("listener-created");
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0) << "绑定失败，错误码 " << WSAGetLastError();
        ASSERT_EQ(::listen(listener, 16), 0) << "进入监听失败，错误码 " << WSAGetLastError();

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath  = ASYN_HANDOFF_WORKER_TOOL;
        configuration.workerCount     = 2;
        configuration.handoff         = WorkerSupervisor::Handoff{listener, std::chrono::seconds{10}};
        configuration.pollInterval    = std::chrono::milliseconds{20};
        configuration.shutdownTimeout = std::chrono::seconds{5};
        WorkerSupervisor supervisor(configuration);

        std::atomic<bool> isRunFinished{false};
        std::atomic<bool> isStoppedAsRequested{false};
        std::thread       supervisorThread(
                [&supervisor, &isRunFinished, &isStoppedAsRequested]
                {
                    isStoppedAsRequested.store(supervisor.run(), std::memory_order_release);
                    isRunFinished.store(true, std::memory_order_release);
                });
        // 线程一存在就把它交出去：下面任何一条 ASSERT 早退都不能留着一个还能 join 的线程
        const SupervisorThreadCleanup cleanup(supervisor, supervisorThread);

        const bool isPoolUp = waitForCondition([&supervisor] { return supervisor.runningWorkerCount() == 2U; }, kWaitTimeout);
        writeConsoleProbeMarker("pool-up");
        appendConsoleProbeTrace(isPoolUp ? "workers-up" : "workers-missing");

        // 按**自己这个进程组**投 CTRL_BREAK：这正是编排者送走 worker 用的那枚事件，而 master 侧的处理器
        // 对 Ctrl+C 与 CTRL_BREAK 走同一条路。用 0 当组号会把事件打到共享这个控制台的所有进程
        // （worker 的组也一起打到），那就不是「编排者收到停机请求」这一问了。
        // 组号必须是「本进程自己那个组」：只有以 CREATE_NEW_PROCESS_GROUP 起来的进程才是组首，
        // 而父侧正是这么起这份探针的（见下一条用例）——少了那个标志，这里的 pid 不是组号，
        // 事件投不出去（2026-10-03 实测：差分探针记不到、进程在 0.3 秒内以退出码 3 消失）
        EXPECT_TRUE(::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, ::GetCurrentProcessId()) != 0) << "事件没发出去，错误码 " << ::GetLastError();
        appendConsoleProbeTrace("event-posted");

        // 编排线程的收尾要送走两个 worker，给的余量比 kWaitTimeout 宽：这里量的是「事件转成了停止请求」
        // 这条链路通不通，不是毫秒级判据
        const bool isThreadFinished = waitForCondition([&isRunFinished] { return isRunFinished.load(std::memory_order_acquire); }, std::chrono::milliseconds{20000});
        appendConsoleProbeTrace(isThreadFinished ? "run-returned" : "run-timeout");
        static_cast<void>(::closesocket(listener));

        ASSERT_TRUE(isPoolUp) << "两个 worker 没在预算内起来，后面的事件判据就没有对象";
        ASSERT_TRUE(isThreadFinished) << "事件送到了进程，但编排没有返回：WorkerSupervisor 的控制台处理器没装上，或装上了却没把事件转成停止请求";
        EXPECT_TRUE(isStoppedAsRequested.load(std::memory_order_acquire)) << "按 CTRL_C 收口应当报「按请求停止」，而不是「整池被放弃」";
        writeConsoleProbeMarker("drained");
    }

    /**
     * @brief 钉住（Windows）：Ctrl+C 真的叫得停多进程编排——前提由本用例自己造
     * @details 宿主没有控制台时（stdout 被管道接走的测试进程）控制台事件无处投递，内层那条只能 SKIP。
     *          本进程不 AllocConsole：那会把三个标准句柄换成控制台缓冲区，同一进程里其余用例的输出就漂了。
     *          做法沿用 `Process.RequestTerminationStopsChildViaConsoleHarness` 的探针形状——把同一枚二进制
     *          以 CREATE_NEW_CONSOLE 再启一份，由那一份跑内层探针，父侧既等退出码也数标记文件。
     */
    TEST(WorkerSupervisor, ConsoleCtrlCStopsWorkersViaConsoleHarness)
    {
        wchar_t     executablePathText[32768] = {};
        const DWORD pathLength                = ::GetModuleFileNameW(nullptr, executablePathText, static_cast<DWORD>(std::size(executablePathText)));
        ASSERT_GT(pathLength, 0U) << "取不到自身路径，错误码 " << ::GetLastError();
        ASSERT_LT(pathLength, std::size(executablePathText)) << "自身路径被截断，本用例失去前提";

        static_cast<void>(::SetEnvironmentVariableA(kConsoleProbeEnvironmentVariable, "1"));
        removeConsoleProbeMarker("pool-up");
        removeConsoleProbeMarker("drained");
        removeConsoleProbeMarker("trace");

        std::wstring commandLine;
        commandLine += L'"';
        commandLine += executablePathText;
        commandLine += L"\" --gtest_filter=WorkerSupervisor.ConsoleCtrlCStopsTheOrchestrationInsteadOfTheProcess";

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION processInformation{};
        // CREATE_NEW_CONSOLE 给前提（探针要有自己的控制台才谈得上收停机事件），
        // CREATE_NEW_PROCESS_GROUP 给投递面：只有带后者的进程才是组首，内层才可以用自己的 pid
        // 当组号投 CTRL_BREAK。少了后者，GenerateConsoleCtrlEvent 会「调用成功而事件没人收到」
        const BOOL isCreated = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startupInfo,
                                                &processInformation) != 0;
        static_cast<void>(::SetEnvironmentVariableA(kConsoleProbeEnvironmentVariable, nullptr));
        ASSERT_TRUE(isCreated) << "带控制台的探针子进程没起来，错误码 " << ::GetLastError();

        const DWORD waitResult    = ::WaitForSingleObject(processInformation.hProcess, kConsoleProbeWaitMilliseconds);
        DWORD       childExitCode = 0;
        static_cast<void>(::GetExitCodeProcess(processInformation.hProcess, &childExitCode));
        ::CloseHandle(processInformation.hProcess);
        ::CloseHandle(processInformation.hThread);

        EXPECT_EQ(waitResult, WAIT_OBJECT_0) << "带控制台的探针子进程没在时限内退出";
        // 退出码之外还要问「探针跑没跑」：过滤器一条也没选中时 gtest 同样回 0，
        // 只看退出码会把「裁判没上场」读成「裁判判了通过」
        EXPECT_TRUE(consoleProbeMarkerExists("pool-up")) << "探针没起到 worker，本用例因此没有证据";
        EXPECT_TRUE(consoleProbeMarkerExists("drained")) << "探针没走完 CTRL_C 之后的收尾：要么 master 被默认处置打死了，要么收尾没完成。探针痕迹：" << readConsoleProbeTrace();
        EXPECT_EQ(static_cast<int>(childExitCode), 0) << "探针子进程非零退出：想看细节就在一个控制台窗口里跑 "
                                                         "TestCore.exe --gtest_filter=WorkerSupervisor.ConsoleCtrlCStopsTheOrchestration*；探针痕迹："
                                                      << readConsoleProbeTrace();
    }
#else

    /**
     * @brief POSIX 上给了移交配置就拒绝：那边的 worker 各自 bind，这份描述符没有使用方
     * @details 留着一个「填了却不生效」的档位，症状是调用方以为走了共享监听、实际什么都没变。
     *          拒绝比忽略好：本平台的分摊靠 SO_REUSEPORT，本类不需要也不该需要那份描述符。
     */
    TEST(WorkerSupervisor, RejectsHandoffConfigurationOnPosix)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 2;
        configuration.handoff        = WorkerSupervisor::Handoff{5};

        try
        {
            const WorkerSupervisor supervisor(configuration);
            FAIL() << "POSIX 上不该接受移交配置";
        } catch (const Base::LogicException &exception)
        {
            const std::string text{exception.what()};
            EXPECT_NE(text.find("SO_REUSEPORT"), std::string::npos) << "要说清本平台用的是哪种分摊：" << text;
        }
    }

    /**
     * @brief 移交配置在 POSIX 上留空时，编排照常构造（拒绝面不许顺手把合法配置一起挡下）
     */
    TEST(WorkerSupervisor, AcceptsEmptyHandoffConfigurationOnPosix)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath = "some-server";
        configuration.workerCount    = 2;

        EXPECT_NO_THROW(static_cast<void>(WorkerSupervisor(configuration)));
    }

    /**
     * @brief 起来就崩的 worker 会被补若干次，超过上限后放弃并让编排结束（不空转刷日志）
     */
    TEST(WorkerSupervisor, GivesUpAfterRepeatedCrashLoop)
    {
        const WorkerLaunchLog launchLog;
        WorkerSupervisor      supervisor(makeConfiguration(launchLog, 2, WorkerBehaviour::CrashImmediately));

        // 两个槽位各崩 crashLoopLimit(3) 次后放弃，编排随即返回
        const auto runStartTime = std::chrono::steady_clock::now();
        EXPECT_FALSE(supervisor.run()) << "整池 worker 都被放弃，这次编排不该报「按请求收口」";

        EXPECT_LT(std::chrono::steady_clock::now() - runStartTime, kWaitTimeout) << "崩溃循环之后编排没有自己结束";
        EXPECT_EQ(launchLog.launchCount(), 6U) << "两个槽位各应被补到上限为止（各 3 次）";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U);
    }

    /**
     * @brief 正常 worker 只起一次就稳定运行；收到停止请求后编排把它送走并返回
     * @details 本用例刻意让一个观察者线程在整个编排期间（含收尾与终止路径）连续读
     *          runningWorkerCount()：观察线程与编排线程碰的是同一批 worker，
     *          Linux TSan 下这是本类唯一覆盖那条竞态的入口
     */
    TEST(WorkerSupervisor, StartsWorkersOnceAndStopsThemOnRequest)
    {
        const WorkerLaunchLog launchLog;
        WorkerSupervisor      supervisor(makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated));

        // run() 阻塞，因此编排跑在另一个线程上；停止请求从本线程发起。返回值交给收尾断言
        std::atomic<bool> isOrchestrationSettled{false};
        std::thread       supervisorThread([&supervisor, &isOrchestrationSettled] { isOrchestrationSettled.store(supervisor.run(), std::memory_order_release); });

        // 观察者线程：只读计数，读到编排线程收口为止（不额外探测句柄，也不改任何状态）
        std::atomic<bool>        isObserving{true};
        std::atomic<std::size_t> maximumObservedCount{0};
        std::thread              observerThread(
                [&supervisor, &isObserving, &maximumObservedCount]
                {
                    while (isObserving.load(std::memory_order_acquire))
                    {
                        const std::size_t observed = supervisor.runningWorkerCount();
                        // 只单调往上报最大值：收尾时读到 0 也不该把已观察到的 2 冲掉
                        std::size_t previous = maximumObservedCount.load(std::memory_order_relaxed);
                        while (observed > previous && !maximumObservedCount.compare_exchange_weak(previous, observed, std::memory_order_relaxed))
                        {
                        }
                    }
                });

        ASSERT_TRUE(waitForCondition([&launchLog] { return launchLog.launchCount() >= 2; }, kWaitTimeout)) << "两个 worker 没有都起来";
        // 计数是编排线程每轮扫描末尾发布的快照（最长滞后一个 pollInterval），因此等它到位再断言
        EXPECT_TRUE(waitForCondition([&supervisor] { return supervisor.runningWorkerCount() >= 2U; }, kWaitTimeout)) << "两个 worker 都起来之后，快照里的在运行个数应当到位";

        // 稳定运行之后再等一小会儿：不该出现「明明活着却被重复补位」的情况
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_EQ(launchLog.launchCount(), 2U) << "worker 还活着却被重复启动";

        supervisor.requestStop();
        supervisorThread.join();
        isObserving.store(false, std::memory_order_release);
        observerThread.join();

        EXPECT_EQ(launchLog.launchCount(), 2U) << "收尾不该再起新 worker";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U) << "收尾之后不该还有 worker 在跑";
        EXPECT_LE(maximumObservedCount.load(std::memory_order_acquire), 2U) << "观察线程看到的个数越过了 worker 总数";
        EXPECT_TRUE(isOrchestrationSettled.load(std::memory_order_acquire)) << "worker 正常起来又被按请求停掉，这次编排应当报「收口成功」";
    }

    /**
     * @brief 首轮起 worker 不尝补位退避：N 个槽位的冷启动不该串成 N × restartBackoff
     * @details 退避是给「起来就崩」准备的。判定式若是「这个槽位现在没进程」，那么首轮每个槽位
     *          也要先睡满一整个退避才起，默认 500 毫秒 × N 就让进程池空转几秒才开始接活。
     *          把退避调到远大于三个冷启动应有的时长，再用启动记录的到位时间判定：只要有一次
     *          退避被串进首轮就会越线。本用例位于本文件的 POSIX 区内（Windows 构造即拒绝多进程）
     */
    TEST(WorkerSupervisor, FirstLaunchesDoNotWaitOutTheRestartBackoff)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 3, WorkerBehaviour::SleepUntilTerminated);
        configuration.restartBackoff                  = std::chrono::milliseconds{1500};

        WorkerSupervisor supervisor(configuration);
        const auto       startedAt = std::chrono::steady_clock::now();
        std::thread      supervisorThread([&supervisor] { static_cast<void>(supervisor.run()); });

        const bool areAllThreeUp = waitForCondition([&launchLog] { return launchLog.launchCount() >= 3; }, std::chrono::milliseconds{1000});
        const auto elapsed       = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        supervisor.requestStop();
        supervisorThread.join();

        EXPECT_TRUE(areAllThreeUp) << "三个 worker 没在 1000 毫秒内都起来（实测等了 " << elapsed.count() << " 毫秒）：冷启动被补位退避串成了 N × restartBackoff";
    }

    /**
     * @brief 收尾有期限：worker 不理会 SIGTERM 时，编排到点强杀并按时返回
     */
    TEST(WorkerSupervisor, ShutdownTimeoutForcesTermination)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepIgnoringTerminate);
        configuration.shutdownTimeout                 = std::chrono::milliseconds{300};

        WorkerSupervisor supervisor(configuration);
        // 强杀兜住之后编排应当报「收口成功」：worker 是被本层送走的，不是整池起不来
        std::atomic<bool> isOrchestrationSettled{false};
        std::thread       supervisorThread([&supervisor, &isOrchestrationSettled] { isOrchestrationSettled.store(supervisor.run(), std::memory_order_release); });

        ASSERT_TRUE(waitForCondition([&launchLog] { return launchLog.launchCount() >= 2; }, kWaitTimeout)) << "两个 worker 没有都起来";

        const auto stopStartTime = std::chrono::steady_clock::now();
        supervisor.requestStop();
        supervisorThread.join();
        const auto stopElapsedTime = std::chrono::steady_clock::now() - stopStartTime;

        EXPECT_GE(stopElapsedTime, configuration.shutdownTimeout) << "worker 不理会 SIGTERM，收尾却早于期限返回了";
        EXPECT_LT(stopElapsedTime, kWaitTimeout) << "收尾超出了期限还在等：强杀那一步没有兜住";
        EXPECT_TRUE(isOrchestrationSettled.load(std::memory_order_acquire)) << "worker 是被本层强杀送走的，不该报成「整池起不来」";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U);
    }

    /**
     * @brief 就绪回调只在整池到位那一刻发生一次，且报的是「两个都在跑」这个现场
     * @details 这一格是给服务管理器的 `READY=1` 用的：报早了那一侧会把「worker 还没起来」读成启动成功，
     *          重复报则让第二条起全是噪声。把「只在凑齐时一次」做成可判的形态，是回调里当场记下快照——
     *          条件写坏成「每轮都调」或「有槽位就调」时，记到的快照就不是 2。
     */
    TEST(WorkerSupervisor, ReadinessCallbackFiresOnceWhenTheWholePoolIsUp)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated);

        std::atomic<std::int32_t> readyCallCount{0};
        std::atomic<std::size_t>  countAtReadyCall{0};
        const WorkerSupervisor   *supervisorPointer{nullptr}; // 回调在编排线程上跑，读的是那份原子快照
        configuration.onAllWorkersRunning = [&]
        {
            readyCallCount.fetch_add(1, std::memory_order_relaxed);
            // 快照由编排线程在同一轮的上一句发布，因此这一刻它一定已是本轮的数
            countAtReadyCall.store(supervisorPointer == nullptr ? 0U : supervisorPointer->runningWorkerCount(), std::memory_order_relaxed);
        };

        WorkerSupervisor supervisor(configuration);
        supervisorPointer = &supervisor;
        std::thread supervisorThread([&supervisor] { static_cast<void>(supervisor.run()); });

        ASSERT_TRUE(waitForCondition([&readyCallCount] { return readyCallCount.load(std::memory_order_acquire) >= 1; }, kWaitTimeout)) << "整池起来后没报就绪";
        EXPECT_EQ(countAtReadyCall.load(std::memory_order_acquire), 2U) << "报就绪时快照里不是整池，说明这条回调发早了";

        // 池子稳定后再等两拍：重复触发会在这里露出来（补位、崩溃都不该让它再报第二次）
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_EQ(readyCallCount.load(std::memory_order_acquire), 1) << "就绪是一次性陈述，凑齐之后每轮都报就成了重复上报";

        supervisor.requestStop();
        supervisorThread.join();
    }

    /**
     * @brief 收尾回调落在 worker 还没被通知退出之前
     * @details 它是 `STOPPING=1` 的位置：监督者从那一秒起才开始计停机超时，等整池送走再报就成了事后说明。
     *          判据是回调里记下的快照——挪到 `stopAllWorkers()` 之后就归零，这条会红。
     */
    TEST(WorkerSupervisor, StopCallbackFiresWhileWorkersAreStillRunning)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated);

        std::atomic<std::int32_t> stopCallCount{0};
        std::atomic<std::size_t>  countAtStopCall{0};
        const WorkerSupervisor   *supervisorPointer{nullptr};
        configuration.onStopRequested = [&]
        {
            stopCallCount.fetch_add(1, std::memory_order_relaxed);
            countAtStopCall.store(supervisorPointer == nullptr ? 0U : supervisorPointer->runningWorkerCount(), std::memory_order_relaxed);
        };

        WorkerSupervisor supervisor(configuration);
        supervisorPointer = &supervisor;
        std::thread supervisorThread([&supervisor] { static_cast<void>(supervisor.run()); });

        ASSERT_TRUE(waitForCondition([&launchLog] { return launchLog.launchCount() >= 2; }, kWaitTimeout)) << "两个 worker 没有都起来";
        ASSERT_TRUE(waitForCondition([&supervisor] { return supervisor.runningWorkerCount() >= 2U; }, kWaitTimeout)) << "快照里还没整池，收尾现场就没前提";

        supervisor.requestStop();
        supervisorThread.join();

        EXPECT_EQ(stopCallCount.load(std::memory_order_acquire), 1) << "收尾只该报一次";
        EXPECT_EQ(countAtStopCall.load(std::memory_order_acquire), 2U) << "报收尾时 worker 已经不在了：这一刻晚于「我在收尾」该有的位置";
        EXPECT_EQ(supervisor.runningWorkerCount(), 0U) << "run() 返回后整池应当已送走";
    }

    /**
     * @brief 有槽位根本起不来的时候，就绪一次都不报
     * @details 这一格钉的是条件的另一半：报「整池就绪」而其中一格永远补不起来，等于让监督者按一条
     *          假陈述放行后续单元。用「可执行文件不存在」造这个现场——进程起不来的判据在 `startWorker`
     *          里就落了，不会像「起来就崩」那样与父线程的回收窗口抢先后，因此这条判据是确定的。
     */
    TEST(WorkerSupervisor, ReadinessCallbackNeverFiresWhenAWorkerSlotCannotStart)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated);
        configuration.executablePath                  = "/nonexistent-asyn-worker-ka13"; // 起不来的那一格：连进程都没有
        configuration.crashLoopLimit                  = 1;

        std::atomic<std::int32_t> readyCallCount{0};
        configuration.onAllWorkersRunning = [&readyCallCount] { readyCallCount.fetch_add(1, std::memory_order_relaxed); };

        WorkerSupervisor supervisor(configuration);
        const bool       isOrderedShutdown = supervisor.run();

        EXPECT_FALSE(isOrderedShutdown) << "全部槽位都起不来，这次编排不该报成按请求收口";
        EXPECT_EQ(readyCallCount.load(std::memory_order_acquire), 0) << "有一格永远补不起来，却报了整池就绪";
    }

#endif

    /**
     * @brief 钉住：编排器的崩溃计数与「放弃补位」槽位数进导出，析构即注销
     * @details 刻意不导出 `runningWorkerCount()`：那个读口会顺手回收子进程并改写退出码，
     *          从抓取线程调它就是与编排线程抢回收。本用例只验登记与生命周期，不起真进程
     */
    TEST(WorkerSupervisorMetrics, RegistersCrashReadoutsAndReleasesThemOnDestruction)
    {
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath  = "unused-by-this-case";
        configuration.workerCount     = 2;
        configuration.pollInterval    = std::chrono::milliseconds{20};
        configuration.shutdownTimeout = std::chrono::seconds{5};
#if ASYN_PLATFORM_WIN32
        const Platform::Socket::Initialization network;
        const int                              listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(listener, 0) << "造不出套接字，本用例的移交档位就没法填";
        configuration.handoff = WorkerSupervisor::Handoff{listener, std::chrono::seconds{10}};
#endif

        ASSERT_FALSE(hasRegistrySample("asyn_worker_crashes_total"));
        {
            WorkerSupervisor supervisor(configuration);
            EXPECT_TRUE(hasRegistrySample("asyn_worker_crashes_total"));
            EXPECT_TRUE(hasRegistrySample("asyn_worker_slots_given_up"));
            EXPECT_EQ(registryValue("asyn_worker_crashes_total"), 0U);
            EXPECT_EQ(registryValue("asyn_worker_slots_given_up"), 0U);
            const auto givenUp = AsynGyanis::TestSupport::findRegistrySample("asyn_worker_slots_given_up");
            ASSERT_TRUE(givenUp.has_value());
            EXPECT_EQ(givenUp->kind, Core::ProcessMetricKind::Counter) << "只增不减的累计数报成 gauge：按 rate() 取增量的人拿不到任何东西，而这条恰恰是要看增量的那个";
        }
        EXPECT_FALSE(hasRegistrySample("asyn_worker_crashes_total")) << "编排器析构后这条读数还挂在导出里";
#if ASYN_PLATFORM_WIN32
        ::closesocket(listener);
#endif
    }

} // namespace AsynGyanis::Core
