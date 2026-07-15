#include "Timer.h"
#include "EventLoop.h"
#include "Base/Exception.h"
#include "Platform/SocketCompat.h"

#include <cstdint>

namespace Core
{
    Timer::Awaiter::Awaiter(Epoll &epoll, const int fileDescriptor) noexcept :
        m_awaiter(epoll, fileDescriptor, EPOLLIN), m_fileDescriptor(fileDescriptor)
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

        [[maybe_unused]] auto _ = Platform::readFileDescriptor(m_fileDescriptor, &expirations, sizeof(expirations));
        m_awaiter.await_resume();
    }

    Timer::Timer(EventLoop &loop) :
        m_loop(loop)
    {
        if (m_timer.fileDescriptor() < 0)
        {
            throw Base::SystemException("TimerFileDescriptor creation failed");
        }
    }

    Timer::~Timer()
    {
        // 先移除 epoll 注册，TimerFileDescriptor 析构会自动关闭文件描述符
        m_loop.epoll().delFileDescriptor(m_timer.fileDescriptor());
    }

    Timer::Awaiter Timer::waitFor(const std::chrono::milliseconds duration)
    {
        m_timer.arm(duration);
        return Awaiter(m_loop.epoll(), m_timer.fileDescriptor());
    }

}
