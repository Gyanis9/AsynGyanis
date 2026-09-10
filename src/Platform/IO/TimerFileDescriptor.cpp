#include "Platform/IO/TimerFileDescriptor.h"
#include "Platform/IO/FileDescriptor.h"

#include <algorithm>

namespace AsynGyanis::Platform
{
    namespace
    {
#if ASYN_PLATFORM_WIN32
        /// TimerQueue 的 dueTime 以 DWORD 毫秒计，超出上限会立即到期
        constexpr DWORD kMaximumDueTimeMilliseconds = 0x7FFF'FFFFUL;
#endif
    } // namespace

    TimerFileDescriptor::TimerFileDescriptor()
    {
#if ASYN_PLATFORM_LINUX
        m_fileDescriptor = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#else
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        if (FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            m_fileDescriptor  = readDescriptor;
            m_writeDescriptor = writeDescriptor;
        }
#endif
    }

    TimerFileDescriptor::~TimerFileDescriptor()
    {
        cancel();
        FileDescriptor::close(m_fileDescriptor);
#if ASYN_PLATFORM_WIN32
        FileDescriptor::close(m_writeDescriptor);
#endif
    }

    int TimerFileDescriptor::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    bool TimerFileDescriptor::isValid() const noexcept
    {
#if ASYN_PLATFORM_LINUX
        return FileDescriptor::isValid(m_fileDescriptor);
#else
        return FileDescriptor::isValid(m_fileDescriptor) && FileDescriptor::isValid(m_writeDescriptor);
#endif
    }

    void TimerFileDescriptor::arm(const std::chrono::milliseconds duration) noexcept
    {
        if (duration.count() <= 0)
        {
            cancel();
            return;
        }

#if ASYN_PLATFORM_LINUX
        if (!isValid())
        {
            return;
        }
        itimerspec timerSpec{};
        timerSpec.it_value.tv_sec  = duration.count() / 1000;
        timerSpec.it_value.tv_nsec = (duration.count() % 1000) * 1000 * 1000;
        ::timerfd_settime(m_fileDescriptor, 0, &timerSpec, nullptr);
#else
        cancel();
        if (!isValid())
        {
            return;
        }
        const auto dueTimeMilliseconds = static_cast<DWORD>(std::min<long long>(duration.count(), kMaximumDueTimeMilliseconds));
        HANDLE     timerHandle         = nullptr;
        if (::CreateTimerQueueTimer(&timerHandle, nullptr, &TimerFileDescriptor::timerCallback, this, dueTimeMilliseconds, 0, WT_EXECUTEONLYONCE | WT_EXECUTEINTIMERTHREAD))
        {
            m_timerHandle = timerHandle;
        }
#endif
    }

    void TimerFileDescriptor::cancel() noexcept
    {
#if ASYN_PLATFORM_LINUX
        if (!isValid())
        {
            return;
        }
        // 零值 itimerspec 表示解除已设定的定时器
        const itimerspec disabled{};
        ::timerfd_settime(m_fileDescriptor, 0, &disabled, nullptr);
#else
        if (m_timerHandle != nullptr)
        {
            const HANDLE finishedHandle = m_timerHandle;
            m_timerHandle               = nullptr;
            // 传 INVALID_HANDLE_VALUE 使调用阻塞至回调结束，确保返回后无残留通知
            ::DeleteTimerQueueTimer(nullptr, finishedHandle, INVALID_HANDLE_VALUE);
        }
#endif
    }

    void TimerFileDescriptor::drain() const noexcept
    {
        if (!isValid())
        {
            return;
        }
        // 读空计数：Linux 返回到期次数，Windows 返回 socket 对累积的标记字节
        char buffer[64];
        while (FileDescriptor::read(m_fileDescriptor, buffer, sizeof(buffer)) > 0)
        {
        }
    }

#if ASYN_PLATFORM_WIN32
    VOID CALLBACK TimerFileDescriptor::timerCallback(PVOID context, BOOLEAN timerOrWaitFired)
    {
        (void) timerOrWaitFired;
        auto *timer = static_cast<TimerFileDescriptor *>(context);
        if (timer != nullptr && timer->m_writeDescriptor >= 0)
        {
            // 回调运行于系统线程池，写端不可写时忽略本次到期
            const char marker = 1;
            FileDescriptor::write(timer->m_writeDescriptor, &marker, sizeof(marker));
        }
    }
#endif
} // namespace AsynGyanis::Platform
