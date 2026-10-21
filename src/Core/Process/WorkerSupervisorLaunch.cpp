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

        // 起来了却拿不到监听引用的进程不会服务任何连接：收掉它、把这个槽位按一次「起来就崩」记上。
        // 留着它等于占着一个名额却谁也不接，收尾时还要多等一轮
        const auto retireWithoutListener = [&](const std::string &failureReason)
        {
            static_cast<void>(Platform::Process::forceTermination(worker.handle));
            // 强杀的投递与调度有个短窗口：等到它真的结束再丢句柄，否则没有任何人收这个尸
            // （isRunning() 观察时顺手回收，这一轮询同时完成「等结束」与「收尸」）
            const auto reapDeadline = std::chrono::steady_clock::now() + kWorkerForcedReapWait;
            while (std::chrono::steady_clock::now() < reapDeadline && Platform::Process::isRunning(worker.handle))
            {
                std::this_thread::sleep_for(kWorkerReapPollInterval);
            }
            worker.handle = Platform::Process::Handle{};
            noteFailedStart(worker, workerIndex, failureReason);
            return false;
        };

        std::optional<UpgradeChannel> handoffChannel;
        if (Platform::Socket::requiresListenerHandoff())
        {
            // 移交模式：通道要先开好——对方进程靠参数里那个地址连回来，而它连回来之后才谈得上按它的
            // 进程号复制一份监听引用过去。每次（重）起都开一条新的，因为一条通道只交给一个对端
            auto openedChannel = UpgradeChannel::open();
            if (!openedChannel)
            {
                noteFailedStart(worker, workerIndex, "开不出交接通道：" + openedChannel.error().message);
                return false;
            }
            handoffChannel.emplace(std::move(*openedChannel));
            launchOptions.arguments.push_back(std::string{kHandedOverListenerArgument});
            launchOptions.arguments.push_back(handoffChannel->address());
        }

        // 心跳管道（细节见 attachWatchdogPipe）：没启用看门狗时它什么都不做
        if (!attachWatchdogPipe(worker, workerIndex, launchOptions))
        {
            return false;
        }

        worker.handle    = Platform::Process::spawn(launchOptions);
        worker.startTime = std::chrono::steady_clock::now();
        if (!worker.handle.isValid())
        {
            // 起不来不重试：把该槽位按「起来就崩」记一次，连续到上限就放弃，免得把日志刷满
            closeWorkerHeartbeatPipe(worker);
            noteFailedStart(worker, workerIndex, "平台错误码 " + std::to_string(Platform::PlatformError::lastErrorCode()));
            return false;
        }

        if (m_pendingWatchdogWriteFd >= 0)
        {
            // master 侧不留写端副本：留着的副本会让「worker 是不是还在写」这件事永远为真
            static_cast<void>(Platform::FileDescriptor::close(m_pendingWatchdogWriteFd));
            m_pendingWatchdogWriteFd = -1;
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

        if (handoffChannel.has_value())
        {
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
        }

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

} // namespace AsynGyanis::Core
