/**
 * @file HttpsServer.cpp
 * @brief HTTPS 服务器，持有 TlsContext 并为每条连接派生 TLS 会话
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "HttpsServer.h"
#include "HttpsSession.h"

#include "Base/Exception.h"
#include "Base/Logger.h"

namespace Net
{
    HttpsServer::HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile) :
        TcpServer(loop, address)
    {
        if (!m_tlsContext.loadCertificate(certificateFile, keyFile))
        {
            throw Base::Exception("HttpsServer: failed to load certificate or key");
        }
        LOG_INFO_FMT("HttpsServer: TLS certificate loaded (certificate={}, key={})", certificateFile, keyFile);
    }

    Router &HttpsServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpsServer::createConnection(Core::AsyncSocket socket)
    {
        SSL *ssl = m_tlsContext.createSSL(socket.fileDescriptor());

        Core::TlsSocket tlsSocket(ssl, m_loop, std::move(socket));
        return std::make_shared<HttpsSession>(m_loop, std::move(tlsSocket), m_router);
    }

}
