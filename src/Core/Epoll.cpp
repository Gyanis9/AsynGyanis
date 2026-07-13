#include "Epoll.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace Core
{
    Epoll::Epoll()
    {
        m_fd = epoll_create1(EPOLL_CLOEXEC);
        if (!epoll_handle_valid(m_fd))
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
        other.m_fd = kInvalidEpollHandle;
    }

    Epoll &Epoll::operator=(Epoll &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_fd       = other.m_fd;
            m_events   = std::move(other.m_events);
            other.m_fd = kInvalidEpollHandle;
        }
        return *this;
    }

    bool Epoll::addFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fd, EPOLL_CTL_ADD,
#ifdef _WIN32
                         static_cast<SOCKET>(fd),
#else
                         fd,
#endif
                         &ev) == 0;
    }

    bool Epoll::modFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fd, EPOLL_CTL_MOD,
#ifdef _WIN32
                         static_cast<SOCKET>(fd),
#else
                         fd,
#endif
                         &ev) == 0;
    }

    bool Epoll::delFd(const int fd) const
    {
        return epoll_ctl(m_fd, EPOLL_CTL_DEL,
#ifdef _WIN32
                         static_cast<SOCKET>(fd),
#else
                         fd,
#endif
                         nullptr) == 0;
    }

    bool Epoll::rearmFd(const int fd, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events | EPOLLONESHOT;
        ev.data.ptr = userData;
        if (epoll_ctl(m_fd, EPOLL_CTL_MOD,
#ifdef _WIN32
                      static_cast<SOCKET>(fd),
#else
                      fd,
#endif
                      &ev) == 0)
            return true;
        // 仅在 fd 尚未注册时才回退到 ADD，其他错误（如 EBADF）直接返回 false
        if (errno == ENOENT)
            return epoll_ctl(m_fd, EPOLL_CTL_ADD,
#ifdef _WIN32
                             static_cast<SOCKET>(fd),
#else
                             fd,
#endif
                             &ev) == 0;
        return false;
    }

    std::span<epoll_event> Epoll::wait(const int timeoutMs)
    {
        const int n = epoll_wait(m_fd, m_events.data(), static_cast<int>(m_events.size()), timeoutMs);
        if (n < 0)
        {
            if (errno == EINTR)
                return {};
            char errBuf[128];
#ifdef _WIN32
            strerror_s(errBuf, sizeof(errBuf), errno);
            throw std::runtime_error(std::string("epoll_wait failed: ") + errBuf);
#else
            throw std::runtime_error(
                    std::string("epoll_wait failed: ") + strerror_r(errno, errBuf, sizeof(errBuf)));
#endif
        }
        // 动态扩容：当返回事件数接近容量上限时翻倍，防止高负载下丢失事件
        if (static_cast<size_t>(n) >= m_events.size() / 2)
        {
            m_events.resize(m_events.size() * 2);
        }
        return {m_events.data(), static_cast<size_t>(n)};
    }

    epoll_handle_t Epoll::fd() const noexcept
    {
        return m_fd;
    }

    void Epoll::destroy()
    {
        if (epoll_handle_valid(m_fd))
        {
#ifdef _WIN32
            epoll_close(m_fd);
#else
            close(m_fd);
#endif
            m_fd = kInvalidEpollHandle;
        }
    }

}
