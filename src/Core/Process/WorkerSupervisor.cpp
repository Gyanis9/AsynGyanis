#include "Core/Process/WorkerSupervisor.h"

#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"
#include "Core/Process/UpgradeChannel.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>
#include <utility>

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
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

        /// 当前正在运行的编排器：处理器只拿得到这一个入口，因此用文件级指针登记
        /// （同一进程同时只该有一个 master，多份编排器注册后装的就只剩最后一个）。
        /// 用原子量而不是 volatile：volatile 只保证「不被优化掉」，处理器线程与主线程之间
        /// 仍缺同步语义；is_always_lock_free 由上面的 static_assert 钉住
        std::atomic<WorkerSupervisor *> g_runningSupervisor{nullptr};

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

#if !ASYN_PLATFORM_WIN32
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
#else
        /**
         * @brief 控制台停机事件的接管：Ctrl+C 与 CTRL_BREAK 转成一次「请体面停止」
         * @details 没有这一段接管时，master 收到 Ctrl+C 走的是默认处置：进程当场没了。而每个 worker 都
         *          挂在自己名下的作业里（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），作业句柄随 master 一起
         *          关闭，于是 worker 是被硬杀的——在途请求不收尾，「按进程组给每个 worker 发 CTRL_BREAK
         *          让它自己收口」那一步根本没机会跑，而启动日志正承诺着这件事。
         *          控制台处理器跑在系统另起的线程上（不是信号上下文），置一个无锁原子同样安全。
         * @param controlType 控制台事件类型
         * @return TRUE 这一类事件已接管，不要执行默认处置
         * @return FALSE 登出／关窗口／系统关机：那不是「请停服」，交回默认处置
         */
        BOOL WINAPI handleConsoleStop(DWORD controlType) noexcept
        {
            if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT)
            {
                if (WorkerSupervisor *const supervisor = g_runningSupervisor.load(std::memory_order_acquire); supervisor != nullptr)
                {
                    supervisor->requestStop();
                }
                return TRUE;
            }
            return FALSE;
        }
#endif
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
#if ASYN_PLATFORM_WIN32
        /**
         * @brief 控制台事件处理器的登记与还原守卫：与下面 POSIX 那份 SignalRegistration 同一条理由
         * @details 先发布指针再装处理器：装反的一拍里事件最多被当成「没人要停」丢掉，而不是解引用一个
         *          还没定下来的 this。还原按「先摘处理器再收回指针」的反序做——处理器还在而指针已收回时，
         *          事件会转达给一个正在消亡的对象。本函数从哪条路退出（含抛出）都由这个守卫收尾。
         */
        class ConsoleStopRegistration
        {
        public:
            explicit ConsoleStopRegistration(WorkerSupervisor &supervisor) noexcept
            {
                g_runningSupervisor.store(&supervisor, std::memory_order_release);
                m_isHandlerInstalled = ::SetConsoleCtrlHandler(&handleConsoleStop, TRUE) != 0;
                if (!m_isHandlerInstalled)
                {
                    LOG_ERROR_FMT("WorkerSupervisor: 安装控制台事件处理器失败（GetLastError={}），Ctrl+C 这一路等于没接上："
                                  "master 会被默认处置当场打死，worker 则随作业句柄一起被硬杀，在途请求不收尾",
                                  static_cast<unsigned long>(::GetLastError()));
                }
            }

            ConsoleStopRegistration(const ConsoleStopRegistration &)            = delete;
            ConsoleStopRegistration &operator=(const ConsoleStopRegistration &) = delete;

            /// 摘掉处理器并收回全局指针：之后再收到控制台事件就与本编排器无关了
            ~ConsoleStopRegistration()
            {
                if (m_isHandlerInstalled)
                {
                    static_cast<void>(::SetConsoleCtrlHandler(&handleConsoleStop, FALSE));
                }
                g_runningSupervisor.store(nullptr, std::memory_order_release);
            }

        private:
            bool m_isHandlerInstalled{false}; ///< 处理器是否真的装上了——没装上就不必去摘别人的
        };

        const ConsoleStopRegistration stopRegistration(*this);
