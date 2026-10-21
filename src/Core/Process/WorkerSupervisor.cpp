#include "Core/Process/WorkerSupervisor.h"

#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"
#include "Core/Process/SignalObserverInstall.h"
#include "Core/Process/UpgradeChannel.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/Pipe.h"
#include "Platform/System/PlatformError.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <thread>
#include <utility>

#include "Core/Process/WorkerSupervisorSupport.h"

namespace AsynGyanis::Core
{
    namespace
    {
        /// 停止请求由 Platform 的信号等待线程置位、编排线程读取，原子必须无锁
        static_assert(std::atomic<bool>::is_always_lock_free, "WorkerSupervisor::requestStop() 要求无锁原子才能在信号等待线程与编排线程之间传递");

        /**
         * @brief 在编排线程上跑一个观察者回调，抛出的异常就地接住
         * @param callback 回调本体；空对象是空操作
         * @param what 日志里写的回调名，报错时要能认出是哪一格
         */
        void runObserver(const std::function<void()> &callback, const char *what) noexcept
        {
            if (!callback)
            {
                return;
            }
            try
            {
                callback();
            } catch (const std::exception &failure)
            {
                // 让一条坏回调带走整池 worker 是不可接受的：抛出之后 worker 一个都没被通知退出，
                // 而 run() 的调用方看到的是一个异常，端口上却还有一堆进程在服务
                LOG_ERROR_FMT("WorkerSupervisor: {} 的观察者抛出异常并被接住：{}（后面的编排照跑）", what, std::string(failure.what()));
            } catch (...)
            {
                LOG_ERROR_FMT("WorkerSupervisor: {} 的观察者抛出非标准异常并被接住（后面的编排照跑）", what);
            }
        }
    } // namespace

    WorkerSupervisor::WorkerSupervisor(Configuration configuration) : m_configuration(std::move(configuration)), m_restartJitterGenerator(std::random_device{}())
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
        if (Platform::Socket::requiresListenerHandoff())
        {
            // 没有 SO_REUSEPORT 的等价物：多个进程各自 bind 同一端口时内核把全部连接交给最后绑上的
            // 那一个，前面的进程一个错都不报却永远收不到连接——那是「起了 N 个进程、只有 1 个在
            // 干活」的假成功。因此只认移交模式：master bind 一次，本类把监听引用逐个复制过去
            if (!m_configuration.handoff)
            {
                throw Base::LogicException("当前平台没有 SO_REUSEPORT 的等价物，多进程必须走监听套接字移交：多个进程各自绑同一端口时"
                                           "内核把全部连接交给最后绑上的那一个，请把 master 已经 bind + listen 的描述符填进 "
                                           "Configuration::handoff（listeningDescriptor）。只想跑单进程时把 workers 设为 1"
                                           "（单进程 + 多工作循环 + 接受分发），不要构造本类");
            }
            if (m_configuration.handoff->listeningDescriptor < 0)
            {
                throw Base::LogicException("多进程的移交配置不合格：handoff.listeningDescriptor 是 " + std::to_string(m_configuration.handoff->listeningDescriptor) +
                                           "，不是一个已 bind + listen 的套接字描述符");
            }
            // 预算非正时「等不到对端」这件事就没有期限：一次 accept 的无限阻塞会把整池的补位与收尾冻住
            if (m_configuration.handoff->waitBudget <= std::chrono::milliseconds::zero())
            {
                throw Base::LogicException("多进程的移交预算必须大于 0：handoff.waitBudget 是 " + std::to_string(m_configuration.handoff->waitBudget.count()) +
                                           " 毫秒，非正数意味着等 worker 连上通道没有期限");
            }
        } else if (m_configuration.handoff)
        {
            // 每个 worker 自己 bind 同一个端口时，移交那套没有使用方：填了它就是一条「填了却不生效」
            // 的路，当场拒而不是静默忽略
            throw Base::LogicException("当前平台每个 worker 都自己 bind 同一端口（SO_REUSEPORT 分摊），不需要移交配置。"
                                       "请把 Configuration::handoff 留空；要跨进程共享一份监听套接字是另一条平台形状");
        }
        m_workers.resize(m_configuration.workerCount);

