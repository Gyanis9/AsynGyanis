#include "TcpServer.h"

#include "Base/Exception.h"

#include "Core/EventLoop.h"
#include "Core/Timer.h"

#include <algorithm>
#include <cerrno>

namespace Net
{
    TcpServer::TcpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        m_loop(loop), m_acceptor(loop, address)
    {
    }

    Core::Task<> TcpServer::start()
    {
        if (!m_acceptor.bind())
            throw Base::Exception("TcpServer: bind failed");

        if (!m_acceptor.listen())
            throw Base::Exception("TcpServer: listen failed");

        m_running                   = true;
        size_t nextCleanupThreshold = 64;

        while (m_running)
        {
            std::optional<Core::AsyncSocket> result;
            try
            {
                result = co_await m_acceptor.accept();
            } catch (const Base::SystemException &e)
            {
                const int error = e.nativeError();
                if (error == EINTR || error == ECONNABORTED || error == EAGAIN || error == EWOULDBLOCK)
                {
                    continue;
                }
                if (error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM)
                {
                    continue;
                }
                m_running = false;
                break;
            }

            if (!result.has_value())
            {
                m_running = false;
                break;
            }

            if (m_maxConnections > 0 && m_connectionManager.activeCount() >= m_maxConnections)
            {
                continue;
            }

            if (auto connection = createConnection(std::move(result.value())))
            {
                m_connectionManager.add(connection);
                auto task = handleConnection(std::move(connection));
                m_loop.scheduler().schedule(task.handle());
                m_connectionTasks.push_back(std::move(task));
            }

            if (m_connectionTasks.size() > nextCleanupThreshold)
            {
                std::erase_if(m_connectionTasks,
                              [](const Core::Task<> &t)
                              {
                                  return t.isReady();
                              });
                nextCleanupThreshold = std::max<size_t>(64, m_connectionTasks.size() + 64);
            }
        }

        // 优雅关闭：等待所有连接任务自然完成。
        // stop() 已设置 m_running=false，close() 已调用 ConnectionManager::shutdown()
        // 强制关闭所有连接 socket，因此此处不会无限阻塞。
        for (auto &t: m_connectionTasks)
        {
            if (!t.isReady())
            {
                co_await t;
            }
        }
        m_connectionManager.waitAll();
    }

    Core::Task<> TcpServer::handleConnection(std::shared_ptr<Core::Connection> connection)
    {
        try
        {
            co_await connection->start();
        } catch (...)
        {
            // 捕获所有异常防止传播到调度器导致进程终止
        }
        m_connectionManager.remove(connection.get());
    }

    void TcpServer::stop()
    {
        m_running = false;
        m_acceptor.close();
    }

    void TcpServer::close()
    {
        stop();
        m_connectionManager.shutdown();
    }

    void TcpServer::setMaxConnections(const size_t max)
    {
        m_maxConnections = max;
    }

    bool TcpServer::isRunning() const
    {
        return m_running;
    }

    std::shared_ptr<Core::Connection> TcpServer::createConnection(Core::AsyncSocket socket)
    {
        return nullptr;
    }

}
