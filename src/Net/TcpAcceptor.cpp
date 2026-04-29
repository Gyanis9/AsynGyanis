#include "TcpAcceptor.h"
#include "Base/Exception.h"

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
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        if (m_addr.family() == AF_INET6)
        {
            constexpr int v6only = 0;
            m_listenSocket.setSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
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
        co_return co_await m_listenSocket.asyncAccept();
    }

    void TcpAcceptor::close()
    {
        m_listenSocket.close();
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
