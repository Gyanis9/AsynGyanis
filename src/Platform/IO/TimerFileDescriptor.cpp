#include "Platform/IO/TimerFileDescriptor.h"
#include "Platform/IO/FileDescriptor.h"

#include <algorithm>

namespace AsynGyanis::Platform
{
    namespace
    {
#if ASYN_PLATFORM_WIN32
        /// 可等待定时器与线程池等待的时长都以 DWORD 毫秒计，超限必须截断，否则会绕回成更早的时刻
        constexpr DWORD kMaximumDueTimeMilliseconds = 0x7FFF'FFFFUL;

        /// 可等待定时器的 due 以 100 纳秒为单位：1 毫秒 = 10'000 个单位
        constexpr LONGLONG kHundredNanosecondsPerMillisecond = 10'000LL;

        /**
         * @brief 造一个可等待定时器，优先要高精度档
         * @details 普通档的到期按系统时钟节度取整（实测 15.6 ms 一节：等 20 ms 实际 31 ms 才醒），
         *          对限速、退避、这类短定时是成倍的尾延迟。高精度档要 Windows 10 1803 以上，
         *          拿不到时退回普通档——精度回到旧行为，而不是让定时器直接不可用。
         * @return HANDLE 定时器句柄；两档都失败时为 nullptr
         */
        HANDLE createWaitableTimer()
        {
            if (HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); timer != nullptr)
            {
                return timer;
            }
            return ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
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

        m_waitableTimer = createWaitableTimer();
        if (m_waitableTimer != nullptr)
        {
            // 一次登记长期有效：每次到期后线程池自己重挂等待，因此武装只是改期，不再建/删内核对象。
            // 等待上限取最大毫秒数（约 24.8 天），正常生命周期内不会因等待超时而误醒
            if (!::RegisterWaitForSingleObject(&m_waitRegistration, m_waitableTimer, &TimerFileDescriptor::timerCallback, this, kMaximumDueTimeMilliseconds,
                                               WT_EXECUTEINTIMERTHREAD))
            {
                m_waitRegistration = nullptr;
                ::CloseHandle(m_waitableTimer);
                m_waitableTimer = nullptr;
            }
        }
#endif
    }

    TimerFileDescriptor::~TimerFileDescriptor()
    {
        cancel();
#if ASYN_PLATFORM_WIN32
        // 注销等待必须阻塞等到回调跑完，而且要在关描述符之前：回调写的是 socket 对写端，
        // 描述符号一旦被别的套接字复用，那次残留的写就是往陌生连接里塞一个字节
        if (m_waitRegistration != nullptr)
        {
            ::UnregisterWaitEx(m_waitRegistration, INVALID_HANDLE_VALUE);
            m_waitRegistration = nullptr;
        }
        if (m_waitableTimer != nullptr)
        {
            ::CloseHandle(m_waitableTimer);
            m_waitableTimer = nullptr;
        }
#endif
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
        return FileDescriptor::isValid(m_fileDescriptor) && FileDescriptor::isValid(m_writeDescriptor) && m_waitableTimer != nullptr && m_waitRegistration != nullptr;
#endif
    }

    bool TimerFileDescriptor::arm(const std::chrono::milliseconds duration) noexcept
    {
        if (duration.count() <= 0)
        {
            cancel();
            return true;
        }

#if ASYN_PLATFORM_LINUX
        if (!isValid())
        {
            return false;
        }
        itimerspec timerSpec{};
        timerSpec.it_value.tv_sec  = duration.count() / 1000;
        timerSpec.it_value.tv_nsec = (duration.count() % 1000) * 1000 * 1000;
        // 返回值必须报出去：定时器没设上就永远不会到期，而调用方正等着这一次唤醒
        return ::timerfd_settime(m_fileDescriptor, 0, &timerSpec, nullptr) == 0;
#else
        if (!isValid())
        {
            return false;
        }
        // 负 due 是「相对此刻」，与调用方传的剩余时长同口径；也免掉与循环外线程共享绝对时钟的麻烦
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::min<long long>(duration.count(), kMaximumDueTimeMilliseconds)) * kHundredNanosecondsPerMillisecond;
        // period 取 0 即一次性，语义与 Linux 侧不重复触发的 itimerspec 一致
        return ::SetWaitableTimer(m_waitableTimer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
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
        if (m_waitableTimer != nullptr)
        {
            ::CancelWaitableTimer(m_waitableTimer);
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
    VOID CALLBACK TimerFileDescriptor::timerCallback(PVOID context, const BOOLEAN timerOrWaitFired)
    {
        // 这个标志在「等可等待定时器对象」的注册上恒为 FALSE（实测），分不出到期与等待超时，
        // 因此不据它过滤：正常生命周期里只有到期会进来，等待自身超时误醒一次也只多一个字节
        (void) timerOrWaitFired;
        if (const auto timer = static_cast<TimerFileDescriptor *>(context); timer != nullptr && timer->m_writeDescriptor >= 0)
        {
            // 回调运行于线程池，写端不可写时忽略本次到期
            constexpr char kmarker = 1;
            FileDescriptor::write(timer->m_writeDescriptor, &kmarker, sizeof(kmarker));
        }
    }
#endif
} // namespace AsynGyanis::Platform
