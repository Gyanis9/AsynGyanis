/**
 * @file HttpServer.h
 * @brief Full HTTP server inheriting from TcpServer
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSERVER_H
#define NET_HTTPSERVER_H

#include "Core/EventLoop.h"

#include "Router.h"
#include "TcpServer.h"

#include <memory>
#include <optional>
#include <string>

namespace Net
{

    class HttpServer : public TcpServer
    {
    public:
        HttpServer(Core::EventLoop &loop, const Core::InetAddress &addr);

        Router &                          router();
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;
        void                              staticFileDir(const std::string &path);

    private:
        Router                     m_router;
        std::optional<std::string> m_staticDir;
    };

}

#endif
