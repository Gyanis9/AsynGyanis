#include "Core/Process/ServiceWatchdog.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/ThreadPool.h"

#include <chrono>
#include <format>

namespace AsynGyanis::Core
{
    ServiceWatchdog::ServiceWatchdog(ThreadPool &pool, Platform::ServiceNotification &notification) noexcept :
        m_pool(pool), m_notification(notification), m_ticker(pool, [this]() { sendPing(); })
    {
    }

    ServiceWatchdog::~ServiceWatchdog()
    {
        // 拆的是拍协程的帧：调用方必须已经 stop() 过线程池（工作线程已 join），否则这里销毁的
        // 是一帧可能正被恢复的协程——Task 的析构是无条件 destroy()，没有「已投递未派发」的窗口保护
    }

    bool ServiceWatchdog::completesRound(const std::vector<std::uint64_t> &progresses, const std::size_t index) noexcept
    {
        return RoundTicker::completesRound(progresses, index);
    }

    bool ServiceWatchdog::arm()
    {
        // 重复调用是空操作：两条节拍会把同一轮喂成两条 WATCHDOG=1，
        // 而「这条通道发过几次」那笔账正是运维要看的
        if (m_ticker.isArmed())
        {
            return true;
        }

        const auto configuration = Platform::ServiceNotification::readWatchdogConfiguration();
        if (!configuration.has_value())
        {
            m_lastError = configuration.error();
            return false;
        }

        if (!m_notification.isOpen())
        {
            // 配了窗口却没开通知通路：这两个变量在 systemd 那边是同一件事的两半，分开出现就是
            // 部署方自己拼的启动环境。不报出来的话，监督者等的是一个永远不会有的回执
            m_lastError = std::format("看门狗窗口是 {} 微秒，但通知通路还没打开：先对 ServiceNotification 调 open()，"
                                      "或确认这次启动本来就不在监督之下",
                                      configuration->timeoutWindow.count());
            return false;
        }

        m_pingIntervalMicroseconds.store(configuration->pingInterval.count(), std::memory_order_relaxed);
        m_lastError.clear();
        static_cast<void>(m_ticker.arm(std::chrono::duration_cast<std::chrono::milliseconds>(configuration->pingInterval)));

        LOG_INFO_FMT("ServiceWatchdog: 看门狗节拍已挂上：窗口 {} 微秒，每 {} 毫秒喂一条 WATCHDOG=1，{} 条循环各占一拍（凑齐一轮才喂）", configuration->timeoutWindow.count(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(configuration->pingInterval).count(), m_ticker.armedLoopCount());
        return true;
    }

    void ServiceWatchdog::sendPing()
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
        return m_ticker.isArmed();
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
        return m_ticker.armedLoopCount();
    }

    const std::string &ServiceWatchdog::lastError() const noexcept
    {
        return m_lastError;
    }
} // namespace AsynGyanis::Core
