#include "EventLoop.h"
#include "Base/Exception.h"

namespace Core
{
    EventLoop::EventLoop()
    {
        m_scheduler.setWakeupNotifier(&m_wakeup);
        m_epoll.addFileDescriptor(m_wakeup.readFileDescriptor(), EPOLLIN, &m_wakeupSentinel);
    }

    EventLoop::~EventLoop()
    {
        if (m_running.load(std::memory_order_acquire))
            stop();
    }

    void EventLoop::run()
    {
        m_running.store(true, std::memory_order_release);

        while (true)
        {
            m_scheduler.runAll();

            if (m_stopRequested.load(std::memory_order_acquire))
            {
                break;
            }

            const int timeoutMs = m_scheduler.hasWork() ? 0 : -1;
            for (auto events = m_epoll.wait(timeoutMs); const auto &ev: events)
            {
                if (ev.data.ptr == &m_wakeupSentinel)
                {
                    m_wakeup.drain();
                    continue;
                }

                if (ev.data.ptr)
                {
                    const auto handle = std::coroutine_handle<>::from_address(ev.data.ptr);
                    m_scheduler.schedule(handle);
                }
            }

            m_scheduler.runAll();
        }

        m_running.store(false, std::memory_order_release);
    }

    void EventLoop::stop()
    {
        m_stopRequested.store(true, std::memory_order_release);
        wake();
    }

    void EventLoop::wake() const
    {
        m_wakeup.notify();
    }

    Epoll &EventLoop::epoll() noexcept
    {
        return m_epoll;
    }

    Scheduler &EventLoop::scheduler() noexcept
    {
        return m_scheduler;
    }

    bool EventLoop::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

}
