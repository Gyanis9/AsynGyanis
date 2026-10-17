// WorkerSupervisor 单元测试：配置校验、补齐 worker、崩溃退避、收尾送走，以及整池到位/开始收口那两条观察者回调

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
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !ASYN_PLATFORM_WIN32
// 看门狗汇聚用例的假收端要的几样（POSIX 专有，Windows 上那几条用例整段不编译）
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

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

            /// 读出全部非空行（新用例按行里的时间戳与槽位自行解析）
            [[nodiscard]] std::vector<std::string> lines() const
            {
                std::vector<std::string> result;
                std::ifstream            file(m_path, std::ios::in);
                std::string              line;
                while (std::getline(file, line))
                {
                    if (!line.empty())
                    {
                        result.push_back(line);
                    }
                }
                return result;
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

    /**
     * @brief 退避的指数增长、封顶与抖动边界（纯函数钉住，不依赖调度时序；两个平台都跑）
     */
    TEST(WorkerSupervisor, RestartBackoffGrowsExponentiallyWithCapAndJitter)
    {
        using namespace std::chrono_literals;
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 0U, 0.5), 100ms) << "首次启动不该被指数放大";
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 1U, 0.5), 100ms);
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 2U, 0.5), 200ms);
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 3U, 0.5), 400ms);
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 4U, 0.5), 800ms);

        // 封顶：指数涨到 30 秒就停，不再无限放大
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(1000ms, 20U, 0.5), kMaximumWorkerRestartBackoff);
        // 配置的基准值本身超过封顶时，首轮等待不被反过来压低
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(60000ms, 1U, 0.5), 60000ms);

        // 抖动边界：±20%，0.5 恰为基准值
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 1U, 0.0), 80ms);
        EXPECT_EQ(WorkerSupervisor::restartBackoffFor(100ms, 1U, 0.999999), 120ms);
        EXPECT_LE(WorkerSupervisor::restartBackoffFor(1000ms, 20U, 0.999999), kMaximumWorkerRestartBackoff) << "抖动把上限顶破了";
        EXPECT_LT(WorkerSupervisor::restartBackoffFor(100ms, 1U, 0.9), WorkerSupervisor::restartBackoffFor(100ms, 2U, 0.1)) << "抖动不该吃掉相邻两档的增长";
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
     * @details 这条路只能这样钉：WSADuplicateSocketW 按目标进程号发凭证，同一个进程里试不出
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

        // 按自己这个进程组投 CTRL_BREAK：这正是编排者送走 worker 用的那枚事件，而 master 侧的处理器
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

#if ASYN_PLATFORM_WIN32
    TEST(WorkerSupervisor, GrowsRestartGapsAcrossRepeatedCrashes)
    {
        GTEST_SKIP() << "假 worker 是 POSIX shell；Windows 侧的跨进程编排由移交夹具覆盖，这条时序判据不在本平台跑";
    }
