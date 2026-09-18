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
        doReturnToPool();
    }

    PooledConnection::PooledConnection(PooledConnection &&other) noexcept :
        m_connection(std::exchange(other.m_connection, nullptr))
        , m_pool(std::exchange(other.m_pool, nullptr))
        , m_poolLiveness(std::move(other.m_poolLiveness))
    {
    }

    PooledConnection &PooledConnection::operator=(PooledConnection &&other) noexcept
    {
        if (this != &other)
        {
            doReturnToPool();

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
        // double-release 防护：已被 release 或移动走时这里安全返回
        if (m_connection == nullptr)
        {
            return;
        }

        // 将连接归还至池；池已（或正在）析构时按文档承诺直接把连接关掉。
        // 判活与归还调用同在一段令牌锁内（见 ConnectionPool::returnConnectionIfAlive）：
        // 只判一个原子量的话，并发销毁时判活刚通过、调用就踩空
        if (m_pool != nullptr)
        {
            m_pool->returnConnectionIfAlive(m_connection, m_poolLiveness, false);
        }

        // 清空所有状态，防止重复归还。连接已被移走（或池已析构）时，
        // 这里析构 unique_ptr 即关闭底层连接
        m_connection = nullptr;
        m_pool       = nullptr;
        m_poolLiveness.reset();
    }

} // namespace AsynGyanis::Database
