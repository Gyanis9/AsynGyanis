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


    std::chrono::milliseconds WorkerSupervisor::enableWatchdogFeeding()
    {
        if (m_configuration.watchdogNotification != nullptr && Platform::ServiceNotification::supportsWatchdog())
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
        // 轮询粒度：喂点判定的分辨率必须细于节拍的一半，否则「到点」会被睡眠粒度量成「晚点」。
        // 没启用时保持调用方给的 pollInterval
        return m_isWatchdogFeedingEnabled ? std::min(m_configuration.pollInterval, std::max(m_watchdogPingInterval / 2, std::chrono::milliseconds{1}))
                                          : m_configuration.pollInterval;
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
        if (!m_isWatchdogFeedingEnabled)
        {
            return true;
        }
        // 读端归本进程（CLOEXEC + 非阻塞——凑齐判定只看「有没有新字节」，阻塞读会把编排循环钉死），
        // 写端随 fork 继承给 worker，号码进参数。重起同一槽位每次都开新的一条：旧管道随旧 worker
        // 一起作废（reapWorker 里关），不复用
        std::optional<Platform::Pipe> heartbeatPipe = Platform::Pipe::create(true);
        if (!heartbeatPipe)
        {
            noteFailedStart(worker, workerIndex, "开不出心跳管道，平台错误码 " + std::to_string(Platform::PlatformError::lastErrorCode()));
            return false;
        }
        worker.heartbeatReadFd = heartbeatPipe->releaseReadEnd();
        if (!Platform::FileDescriptor::setNonBlocking(worker.heartbeatReadFd))
        {
            // 阻塞读会把编排循环钉在一条没人写的管道上：设不成非阻塞就当场放弃这个槽位
            const int failure = Platform::PlatformError::lastErrorCode();
            closeWorkerHeartbeatPipe(worker);
            noteFailedStart(worker, workerIndex, "心跳管道读端设不成非阻塞，平台错误码 " + std::to_string(failure));
            return false;
        }
        m_pendingWatchdogWriteFd = heartbeatPipe->releaseWriteEnd();

        launchOptions.arguments.push_back(std::string{kWatchdogPipeArgument});
        launchOptions.arguments.push_back(std::to_string(m_pendingWatchdogWriteFd));
        launchOptions.arguments.push_back(std::string{kWatchdogPingArgument});
        launchOptions.arguments.push_back(std::to_string(m_watchdogPingInterval.count()));
        return true;
    }

    void WorkerSupervisor::pollWorkerHeartbeats(const std::chrono::steady_clock::time_point now) noexcept
    {
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
            while (Platform::FileDescriptor::read(worker.heartbeatReadFd, drainBuffer.data(), drainBuffer.size()) > 0)
            {
                // 记的是「上一拍什么时候到的」而不是「这一轮读到过没有」：后者与出拍共用同一个周期，
                // 检查抢到拍子之前完成的那一轮会把标志清掉，于是永远喂不出去（判据见 isHeartbeatFresh）
                worker.lastHeartbeatAt = now;
            }
        }
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
        if (worker.heartbeatReadFd >= 0)
        {
            static_cast<void>(Platform::FileDescriptor::close(worker.heartbeatReadFd));
            worker.heartbeatReadFd = -1;
        }
        // 起 worker 失败时写端还挂在成员上：下一次 attachWatchdogPipe 会覆盖它，
        // 不在这里关就是每失败一次漏一个描述符
        if (m_pendingWatchdogWriteFd >= 0)
        {
            static_cast<void>(Platform::FileDescriptor::close(m_pendingWatchdogWriteFd));
            m_pendingWatchdogWriteFd = -1;
        }
    }

} // namespace AsynGyanis::Core
