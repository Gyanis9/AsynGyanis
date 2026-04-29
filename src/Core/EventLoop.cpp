#include "EventLoop.h"
#include "Base/IOUringContext.h"
#include "Base/Exception.h"
#include "UringAwaiter.h"

#include <liburing.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace Core
{
    EventLoop::EventLoop(const unsigned queueDepth, const unsigned uringFlags) :
        m_uringCtx(std::make_unique<Base::IOUringContext>(queueDepth, uringFlags)),
        m_uring(*m_uringCtx)
    {
        m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wakeupFd < 0)
        {
            throw Base::SystemException("eventfd creation failed");
        }
        m_epoll.addFd(m_wakeupFd, EPOLLIN, &m_wakeupSentinel);

        m_uringEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_uringEventFd < 0)
        {
            close(m_wakeupFd);
            throw Base::SystemException("uring eventfd creation failed");
        }
        m_uring.registerEventFd(m_uringEventFd);
        m_epoll.addFd(m_uringEventFd, EPOLLIN, &m_uringSentinel);
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
        if (m_uringEventFd >= 0)
        {
            close(m_uringEventFd);
            m_uringEventFd = -1;
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

            // 100ms timeout as safety net for kernels where io_uring
            // eventfd notification becomes unreliable after re-registration
            for (auto events = m_epoll.wait(100); const auto &ev: events)
            {
                if (ev.data.ptr == &m_wakeupSentinel)
                {
                    uint64_t val;
                    read(m_wakeupFd, &val, sizeof(val));
                    continue;
                }

                if (ev.data.ptr == &m_uringSentinel)
                {
                    harvestUringCompletions();
                    uint64_t val;
                    read(m_uringEventFd, &val, sizeof(val));
                    m_uring.registerEventFd(m_uringEventFd);
                    continue;
                }

                if (ev.data.ptr)
                {
                    const auto handle = std::coroutine_handle<>::from_address(ev.data.ptr);
                    m_scheduler.schedule(handle);
                }
            }

            // Harvest any completions that might have been missed
            // due to eventfd notification issues
            harvestUringCompletions();

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

    Uring &EventLoop::uring() noexcept
    {
        return m_uring;
    }

    Scheduler &EventLoop::scheduler() noexcept
    {
        return m_scheduler;
    }

    bool EventLoop::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void EventLoop::harvestUringCompletions()
    {
        io_uring_cqe *cqe = nullptr;
        while ((cqe = m_uring.peekCqe()) != nullptr)
        {
            if (auto *userData = io_uring_cqe_get_data(cqe))
            {
                UringAwaiter::complete(userData, cqe->res);
                const auto handle = std::coroutine_handle<>::from_address(userData);
                m_scheduler.schedule(handle);
            }
            m_uring.cqeSeen(cqe);
        }
    }

}
