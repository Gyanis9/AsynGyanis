#include "Timer.h"
#include "EventLoop.h"
#include "Base/Exception.h"

#include <cstdint>
#include <sys/timerfd.h>
#include <unistd.h>

namespace Core
{
    Timer::Awaiter::Awaiter(Epoll &epoll, const int fd) noexcept :
        m_awaiter(epoll, fd, EPOLLIN), m_fd(fd)
    {
    }

    bool Timer::Awaiter::await_ready() const noexcept
    {
        return m_awaiter.await_ready();
    }

    void Timer::Awaiter::await_suspend(const std::coroutine_handle<> handle) noexcept
    {
        m_awaiter.await_suspend(handle);
    }

    void Timer::Awaiter::await_resume() const
    {
        uint64_t expirations = 0;
        (void) ::read(m_fd, &expirations, sizeof(expirations));
        m_awaiter.await_resume();
    }

    Timer::Timer(EventLoop &loop) :
        m_loop(loop)
    {
        m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (m_timerFd < 0)
        {
            throw Base::SystemException("timerfd_create failed");
        }
    }

    Timer::~Timer()
    {
        if (m_timerFd >= 0)
        {
            ::close(m_timerFd);
            m_timerFd = -1;
        }
    }

    Timer::Awaiter Timer::waitFor(const std::chrono::milliseconds duration) const
    {
        itimerspec ts{};
        ts.it_value.tv_sec  = duration.count() / 1000;
        ts.it_value.tv_nsec = (duration.count() % 1000) * 1000000;
        timerfd_settime(m_timerFd, 0, &ts, nullptr);
        return Timer::Awaiter(m_loop.epoll(), m_timerFd);
    }

}
