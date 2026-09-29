#include "Core/Process/WorkerSupervisor.h"

#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"
#include "Core/Process/UpgradeChannel.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <algorithm>
#include <optional>
#include <thread>
#include <utility>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        /// 等待 worker 退出时的轮询间隔：比 pollInterval 细，让收尾尽量贴着 shutdownTimeout 收干净
        constexpr std::chrono::milliseconds kReapPollInterval{20};

        /// 强杀之后等子进程被回收的上限：SIGKILL 的投递与调度有个短窗口，
        /// 立刻释放句柄会把 pid 丢掉、从此没人收尸（POSIX 上就是僵尸进程）
        constexpr std::chrono::milliseconds kForcedReapWait{2000};

        /// 停止请求的原子必须无锁，否则不能在信号处理函数里置位
        static_assert(std::atomic<bool>::is_always_lock_free, "WorkerSupervisor::requestStop() 要求无锁原子才能在信号处理函数里调用");

#if !ASYN_PLATFORM_WIN32
        /// 当前正在运行的编排器：信号处理函数只拿得到这一个入口，因此用文件级指针登记
        /// （同一进程同时只该有一个 master，多份编排器注册后装的就只剩最后一个）。
        /// 用原子量而不是 volatile：volatile 只保证「不被优化掉」，信号线程与主线程之间
        /// 仍缺同步语义；is_always_lock_free 由上面的 static_assert 钉住
        std::atomic<WorkerSupervisor *> g_runningSupervisor{nullptr};

        /**
         * @brief 停止信号的处理函数：只置原子标记，退出流程留给 run() 的循环
         * @param signalNumber 信号号（未使用）
         */
        void handleStopSignal(int signalNumber) noexcept
        {
            static_cast<void>(signalNumber);
            if (WorkerSupervisor *const supervisor = g_runningSupervisor.load(std::memory_order_acquire); supervisor != nullptr)
            {
                supervisor->requestStop();
            }
        }