#else
    /**
     * @brief 真起一个必崩 worker：补位间隔要逐次变长（不是恒定 restartBackoff）
     * @details 槽位 0 必崩、槽位 1 常驻，这样只有槽位 0 在走退避（两个槽位都崩时，补位循环里
     *          彼此串行的睡眠会把「间隔」量成别人的等待，恒定退避也能伪装成逐次变长）。
     *          脚本自己把启动时刻（纳秒）与槽位号写进文件；按槽位分组后量相邻两次启动的间隔。
     *          基准 500 毫秒、抖动 ±20% 时第一档落在 [400,600]、第二档 [800,1200]，因此「第二段
     *          至少比第一段长 15%」在不赌具体数值的前提下成立
     */
    TEST(WorkerSupervisor, GrowsRestartGapsAcrossRepeatedCrashes)
    {
        const WorkerLaunchLog launchLog;
        const std::string     script = "echo \"$(date +%s%N) $1\" >> \"" + launchLog.path().string() + "\"; if [ \"$1\" = \"0\" ]; then exit 1; else exec sleep 5; fi";

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath      = "/bin/sh";
        configuration.workerArguments     = {"-c", script};
        configuration.workerIndexArgument = "--slot";
        configuration.workerCount         = 2;
        configuration.pollInterval        = std::chrono::milliseconds{20};
        configuration.restartBackoff      = std::chrono::milliseconds{500};
        configuration.shutdownTimeout     = std::chrono::milliseconds{2000};
        configuration.crashLoopWindow     = std::chrono::milliseconds{5000};
        configuration.crashLoopLimit      = 3;

        WorkerSupervisor  supervisor(configuration);
        std::atomic<bool> isSettled{false};
        std::thread       supervisorThread([&supervisor, &isSettled] { isSettled.store(supervisor.run(), std::memory_order_release); });

        // 槽位 0 起 3 次（首次 + 两次补位）加上槽位 1 的首次启动，共 4 行
        const bool hasEnoughLaunches = waitForCondition([&launchLog] { return launchLog.launchCount() >= 4U; }, kWaitTimeout);
        supervisor.requestStop();
        supervisorThread.join();
        ASSERT_TRUE(hasEnoughLaunches) << "槽位 0 没有补到上限（启动记录 " << launchLog.launchCount() << " 行）";
        EXPECT_TRUE(isSettled.load(std::memory_order_acquire)) << "请求停止之后编排没有正常收口";

        std::vector<long long> slotZeroStarts;
        for (const std::string &line: launchLog.lines())
        {
            const std::size_t separator = line.rfind(' ');
            ASSERT_NE(separator, std::string::npos) << line;
            if (line.substr(separator + 1U) == "0")
            {
                slotZeroStarts.push_back(std::stoll(line.substr(0, separator)));
            }
        }
        ASSERT_EQ(slotZeroStarts.size(), 3U) << "槽位 0 应当被起 3 次（首次 + 两次补位）";
        for (std::size_t index = 1; index < slotZeroStarts.size(); ++index)
        {
            ASSERT_GT(slotZeroStarts[index], slotZeroStarts[index - 1U]) << "启动时刻没有单调递增";
        }

        const long long firstGap  = slotZeroStarts[1] - slotZeroStarts[0];
        const long long secondGap = slotZeroStarts[2] - slotZeroStarts[1];
        EXPECT_GE(firstGap, 300'000'000LL) << "第一次补位没有按基准值退避：" << firstGap / 1'000'000 << " 毫秒";
        EXPECT_GT(secondGap, firstGap * 115 / 100) << "补位间隔没有逐次变长（恒定退避？）：" << firstGap / 1'000'000 << " → " << secondGap / 1'000'000 << " 毫秒";
    }

    /**
     * @brief 放弃之后的自愈口径：其他槽位还活着时，被放弃的槽位静默够久会重新纳入一次
     * @details 槽位 0 必崩、槽位 1 常驻；crashLoopLimit=1 让槽位 0 一崩就被放弃，窗口 100 毫秒
     *          因此静默 1 秒后重新纳入。判据落在「启动记录出现第 3 行」上：前两行是冷启动，
     *          第三行只能是槽位 0 的自愈补位
     */
    TEST(WorkerSupervisor, ReAdmitsAGivenUpSlotAfterItsQuietWindow)
    {
        const WorkerLaunchLog launchLog;
        const std::string     script = "echo worker >> \"" + launchLog.path().string() + "\"; if [ \"$1\" = \"0\" ]; then exit 1; else exec sleep 5; fi";

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath      = "/bin/sh";
        configuration.workerArguments     = {"-c", script};
        configuration.workerIndexArgument = "--slot";
        configuration.workerCount         = 2;
        configuration.pollInterval        = std::chrono::milliseconds{20};
        configuration.restartBackoff      = std::chrono::milliseconds{50};
        configuration.shutdownTimeout     = std::chrono::milliseconds{2000};
        configuration.crashLoopWindow     = std::chrono::milliseconds{100};
        configuration.crashLoopLimit      = 1;

        WorkerSupervisor  supervisor(configuration);
        std::atomic<bool> isSettled{false};
        std::thread       supervisorThread([&supervisor, &isSettled] { isSettled.store(supervisor.run(), std::memory_order_release); });

        const bool isReAdmitted = waitForCondition([&launchLog] { return launchLog.launchCount() >= 3U; }, kWaitTimeout);
        supervisor.requestStop();
        supervisorThread.join();

        EXPECT_TRUE(isReAdmitted) << "被放弃的槽位没有在静默窗口之后重新纳入（启动记录停在 " << launchLog.launchCount() << " 行）";
        EXPECT_TRUE(isSettled.load(std::memory_order_acquire)) << "请求停止之后编排没有正常收口";
    }
