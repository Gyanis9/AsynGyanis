/**
 * @file TcpAcceptor.h
 * @brief TCP listening socket — asynchronous accept
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPACCEPTOR_H
#define NET_TCPACCEPTOR_H

#include "Core/AsyncSocket.h"
#include "Core/Task.h"
#include "Core/InetAddress.h"

#include <optional>

namespace Net
{
    using Core::InetAddress;

    class TcpAcceptor
    {
    public:
        TcpAcceptor(Core::EventLoop &loop, const InetAddress &addr);

        TcpAcceptor(const TcpAcceptor &)            = delete;
        TcpAcceptor &operator=(const TcpAcceptor &) = delete;
        TcpAcceptor(TcpAcceptor &&)                 = default;
        TcpAcceptor &operator=(TcpAcceptor &&)      = default;

        ~TcpAcceptor() = default;

        bool                                         bind();
        bool                                         listen(int backlog = SOMAXCONN) const;
        Core::Task<std::optional<Core::AsyncSocket>> accept();
        void                                         close();

        [[nodiscard]] Core::InetAddress localAddress() const;
        [[nodiscard]] int               fd() const;

    private:
        Core::EventLoop & m_loop;
        Core::AsyncSocket m_listenSocket;
        Core::InetAddress m_addr;
        bool              m_bound{false};
    };

}

#endif
