#include "Timer.h"
#include "EventLoop.h"
#include "Base/Exception.h"

#include <sys/timerfd.h>

namespace Core
{
    Timer::Timer(EventLoop &loop) :
        m_loop(loop)
    {
        m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (m_timerFd < 0)
        {
            throw Base::SystemException("timerfd_create failed");
        }
    }

    EpollAwaiter Timer::waitFor(const std::chrono::milliseconds duration) const
    {
        itimerspec ts{};
        ts.it_value.tv_sec  = duration.count() / 1000;
        ts.it_value.tv_nsec = (duration.count() % 1000) * 1000000;
        timerfd_settime(m_timerFd, 0, &ts, nullptr);
        return EpollAwaiter(m_loop.epoll(), m_timerFd, EPOLLIN);
    }

}
