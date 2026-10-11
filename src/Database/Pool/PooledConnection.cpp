#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"

#include <chrono>
#include <utility>

namespace AsynGyanis::Database
{

    PooledConnection::PooledConnection(std::unique_ptr<DatabaseConnection> connection, ConnectionPool *pool) noexcept : m_connection(std::move(connection)), m_pool(pool)
    {
        // 构造时取一份池存活令牌：池析构之后归还路径据此直接关闭连接，
        // 而不是回头调用已释放的池（构造函数不检查参数有效性：空连接 + 空池是合法的「空包装」）
        if (m_pool != nullptr)
        {
            m_poolLiveness = m_pool->livenessToken();
        }

        // 借出账本登记（见 PoolLiveness::borrowStartNanos）：池级「最久在借」读数与
        // heldDuration() 共用这一格。账本挂在令牌上、不碰池对象本身，池析构后本条路径仍安全
        if (m_connection != nullptr && m_poolLiveness != nullptr)
        {
            m_borrowedAt = std::chrono::steady_clock::now();
            m_borrowSlot = m_poolLiveness->claimBorrowSlot(std::chrono::duration_cast<std::chrono::nanoseconds>(m_borrowedAt.time_since_epoch()).count());
        }
    }

    PooledConnection::~PooledConnection()
    {
        doReturnToPool();
    }

    PooledConnection::PooledConnection(PooledConnection &&other) noexcept :
        m_connection(std::exchange(other.m_connection, nullptr)), m_pool(std::exchange(other.m_pool, nullptr)), m_poolLiveness(std::move(other.m_poolLiveness)),
        m_borrowSlot(std::exchange(other.m_borrowSlot, PoolLiveness::kNoBorrowSlot)), m_borrowedAt(std::exchange(other.m_borrowedAt, {}))
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
            m_borrowSlot   = std::exchange(other.m_borrowSlot, PoolLiveness::kNoBorrowSlot);
            m_borrowedAt   = std::exchange(other.m_borrowedAt, {});
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

    void PooledConnection::discard()
    {
        doReturnToPool(true);
    }

    std::chrono::steady_clock::time_point PooledConnection::borrowedAt() const noexcept
    {
        return m_borrowedAt;
    }

    std::chrono::milliseconds PooledConnection::heldDuration() const noexcept
    {
        // 空包装（含已归还/已移动走的）没有持有期可言：给 0 而不是「纪元到现在」那种荒谬值
        if (m_connection == nullptr)
        {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_borrowedAt);
    }

    void PooledConnection::doReturnToPool(const bool isDiscard)
    {
        // double-release 防护：已被 release 或移动走时这里安全返回
        if (m_connection == nullptr)
        {
            return;
        }

        // 账本注销排在归还调用之前：注销只碰令牌共享的账本（不碰池对象），
        // 而归还调用要在令牌锁里判活——顺序无关正确性，先注销让「最久在借」读数尽早反映离场
        if (m_poolLiveness != nullptr)
        {
            m_poolLiveness->releaseBorrowSlot(m_borrowSlot);
        }

        // 将连接归还至池；池已（或正在）析构时按文档承诺直接把连接关掉。
        // 判活与归还调用同在一段令牌锁内（见 ConnectionPool::returnConnectionIfAlive）：
        // 只判一个原子量的话，并发销毁时判活刚通过、调用就踩空
        if (m_pool != nullptr)
        {
            m_pool->returnConnectionIfAlive(m_connection, m_poolLiveness, isDiscard);
        }

        // 清空所有状态，防止重复归还。连接已被移走（或池已析构）时，
        // 这里析构 unique_ptr 即关闭底层连接
        m_connection = nullptr;
        m_pool       = nullptr;
        m_poolLiveness.reset();
        m_borrowSlot = PoolLiveness::kNoBorrowSlot;
        m_borrowedAt = {};
    }

} // namespace AsynGyanis::Database