#else
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

    bool WorkerSupervisor::allHeartbeatsFresh(const std::vector<bool> &heartbeatFreshBySlot, const std::vector<bool> &isGivenUpBySlot) noexcept
    {
        // 空池不算「都健康」：与 isPoolComplete 同一条口径——没有槽位时「全部新鲜」是句空话，
        // 而喂出去的每一条 WATCHDOG=1 都是对监督者的陈述
        if (heartbeatFreshBySlot.empty() || heartbeatFreshBySlot.size() != isGivenUpBySlot.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < heartbeatFreshBySlot.size(); ++index)
        {
            // 放弃的槽位不参与：它那一路的归宿是「整池放弃」的判定，不该在这里再拦一道
            if (!isGivenUpBySlot[index] && !heartbeatFreshBySlot[index])
            {
                return false;
            }
        }
        return true;
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

    std::chrono::milliseconds WorkerSupervisor::enableWatchdogFeeding()
    {
#if !ASYN_PLATFORM_WIN32
        if (m_configuration.watchdogNotification != nullptr)
        {
            const auto watchdogWindow = Platform::ServiceNotification::readWatchdogConfiguration();
            if (watchdogWindow.has_value())
            {
                // 显式折算：微秒→毫秒是可能截断的转换，不给编译器留「隐式放行」的口实（GCC 在这里是硬错）
                m_watchdogPingInterval     = std::chrono::duration_cast<std::chrono::milliseconds>(watchdogWindow->pingInterval);
                m_isWatchdogFeedingEnabled = true;
                LOG_INFO_FMT("WorkerSupervisor: 看门狗汇聚已启用：窗口 {} 微秒，每 {} 毫秒喂一条 WATCHDOG=1，待 {} 个槽位的心跳全部新鲜", watchdogWindow->timeoutWindow.count(),
                             std::chrono::duration_cast<std::chrono::milliseconds>(watchdogWindow->pingInterval).count(), m_workers.size());
            } else
            {
                LOG_INFO_FMT("多进程模式未启用看门狗汇聚：{}（这不是错误——没配 WatchdogSec= 时本就没有要喂的超时）", watchdogWindow.error());
            }
        }
#endif
        // 轮询粒度：喂点判定的分辨率必须细于节拍的一半，否则「到点」会被睡眠粒度量成「晚点」。
        // 没启用时保持调用方给的 pollInterval
        return m_isWatchdogFeedingEnabled ? std::min(m_configuration.pollInterval, std::max(m_watchdogPingInterval / 2, std::chrono::milliseconds{1}))
                                          : m_configuration.pollInterval;
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

    bool WorkerSupervisor::isHeartbeatFresh(const std::optional<std::chrono::steady_clock::time_point> &lastBeatAt, const std::chrono::steady_clock::time_point now,
                                            const std::chrono::steady_clock::time_point startupGraceUntil, const std::chrono::milliseconds pingInterval) noexcept
    {
        if (!lastBeatAt.has_value())
        {
            // 成立以来一拍都没读到过：只认启动宽限，宽限之外就是这条 worker 没在推进
            return now < startupGraceUntil;
        }
        // 允许陈旧到一个半节拍：正常到点的拍子最坏也只老一个节拍多一点（检查与出拍共用同一周期，
        // 相位差是毫秒级），而连着两拍没到的一定超过这条线 → 真停摆照样在一个看门狗窗口内停喂
        return now - *lastBeatAt < pingInterval + pingInterval / 2;
    }

    bool WorkerSupervisor::attachWatchdogPipe(Worker &worker, const std::size_t workerIndex, Platform::Process::LaunchOptions &launchOptions)
    {
#if !ASYN_PLATFORM_WIN32
        if (!m_isWatchdogFeedingEnabled)
        {
            return true;
        }
        // 读端归本进程（CLOEXEC + 非阻塞——凑齐判定只看「有没有新字节」，阻塞读会把编排循环钉死），
        // 写端随 fork 继承给 worker，号码进参数。重起同一槽位每次都开新的一条：旧管道随旧 worker
        // 一起作废（reapWorker 里关），不复用
        int pipeDescriptors[2]{-1, -1};
        if (::pipe(pipeDescriptors) != 0)
        {
            noteFailedStart(worker, workerIndex, "开不出心跳管道，平台错误码 " + std::to_string(Platform::PlatformError::lastErrorCode()));
            return false;
        }
        worker.heartbeatReadFd = pipeDescriptors[0];
        static_cast<void>(::fcntl(worker.heartbeatReadFd, F_SETFD, FD_CLOEXEC));
        static_cast<void>(::fcntl(worker.heartbeatReadFd, F_SETFL, O_NONBLOCK));
        m_pendingWatchdogWriteFd = pipeDescriptors[1];

        launchOptions.arguments.push_back(std::string{kWatchdogPipeArgument});
        launchOptions.arguments.push_back(std::to_string(m_pendingWatchdogWriteFd));
        launchOptions.arguments.push_back(std::string{kWatchdogPingArgument});
        launchOptions.arguments.push_back(std::to_string(m_watchdogPingInterval.count()));
#else
        static_cast<void>(worker);
        static_cast<void>(workerIndex);
        static_cast<void>(launchOptions);
#endif
        return true;
    }

    void WorkerSupervisor::pollWorkerHeartbeats(const std::chrono::steady_clock::time_point now) noexcept
    {
#if !ASYN_PLATFORM_WIN32
        if (!m_isWatchdogFeedingEnabled)
        {
            return;
        }
        std::array<char, 128> drainBuffer{};
        for (Worker &worker: m_workers)
        {
            if (worker.heartbeatReadFd < 0)
            {
                continue;
            }
            // 把攒下的字节一次读干：只要本窗口内有过一拍就够，读到的条数本身没有意义
            while (::read(worker.heartbeatReadFd, drainBuffer.data(), drainBuffer.size()) > 0)
            {
                // 记的是「上一拍什么时候到的」而不是「这一轮读到过没有」：后者与出拍共用同一个周期，
                // 检查抢到拍子之前完成的那一轮会把标志清掉，于是永远喂不出去（判据见 isHeartbeatFresh）
                worker.lastHeartbeatAt = now;
            }
        }
#else
        static_cast<void>(now);
#endif
    }

    bool WorkerSupervisor::isEligibleToFeed(const bool enabled, const bool readinessAnnounced, const bool stopRequested) noexcept
    {
        return enabled && readinessAnnounced && !stopRequested;
    }

    void WorkerSupervisor::feedWatchdogIfDue(const std::chrono::steady_clock::time_point now)
    {
        if (!isEligibleToFeed(m_isWatchdogFeedingEnabled, m_hasAnnouncedReadiness, m_isStopRequested.load(std::memory_order_acquire)))
        {
            return;
        }
        if (now - m_lastWatchdogFeed < m_watchdogPingInterval)
        {
            return;
        }
        m_lastWatchdogFeed = now;

        std::vector<bool> freshBySlot;
        std::vector<bool> givenUpBySlot;
        freshBySlot.reserve(m_workers.size());
        givenUpBySlot.reserve(m_workers.size());
        for (const Worker &worker: m_workers)
        {
            freshBySlot.push_back(isHeartbeatFresh(worker.lastHeartbeatAt, now, worker.heartbeatGraceUntil, m_watchdogPingInterval));

            givenUpBySlot.push_back(worker.isGivenUp);
        }
        if (!allHeartbeatsFresh(freshBySlot, givenUpBySlot))
        {
            // 判定本身每节拍至多一次，这条 WARN 因此天然被限频；点名陈旧槽位——「哪条 worker 的心跳
            // 停了」正是排障要的第一句。停喂的后果由监督者执行（按超时重启整个单元）
            std::string staleSlots;
            for (std::size_t index = 0; index < m_workers.size(); ++index)
            {
                if (!givenUpBySlot[index] && !freshBySlot[index])
                {
                    staleSlots += (staleSlots.empty() ? "" : "、") + std::to_string(index);
                }
            }
            LOG_WARN_FMT("WorkerSupervisor: worker {} 的心跳已停摆，本轮不喂看门狗（{} 毫秒后监督者会按超时重启本单元）", staleSlots, m_watchdogPingInterval.count());
            return;
        }

        if (m_configuration.watchdogNotification->send(Platform::ServiceNotification::kWatchdogPingState))
        {
            return;
        }
        // 发失败只报首条：每节拍一条会把日志刷满，而这一档的后果同样由监督者执行（超时重启）
        if (!m_hasLoggedWatchdogFeedFailure)
        {
            m_hasLoggedWatchdogFeedFailure = true;
            LOG_WARN_FMT("WorkerSupervisor: WATCHDOG=1 没有送达通知通路：{}（监督者会按看门狗超时处理本进程）", m_configuration.watchdogNotification->lastError());
        }
    }

    void WorkerSupervisor::closeWorkerHeartbeatPipe(Worker &worker) noexcept
    {
#if !ASYN_PLATFORM_WIN32
        if (worker.heartbeatReadFd >= 0)
        {
            static_cast<void>(::close(worker.heartbeatReadFd));
            worker.heartbeatReadFd = -1;
        }
#else
        static_cast<void>(worker);
#endif
    }

    void WorkerSupervisor::launchMissingAndReapExited()
    {
        for (std::size_t workerIndex = 0; workerIndex < m_workers.size(); ++workerIndex)
        {
            Worker &worker = m_workers[workerIndex];
            if (worker.isGivenUp)
            {
                // 自愈口径：只要池子没全灭，槽位静默满 10 个 crashLoopWindow 就重新纳入一次。
                // 不立即重试是为了不让一个必崩的槽位每轮都刷日志；窗口取得比崩溃判定长，
                // 是为了等「配置改好」「依赖恢复」这类外部修复落定
                const auto now = std::chrono::steady_clock::now();
                if (now - worker.givenUpAt < m_configuration.crashLoopWindow * 10)
                {
                    continue;
                }
                LOG_INFO_FMT("WorkerSupervisor: worker {} 已静默 {} 毫秒，重新纳入补位（上次连续崩了 {} 次）", workerIndex,
                             std::chrono::duration_cast<std::chrono::milliseconds>(now - worker.givenUpAt).count(), worker.crashCount);
                worker.isGivenUp  = false;
                worker.crashCount = 0;
            }

            if (!worker.handle.isValid())
            {
                // 退避只加在「这个槽位已经崩过」之后：崩溃循环里不留一段满速重启的窗口。
                // 首轮起进程时崩计数还是 0，若照样退避，N 个 worker 的冷启动就要串行等
                // N × restartBackoff（默认 500 毫秒），进程池要空转几秒才开始接活。
                // 指数的基准是配置值，封顶与抖动见 restartBackoffFor()
                if (worker.crashCount > 0)
                {
                    const std::chrono::milliseconds backoff =
                            restartBackoffFor(m_configuration.restartBackoff, worker.crashCount, m_restartJitterDistribution(m_restartJitterGenerator));
                    LOG_WARN_FMT("WorkerSupervisor: worker {} 连续第 {} 次崩溃，补位前退避 {} 毫秒", workerIndex, worker.crashCount, backoff.count());
                    std::this_thread::sleep_for(backoff);
                    // 退避期间来了停止请求就到此为止：剩下那些槽位不该在收尾时补出新的 worker，
                    // 交给外层的循环条件收（少补一个是「收尾更快」，多补一个是「收尾时还得起新进程」）
                    if (m_isStopRequested.load(std::memory_order_acquire))
                    {
                        return;
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

    Platform::Process::LaunchOptions WorkerSupervisor::buildLaunchOptions(const std::size_t workerIndex) const
    {
        Platform::Process::LaunchOptions launchOptions;
        launchOptions.executablePath = m_configuration.executablePath;
        launchOptions.arguments      = m_configuration.workerArguments;
        // 槽位序号按进程错开东西的调用方需要它（补起的 worker 沿用同一号，所以号与端口是一一对得上的）。
        // 追加在固定参数之后、移交参数之前：两者都在尾部，先后无所谓，但序号必须在移交之前落定，
        // 否则读参数的那一侧看到的位置会随平台变
        if (!m_configuration.workerIndexArgument.empty())
        {
            launchOptions.arguments.push_back(m_configuration.workerIndexArgument);
            launchOptions.arguments.push_back(std::to_string(workerIndex));
        }
        // 每个 worker 进自己名下的进程组：这是 Windows 上 requestTermination() 发得出 CTRL_BREAK 的前提
        // （组是控制台事件的投递单位，不给独立组就只能整组广播，那会打断 master 自己）。
        // POSIX 上这个字段无效果——那边的体面退出走 SIGTERM
        launchOptions.ownProcessGroup = true;
        // 子进程随 master 消失：master 被硬杀（Taskkill /F、OOM killer、容器被删）时不留一群还在占端口
        // 却没人编排的 worker。POSIX 侧这条由 spawn 内装的 PDEATHSIG 兜住，Windows 靠作业句柄
        launchOptions.killWithParent = true;
        return launchOptions;
    }

    bool WorkerSupervisor::startWorker(Worker &worker, const std::size_t workerIndex)
    {
        Platform::Process::LaunchOptions launchOptions = buildLaunchOptions(workerIndex);

#if ASYN_PLATFORM_WIN32
        // 移交模式：通道要先开好——对方进程靠参数里那个地址连回来，而它连回来之后才谈得上按它的
        // 进程号复制一份监听引用过去。每次（重）起都开一条新的，因为一条通道只交给一个对端
        auto handoffChannel = UpgradeChannel::open();
        if (!handoffChannel)
        {
            noteFailedStart(worker, workerIndex, "开不出交接通道：" + handoffChannel.error().message);
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

#if !ASYN_PLATFORM_WIN32
        // 心跳管道（细节见 attachWatchdogPipe）：没启用看门狗时它什么都不做
        if (!attachWatchdogPipe(worker, workerIndex, launchOptions))
        {
            return false;
        }
#endif

        worker.handle    = Platform::Process::spawn(launchOptions);
        worker.startTime = std::chrono::steady_clock::now();
        if (!worker.handle.isValid())
        {
            // 起不来不重试：把该槽位按「起来就崩」记一次，连续到上限就放弃，免得把日志刷满
            closeWorkerHeartbeatPipe(worker);
#if !ASYN_PLATFORM_WIN32
            // 写端由 attachWatchdogPipe 记在成员上：spawn 没成功就没人会用它，下一次起 worker
            // 会直接覆盖这个槽位，不在这里关就是每失败一次漏一个描述符
            if (m_pendingWatchdogWriteFd >= 0)
            {
                static_cast<void>(::close(m_pendingWatchdogWriteFd));
                m_pendingWatchdogWriteFd = -1;
            }
#endif
            noteFailedStart(worker, workerIndex, "平台错误码 " + std::to_string(Platform::PlatformError::lastErrorCode()));
            return false;
        }

#if !ASYN_PLATFORM_WIN32
        if (m_pendingWatchdogWriteFd >= 0)
        {
            // master 侧不留写端副本：留着的副本会让「worker 是不是还在写」这件事永远为真
            static_cast<void>(::close(m_pendingWatchdogWriteFd));
            m_pendingWatchdogWriteFd = -1;
        }
#endif

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
            return retireWithoutListener("等 worker 连上交接通道：" + peer.error().message);
        }
        const auto isHandedOver = handoffChannel->handOffListener(*peer, m_configuration.handoff->listeningDescriptor, static_cast<std::uint64_t>(worker.handle.processId()));
        // 通道交完就没用了：本函数不关它由调用方收，这里显式收口，别留一条还能连的通道
        static_cast<void>(Platform::FileDescriptor::close(*peer));
        if (!isHandedOver)
        {
            return retireWithoutListener("交出监听套接字：" + isHandedOver.error().message);
        }
        LOG_INFO_FMT("WorkerSupervisor: worker {} 已接手监听套接字（进程号 {}）", workerIndex, worker.handle.processId());
#endif

        return true;
    }

    void WorkerSupervisor::noteFailedStart(Worker &worker, const std::size_t workerIndex, const std::string &failureReason)
    {
        noteWorkerCrash(worker);
        LOG_ERROR_FMT("WorkerSupervisor: worker {} 起不来（{}），这是连续第 {} 次。路径 {}", workerIndex, failureReason, worker.crashCount, m_configuration.executablePath);
        if (worker.crashCount >= m_configuration.crashLoopLimit)
        {
            noteWorkerGivenUp(worker);
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
        closeWorkerHeartbeatPipe(worker);

        if (isFastExit)
        {
            noteWorkerCrash(worker);
            if (worker.crashCount >= m_configuration.crashLoopLimit)
            {
                noteWorkerGivenUp(worker);
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
