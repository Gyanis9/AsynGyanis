/**
 * @file TcpServer.h
 * @brief TCP server: composes TcpAcceptor and ConnectionManager
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPSERVER_H
#define NET_TCPSERVER_H

#include "Core/Connection.h"
#include "Core/ConnectionManager.h"
#include "Core/Task.h"
#include "TcpAcceptor.h"

#include <memory>
#include <vector>

namespace Net
{
    class TcpServer
    {
    public:
        TcpServer(Core::EventLoop &loop, const Core::InetAddress &addr);

        TcpServer(const TcpServer &)            = delete;
        TcpServer &operator=(const TcpServer &) = delete;

        virtual ~TcpServer() = default;

        Core::Task<> start();
        void         stop();
        void         close();
        bool         isRunning() const;

        virtual std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket);

    protected:
        Core::EventLoop &       m_loop;
        TcpAcceptor             m_acceptor;
        Core::ConnectionManager m_connManager;

    private:
        Core::Task<>                  handleConnection(std::shared_ptr<Core::Connection> conn);
        bool                          m_running{false};
        std::vector<Core::Task<void>> m_connTasks;
    };

}

#endif
