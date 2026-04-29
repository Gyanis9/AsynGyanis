#include "AsyncSocket.h"
#include "EpollAwaiter.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Base/Exception.h"

#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
#include <sys/socket.h>

namespace Core
{
    AsyncSocket::AsyncSocket(EventLoop &loop, const int fd) :
        m_loop(loop), m_fd(fd)
    {
        if (m_fd >= 0)
            setNonBlocking();
    }

    AsyncSocket::~AsyncSocket()
    {
        close();
    }

    AsyncSocket::AsyncSocket(AsyncSocket &&other) noexcept :
        m_loop(other.m_loop), m_fd(std::exchange(other.m_fd, -1))
    {
    }

    AsyncSocket &AsyncSocket::operator=(AsyncSocket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    AsyncSocket AsyncSocket::create(EventLoop &loop, const int domain, const int type)
    {
        const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw Base::SystemException("socket creation failed");

        if (type == SOCK_STREAM)
        {
            constexpr int opt = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        }

        return AsyncSocket(loop, fd);
    }

    bool AsyncSocket::bind(const sockaddr *const addr, const socklen_t addrLen) const
    {
        constexpr int opt = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        return ::bind(m_fd, addr, addrLen) == 0;
    }

    bool AsyncSocket::bind(const InetAddress &addr) const
    {
        return bind(addr.addr(), addr.addrLen());
    }

    bool AsyncSocket::listen(const int backlog) const
    {
        return ::listen(m_fd, backlog) == 0;
    }

    Task<AsyncSocket> AsyncSocket::asyncAccept() const
    {
        while (true)
        {
            sockaddr_storage addr{};
            socklen_t        addrLen = sizeof(addr);
            const int        fd      = ::accept4(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                co_return AsyncSocket(m_loop, fd);
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }
            throw Base::SystemException("accept4 failed");
        }
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const addr, const socklen_t addrLen) const
    {
        if (const int ret = ::connect(m_fd, addr, addrLen); ret == 0)
            co_return;
        else if (errno != EINPROGRESS)
            throw Base::SystemException("connect failed");

        co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLOUT);

        int       err = 0;
        socklen_t len = sizeof(err);
        getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0)
            throw Base::SystemException("connect failed", std::error_code(err, std::system_category()));
        co_return;
    }

    Task<void> AsyncSocket::asyncConnect(const InetAddress &addr) const
    {
        co_return co_await asyncConnect(addr.addr(), addr.addrLen());
    }

    Task<ssize_t> AsyncSocket::asyncRecv(void *const buf, const size_t len) const
    {
        if (len == 0)
            co_return 0;

        while (true)
        {
            const ssize_t n = ::recv(m_fd, buf, len, MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }
            if (errno == EINTR)
                continue;
            throw Base::SystemException("recv failed");
        }
    }

    Task<ssize_t> AsyncSocket::asyncSend(const void *const buf, const size_t len) const
    {
        if (len == 0)
            co_return 0;

        while (true)
        {
            const ssize_t n = ::send(m_fd, buf, len, MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLOUT);
                continue;
            }
            if (errno == EINTR)
                continue;
            throw Base::SystemException("send failed");
        }
    }

    void AsyncSocket::close()
    {
        if (m_fd >= 0)
        {
            ::shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
            m_fd = -1;
        }
    }

    int AsyncSocket::fd() const noexcept
    {
        return m_fd;
    }

    void AsyncSocket::setNonBlocking() const
    {
        if (m_fd >= 0)
        {
            const int flags = fcntl(m_fd, F_GETFL, 0);
            fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    bool AsyncSocket::setSockOpt(const int level, const int opt, const void *const val, const socklen_t len) const
    {
        return setsockopt(m_fd, level, opt, val, len) == 0;
    }

    InetAddress AsyncSocket::remoteAddress() const
    {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(addr);
        if (getpeername(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0)
            return InetAddress();
        return InetAddress(addr, addrLen);
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(addr);
        if (getsockname(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0)
            return InetAddress();
        return InetAddress(addr, addrLen);
    }

}
