#include "TcpAcceptor.h"
#include "Base/Exception.h"
#include "Core/EpollAwaiter.h"
#include "Core/EventLoop.h"
#include "Platform/SocketCompat.h"

#include <cerrno>


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
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
        // SO_REUSEPORT 仅 Linux 3.9+ 支持，Windows 不支持此选项
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&opt), sizeof(opt));
#endif

        if (m_addr.family() == AF_INET6)
        {
            constexpr int         v6only = 0;
            [[maybe_unused]] auto _      = m_listenSocket.setSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
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
            const int        fd      = Platform::acceptSocket(listenFd, reinterpret_cast<sockaddr *>(&addr), &addrLen);
            if (fd >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                Core::AsyncSocket first(m_loop, fd);

                while (true)
                {
                    sockaddr_storage extra{};
                    socklen_t        extraLen = sizeof(extra);
                    const int        extraFd  = Platform::acceptSocket(listenFd, reinterpret_cast<sockaddr *>(&extra), &extraLen);
                    if (extraFd >= 0)
                    {
                        setsockopt(extraFd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                        m_pending.emplace_back(m_loop, extraFd);
                        continue;
                    }
                    if (ASYN_ERRNO == ASYN_EAGAIN || ASYN_ERRNO == ASYN_EWOULDBLOCK)
                        break;
                    if (ASYN_ERRNO == ASYN_EINTR || ASYN_ERRNO == ASYN_ECONNABORTED)
                        continue;
                    break;
                }

                co_return first;
            }

            if (ASYN_ERRNO == ASYN_EAGAIN || ASYN_ERRNO == ASYN_EWOULDBLOCK)
            {
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFd, EPOLLIN);
                continue;
            }
            if (ASYN_ERRNO == ASYN_EINTR || ASYN_ERRNO == ASYN_ECONNABORTED)
                continue;
            if (ASYN_ERRNO == ASYN_EMFILE || ASYN_ERRNO == ASYN_ENFILE || ASYN_ERRNO == ASYN_ENOBUFS || ASYN_ERRNO == ASYN_ENOMEM)
            {
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFd, EPOLLIN);
                continue;
            }

            throw Base::SystemException("accept failed");
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
