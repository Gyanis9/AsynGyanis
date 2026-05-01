#include "TcpAcceptor.h"
#include "Base/Exception.h"
#include "Core/EpollAwaiter.h"
#include "Core/EventLoop.h"

#include <cerrno>
#include <netinet/tcp.h>


namespace Net
{
    TcpAcceptor::TcpAcceptor(Core::EventLoop &loop, const Core::InetAddress &addr) :
        m_loop(loop),
        m_listenSocket(Core::AsyncSocket::create(loop, addr.family() == AF_INET6 ? AF_INET6 : AF_INET)),
        m_addr(addr)
    {
    }

    bool TcpAcceptor::bind()
    {
        const int     fd  = m_listenSocket.fd();
        constexpr int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        // SO_REUSEPORT 仅 Linux 3.9+ 支持，旧内核失败时忽略（不影响单实例运行）
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        {
            // 非致命错误，仅在 EOPNOTSUPP 时静默忽略
        }

        if (m_addr.family() == AF_INET6)
        {
            constexpr int v6only = 0;
            [[maybe_unused]] auto _ = m_listenSocket.setSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }

        if (!m_listenSocket.bind(m_addr))
            return false;

        m_bound = true;
        return true;
    }

    bool TcpAcceptor::listen(const int backlog) const
    {
        if (!m_bound)
            return false;
        return m_listenSocket.listen(backlog);
    }

    Core::Task<std::optional<Core::AsyncSocket>> TcpAcceptor::accept()
    {
        if (!m_pending.empty())
        {
            Core::AsyncSocket sock = std::move(m_pending.front());
            m_pending.pop_front();
            co_return sock;
        }

        const int listenFd = m_listenSocket.fd();
        if (listenFd < 0)
        {
            co_return std::nullopt;
        }

        while (true)
        {
            sockaddr_storage addr{};
            socklen_t        addrLen = sizeof(addr);
            const int        fd      = ::accept4(listenFd, reinterpret_cast<sockaddr *>(&addr), &addrLen,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                Core::AsyncSocket first(m_loop, fd);

                while (true)
                {
                    sockaddr_storage extra{};
                    socklen_t        extraLen = sizeof(extra);
                    const int        extraFd  = ::accept4(listenFd, reinterpret_cast<sockaddr *>(&extra), &extraLen,
                                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (extraFd >= 0)
                    {
                        setsockopt(extraFd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                        m_pending.emplace_back(m_loop, extraFd);
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR || errno == ECONNABORTED)
                        continue;
                    break;
                }

                co_return first;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFd, EPOLLIN);
                continue;
            }
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
            {
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFd, EPOLLIN);
                continue;
            }

            throw Base::SystemException("accept4 failed");
        }
    }

    void TcpAcceptor::close()
    {
        m_listenSocket.close();
        m_pending.clear();
        m_bound = false;
    }

    Core::InetAddress TcpAcceptor::localAddress() const
    {
        return m_addr;
    }

    int TcpAcceptor::fd() const
    {
        return m_listenSocket.fd();
    }

}
