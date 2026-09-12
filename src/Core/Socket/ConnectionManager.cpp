#include "Core/Socket/ConnectionManager.h"
#include "Core/Socket/Connection.h"

#include <ranges>
#include <vector>


namespace AsynGyanis::Core
{
    void ConnectionManager::add(const std::shared_ptr<Connection> &connection)
    {
        if (!connection)
        {
            return;
        }
        {
            std::unique_lock lock(m_mutex);
            m_connections.emplace(connection.get(), connection);
        }

        // 关闭已开始后才挂上来的连接必须立刻收尾。shutdown() 遍历的是它调用那一刻的快照，
        // 而接受新连接与本函数可能并发；漏掉这一条，它会永远留在活跃表里，
        // 服务器等它退出就会一直等不到（等待全部连接结束的收尾阶段将挂住）
        if (m_isShuttingDown.load(std::memory_order_acquire))
        {
            [[maybe_unused]] auto _ = connection->cancelable().requestStop();
            connection->close();
        }
    }

    void ConnectionManager::remove(const Connection *const connection)
    {
        if (!connection)
        {
            return;
        }

        std::unique_lock lock(m_mutex);
        if (m_connections.erase(connection))
        {
            m_condition.notify_all();
        }
    }

    size_t ConnectionManager::activeCount() const
    {
        std::shared_lock lock(m_mutex);
        return m_connections.size();
    }

    void ConnectionManager::shutdown()
    {
        // 标志必须先于快照置位：否则「取完快照、还没置位」这一小段里 add() 的新连接
        // 既不在快照中，也不会被 add() 就地收尾，成了漏网的一条
        m_isShuttingDown.store(true, std::memory_order_release);

        std::vector<std::shared_ptr<Connection> > snapshot;
        {
            std::shared_lock lock(m_mutex);
            snapshot.reserve(m_connections.size());
            for (const auto &connection: m_connections | std::views::values)
            {
                snapshot.push_back(connection);
            }
        }
        // 锁外调用 close()，防止回调中的 remove() 死锁
        for (auto &connection: snapshot)
        {
            if (connection)
            {
                [[maybe_unused]] auto _ = connection->cancelable().requestStop();
                connection->close();
            }
        }
    }

    void ConnectionManager::waitAll()
    {
        std::shared_lock lock(m_mutex);
        m_condition.wait(lock, [this]()
        {
            return m_connections.empty();
        });
    }

}
