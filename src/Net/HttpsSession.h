/**
 * @file HttpsSession.h
 * @brief HTTPS session — extends Connection, performs TLS handshake then HTTP over encrypted I/O
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSSESSION_H
#define NET_HTTPSSESSION_H

#include "Core/Connection.h"
#include "Core/TlsSocket.h"
#include "Core/Task.h"
#include "HttpParser.h"
#include "Router.h"

namespace Net
{
    class HttpsSession : public Core::Connection
    {
    public:
        HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router);

        Core::Task<> start() override;

    private:
        Core::TlsSocket   m_tlsSocket;
        Router &          m_router;
        HttpParser        m_parser;
        std::vector<char> m_recvBuffer;
        static constexpr int m_recvBufferSize = 8192;
    };

}

#endif
