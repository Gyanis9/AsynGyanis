#include "AsyncSocket.h"
#include "EpollAwaiter.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Base/Exception.h"
#include "Platform/SocketCompat.h"

#include <cerrno>

namespace Core
{
    AsyncSocket::AsyncSocket(EventLoop &loop, const int fd) :
        m_loop(loop), m_fd(fd)
    {
        if (m_fd >= 0)
        {
            setNonBlocking();
        }
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
#ifdef _WIN32
        const int fd = ::socket(domain, type, 0);
#else
        const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
        if (fd < 0)
        {
            throw Base::SystemException("socket creation failed");
        }

#ifdef _WIN32
        Platform::setNonBlocking(fd);
#endif

        if (type == SOCK_STREAM)
        {
            constexpr int opt = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
        }

        return AsyncSocket(loop, fd);
    }

    bool AsyncSocket::bind(const sockaddr *const addr, const socklen_t addrLen) const
    {
        constexpr int opt = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
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

            socklen_t addrLen = sizeof(addr);
            if (const int fd = Platform::acceptSocket(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen);
                fd >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                co_return AsyncSocket(m_loop, fd);
            }

            if (ASYN_ERRNO == ASYN_EAGAIN || ASYN_ERRNO == ASYN_EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }

            if (ASYN_ERRNO == ASYN_EINTR || ASYN_ERRNO == ASYN_ECONNABORTED)
            {
                continue;
            }

            if (ASYN_ERRNO == ASYN_EMFILE || ASYN_ERRNO == ASYN_ENFILE || ASYN_ERRNO == ASYN_ENOBUFS || ASYN_ERRNO == ASYN_ENOMEM)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }

            throw Base::SystemException("accept failed");
        }
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const addr, const socklen_t addrLen) const
    {
        if (const int ret = ::connect(m_fd, addr, addrLen); ret == 0)
        {
            co_return;
        } else if (ASYN_ERRNO != ASYN_EINPROGRESS)
        {
            throw Base::SystemException("connect failed");
        }

        co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLOUT);

        int       err = 0;
        socklen_t len = sizeof(err);
        getsockopt(m_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
        if (err != 0)
        {
            throw Base::SystemException("connect failed", std::error_code(err, std::system_category()));
        }
        co_return;
    }

    Task<> AsyncSocket::asyncConnect(const InetAddress &addr) const
    {
        co_return co_await asyncConnect(addr.addr(), addr.addrLen());
    }

    Task<ssize_t> AsyncSocket::asyncRecv(void *const buf, const size_t len) const
    {
        if (len == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const ssize_t n = ::recv(m_fd, static_cast<char *>(buf), static_cast<int>(len), MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return 0;
            if (ASYN_ERRNO == ASYN_EAGAIN || ASYN_ERRNO == ASYN_EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                continue;
            }
            if (ASYN_ERRNO == ASYN_EINTR)
                continue;
            throw Base::SystemException("recv failed");
        }
    }

    Task<ssize_t> AsyncSocket::asyncSend(const void *const buf, const size_t len) const
    {
        if (len == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const ssize_t n = ::send(m_fd, static_cast<const char *>(buf), static_cast<int>(len), MSG_NOSIGNAL);
            if (n > 0)
                co_return n;
            if (n == 0)
                co_return -1; // 对端已关闭连接
            if (ASYN_ERRNO == ASYN_EAGAIN || ASYN_ERRNO == ASYN_EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLOUT);
                continue;
            }
            if (ASYN_ERRNO == ASYN_EINTR)
                continue;
            throw Base::SystemException("send failed");
        }
    }

    void AsyncSocket::close()
    {
        if (m_fd >= 0)
        {
            ::shutdown(m_fd, SHUT_RDWR);
            Platform::closeFd(m_fd);
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
            Platform::setNonBlocking(m_fd);
        }
    }

    bool AsyncSocket::setSockOpt(const int level, const int opt, const void *const val, const socklen_t len) const
    {
        return setsockopt(m_fd, level, opt, static_cast<const char *>(val), len) == 0;
    }

    InetAddress AsyncSocket::remoteAddress() const
    {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(addr);
        if (getpeername(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0)
        {
            throw Base::SystemException("getpeername failed");
        }
        return InetAddress(addr, addrLen);
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(addr);
        if (getsockname(m_fd, reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0)
        {
            throw Base::SystemException("getsockname failed");
        }
        return InetAddress(addr, addrLen);
    }
}
