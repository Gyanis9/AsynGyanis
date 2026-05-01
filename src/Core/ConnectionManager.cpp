#include "ConnectionManager.h"
#include "Connection.h"

#include <ranges>
#include <vector>


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

    void ConnectionManager::shutdown()
    {
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::shared_lock lock(m_mutex);
            snapshot.reserve(m_connections.size());
            for (const auto &conn: m_connections | std::views::values)
            {
                snapshot.push_back(conn);
            }
        }
        // 锁外调用 close()，防止回调中的 remove() 死锁
        for (auto &conn: snapshot)
        {
            if (conn)
            {
                [[maybe_unused]] auto _ = conn->cancelable().requestStop();
                conn->close();
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