#endif

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
     * @brief 一个 worker 都起不来时，编排报「整池起不来」而不是按请求收口
     * @details 这条只判返回值与闩锁。就绪该不该报不押在这里：`posix_spawn` 对不存在的可执行文件在有的
     *          机器上先报成功、子进程随后以 127 退出，那一轮「整池看起来齐了」是真切发生过的（CI 上就
     *          这么红过一次），把条件本身钉成可确定复跑的判据交给下面那条纯函数用例。
     */
    TEST(WorkerSupervisor, GivesUpWhenNoWorkerCanBeStarted)
    {
        const WorkerLaunchLog           launchLog;
        WorkerSupervisor::Configuration configuration = makeConfiguration(launchLog, 2, WorkerBehaviour::SleepUntilTerminated);
        configuration.executablePath                  = "/nonexistent-asyn-worker-ka13"; // 起不来的那一格：连进程都没有
        configuration.crashLoopLimit                  = 1;

        std::atomic<std::int32_t> readyCallCount{0};
        configuration.onAllWorkersRunning = [&readyCallCount] { readyCallCount.fetch_add(1, std::memory_order_relaxed); };

        WorkerSupervisor supervisor(configuration);
        EXPECT_FALSE(supervisor.run()) << "全部槽位都起不来，这次编排不该报成按请求收口";
        // 闩锁在这条上也要成立：无论池子看起来齐过几次，同一次编排里就绪最多报一份
        EXPECT_LE(readyCallCount.load(std::memory_order_acquire), 1) << "同一次编排里就绪报了多份";
    }

    /**
     * @brief 就绪条件本身：每个槽位都要有一个活着的 worker，空池永远不算齐
     * @details 这一格不等任何进程起来，因此可以确定复跑；上面那两条端到端的形状只判自己能稳的部分。
     */
    TEST(WorkerSupervisor, PoolCompletenessRequiresOneLiveWorkerPerSlot)
    {
        EXPECT_TRUE(WorkerSupervisor::isPoolComplete(2, 2));
        EXPECT_TRUE(WorkerSupervisor::isPoolComplete(1, 1));
        EXPECT_FALSE(WorkerSupervisor::isPoolComplete(1, 2)) << "还缺一格就把就绪报出去，监督者会按整池在服务放行后续单元";
        EXPECT_FALSE(WorkerSupervisor::isPoolComplete(0, 2));
        EXPECT_FALSE(WorkerSupervisor::isPoolComplete(3, 2)) << "个数多于槽位说明这一轮数错了，不该当成齐";
        EXPECT_FALSE(WorkerSupervisor::isPoolComplete(0, 0)) << "空池没有「都到位」这回事";
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

    namespace
    {
        /// 看门狗汇聚用例要摆的三枚环境变量（与 Platform 实现按 sd_watchdog_enabled(3) 的约定同名）
        constexpr const char *kWatchdogWindowVariable   = "WATCHDOG_USEC";
        constexpr const char *kWatchdogPidVariable      = "WATCHDOG_PID";
        constexpr const char *kNotifySocketVariableName = "NOTIFY_SOCKET";

        /// 1 秒窗口：折出的节拍 500 毫秒——正反两条判据都在秒级内可判
        constexpr const char *kWatchdogWindowText = "1000000";

        void setWatchdogVariable(const char *name, const std::optional<std::string> &value)
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
        std::string makeWatchdogSocketName()
        {
            static std::atomic<std::uint64_t> sequence{0};
            return std::format("asyn-worker-watchdog-{}-{}", static_cast<std::uint64_t>(::getpid()), sequence.fetch_add(1));
        }

        /**
         * @brief 建一个「假的服务管理器」：按抽象命名空间 bind 一个数据报套接字
         * @details 地址是独立写的一份，不复用被测代码里的发送端——两边共用同一段时一起错也抓不到
         * @param name 不带前导 '@' 的名字
         * @return int 已 bind 的描述符；失败返回 -1
         */
        int bindWatchdogReceiver(const std::string &name)
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
         * @brief 有界收一条数据报：等到点为止没有就交回空串（用例自己判红，不把作业挂在永久阻塞上）
         * @param descriptor 收端描述符
         * @param budget 等待预算
         * @return std::string 收到的字节；没等到时为空
         */
        std::string receiveOneWatchdogDatagram(const int descriptor, const std::chrono::milliseconds budget)
        {
            const auto deadline = std::chrono::steady_clock::now() + budget;
            while (std::chrono::steady_clock::now() < deadline)
            {
                pollfd pollDescriptor{};
                pollDescriptor.fd     = descriptor;
                pollDescriptor.events = POLLIN;
                if (::poll(&pollDescriptor, 1, 50) > 0 && (pollDescriptor.revents & POLLIN) != 0)
                {
                    std::array<char, 256> buffer{};
                    const auto            received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
                    if (received > 0)
                    {
                        return std::string(buffer.data(), static_cast<std::size_t>(received));
                    }
                }
            }
            return {};
        }
#endif
    } // namespace

    /**
     * @brief 纯判据：每个未放弃的槽位都新鲜才算齐；空池与长度不配都不算
     */
    TEST(WorkerSupervisor, AllHeartbeatsFreshJudgesEveryNonGivenUpSlot)
    {
        // 具名局部量而不是内联花括号：gtest 宏会把花括号列表里的逗号当参数分隔符拆开
        const std::vector<bool> emptyTable{};
        const std::vector<bool> noGivenUp{false, false};
        const std::vector<bool> secondGivenUp{false, true};
        const std::vector<bool> allFresh{true, true};
        const std::vector<bool> secondStale{true, false};
        const std::vector<bool> allStale{false, false};

        EXPECT_FALSE(WorkerSupervisor::allHeartbeatsFresh(emptyTable, emptyTable)) << "空池没有「都新鲜」可言";
        EXPECT_FALSE(WorkerSupervisor::allHeartbeatsFresh(secondStale, emptyTable)) << "两张表长度不配是装配坏了，不许当真";
        EXPECT_FALSE(WorkerSupervisor::allHeartbeatsFresh(emptyTable, noGivenUp)) << "同上（另一侧空）";
        EXPECT_TRUE(WorkerSupervisor::allHeartbeatsFresh(allFresh, noGivenUp));
        EXPECT_FALSE(WorkerSupervisor::allHeartbeatsFresh(secondStale, noGivenUp)) << "有一个陈旧就不齐";
        EXPECT_TRUE(WorkerSupervisor::allHeartbeatsFresh(secondStale, secondGivenUp)) << "已放弃的槽位不参与：它那一路由「整池放弃」处理";
        EXPECT_FALSE(WorkerSupervisor::allHeartbeatsFresh(allStale, noGivenUp));
    }

    /**
     * @brief 纯判据：喂狗资格三件——启用过、就绪已报、未收停机请求
     * @details 就绪前那段窗口在端到端里短到观察不到（池比一次轮询还齐得快），这一格的确定性只在
     *          这里钉得住；就绪门防「池缺人（有槽位被放弃、池永远不齐）还喂健康」，停机门防
     *          「收尾流程里假装服务健康」
     */
    TEST(WorkerSupervisor, IsEligibleToFeedShutsOffBeforeReadinessAndAfterStop)
    {
        EXPECT_FALSE(WorkerSupervisor::isEligibleToFeed(false, true, false)) << "没启用不谈资格";
        EXPECT_FALSE(WorkerSupervisor::isEligibleToFeed(true, false, false)) << "就绪没报不给喂（池缺人时让监督者按超时接手）";
        EXPECT_FALSE(WorkerSupervisor::isEligibleToFeed(true, true, true)) << "停机请求之后不喂";
        EXPECT_FALSE(WorkerSupervisor::isEligibleToFeed(true, false, true));
        EXPECT_TRUE(WorkerSupervisor::isEligibleToFeed(true, true, false));
    }

    /**
     * @brief 钉住：心跳新鲜度按「距上一拍多久」判，晚到不到半拍的拍子不许被判陈旧
     * @details master 的喂狗检查与 worker 的出拍共用同一个周期（都取窗口折半），谁先谁后取决于 fork 与
     *          挂节拍的毫秒级相位。新鲜度若只记「这一轮读到新字节没有」，检查抢到拍子之前完成的那一轮就把它
     *          清掉，而下一轮的相位关系一模一样，于是永远喂不出去、单元被自己的看门狗循环重启。按经过的
     *          时间判之后，晚到不足半拍的拍子仍作数，真停摆（连着两拍没到）照样停喂。
     */
    TEST(WorkerSupervisor, HeartbeatStaysFreshUntilAWholeBeatIsMissed)
    {
        using namespace std::chrono;
        constexpr steady_clock::time_point nowPoint{}; ///< 判定时刻取零点，偏移全用相对量表达
        constexpr milliseconds             pingInterval{1000};

        EXPECT_TRUE(WorkerSupervisor::isHeartbeatFresh(nowPoint - milliseconds{10}, nowPoint, nowPoint + milliseconds{1}, pingInterval)) << "上一拍就在眼前，必须新鲜";
        EXPECT_TRUE(WorkerSupervisor::isHeartbeatFresh(nowPoint - milliseconds{1100}, nowPoint, nowPoint + milliseconds{1}, pingInterval))
                << "晚到不到半拍的这一拍被判陈旧，就是「永远喂不出去」那条路";
        EXPECT_FALSE(WorkerSupervisor::isHeartbeatFresh(nowPoint - milliseconds{2600}, nowPoint, nowPoint + milliseconds{1}, pingInterval)) << "连着两拍没到还喂，等于对停摆报健康";

        EXPECT_TRUE(WorkerSupervisor::isHeartbeatFresh(std::nullopt, nowPoint, nowPoint + milliseconds{500}, pingInterval)) << "起拍要时间：宽限内没读到拍不该算停摆";
        EXPECT_FALSE(WorkerSupervisor::isHeartbeatFresh(std::nullopt, nowPoint, nowPoint - milliseconds{1}, pingInterval)) << "宽限过了还一拍没到，就是这条 worker 没在推进";
    }

    /**
     * @brief 看门狗汇聚用例的夹具：进出都把三枚环境变量恢复原样
     * @details 这组用例会动它们；不复原的话后跑的用例读到的是上一个用例留下的窗口，
     *          表现为「突然开始喂狗」这种随执行顺序而变的形态
     */
    class WorkerSupervisorWatchdogTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_savedWindow = Platform::ProcessInfo::environmentVariable(kWatchdogWindowVariable);
            m_savedPid    = Platform::ProcessInfo::environmentVariable(kWatchdogPidVariable);
            m_savedNotify = Platform::ProcessInfo::environmentVariable(kNotifySocketVariableName);
            setWatchdogVariable(kWatchdogWindowVariable, std::nullopt);
            setWatchdogVariable(kWatchdogPidVariable, std::nullopt);
            setWatchdogVariable(kNotifySocketVariableName, std::nullopt);
        }

        void TearDown() override
        {
            setWatchdogVariable(kWatchdogWindowVariable, m_savedWindow);
            setWatchdogVariable(kWatchdogPidVariable, m_savedPid);
            setWatchdogVariable(kNotifySocketVariableName, m_savedNotify);
        }

        std::optional<std::string> m_savedWindow{};
        std::optional<std::string> m_savedPid{};
        std::optional<std::string> m_savedNotify{};
    };

    /**
     * @brief 汇聚的整链：真 worker 工具拍心跳经管道到 master，假服务管理器收到 WATCHDOG=1
     * @details 走的是完整跨进程路径：编排器建管道 → 参数传 fd → fork 继承 → worker 侧起拍 →
     *          master 收齐两条才喂。同进程内自造管道测不到「参数传递 + 继承」那一段
     */
    TEST_F(WorkerSupervisorWatchdogTest, FeedsWatchdogWhenEveryWorkerHeartbeatIsFresh)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "本平台没有 systemd 看门狗这条通道";
