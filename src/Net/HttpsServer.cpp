#include "HttpsServer.h"
#include "HttpsSession.h"

#include "Base/Exception.h"
#include "Base/Logger.h"

namespace Net
{
    HttpsServer::HttpsServer(Core::EventLoop &loop, const Core::InetAddress &addr, const std::string &certFile, const std::string &keyFile) :
        TcpServer(loop, addr)
    {
        if (!m_tlsContext.loadCertificate(certFile, keyFile))
        {
            throw Base::Exception("HttpsServer: failed to load certificate or key");
        }
        LOG_INFO_FMT("HttpsServer: TLS certificate loaded (cert={}, key={})", certFile, keyFile);
    }

    Router &HttpsServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpsServer::createConnection(Core::AsyncSocket socket)
    {
        SSL *ssl = m_tlsContext.createSSL(socket.fd());

        Core::TlsSocket tlsSocket(ssl, m_loop, std::move(socket));
        return std::make_shared<HttpsSession>(m_loop, std::move(tlsSocket), m_router);
    }

}
