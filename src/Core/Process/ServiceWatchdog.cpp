#include "Core/Process/ServiceWatchdog.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <utility>

namespace AsynGyanis::Core
{
    ServiceWatchdog::ServiceWatchdog(ThreadPool &pool, Platform::ServiceNotification &notification) noexcept : m_pool(pool), m_notification(notification)
    {
    }

    ServiceWatchdog::~ServiceWatchdog()
    {
        // 拆的是拍协程的帧：调用方必须已经 stop() 过线程池（工作线程已 join），否则这里销毁的
        // 是一帧可能正被恢复的协程——Task 的析构是无条件 destroy()，没有「已投递未派发」的窗口保护
    }

    bool ServiceWatchdog::completesRound(const std::vector<std::uint64_t> &progresses, const std::size_t index) noexcept
    {
        // 下标越界只可能是编号表被改坏（那条循环根本不存在）。这一格取「不喂」而不是抛：
        // 在一个认不出自己有几条循环的状态下把狗喂了，报出去的是「整个运行时还在推进」这个假证
        if (index >= progresses.size())
        {
            return false;
        }

        // 本轮的编号就是「我刚走完第几拍」：每条循环都要至少走到这一拍，整轮才算齐
        const std::uint64_t round = progresses[index];
        for (const std::uint64_t progressed: progresses)
        {
            if (progressed < round)
            {
                return false;
            }
        }
        return true;
    }

    bool ServiceWatchdog::arm()
    {
        {
            // 重复调用是空操作：两条节拍会把同一轮喂成两条 WATCHDOG=1，
            // 而「这条通道发过几次」那笔账正是运维要看的
            const std::lock_guard lock(m_stateMutex);
            if (m_isArmed.load(std::memory_order_acquire))
            {
                return true;
            }
        }

        const auto configuration = Platform::ServiceNotification::readWatchdogConfiguration();
        if (!configuration.has_value())
        {
            const std::lock_guard lock(m_stateMutex);
            m_lastError = configuration.error();
            return false;
        }

        if (!m_notification.isOpen())
        {
            // 配了窗口却没开通知通路：这两个变量在 systemd 那边是同一件事的两半，分开出现就是
            // 部署方自己拼的启动环境。不报出来的话，监督者等的是一个永远不会有的回执
            const std::lock_guard lock(m_stateMutex);
            m_lastError = std::format("看门狗窗口是 {} 微秒，但通知通路还没打开：先对 ServiceNotification 调 open()，"
                                      "或确认这次启动本来就不在监督之下",
                                      configuration->timeoutWindow.count());
            return false;
        }

        const std::size_t loopCount = m_pool.threadCount();
        {
            const std::lock_guard lock(m_stateMutex);
            m_progresses.assign(loopCount, 0);
            m_pingIntervalMicroseconds.store(configuration->pingInterval.count(), std::memory_order_relaxed);
            m_lastError.clear();
        }

        // 先入表、后排度：交出去的是裸句柄，而本对象是这些帧唯一的持有者，
        // 入表这一步抛出就先留下一个没人持有却能被打发的句柄（见 Scheduler 那条帧归属注释）
        m_tickTasks.reserve(loopCount);
        for (std::size_t index = 0; index < loopCount; ++index)
        {
            m_tickTasks.emplace_back(tickLoop(index));
        }
        for (std::size_t index = 0; index < loopCount; ++index)
        {
            // 排度走 scheduleRemote()：arm() 常在主线程上调用，而那些循环正在别处跑，
            // 本地排队会被当场拒掉（Scheduler::schedule() 的线程判据）
            m_pool.scheduler(index).scheduleRemote(m_tickTasks[index].handle());
        }

        // 标记排在最后：拍数表、节拍与那批帧都落好了才对外说「已挂上」，
        // 别的线程读到 isArmed() 为真时不会撞上一个还在半路的装配现场
        m_isArmed.store(true, std::memory_order_release);

        LOG_INFO_FMT("ServiceWatchdog: 看门狗节拍已挂上：窗口 {} 微秒，每 {} 毫秒喂一条 WATCHDOG=1，{} 条循环各占一拍（凑齐一轮才喂）", configuration->timeoutWindow.count(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(configuration->pingInterval).count(), loopCount);
        return true;
    }

    Task<> ServiceWatchdog::tickLoop(const std::size_t index)
    {
        // 本协程只碰自己那条循环的定时器：Timer 与它的等待器都按循环归属，跨循环用就是数据竞态
        Timer timer{m_pool.eventLoop(index)};

        // 向下取整：节拍只会比折出的值更早到，不会更晚——看门狗怕的是晚到，早一毫秒不构成任何后果。
        // 窗口折不出至少 1 毫秒的节拍在 Platform 那一层就已经被拒了，这里不会睡成 0（那是忙等）
        const std::chrono::milliseconds interval =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(m_pingIntervalMicroseconds.load(std::memory_order_relaxed)));

        while (true)
        {
            co_await timer.waitFor(interval);

            bool shouldPing = false;
            {
                const std::lock_guard lock(m_stateMutex);
                ++m_progresses[index];
                // 醒来的这一刻这条循环确实在推进；只有把最后那一拍补齐的那条循环才发，
                // 于是每轮恰好一条 WATCHDOG=1，而任何一条停摆都会让这一轮永远凑不齐
                shouldPing = completesRound(m_progresses, index);
                if (shouldPing)
                {
                    sendPingLocked();
                }
            }
        }
    }

    void ServiceWatchdog::sendPingLocked()
    {
        // 计数在发送之前落：这条读数要回答的是「本进程交出去过几条节拍」，
        // 数据报没有回执，发送失败也一样已经交出（失败原因在下面那条 WARN 里，不在这里）
        m_pingCount.fetch_add(1, std::memory_order_relaxed);

        if (m_notification.send(Platform::ServiceNotification::kWatchdogPingState))
        {
            return;
        }

        // 只记一条 WARN 就回去：这一档的后果由监督者执行（按超时重启本进程），在这里重试会把
        // 「这条节拍发过几次」那笔账弄浑，而循环线程上堵着等通知通路更不是它该干的事
        LOG_WARN_FMT("ServiceWatchdog: WATCHDOG=1 没有送达通知通路：{}（监督者会按看门狗超时处理本进程）", m_notification.lastError());
    }

    bool ServiceWatchdog::isArmed() const noexcept
    {
        return m_isArmed.load(std::memory_order_acquire);
    }

    std::uint64_t ServiceWatchdog::pingCount() const noexcept
    {
        return m_pingCount.load(std::memory_order_relaxed);
    }

    std::chrono::milliseconds ServiceWatchdog::pingInterval() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(m_pingIntervalMicroseconds.load(std::memory_order_relaxed)));
    }

    std::size_t ServiceWatchdog::armedLoopCount() const noexcept
    {
        // 未挂上时那张表还是空的；挂上后它的条数就是「一轮要凑齐几条」
        return m_tickTasks.size();
    }

    const std::string &ServiceWatchdog::lastError() const noexcept
    {
        return m_lastError;
    }
} // namespace AsynGyanis::Core
