#include "HttpServer.h"
#include "Core/Task.h"
#include "HttpSession.h"

namespace Net
{

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &addr) :
        TcpServer(loop, addr)
    {
    }

    Router &HttpServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpServer::createConnection(Core::AsyncSocket socket)
    {
        return std::make_shared<HttpSession>(m_loop, std::move(socket), m_router);
    }

    void HttpServer::staticFileDir(const std::string &path)
    {
        m_staticDir = path;
    }

}