#endif
    } // namespace

    WorkerSupervisor::WorkerSupervisor(Configuration configuration) : m_configuration(std::move(configuration))
    {
        if (m_configuration.executablePath.empty())
        {
            throw Base::LogicException("多进程编排无法启动：可执行文件路径为空。请在配置里给出服务器可执行文件的路径");
        }
        if (m_configuration.workerCount < 2)
        {
            throw Base::LogicException("多进程编排要求 worker 数至少为 2（当前 " + std::to_string(m_configuration.workerCount) +
                                       "）。只想跑单进程时不要构造 WorkerSupervisor，直接启动服务器即可");
        }
        if (m_configuration.pollInterval <= std::chrono::milliseconds::zero() || m_configuration.shutdownTimeout <= std::chrono::milliseconds::zero())
        {
            throw Base::LogicException("多进程编排的轮询间隔与收尾期限都必须大于 0，否则循环会空转或收尾没有期限");
        }
        // 崩溃判据单独判：窗口非正时「存活不足窗口才算一次起来就崩」这条比较恒不成立，崩溃计数会
        // 走「稳定运行后退出」那条清零分支——既不涨计数也就永不放弃、永不退避，秒退型 worker 被按
        // 轮询间隔满速重启，run() 永不返回。上限取 0 是反方向的写错：崩一次就放弃整池
        if (m_configuration.crashLoopWindow <= std::chrono::milliseconds::zero() || m_configuration.crashLoopLimit < 1)
        {
            throw Base::LogicException("多进程编排的崩溃判据非法：crashLoopWindow 必须大于 0（存活不足它就退出才算一次"
                                       "「起来就崩」，取 0 或负数会让这条判定永不成立，秒退的 worker 会被满速重启且永不放弃）；"
                                       "crashLoopLimit 至少为 1（连续崩这么多次就停止补那个 worker）");
        }
#if ASYN_PLATFORM_WIN32
        // Windows 上没有 SO_REUSEPORT 的等价物：多个进程各自 bind 同一端口时内核把全部连接交给
        // 最后绑上的那一个，前面的进程一个错都不报却永远收不到连接——那是「起了 N 个进程、只有 1 个
        // 在干活」的假成功。因此这边只认移交模式：master bind 一次，本类把那份监听引用逐个复制过去
        if (!m_configuration.handoff)
        {
            throw Base::LogicException("Windows 上多进程必须走监听套接字移交：那边没有 SO_REUSEPORT 的等价物，多个进程各自绑同一端口时"
                                       "内核把全部连接交给最后绑上的那一个，请把 master 已经 bind + listen 的描述符填进 "
                                       "Configuration::handoff（listeningDescriptor）。只想跑单进程时把 workers 设为 1"
                                       "（单进程 + 多工作循环 + 接受分发），不要构造本类");
        }
        if (m_configuration.handoff->listeningDescriptor < 0)
        {
            throw Base::LogicException("Windows 上多进程的移交配置不合格：handoff.listeningDescriptor 是 " + std::to_string(m_configuration.handoff->listeningDescriptor) +
                                       "，不是一个已 bind + listen 的套接字描述符");
        }
        // 预算非正时「等不到对端」这件事就没有期限：一次 accept 的无限阻塞会把整池的补位与收尾冻住
        if (m_configuration.handoff->waitBudget <= std::chrono::milliseconds::zero())
        {
            throw Base::LogicException("Windows 上多进程的移交预算必须大于 0：handoff.waitBudget 是 " + std::to_string(m_configuration.handoff->waitBudget.count()) +
                                       " 毫秒，非正数意味着等 worker 连上通道没有期限");
        }
#else
        // POSIX 上每个 worker 自己 bind 同一个端口（SO_REUSEPORT 由内核分摊），移交那套在本平台
        // 没有使用方：填了它就是一条「填了却不生效」的路，当场拒而不是静默忽略
        if (m_configuration.handoff)
        {
            throw Base::LogicException("POSIX 上不需要移交配置：这里的每个 worker 都自己 bind 同一个端口（SO_REUSEPORT 分摊）。"
                                       "请把 Configuration::handoff 留空；要跨进程共享一份监听套接字是 Windows 那条形状");
        }
#endif
        m_workers.resize(m_configuration.workerCount);
    }

    WorkerSupervisor::~WorkerSupervisor()
    {
        // 兜底：run() 里抛异常（或调用方中途销毁）时不留孤儿进程占着端口。
        // 正常路径下 run() 已经把所有 worker 送走，这里不会真的杀谁
        for (Worker &worker: m_workers)
        {
            if (worker.handle.isValid() && Platform::Process::isRunning(worker.handle))
            {
                static_cast<void>(Platform::Process::forceTermination(worker.handle));
            }
        }

        // 对象马上就没人了，收尸只能在这里等完：跳过这一步的话，强杀掉的 worker 会以僵尸形式
        // 挂在父进程上，退出码再也查不到（句柄随后随本对象一起析构）
        waitForForcedTerminationsToLand();
    }

    bool WorkerSupervisor::run()
    {
#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 信号处理登记与还原的守卫：本函数从哪条路退出都把它留下的痕迹抹平
         * @details 下面的编排循环会分配（日志、进程句柄、vector），抛出时若只靠函数末尾那三行
         *          还原，g_runningSupervisor 就一直指向这个正在栈展开中消亡的对象——
         *          下一次 SIGTERM/SIGINT 的 handler 只做「读那个全局指针并调 requestStop()」，
         *          那是往已释放内存上写标记。登记与还原成对放进析构里，异常路径与正常路径同一条
         */
        class SignalRegistration
        {
        public:
            explicit SignalRegistration(WorkerSupervisor &supervisor) noexcept :
                m_previousTerminateHandler(std::signal(SIGTERM, &handleStopSignal)), m_previousInterruptHandler(std::signal(SIGINT, &handleStopSignal))
            {
                // 先装 handler 再发布指针：handler 只在指针非空时才转达，装反的一拍里
                // 信号最多被当成「没人要停」丢掉，而不是解引用一个还没定下来的 this
                g_runningSupervisor.store(&supervisor, std::memory_order_release);
            }

            SignalRegistration(const SignalRegistration &)            = delete;
            SignalRegistration &operator=(const SignalRegistration &) = delete;

            /// 还原先前两个 handler 并收回全局指针：之后再收到停止信号就与本编排器无关了
            ~SignalRegistration()
            {
                std::signal(SIGTERM, m_previousTerminateHandler);
                std::signal(SIGINT, m_previousInterruptHandler);
                g_runningSupervisor.store(nullptr, std::memory_order_release);
            }

        private:
            void (*m_previousTerminateHandler)(int); ///< 被本登记换掉的 SIGTERM 处理函数
            void (*m_previousInterruptHandler)(int); ///< 被本登记换掉的 SIGINT 处理函数
        };

        const SignalRegistration signalRegistration(*this);
#endif

        LOG_INFO_FMT("WorkerSupervisor: 开始编排 {} 个 worker，可执行文件 {}", m_configuration.workerCount, m_configuration.executablePath);

        // 「整池子都起不来」与「按请求停掉」是两种结果：前者调用方要报非 0 退出码，
        // 否则进程管理器与脚本只看退出码的话，会把一次彻底失败当成一次正常停机
        bool isPoolGivenUp = false;

        while (!m_isStopRequested.load(std::memory_order_acquire))
        {
            // 每一轮把每个槽位看一遍：没在跑的补上、已退出的收尸并决定要不要补
            for (std::size_t workerIndex = 0; workerIndex < m_workers.size(); ++workerIndex)
            {
                Worker &worker = m_workers[workerIndex];
                if (worker.isGivenUp)
                {
                    continue;
                }

                if (!worker.handle.isValid())
                {
                    // 退避只加在「这个槽位已经崩过」之后：崩溃循环里不留一段满速重启的窗口。
                    // 首轮起进程时崩计数还是 0，若照样退避，N 个 worker 的冷启动就要串行等
                    // N × restartBackoff（默认 500 毫秒），进程池要空转几秒才开始接活
                    if (worker.crashCount > 0)
                    {
                        std::this_thread::sleep_for(m_configuration.restartBackoff);
                        if (m_isStopRequested.load(std::memory_order_acquire))
                        {
                            break;
                        }
                    }
                    static_cast<void>(startWorker(worker, workerIndex));
                    continue;
                }

                if (!Platform::Process::isRunning(worker.handle))
                {
                    static_cast<void>(reapWorker(worker, workerIndex));
                }
            }

            // 全部槽位都放弃了就不再空转：日志已经交代过原因，交给调用方决定怎么处理。
            // 每轮重新数一遍（而不是累加计数）：同一个槽位连续失败只算它自己那一份。
            // 在运行的个数在同一趟里数出来再发布：观察者线程只读那份原子量，不碰句柄
            std::size_t givenUpWorkerCount = 0;
            std::size_t runningWorkerCount = 0;
            for (const Worker &worker: m_workers)
            {
                if (worker.isGivenUp)
                {
                    ++givenUpWorkerCount;
                    continue;
                }
                if (worker.handle.isValid() && Platform::Process::isRunning(worker.handle))
                {
                    ++runningWorkerCount;
                }
            }
            m_runningWorkerCount.store(runningWorkerCount, std::memory_order_release);

            if (givenUpWorkerCount >= m_workers.size())
            {
                LOG_ERROR_FMT("WorkerSupervisor: {} 个 worker 全部因「起来就崩」被放弃，编排退出（请检查可执行文件与配置）", givenUpWorkerCount);
                isPoolGivenUp = true;
                break;
            }

            std::this_thread::sleep_for(m_configuration.pollInterval);
        }

        stopAllWorkers();

        // 停止信号的登记由 signalRegistration 在离开作用域时撤销：正常返回与异常展开走同一条
        LOG_INFO_FMT("WorkerSupervisor: 编排结束");
        return !isPoolGivenUp;
    }

    void WorkerSupervisor::requestStop() noexcept
    {
        m_isStopRequested.store(true, std::memory_order_release);
    }

    std::size_t WorkerSupervisor::runningWorkerCount() const noexcept
    {
        // 只读编排线程发布的快照：在这里探句柄等于在观察者的线程上回收子进程，
        // 与编排线程同时读写同一份进程号/退出码缓存（见头文件里那两条理由）
        return m_runningWorkerCount.load(std::memory_order_acquire);
    }

    bool WorkerSupervisor::startWorker(Worker &worker, const std::size_t workerIndex)
    {
        Platform::Process::LaunchOptions launchOptions;
        launchOptions.executablePath = m_configuration.executablePath;
        launchOptions.arguments      = m_configuration.workerArguments;
        // 每个 worker 进自己名下的进程组：这是 Windows 上 requestTermination() 发得出 CTRL_BREAK 的前提
        // （组是控制台事件的投递单位，不给独立组就只能整组广播，那会打断 master 自己）。
        // POSIX 上这个字段无效果——那边的体面退出走 SIGTERM
        launchOptions.ownProcessGroup = true;
        // 子进程随 master 消失：master 被硬杀（Taskkill /F、OOM killer、容器被删）时不留一群还在占端口
        // 却没人编排的 worker。POSIX 侧这条由 spawn 内装的 PDEATHSIG 兜住，Windows 靠作业句柄
        launchOptions.killWithParent = true;

#if ASYN_PLATFORM_WIN32
        // 移交模式：通道要先开好——对方进程靠参数里那个地址连回来，而它连回来之后才谈得上按它的
        // 进程号复制一份监听引用过去。每次（重）起都开一条新的，因为一条通道只交给一个对端
        auto handoffChannel = UpgradeChannel::open();
        if (!handoffChannel)
        {
            noteFailedStart(worker, workerIndex, "开不出交接通道：" + handoffChannel.error());
            return false;
        }
        launchOptions.arguments.push_back(std::string{kHandedOverListenerArgument});
        launchOptions.arguments.push_back(handoffChannel->address());

        // 起来了却拿不到监听引用的进程不会服务任何连接：收掉它、把这个槽位按一次「起来就崩」记上。
        // 留着它等于占着一个名额却谁也不接，收尾时还要多等一轮
        const auto retireWithoutListener = [&](const std::string &failureReason)
        {
            static_cast<void>(Platform::Process::forceTermination(worker.handle));
            // 强杀的投递与调度有个短窗口：等到它真的结束再丢句柄，否则没有任何人收这个尸
            // （isRunning() 观察时顺手回收，这一轮询同时完成「等结束」与「收尸」）
            const auto reapDeadline = std::chrono::steady_clock::now() + kForcedReapWait;
            while (std::chrono::steady_clock::now() < reapDeadline && Platform::Process::isRunning(worker.handle))
            {
                std::this_thread::sleep_for(kReapPollInterval);
            }
            worker.handle = Platform::Process::Handle{};
            noteFailedStart(worker, workerIndex, failureReason);
            return false;
        };
#endif

        worker.handle    = Platform::Process::spawn(launchOptions);
        worker.startTime = std::chrono::steady_clock::now();
        if (!worker.handle.isValid())
        {
            // 起不来不重试：把该槽位按「起来就崩」记一次，连续到上限就放弃，免得把日志刷满
            noteFailedStart(worker, workerIndex, "平台错误码 " + std::to_string(Platform::PlatformError::lastErrorCode()));
            return false;
        }

        LOG_INFO_FMT("WorkerSupervisor: worker {} 已启动，进程号 {}", workerIndex, worker.handle.processId());

        // 保护缺席必须点名：本主机挂不上作业时（多为已处在一个禁止嵌套的作业里，某些容器与 CI 就这样），
        // master 被硬杀后 worker 会留着占端口而没人编排——这与「随父终止」的承诺只差一层嵌套作业权限，
        // 事后没人想得起来是这里缺的。只报第一次，因为挂不上是宿主性质，每个 worker 都会一样
        if (!worker.handle.killWithParentGuardActive() && !m_killGuardAbsenceReported)
        {
            m_killGuardAbsenceReported = true;
            LOG_WARN("WorkerSupervisor: 本主机不给子进程挂作业（常见于已处在禁止嵌套的作业里，如某些容器与 CI），"
                     "「master 被硬杀时 worker 一并退出」这条保护不可用；worker 会留着占住端口，收口只能交给进程管理器");
        }

#if ASYN_PLATFORM_WIN32
        const std::chrono::milliseconds handoffBudget = m_configuration.handoff->waitBudget;
        const auto                      peer          = handoffChannel->waitForPeer(handoffBudget);
        if (!peer)
        {
            return retireWithoutListener("等 worker 连上交接通道：" + peer.error());
        }
        const auto isHandedOver = handoffChannel->handOffListener(*peer, m_configuration.handoff->listeningDescriptor, static_cast<std::uint64_t>(worker.handle.processId()));
        // 通道交完就没用了：本函数不关它由调用方收，这里显式收口，别留一条还能连的通道
        static_cast<void>(Platform::FileDescriptor::close(*peer));
        if (!isHandedOver)
        {
            return retireWithoutListener("交出监听套接字：" + isHandedOver.error());
        }
        LOG_INFO_FMT("WorkerSupervisor: worker {} 已接手监听套接字（进程号 {}）", workerIndex, worker.handle.processId());
#endif

        return true;
    }

    void WorkerSupervisor::noteFailedStart(Worker &worker, const std::size_t workerIndex, const std::string &failureReason)
    {
        ++worker.crashCount;
        LOG_ERROR_FMT("WorkerSupervisor: worker {} 起不来（{}），这是连续第 {} 次。路径 {}", workerIndex, failureReason, worker.crashCount, m_configuration.executablePath);
        if (worker.crashCount >= m_configuration.crashLoopLimit)
        {
            worker.isGivenUp = true;
            LOG_ERROR_FMT("WorkerSupervisor: worker {} 连续 {} 次起不来，放弃补它", workerIndex, worker.crashCount);
        }
    }

    bool WorkerSupervisor::reapWorker(Worker &worker, const std::size_t workerIndex)
    {
        const std::optional<int> exitCode   = Platform::Process::pollExitCode(worker.handle);
        const bool               isFastExit = std::chrono::steady_clock::now() - worker.startTime < m_configuration.crashLoopWindow;
        LOG_INFO_FMT("WorkerSupervisor: worker {} 已退出（进程号 {}，退出码 {}，存活 {} 毫秒）{}", workerIndex, worker.handle.processId(), exitCode.value_or(-1),
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - worker.startTime).count(),
                     isFastExit ? "，按「起来就崩」记一次" : "");
        worker.handle.close();

        if (isFastExit)
        {
            ++worker.crashCount;
            if (worker.crashCount >= m_configuration.crashLoopLimit)
            {
                worker.isGivenUp = true;
                LOG_ERROR_FMT("WorkerSupervisor: worker {} 连续 {} 次存活不足 {} 毫秒就退出，放弃补它（请检查它的启动日志）", workerIndex, worker.crashCount,
                              m_configuration.crashLoopWindow.count());
                return true;
            }
            return false;
        }

        // 稳定跑过一段时间的 worker 退出：清掉崩溃计数，正常补一个新的
        worker.crashCount = 0;
        return false;
    }

    void WorkerSupervisor::stopAllWorkers()
    {
        std::size_t runningCount = 0;
        for (Worker &worker: m_workers)
        {
            if (!worker.handle.isValid())
            {
                continue;
            }
            // 请求体面退出：worker 自己会走 stop()/drain() 把在途请求做完
            if (Platform::Process::requestTermination(worker.handle))
            {
                ++runningCount;
                continue;
            }
            // 发不出去才强杀：Windows 上宿主没有控制台（服务、被 DETACHED_PROCESS 派出来的进程）时
            // 控制台事件无处投递。此时宁可强杀也不能把收尾卡死——但这条路径是有代价的，出声记下
            LOG_WARN_FMT("WorkerSupervisor: 无法请求进程号 {} 体面退出，直接强杀", worker.handle.processId());
            static_cast<void>(Platform::Process::forceTermination(worker.handle));
        }

        if (runningCount != 0)
        {
            LOG_INFO_FMT("WorkerSupervisor: 已请求 {} 个 worker 体面退出，最多等 {} 毫秒", runningCount, m_configuration.shutdownTimeout.count());
        }

        const auto deadline = std::chrono::steady_clock::now() + m_configuration.shutdownTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::size_t aliveCount = 0;
            for (Worker &worker: m_workers)
            {
                if (worker.handle.isValid() && Platform::Process::isRunning(worker.handle))
                {
                    ++aliveCount;
                }
            }
            if (aliveCount == 0)
            {
                break;
            }
            std::this_thread::sleep_for(kReapPollInterval);
        }

        // 期限到了还在的一律强杀：收尾不能没有尽期，否则一次卡住的退出会让 master 永远关不掉
        for (Worker &worker: m_workers)
        {
            if (!worker.handle.isValid())
            {
                continue;
            }
            if (Platform::Process::isRunning(worker.handle))
            {
                LOG_ERROR_FMT("WorkerSupervisor: worker 进程号 {} 在收尾期限内没有退出，已强杀", worker.handle.processId());
                static_cast<void>(Platform::Process::forceTermination(worker.handle));
            }
        }

        waitForForcedTerminationsToLand();

        for (Worker &worker: m_workers)
        {
            if (!worker.handle.isValid())
            {
                continue;
            }
            if (Platform::Process::isRunning(worker.handle))
            {
                LOG_WARN_FMT("WorkerSupervisor: worker 进程号 {} 在强杀后仍未结束，句柄已释放但没回收（POSIX 上可能留下僵尸）", worker.handle.processId());
            }
            worker.handle.close();
        }

        // 收尾把句柄全交还了：此后没有「还在跟踪且在运行」的 worker，快照归零。
        // 不在这儿发布的话，观察者会一直读到停机前那一轮的个数
        m_runningWorkerCount.store(0, std::memory_order_release);
    }

    void WorkerSupervisor::waitForForcedTerminationsToLand()
    {
        // 强杀之后要等到子进程真的被回收再释放句柄：SIGKILL 的投递与调度有个短窗口，立刻 close()
        // 会把 pid 丢掉、从此没有任何人收尸（POSIX 上就是僵尸进程，master 长期运行时这些僵尸会一直
        // 堆着）。isRunning() 观察时顺手回收，因此这里的轮询同时完成「等它结束」与「收尸」两件事；
        // 等待仍然有界，通知早已发出
        const auto reapDeadline = std::chrono::steady_clock::now() + kForcedReapWait;
        while (std::chrono::steady_clock::now() < reapDeadline)
        {
            const bool hasUnreapedWorker =
                    std::ranges::any_of(m_workers, [](const Worker &worker) { return worker.handle.isValid() && Platform::Process::isRunning(worker.handle); });
            if (!hasUnreapedWorker)
            {
                return;
            }
            std::this_thread::sleep_for(kReapPollInterval);
        }
    }
} // namespace AsynGyanis::Core
