#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cstdint>

namespace AsynGyanis::Core
{
    Timer::Awaiter::Awaiter(IoWatcher &watcher, const int fileDescriptor) noexcept :
        m_awaiter(watcher.waitReadable()), m_fileDescriptor(fileDescriptor)
    {
    }

    bool Timer::Awaiter::await_ready() noexcept
    {
        return m_awaiter.await_ready();
    }

    bool Timer::Awaiter::await_suspend(const std::coroutine_handle<> handle)
    {
        return m_awaiter.await_suspend(handle);
    }

    void Timer::Awaiter::await_resume()
    {
        // 注册已失效（定时器被销毁）时没什么可读的，直接返回；等待方按「未超时」处理
        if (!m_awaiter.await_resume())
        {
            return;
        }

        // 读走过期计数。这一步不是可选的：timerfd 的「可读」状态要靠读清掉，
        // 只等不读会让它一直保持可读，而边沿触发不会再报第二次——下一次等待就再也等不到
        uint64_t expirations = 0;
        [[maybe_unused]] auto _ = Platform::FileDescriptor::read(m_fileDescriptor, &expirations, sizeof(expirations));
    }

    Timer::Timer(EventLoop &loop) :
        m_loop(loop), m_watcher(loop, m_timer.fileDescriptor(), EPOLLIN)
    {
        if (m_timer.fileDescriptor() < 0)
        {
            // 定时器描述符建不起来说明本平台不支持该机制（Linux 上为 timerfd）：
            // 这是不可恢复的启动期故障，带着 errno 抛出便于定位
            throw Base::SystemException("创建定时器文件描述符失败");
        }
    }

    Timer::~Timer() = default;

    Timer::Awaiter Timer::waitFor(const std::chrono::milliseconds duration)
    {
        m_timer.arm(duration);
        return Awaiter(m_watcher, m_timer.fileDescriptor());
    }

}