        m_metricHandles = {
                Core::ProcessMetricsRegistry::registerMetric("asyn_worker_crashes_total", "worker「起来就崩」的累计次数（含补上去的那几轮）", Core::ProcessMetricKind::Counter,
                                                             Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_totalCrashCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_worker_slots_given_up",
                                                             "被放弃补位的 worker 槽位累计次数：满编时少一个进程就少一份容量；槽位静默足够久会重新纳入，那时本计数不回退",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_givenUpWorkerCount.load(std::memory_order_relaxed)); }),
        };
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
        // 停机信号交给 Platform：POSIX 是屏蔽 + 等待线程，Windows 是控制台处理器。没有这段接管时
        // master 收到 Ctrl+C 走默认处置，进程当场没了；而每个 worker 都挂在自己名下的作业里，作业
        // 句柄随 master 关闭，worker 于是被硬杀，在途请求不收尾。关闭、注销与关机不是「请停服」，
        // 交回系统缺省处置。观察者活到 run() 返回为止，异常路径同样会摘干净。
        [[maybe_unused]] const auto stopWatcher = installSignalObserver(
                Platform::SignalSet::forShutdown(),
                [this](const Platform::SignalEvent event) noexcept
                {
                    if (event != Platform::SignalEvent::Interrupt && event != Platform::SignalEvent::Terminate)
                    {
                        return false;
                    }
                    requestStop();
                    return true;
                },
                "WorkerSupervisor", "仍可走 requestStop()", "本平台没有可用的停机路径");

        LOG_INFO_FMT("WorkerSupervisor: 开始编排 {} 个 worker，可执行文件 {}", m_configuration.workerCount, m_configuration.executablePath);

        // 看门狗汇聚与轮询粒度（细节见 enableWatchdogFeeding 的说明）
        const std::chrono::milliseconds pollSleep = enableWatchdogFeeding();

        // 「整池子都起不来」与「按请求停掉」是两种结果：前者调用方要报非 0 退出码，
        // 否则进程管理器与脚本只看退出码的话，会把一次彻底失败当成一次正常停机
        bool isPoolGivenUp = false;

        while (!m_isStopRequested.load(std::memory_order_acquire))
        {
            // 每一轮把每个槽位看一遍：没在跑的补上、已退出的收尸并决定要不要补
            launchMissingAndReapExited();

            const SlotScanResult slotScan = scanWorkerSlots();

            // 凑齐整池就算就绪（回调有没有人订不影响这一格）：有一格被放弃时永远不进这里——
            // 那一格起不来本身就是要让人看见的失败。就绪回调在 handlePoolReady 里按需触发，
            // 而看门狗汇聚以这一格为起点，所以它不能挂在「有没有观察者」上
            if (!m_hasAnnouncedReadiness && isPoolComplete(slotScan.runningCount, m_workers.size()))
            {
                handlePoolReady();
            }

            if (slotScan.givenUpCount >= m_workers.size())
            {
                LOG_ERROR_FMT("WorkerSupervisor: {} 个 worker 全部因「起来就崩」被放弃，编排退出（请检查可执行文件与配置）", slotScan.givenUpCount);
                isPoolGivenUp = true;
                break;
            }

            // 心跳汇聚与喂狗：未启用时两条都是空操作（见各自的说明）
            const auto watchdogNow = std::chrono::steady_clock::now();
            pollWorkerHeartbeats(watchdogNow);
            feedWatchdogIfDue(watchdogNow);

            std::this_thread::sleep_for(pollSleep);
        }

        // 收口的这一刻报给调用方：worker 还都在跑，正是「我在收尾」这句话该出去的位置。
        // 放在 run() 返回之后就晚了——那会儿整池已经送走，监督者等的是一条事后说明
        runObserver(m_configuration.onStopRequested, "onStopRequested");

        stopAllWorkers();

        // 停止信号的登记由 signalRegistration 在离开作用域时撤销：正常返回与异常展开走同一条
        LOG_INFO_FMT("WorkerSupervisor: 编排结束");
        return !isPoolGivenUp;
    }

    bool WorkerSupervisor::isPoolComplete(const std::size_t runningWorkerCount, const std::size_t slotCount) noexcept
    {
        // 空池不算「都到位」：槽位数为 0 时任何个数都「等于」它，而一条READY=1 说的是「有人能接活」
        if (slotCount == 0)
        {
            return false;
        }
        return runningWorkerCount == slotCount;
    }


    WorkerSupervisor::SlotScanResult WorkerSupervisor::scanWorkerSlots() noexcept
    {
        SlotScanResult scan;
        // 每轮重新数一遍（而不是累加计数）：同一个槽位连续失败只算它自己那一份。
        // 在运行的个数在同一趟里数出来再发布：观察者线程只读那份原子量，不碰句柄
        for (const Worker &worker: m_workers)
        {
            if (worker.isGivenUp)
            {
                ++scan.givenUpCount;
                continue;
            }
            if (worker.handle.isValid() && Platform::Process::isRunning(worker.handle))
            {
                ++scan.runningCount;
            }
        }
        m_runningWorkerCount.store(scan.runningCount, std::memory_order_release);
        return scan;
    }


    void WorkerSupervisor::handlePoolReady()
    {
        m_hasAnnouncedReadiness = true;
        runObserver(m_configuration.onAllWorkersRunning, "onAllWorkersRunning");

        // 看门狗从就绪起计：就绪前不判定也不喂（与单进程形态「先报就绪再挂节拍」同一顺序）。
        // 同一轮把各槽位的心跳账本翻到「宽限中」，宽限给一整个窗口（2×节拍）：监督者第一次判超时的时点
        // 就是就绪之后一整个窗口，而首拍落在什么时候取决于 fork 与挂节拍的毫秒级相位——宽限只给半程时，
        // 首拍晚到几毫秒就把第一次喂狗压到窗口末尾
        if (m_isWatchdogFeedingEnabled)
        {
            const auto graceUntil = std::chrono::steady_clock::now() + m_watchdogPingInterval * 2;
            for (Worker &worker: m_workers)
            {
                // 账本翻回「还没拍到、但在宽限内」：起拍要时间，这段时间不该由监督者的超时来赔
                worker.lastHeartbeatAt     = std::nullopt;
                worker.heartbeatGraceUntil = graceUntil;
            }
            m_lastWatchdogFeed = std::chrono::steady_clock::now();
        }
    }


    std::chrono::milliseconds WorkerSupervisor::restartBackoffFor(const std::chrono::milliseconds base, const std::size_t crashCount, const double jitterUnit) noexcept
    {
        if (base <= std::chrono::milliseconds::zero())
        {
            return base;
        }
        // 封顶至少是基准值：配置本身已经超过 30 秒时，不把它的首轮等待反过来压低
        const std::chrono::milliseconds cap     = std::max(base, kMaximumWorkerRestartBackoff);
        std::chrono::milliseconds       nominal = base;
        for (std::size_t step = 1; step < crashCount && nominal < cap; ++step)
        {
            nominal = std::min(nominal * 2, cap);
        }

        // 抖动：jitterUnit 落在 [0,1)，映射到 [1-ratio, 1+ratio]；0.5 恰为基准值，用例因此可钉边界。
        // 取整用四舍五入而不是截断：0.999999 的边界值乘出来是 119.99998，截断会造出一条测试里看得见的 119。
        // 抖动之后再过一次封顶：否则到顶那一档的 +20% 会把「上限」悄悄顶破
        const double boundedJitter = std::clamp(jitterUnit, 0.0, 0.999999);
        const double factor        = 1.0 - kWorkerRestartBackoffJitterRatio + 2.0 * kWorkerRestartBackoffJitterRatio * boundedJitter;
        const auto   jittered      = std::chrono::milliseconds{static_cast<std::int64_t>(std::llround(static_cast<double>(nominal.count()) * factor))};
        return std::min(jittered, cap);
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
            std::this_thread::sleep_for(kWorkerReapPollInterval);
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
            closeWorkerHeartbeatPipe(worker);
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
        const auto reapDeadline = std::chrono::steady_clock::now() + kWorkerForcedReapWait;
        while (std::chrono::steady_clock::now() < reapDeadline)
        {
            const bool hasUnreapedWorker =
                    std::ranges::any_of(m_workers, [](const Worker &worker) { return worker.handle.isValid() && Platform::Process::isRunning(worker.handle); });
            if (!hasUnreapedWorker)
            {
                return;
            }
            std::this_thread::sleep_for(kWorkerReapPollInterval);
        }
    }
} // namespace AsynGyanis::Core
