#include "EventLoop.h"
#include "Base/Exception.h"

#include <sys/eventfd.h>
#include <unistd.h>

namespace Core
{
    EventLoop::EventLoop()
    {
        m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wakeupFd < 0)
        {
            throw Base::SystemException("eventfd creation failed");
        }
        m_epoll.addFd(m_wakeupFd, EPOLLIN, &m_wakeupSentinel);
    }

    EventLoop::~EventLoop()
    {
        if (m_running.load(std::memory_order_acquire))
            stop();

        if (m_wakeupFd >= 0)
        {
            close(m_wakeupFd);
            m_wakeupFd = -1;
        }
    }

    void EventLoop::run()
    {
        m_running.store(true, std::memory_order_release);

        while (!m_stopRequested.load(std::memory_order_acquire))
        {
            m_scheduler.runAll();
            if (m_stopRequested.load(std::memory_order_acquire))
                break;

            for (auto events = m_epoll.wait(1); const auto &ev: events)
            {
                if (ev.data.ptr == &m_wakeupSentinel)
                {
                    uint64_t val;
                    read(m_wakeupFd, &val, sizeof(val));
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
        constexpr uint64_t val = 1;
        ::write(m_wakeupFd, &val, sizeof(val));
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
