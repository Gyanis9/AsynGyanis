#include "Timer.h"
#include "EventLoop.h"
#include "Base/Exception.h"
#include "Platform/SocketCompat.h"

#include <cstdint>

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

    void Timer::Awaiter::await_suspend(const std::coroutine_handle<> handle) const noexcept
    {
        m_awaiter.await_suspend(handle);
    }

    void Timer::Awaiter::await_resume() const
    {
        uint64_t expirations = 0;

        [[maybe_unused]] auto _ = Platform::readFd(m_fd, &expirations, sizeof(expirations));
        m_awaiter.await_resume();
    }

    Timer::Timer(EventLoop &loop) :
        m_loop(loop)
    {
        if (m_timer.fd() < 0)
        {
            throw Base::SystemException("TimerFd creation failed");
        }
    }

    Timer::~Timer()
    {
        // 先移除 epoll 注册，TimerFd 析构会自动关闭 fd
        m_loop.epoll().delFd(m_timer.fd());
    }

    Timer::Awaiter Timer::waitFor(const std::chrono::milliseconds duration)
    {
        m_timer.arm(duration);
        return Awaiter(m_loop.epoll(), m_timer.fd());
    }

}
