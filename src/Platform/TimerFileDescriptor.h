/**
 * @file TimerFileDescriptor.h
 * @brief 跨平台定时器文件描述符 — Linux 用 timerfd，Windows 用 socket pair + TimerQueueTimer
 * @copyright Copyright (c) 2026
 */

#ifndef PLATFORM_TIMERFILEDESCRIPTOR_H
#define PLATFORM_TIMERFILEDESCRIPTOR_H

#include "Platform.h"
#include "SocketCompat.h"

#include <chrono>

namespace Platform
{
    /**
     * @brief 跨平台定时器文件描述符
     *
     * Linux: 基于 timerfd_create / timerfd_settime 实现。
     * Windows: 基于 loopback socket pair + CreateTimerQueueTimer 实现，
     *          定时器到期时向 socket 写入数据使其变为可读。
     *
     * 用法：
     * 1. 将 fileDescriptor() 注册到 epoll 监听 EPOLLIN
     * 2. 调用 arm(duration) 设置定时器
     * 3. 被 epoll 唤醒后调用 drain() 清空可读数据
     */
    class TimerFileDescriptor
    {
    public:
        TimerFileDescriptor()
        {
#ifdef _WIN32
            int readFileDescriptor = -1, writeFileDescriptor = -1;
            if (createSocketPair(readFileDescriptor, writeFileDescriptor))
            {
                m_readFileDescriptor  = readFileDescriptor;
                m_writeFileDescriptor = writeFileDescriptor;
                setNonBlocking(m_readFileDescriptor);
            }
#else
            m_fileDescriptor = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
        }

        ~TimerFileDescriptor()
        {
#ifdef _WIN32
            cancel();
            if (m_readFileDescriptor >= 0)
                closeFileDescriptor(m_readFileDescriptor);
            if (m_writeFileDescriptor >= 0)
                closeFileDescriptor(m_writeFileDescriptor);
#else
            if (m_fileDescriptor >= 0)
                ::close(m_fileDescriptor);
#endif
        }

        TimerFileDescriptor(const TimerFileDescriptor &)            = delete;
        TimerFileDescriptor &operator=(const TimerFileDescriptor &) = delete;
        TimerFileDescriptor(TimerFileDescriptor &&)                 = delete;
        TimerFileDescriptor &operator=(TimerFileDescriptor &&)      = delete;

        /**
         * @brief 获取供 epoll 监听的文件描述符
         */
        int fileDescriptor() const noexcept
        {
#ifdef _WIN32
            return m_readFileDescriptor;
#else
            return m_fileDescriptor;
#endif
        }

        /**
         * @brief 设置一次性定时器
         * @param duration 等待时长
         */
        void arm(std::chrono::milliseconds duration)
        {
#ifdef _WIN32
            cancel();

            DWORD dueTime = static_cast<DWORD>(duration.count());
            HANDLE timer  = nullptr;
            CreateTimerQueueTimer(
                &timer, nullptr, &TimerFileDescriptor::timerCallback, this,
                dueTime, 0, WT_EXECUTEONLYONCE | WT_EXECUTEINTIMERTHREAD);
            m_timer = timer;
#else
            itimerspec ts{};
            ts.it_value.tv_sec  = duration.count() / 1000;
            ts.it_value.tv_nsec = (duration.count() % 1000) * 1000000;
            ::timerfd_settime(m_fileDescriptor, 0, &ts, nullptr);
#endif
        }

        /**
         * @brief 读取并清空定时器到期数据
         */
        void drain() const
        {
#ifdef _WIN32
            if (m_readFileDescriptor >= 0)
            {
                char buffer[64];
                while (::recv(m_readFileDescriptor, buffer, sizeof(buffer), 0) > 0)
                {
                }
            }
#else
            if (m_fileDescriptor >= 0)
            {
                uint64_t expirations;
                ::read(m_fileDescriptor, &expirations, sizeof(expirations));
            }
#endif
        }

    private:
#ifdef _WIN32
        /**
         * @brief 取消当前定时器
         */
        void cancel()
        {
            if (m_timer)
            {
                HANDLE old = m_timer;
                m_timer    = nullptr;
                // INVALID_HANDLE_VALUE 使函数等待回调完成再返回
                DeleteTimerQueueTimer(nullptr, old, INVALID_HANDLE_VALUE);
            }
        }

        /**
         * @brief 定时器回调，向 socket pair 写端写入数据触发可读
         */
        static VOID CALLBACK timerCallback(PVOID context, BOOLEAN /*timerOrWaitFired*/)
        {
            auto *self = static_cast<TimerFileDescriptor *>(context);
            if (self->m_writeFileDescriptor >= 0)
            {
                char byte = 1;
                ::send(self->m_writeFileDescriptor, &byte, 1, 0);
            }
        }

        int    m_readFileDescriptor{-1};   ///< 读端 socket（注册到 epoll）
        int    m_writeFileDescriptor{-1};  ///< 写端 socket（定时器回调写入）
        HANDLE m_timer{nullptr}; ///< TimerQueue 定时器句柄
#else
        int m_fileDescriptor{-1}; ///< timerfd 文件描述符
#endif
    };

} // namespace Platform

#endif // PLATFORM_TIMERFILEDESCRIPTOR_H
