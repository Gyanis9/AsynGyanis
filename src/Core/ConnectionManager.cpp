#include "ConnectionManager.h"
#include "Connection.h"

namespace Core
{
    void ConnectionManager::add(const std::shared_ptr<Connection> &conn)
    {
        if (!conn)
            return;
        std::unique_lock lock(m_mutex);
        m_connections.insert(conn);
    }

    void ConnectionManager::remove(const Connection *const conn)
    {
        if (!conn)
            return;
        std::unique_lock lock(m_mutex);
        for (auto it = m_connections.begin(); it != m_connections.end(); ++it)
        {
            if (it->get() == conn)
            {
                m_connections.erase(it);
                m_cv.notify_all();
                break;
            }
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
        for (auto &conn: m_connections)
        {
            if (conn)
            {
                conn->cancelable().requestStop();
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
