/**
 * @file TcpAcceptor.cpp
 * @brief TCP 监听套接字，异步接受新连接
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "TcpAcceptor.h"
#include "Base/Exception.h"
#include "Core/EpollAwaiter.h"
#include "Core/EventLoop.h"
#include "Platform/SocketCompat.h"

#include <cerrno>


namespace Net
{
    TcpAcceptor::TcpAcceptor(Core::EventLoop &loop, const Core::InetAddress &address) :
        m_loop(loop),
        m_listenSocket(Core::AsyncSocket::create(loop, address.family() == AF_INET6 ? AF_INET6 : AF_INET)),
        m_address(address)
    {
    }

    bool TcpAcceptor::bind()
    {
        const int     fileDescriptor = m_listenSocket.fileDescriptor();
        constexpr int opt            = 1;
        setsockopt(fileDescriptor, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
        // SO_REUSEPORT 仅 Linux 3.9+ 支持，Windows 不支持此选项
#ifdef SO_REUSEPORT
        setsockopt(fileDescriptor, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&opt), sizeof(opt));
#endif

        if (m_address.family() == AF_INET6)
        {
            constexpr int         v6only = 0;
            [[maybe_unused]] auto _      = m_listenSocket.setSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }

        if (!m_listenSocket.bind(m_address))
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
            Core::AsyncSocket socket = std::move(m_pending.front());
            m_pending.pop_front();
            co_return socket;
        }

        const int listenFileDescriptor = m_listenSocket.fileDescriptor();
        if (listenFileDescriptor < 0)
        {
            co_return std::nullopt;
        }

        while (true)
        {
            sockaddr_storage address{};
            socklen_t        addressLength = sizeof(address);
            const int        fileDescriptor = Platform::acceptSocket(listenFileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength);
            if (fileDescriptor >= 0)
            {
                constexpr int opt = 1;
                setsockopt(fileDescriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                Core::AsyncSocket first(m_loop, fileDescriptor);

                while (true)
                {
                    sockaddr_storage extra{};
                    socklen_t        extraLength         = sizeof(extra);
                    const int        extraFileDescriptor = Platform::acceptSocket(listenFileDescriptor, reinterpret_cast<sockaddr *>(&extra), &extraLength);
                    if (extraFileDescriptor >= 0)
                    {
                        setsockopt(extraFileDescriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&opt), sizeof(opt));
                        m_pending.emplace_back(m_loop, extraFileDescriptor);
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
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFileDescriptor, EPOLLIN);
                continue;
            }
            if (ASYN_ERRNO == ASYN_EINTR || ASYN_ERRNO == ASYN_ECONNABORTED)
                continue;
            if (ASYN_ERRNO == ASYN_EMFILE || ASYN_ERRNO == ASYN_ENFILE || ASYN_ERRNO == ASYN_ENOBUFS || ASYN_ERRNO == ASYN_ENOMEM)
            {
                co_await Core::EpollAwaiter(m_loop.epoll(), listenFileDescriptor, EPOLLIN);
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
        return m_address;
    }

    int TcpAcceptor::fileDescriptor() const
    {
        return m_listenSocket.fileDescriptor();
    }

}
