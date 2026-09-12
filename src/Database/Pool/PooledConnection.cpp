#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"

#include <utility>

namespace AsynGyanis::Database
{

    PooledConnection::PooledConnection(std::unique_ptr<DatabaseConnection> connection, ConnectionPool *pool) noexcept
        : m_connection(std::move(connection))
        , m_pool(pool)
    {
        // 构造函数不检查参数有效性：空连接 + 空池的组合是合法的「空包装」
    }

    PooledConnection::~PooledConnection()
    {
        // 析构时自动归还，仅当 m_connection 和 m_pool 均有效时才执行归还
        doReturnToPool();
    }

    PooledConnection::PooledConnection(PooledConnection &&other) noexcept
        : m_connection(std::exchange(other.m_connection, nullptr))
        , m_pool(std::exchange(other.m_pool, nullptr))
    {
        // 移动后源对象完全清空：m_connection 与 m_pool 均为空，
        // 源对象析构时不会触发归还操作
    }

    PooledConnection &PooledConnection::operator=(PooledConnection &&other) noexcept
    {
        if (this != &other)
        {
            // 先归还当前持有的连接（如有）
            doReturnToPool();

            // 转移所有权
            m_connection = std::exchange(other.m_connection, nullptr);
            m_pool       = std::exchange(other.m_pool, nullptr);
        }
        return *this;
    }

    DatabaseConnection *PooledConnection::operator->()
    {
        return m_connection.get();
    }

    DatabaseConnection &PooledConnection::operator*()
    {
        return *m_connection;
    }

    PooledConnection::operator bool() const noexcept
    {
        return m_connection != nullptr;
    }

    void PooledConnection::release()
    {
        doReturnToPool();
    }

    void PooledConnection::doReturnToPool()
    {
        // double-release 防护：当 m_connection 已被 release 或移动走后，
        // m_connection 为空，此处安全返回
        if (m_connection == nullptr)
        {
            return;
        }

        // 将连接归还至池：回调 ConnectionPool 的归还方法
        // 注意：m_pool 在构造时设置，当 PooledConnection 被默认构造时不持有池指针，
        // 此时 m_pool 为 nullptr，则直接丢弃连接（不归还）
        if (m_pool != nullptr)
        {
            m_pool->returnConnection(std::move(m_connection));
        }

        // 清空所有状态，防止重复归还
        m_connection = nullptr;
        m_pool       = nullptr;
    }

} // namespace AsynGyanis::Database