/**
 * @file EpollAwaiter.h
 * @brief Awaitable that registers an fd with edge-triggered epoll and suspends until readiness
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_EPOLLWAITER_H
#define CORE_EPOLLWAITER_H

#include "Epoll.h"

#include <coroutine>

namespace Core
{
    class EpollAwaiter
    {
    public:
        EpollAwaiter(Epoll &epoll, const int fd, const uint32_t eventMask) :
            m_epoll(&epoll), m_fd(fd), m_eventMask(eventMask)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            m_epoll->addFd(m_fd, m_eventMask | EPOLLET, handle.address());
        }

        void await_resume() const
        {
            m_epoll->delFd(m_fd);
        }

    private:
        Epoll *  m_epoll;
        int      m_fd;
        uint32_t m_eventMask;
    };

}

#endif
