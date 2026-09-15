#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"

#include <utility>

namespace AsynGyanis::Database
{

    PooledConnection::PooledConnection(std::unique_ptr<DatabaseConnection> connection, ConnectionPool *pool) noexcept :
        m_connection(std::move(connection))
        , m_pool(pool)
    {
        // 构造时取一份池存活令牌：池析构之后归还路径据此直接关闭连接，
        // 而不是回头调用已释放的池（构造函数不检查参数有效性：空连接 + 空池是合法的「空包装」）
        if (m_pool != nullptr)
        {
            m_poolLiveness = m_pool->livenessToken();
        }
    }

    PooledConnection::~PooledConnection()
    {
        // 析构时自动归还，仅当 m_connection 和 m_pool 均有效时才执行归还
        doReturnToPool();
    }

    PooledConnection::PooledConnection(PooledConnection &&other) noexcept :
        m_connection(std::exchange(other.m_connection, nullptr))
        , m_pool(std::exchange(other.m_pool, nullptr))
        , m_poolLiveness(std::move(other.m_poolLiveness))
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
            m_connection   = std::exchange(other.m_connection, nullptr);
            m_pool         = std::exchange(other.m_pool, nullptr);
            m_poolLiveness = std::move(other.m_poolLiveness);
        }
        return *this;
    }

    DatabaseConnection *PooledConnection::operator->() const
    {
        return m_connection.get();
    }

    DatabaseConnection &PooledConnection::operator*() const
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

        // 将连接归还至池；池已析构时按文档承诺直接把连接关掉。
        // 判据只看存活令牌：池析构后其指针本身已经是悬垂值，取消引用它是释放后使用
        const bool isPoolAlive = m_pool != nullptr && m_poolLiveness != nullptr &&
                                 m_poolLiveness->load(std::memory_order_acquire);
        if (isPoolAlive)
        {
            m_pool->returnConnection(std::move(m_connection));
        }

        // 清空所有状态，防止重复归还。连接已被移走（或池已析构）时，
        // 这里析构 unique_ptr 即关闭底层连接
        m_connection = nullptr;
        m_pool       = nullptr;
        m_poolLiveness.reset();
    }

} // namespace AsynGyanis::Database
