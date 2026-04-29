/**
 * @file Timer.h
 * @brief 可 co_await 的定时器（基于 timerfd + epoll）
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include "EpollAwaiter.h"

#include <chrono>

namespace Core
{
    class EventLoop;

    class Timer
    {
    public:
        explicit Timer(EventLoop &loop);

        EpollAwaiter waitFor(std::chrono::milliseconds duration) const;

    private:
        EventLoop &m_loop;
        int        m_timerFd{-1};
    };

}

#endif
