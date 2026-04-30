#include "ConnectionManager.h"
#include "Connection.h"

#include <ranges>


namespace Core
{
    void ConnectionManager::add(const std::shared_ptr<Connection> &conn)
    {
        if (!conn)
        {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_connections.emplace(conn.get(), conn);
    }

    void ConnectionManager::remove(const Connection *const conn)
    {
        if (!conn)
        {
            return;
        }

        std::unique_lock lock(m_mutex);
        if (m_connections.erase(conn))
        {
            m_cv.notify_all();
        }
    }

    size_t ConnectionManager::activeCount() const
    {
        std::shared_lock lock(m_mutex);
        return m_connections.size();
    }

    void ConnectionManager::shutdown() const
    {
        std::shared_lock lock(m_mutex);
        for (const auto &conn: m_connections | std::views::values)
        {
            if (conn)
            {
                [[maybe_unused]] auto _ = conn->cancelable().requestStop();
            }
        }
    }

    void ConnectionManager::waitAll()
    {
        std::shared_lock lock(m_mutex);
        m_cv.wait(lock, [this]()
        {
            return m_connections.empty();
        });
    }

}