#else
        const std::string socketName = makeWatchdogSocketName();
        const int         listener   = bindWatchdogReceiver(socketName);
        ASSERT_GE(listener, 0) << "假的服务管理器 bind 不上，这条用例没法判";

        setWatchdogVariable(kWatchdogWindowVariable, kWatchdogWindowText);
        setWatchdogVariable(kWatchdogPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setWatchdogVariable(kNotifySocketVariableName, std::string("@") + socketName);

        Platform::ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        WorkerSupervisor::Configuration configuration;
        configuration.executablePath       = ASYN_HEARTBEAT_WORKER_TOOL;
        configuration.workerCount          = 2;
        configuration.pollInterval         = std::chrono::milliseconds{20};
        configuration.restartBackoff       = std::chrono::milliseconds{50};
        configuration.shutdownTimeout      = std::chrono::milliseconds{2000};
        configuration.crashLoopWindow      = std::chrono::milliseconds{300};
        configuration.crashLoopLimit       = 3;
        configuration.watchdogNotification = &notification;

        WorkerSupervisor  supervisor(std::move(configuration));
        std::atomic<bool> isOrchestrationSettled{false};
        std::thread       supervisorThread([&supervisor, &isOrchestrationSettled] { isOrchestrationSettled.store(supervisor.run(), std::memory_order_release); });

        // 假服务管理器的收端：窗口 1 秒、节拍 500 毫秒，给 8 秒预算收第一条 WATCHDOG=1——
        // worker 起进程 + 起池 + 起拍本身要秒级时间，这里不赌调度只等事实
        const std::string firstPing = receiveOneWatchdogDatagram(listener, std::chrono::seconds{8});
        EXPECT_EQ(firstPing, "WATCHDOG=1") << "两条 worker 的心跳都新鲜时，master 该喂一条 WATCHDOG=1";

        supervisor.requestStop();
        supervisorThread.join();
        EXPECT_TRUE(isOrchestrationSettled.load(std::memory_order_acquire)) << "按请求收口应当报成功";

        static_cast<void>(::close(listener));
#endif
    }

    /**
     * @brief 反例：worker 从不拍心跳（不是本仓库的心跳工具）时，master 在起拍宽限之后不再喂
     * @details 「停喂」是这条通道的判据本体。就绪后头一个节拍之内可以放行（worker 起拍本身要时间，
     *          这条放行由 `isHeartbeatFresh` 的宽限分支兑现），但放行条数被喂狗门的算术钉住；
     *          宽限之后还收得到东西，就说明汇聚没在看心跳而是「时间到了就喂」
     */
    TEST_F(WorkerSupervisorWatchdogTest, StopsFeedingWhenWorkersNeverHeartbeat)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "本平台没有 systemd 看门狗这条通道";
