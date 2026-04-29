/**
 * @file HttpsServer.h
 * @brief HTTPS server — holds TlsContext, creates TLS-wrapped HttpsSession per connection
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSSERVER_H
#define NET_HTTPSSERVER_H

#include "Core/EventLoop.h"
#include "Core/TlsContext.h"
#include "Core/TlsSocket.h"

#include "Router.h"
#include "TcpServer.h"

#include <memory>
#include <string>

namespace Net
{
    class HttpsServer : public TcpServer
    {
    public:
        HttpsServer(Core::EventLoop &loop, const Core::InetAddress &addr,
                    const std::string &certFile, const std::string &keyFile);

        Router &router();

        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

    private:
        Router          m_router;
        Core::TlsContext m_tlsContext;
    };

}

#endif
