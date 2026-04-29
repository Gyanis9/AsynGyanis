/**
 * @file EpollAwaiter.h
 * @brief Awaitable that registers an fd with epoll and suspends until readiness
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_EPOLLWAITER_H
#define CORE_EPOLLWAITER_H

#include "Epoll.h"

#include <coroutine>
#include <poll.h>

namespace Core
{
    class EpollAwaiter
    {
    public:
        EpollAwaiter(Epoll &epoll, const int fd, const uint32_t eventMask) :
            m_epoll(&epoll), m_fd(fd), m_eventMask(eventMask), m_resultEvents(0)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            m_epoll->addFd(m_fd, m_eventMask, handle.address());
        }

        uint32_t await_resume()
        {
            m_epoll->delFd(m_fd);
            pollfd pfd{};
            pfd.fd      = m_fd;
            pfd.events  = static_cast<short>(m_eventMask);
            pfd.revents = 0;
            ::poll(&pfd, 1, 0);
            m_resultEvents = static_cast<uint32_t>(pfd.revents);
            return m_resultEvents;
        }

    private:
        Epoll *  m_epoll;
        int      m_fd;
        uint32_t m_eventMask;
        uint32_t m_resultEvents;
    };

}

#endif
