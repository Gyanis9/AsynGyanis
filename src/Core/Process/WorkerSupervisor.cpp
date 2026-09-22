#include "Core/Process/WorkerSupervisor.h"

#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"
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

    WorkerSupervisor::WorkerSupervisor(Configuration configuration) :
        m_configuration(std::move(configuration))
    {
        if (m_configuration.executablePath.empty())
        {
            throw CoreException("多进程编排无法启动：可执行文件路径为空。请在配置里给出服务器可执行文件的路径");
        }
        if (m_configuration.workerCount < 2)
        {
            throw CoreException("多进程编排要求 worker 数至少为 2（当前 " + std::to_string(m_configuration.workerCount) +
                                "）。只想跑单进程时不要构造 WorkerSupervisor，直接启动服务器即可");
        }
        if (m_configuration.pollInterval <= std::chrono::milliseconds::zero() || m_configuration.shutdownTimeout <= std::chrono::milliseconds::zero())
        {
            throw CoreException("多进程编排的轮询间隔与收尾期限都必须大于 0，否则循环会空转或收尾没有期限");
        }
#if ASYN_PLATFORM_WIN32
        // Windows 上没有 SO_REUSEPORT 的等价物，端口共享无从谈起：这里当场拒绝，
        // 而不是让调用方拿到一个「启动了多个进程但只有一个能绑定端口」的假成功
        throw CoreException("Windows 不支持多进程 worker 模型：端口共享依赖 SO_REUSEPORT，而 Windows 没有等价物。"
                            "请把 workers 设为 1（单进程 + 多工作循环），或改在 Linux 上部署");
#else
        m_workers.resize(m_configuration.workerCount);
#endif
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
            explicit SignalRegistration(WorkerSupervisor &supervisor) noexcept
                : m_previousTerminateHandler(std::signal(SIGTERM, &handleStopSignal)),
                  m_previousInterruptHandler(std::signal(SIGINT, &handleStopSignal))
            {
                // 先装 handler 再发布指针：handler 只在指针非空时才转达，装反的一拍里
                // 信号最多被当成「没人要停」丢掉，而不是解引用一个还没定下来的 this
                g_runningSupervisor.store(&supervisor, std::memory_order_release);
            }

            SignalRegistration(const SignalRegistration &) = delete;
            SignalRegistration &operator=(const SignalRegistration &) = delete;

            /// 还原先前两个 handler 并收回全局指针：之后再收到停止信号就与本编排器无关了
            ~SignalRegistration()
            {
                std::signal(SIGTERM, m_previousTerminateHandler);
                std::signal(SIGINT, m_previousInterruptHandler);
                g_runningSupervisor.store(nullptr, std::memory_order_release);
            }

        private:
            void (*m_previousTerminateHandler)(int);  ///< 被本登记换掉的 SIGTERM 处理函数
            void (*m_previousInterruptHandler)(int);   ///< 被本登记换掉的 SIGINT 处理函数
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
                LOG_ERROR_FMT("WorkerSupervisor: {} 个 worker 全部因「起来就崩」被放弃，编排退出（请检查可执行文件与配置）",
                              givenUpWorkerCount);
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

        worker.handle    = Platform::Process::spawn(launchOptions);
        worker.startTime = std::chrono::steady_clock::now();
        if (!worker.handle.isValid())
        {
            // 起不来不重试：把该槽位按「起来就崩」记一次，连续到上限就放弃，免得把日志刷满
            ++worker.crashCount;
            LOG_ERROR_FMT("WorkerSupervisor: worker {} 起不来（平台错误码 {}），这是连续第 {} 次。路径 {}", workerIndex,
                          Platform::PlatformError::lastErrorCode(), worker.crashCount, m_configuration.executablePath);
            if (worker.crashCount >= m_configuration.crashLoopLimit)
            {
                worker.isGivenUp = true;
                LOG_ERROR_FMT("WorkerSupervisor: worker {} 连续 {} 次起不来，放弃补它", workerIndex, worker.crashCount);
            }
            return false;
        }

        LOG_INFO_FMT("WorkerSupervisor: worker {} 已启动，进程号 {}", workerIndex, worker.handle.processId());
        return true;
    }

    bool WorkerSupervisor::reapWorker(Worker &worker, const std::size_t workerIndex)
    {
        const std::optional<int> exitCode = Platform::Process::pollExitCode(worker.handle);
        const bool               isFastExit = std::chrono::steady_clock::now() - worker.startTime < m_configuration.crashLoopWindow;
        LOG_INFO_FMT("WorkerSupervisor: worker {} 已退出（进程号 {}，退出码 {}，存活 {} 毫秒）{}", workerIndex, worker.handle.processId(),
                     exitCode.value_or(-1),
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - worker.startTime).count(),
                     isFastExit ? "，按「起来就崩」记一次" : "");
        worker.handle.close();

        if (isFastExit)
        {
            ++worker.crashCount;
            if (worker.crashCount >= m_configuration.crashLoopLimit)
            {
                worker.isGivenUp = true;
                LOG_ERROR_FMT("WorkerSupervisor: worker {} 连续 {} 次存活不足 {} 毫秒就退出，放弃补它（请检查它的启动日志）",
                              workerIndex, worker.crashCount, m_configuration.crashLoopWindow.count());
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
            // 平台不支持（Windows）或请求发不出去时直接强杀，避免收尾卡在这里
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
                LOG_WARN_FMT("WorkerSupervisor: worker 进程号 {} 在强杀后仍未结束，句柄已释放但没回收（POSIX 上可能留下僵尸）",
                             worker.handle.processId());
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
            const bool hasUnreapedWorker = std::ranges::any_of(m_workers,
                                                               [](const Worker &worker)
                                                               {
                                                                   return worker.handle.isValid() &&
                                                                          Platform::Process::isRunning(worker.handle);
                                                               });
            if (!hasUnreapedWorker)
            {
                return;
            }
            std::this_thread::sleep_for(kReapPollInterval);
        }
    }
} // namespace AsynGyanis::Core
