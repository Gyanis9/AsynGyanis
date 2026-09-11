/**
 * @file ConnectionManager.cpp
 * @brief 全局连接跟踪器，支持优雅关闭
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
        std::unique_lock lock(m_mutex);
        m_connections.emplace(connection.get(), connection);
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