#else
        const std::string socketName = makeWatchdogSocketName();
        const int         listener   = bindWatchdogReceiver(socketName);
        ASSERT_GE(listener, 0) << "假的服务管理器 bind 不上，这条用例没法判";

        setWatchdogVariable(kWatchdogWindowVariable, kWatchdogWindowText);
        setWatchdogVariable(kWatchdogPidVariable, std::to_string(static_cast<std::uint64_t>(::getpid())));
        setWatchdogVariable(kNotifySocketVariableName, std::string("@") + socketName);

        Platform::ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        // 假 worker：一条只睡着的 shell（末尾的参数会被 master 追加成 $0/$1，脚本不读它们）。
        // 它不会拍任何心跳——正是「worker 活着但心跳停摆」这一形态
        WorkerSupervisor::Configuration configuration;
        configuration.executablePath       = "/bin/sh";
        configuration.workerArguments      = {"-c", "exec sleep 5"};
        configuration.workerCount          = 2;
        configuration.pollInterval         = std::chrono::milliseconds{20};
        configuration.restartBackoff       = std::chrono::milliseconds{50};
        configuration.shutdownTimeout      = std::chrono::milliseconds{2000};
        configuration.crashLoopWindow      = std::chrono::milliseconds{300};
        configuration.crashLoopLimit       = 3;
        configuration.watchdogNotification = &notification;

        WorkerSupervisor  supervisor(std::move(configuration));
        std::atomic<bool> isOrchestrationSettled{false};
        std::thread       supervisorThread([&supervisor, &isOrchestrationSettled] { isOrchestrationSettled.store(supervisor.run(), std::memory_order_release); });

        // 等到就绪快照到位（编排线程每轮发布），再睡过起拍宽限：宽限内可以放行一条（worker 起拍本身要
        // 时间），所以判据不是「一条都没喂过」而是「宽限之后不再喂」。放行条数由喂狗门的算术钉住——
        // 节拍 500 毫秒、宽限 1000 毫秒，检查至少相隔一个节拍，因而第二个节拍上的判定已过了宽限
        ASSERT_TRUE(waitForCondition([&supervisor] { return supervisor.runningWorkerCount() >= 2U; }, kWaitTimeout)) << "两条假 worker 没有都起来";
        std::this_thread::sleep_for(std::chrono::milliseconds{2000});
        int permittedDuringGrace = 0;
        while (!receiveOneWatchdogDatagram(listener, std::chrono::milliseconds{20}).empty())
        {
            ++permittedDuringGrace;
        }
        EXPECT_LE(permittedDuringGrace, 1) << "起拍宽限放行了 " << permittedDuringGrace << " 条：宽限只该覆盖到第一个节拍之后";

        // 停喂那一段：观察窗取得比一整个窗口还宽，慢机器只会让它更容易成立
        const std::string unexpectedPing = receiveOneWatchdogDatagram(listener, std::chrono::milliseconds{1500});
        EXPECT_TRUE(unexpectedPing.empty()) << "worker 从不拍心跳，宽限过后 master 却还在喂：「" << unexpectedPing << "」";

        supervisor.requestStop();
        supervisorThread.join();
        EXPECT_TRUE(isOrchestrationSettled.load(std::memory_order_acquire)) << "按请求收口应当报成功";

        static_cast<void>(::close(listener));
#endif
    }

} // namespace AsynGyanis::Core
