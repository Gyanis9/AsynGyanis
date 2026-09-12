#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cstdint>

namespace AsynGyanis::Core
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

        [[maybe_unused]] auto _ = Platform::FileDescriptor::read(m_fileDescriptor, &expirations, sizeof(expirations));
        m_awaiter.await_resume();
    }

    Timer::Timer(EventLoop &loop) :
        m_loop(loop)
    {
        if (m_timer.fileDescriptor() < 0)
        {
            // 定时器描述符建不起来说明本平台不支持该机制（Linux 上为 timerfd）：
            // 这是不可恢复的启动期故障，带着 errno 抛出便于定位
            throw Base::SystemException("创建定时器文件描述符失败");
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
