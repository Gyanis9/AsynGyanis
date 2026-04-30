#include "Epoll.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace Core
{
    Epoll::Epoll()
    {
        m_fd = epoll_create1(EPOLL_CLOEXEC);
        if (m_fd < 0)
        {
            throw std::runtime_error("epoll_create1 failed");
        }
        m_events.resize(DEFAULT_MAX_EVENTS);
    }

    Epoll::~Epoll()
    {
        destroy();
    }

    Epoll::Epoll(Epoll &&other) noexcept :
        m_fd(other.m_fd), m_events(std::move(other.m_events))
    {
        other.m_fd = -1;
    }

    Epoll &Epoll::operator=(Epoll &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_fd       = other.m_fd;
            m_events   = std::move(other.m_events);
            other.m_fd = -1;
        }
        return *this;
    }

    bool Epoll::addFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool Epoll::modFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    bool Epoll::delFd(const int fd) const
    {
        return epoll_ctl(m_fd, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    bool Epoll::rearmFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events | EPOLLONESHOT;
        ev.data.ptr = userData;
        if (epoll_ctl(m_fd, EPOLL_CTL_MOD, fd, &ev) == 0)
            return true;
        return epoll_ctl(m_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    std::span<epoll_event> Epoll::wait(const int timeoutMs)
    {
        const int n = epoll_wait(m_fd, m_events.data(), static_cast<int>(m_events.size()), timeoutMs);
        if (n < 0)
        {
            if (errno == EINTR)
                return {};
            throw std::runtime_error(std::string("epoll_wait failed: ") + std::strerror(errno));
        }
        return {m_events.data(), static_cast<size_t>(n)};
    }

    int Epoll::fd() const noexcept
    {
        return m_fd;
    }

    void Epoll::destroy()
    {
        if (m_fd >= 0)
        {
            close(m_fd);
            m_fd = -1;
        }
    }

}
