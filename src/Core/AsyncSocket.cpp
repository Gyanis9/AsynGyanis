#include "AsyncSocket.h"
#include "EpollAwaiter.h"
#include "UringAwaiter.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Base/Exception.h"

#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cerrno>

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

    Task<AsyncSocket> AsyncSocket::asyncAccept()
    {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(addr);
        const int        result  = co_await uringAccept(m_loop.uring(), m_fd,
                                                reinterpret_cast<sockaddr *>(&addr), &addrLen);
        if (result < 0)
        {
            if (result == -EAGAIN || result == -EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                co_return co_await asyncAccept();
            }
            if (result == -EINTR || result == -ECONNABORTED)
            {
                co_return co_await asyncAccept();
            }
            throw Base::SystemException("accept failed", std::error_code(-result, std::system_category()));
        }
        co_return AsyncSocket(m_loop, result);
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const addr, const socklen_t addrLen) const
    {
        // 非阻塞 connect: 先尝试，EINPROGRESS 则等待 epoll
        if (const int ret = ::connect(m_fd, addr, addrLen); ret == 0)
            co_return;
        if (errno != EINPROGRESS)
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

    Task<ssize_t> AsyncSocket::asyncRecv(void *const buf, const size_t len)
    {
        const int result = co_await uringRead(m_loop.uring(), m_fd, buf, static_cast<unsigned>(len), -1);
        if (result < 0)
        {
            if (result == -EAGAIN || result == -EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLIN);
                co_return co_await asyncRecv(buf, len);
            }
            throw Base::SystemException("recv failed", std::error_code(-result, std::system_category()));
        }
        co_return static_cast<ssize_t>(result);
    }

    Task<ssize_t> AsyncSocket::asyncSend(const void *const buf, const size_t len)
    {
        const int result = co_await uringWrite(m_loop.uring(), m_fd, buf, static_cast<unsigned>(len), -1);
        if (result < 0)
        {
            if (result == -EAGAIN || result == -EWOULDBLOCK)
            {
                co_await EpollAwaiter(m_loop.epoll(), m_fd, EPOLLOUT);
                co_return co_await asyncSend(buf, len);
            }
            throw Base::SystemException("send failed", std::error_code(-result, std::system_category()));
        }
        co_return static_cast<ssize_t>(result);
    }

    void AsyncSocket::close()
    {
        if (m_fd >= 0)
        {
            // Shutdown before close to avoid TCP RST.
            // If the client has already sent FIN (half-close) and the server
            // hasn't read it yet, ::close() alone would send RST because there's
            // "unread" data (the FIN byte in the sequence space).
            // SHUT_RDWR sends a proper FIN+ACK handshake before closing.
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
