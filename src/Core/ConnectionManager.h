/**
 * @file ConnectionManager.h
 * @brief Global connection tracker with graceful shutdown support
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_CONNECTIONMANAGER_H
#define CORE_CONNECTIONMANAGER_H

#include <condition_variable>
#include <memory>
#include <shared_mutex>
#include <unordered_set>

namespace Core
{
    class Connection;

    class ConnectionManager
    {
    public:
        ConnectionManager() = default;

        void                 add(const std::shared_ptr<Connection> &conn);
        void                 remove(const Connection *conn);
        [[nodiscard]] size_t activeCount() const;
        void                 shutdown() const;
        void                 waitAll();

    private:
        mutable std::shared_mutex                       m_mutex;
        std::unordered_set<std::shared_ptr<Connection>> m_connections;
        std::condition_variable_any                     m_cv;
    };

}

#endif
