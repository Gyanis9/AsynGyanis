/**
 * @file Timer.h
 * @brief 可 co_await 的定时器（基于 timerfd + epoll）
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include "EpollAwaiter.h"

#include <chrono>
#include <coroutine>

namespace Core
{
    class EventLoop;

    class Timer
    {
    public:
        class Awaiter
        {
        public:
            Awaiter(Epoll &epoll, int fd) noexcept;

            bool await_ready() const noexcept;
            void await_suspend(std::coroutine_handle<> handle) noexcept;
            void await_resume() const;

        private:
            EpollAwaiter m_awaiter;
            int          m_fd;
        };

        explicit Timer(EventLoop &loop);

        ~Timer();

        Awaiter waitFor(std::chrono::milliseconds duration) const;

    private:
        EventLoop &m_loop;
        int        m_timerFd{-1};
    };

}

#endif
