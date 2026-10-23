#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/ConnectionPool.h"

#include <chrono>
#include <optional>
#include <utility>

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 结果集存活期间的取消回调：只握裸连接指针，在请求停止的那条线程上同步执行
         */
        struct ResultCancelAction
        {
            DatabaseConnection *connection{nullptr}; ///< 结果集当前租着的连接

            void operator()() const noexcept
            {
                if (connection != nullptr)
                {
                    static_cast<void>(connection->requestQueryCancel());
                }
            }
        };

        /**
         * @brief 跟随结果集生命周期的连接租约：取消令牌在结果集存活期间一直挂在驱动上
         * @details 结果集析构时先摘掉回调（正在执行时等它结束），再按令牌决定归还还是丢弃：
         *          被取消过的连接可能停在半条语句或事务上，不交回池里复用。
         */
        class ResultConnectionLease
        {
        public:
            /**
             * @brief 接管连接并在令牌上挂一个取消回调
             * @param connection 本次借出的包装（所有权移交）
             * @param stopToken 协作取消令牌；结果集存活期间一直有效
             */
            ResultConnectionLease(PooledConnection &&connection, const std::stop_token stopToken) :
                m_connection(std::move(connection)), m_cancelToken(stopToken), m_cancelOnStop(std::in_place, m_cancelToken, ResultCancelAction{m_connection.operator->()})
            {
            }

            ~ResultConnectionLease()
            {
                // 先摘回调再动连接：回调可能在别的线程上正调用 requestQueryCancel，reset 会等它跑完
                m_cancelOnStop.reset();
                if (m_cancelToken.stop_requested())
                {
                    m_connection.discard();
                }
            }

            ResultConnectionLease(const ResultConnectionLease &) = delete;

            ResultConnectionLease &operator=(const ResultConnectionLease &) = delete;

        private:
            PooledConnection                                      m_connection;   ///< 声明在前：最后析构，驱动先收完游标
            std::stop_token                                       m_cancelToken;  ///< 结果集析构时据此决定丢弃还是归还
            std::optional<std::stop_callback<ResultCancelAction>> m_cancelOnStop; ///< 先析构：取消注册并等正在跑的回调收尾
        };

        /**
         * @brief 把包装交给结果集：结果集析构时包装析构，连接随那次析构归还
         * @details 只在结果集非空时移交；执行失败时包装原样留在调用方手里，连接不丢。
         *          带停止令牌时用会保持到结果集析构的租约，游标式结果集后续 next() 也能被打断。
         * @param result 驱动交出的结果集
         * @param connection 本次借出的包装（右值，所有权在此交出）
         * @param stopToken 协作取消令牌；不可停止时退化成普通租约，不付回调的分配
         */
        void lendConnectionToResult(const std::unique_ptr<DatabaseResult> &result, PooledConnection &&connection, const std::stop_token stopToken)
        {
            if (result == nullptr)
            {
                return;
            }
            if (stopToken.stop_possible())
            {
                result->attachLifetimeLease(std::make_shared<ResultConnectionLease>(std::move(connection), stopToken));
                return;
            }
            result->attachLifetimeLease(std::make_shared<PooledConnection>(std::move(connection)));
        }

        /**
         * @brief 带取消令牌执行一次驱动调用，并按结果决定租出、丢弃还是把连接留在包装里
         * @details 停止回调在「请求停止的那条线程」上同步执行，因此只握裸连接指针，不碰可能正被移动的包装。
         *          进入时已停止的执行不启动；执行因取消而失败时连接按「会话可能停在半条事务上」丢弃。
         * @tparam DriverCall 无参可调用体，返回 std::unique_ptr<DatabaseResult>
         * @param connection 本次借出的包装（成功时所有权交出）
         * @param driver 包装持有的连接，回调与调用都只经它进行
         * @param stopToken 协作取消令牌
         * @param driverCall 真正执行驱动操作的调用
         * @return std::unique_ptr<DatabaseResult> 驱动结果；进入时已停止或取消导致失败时为空
         */
        template<typename DriverCall>
        std::unique_ptr<DatabaseResult> executeWithCancellation(PooledConnection &connection, DatabaseConnection &driver, const std::stop_token stopToken, DriverCall &&driverCall)
        {
            if (stopToken.stop_requested())
            {
                return nullptr;
            }

            std::unique_ptr<DatabaseResult> result;
            {
                const std::stop_callback cancelOnStop(stopToken, [&driver] { static_cast<void>(driver.requestQueryCancel()); });
                try
                {
                    result = driverCall();
                } catch (...)
                {
                    // 取消期间抛出的异常不能把半条语句留在连接上：按同一口径丢弃后再原样抛出
                    if (stopToken.stop_requested())
                    {
                        connection.discard();
                    }
                    throw;
                }
            }

            if (result == nullptr && stopToken.stop_requested())
            {
                // 中断过的连接可能停在半条事务上：池不知道它脏在哪，丢弃比复位可靠
                connection.discard();
                return nullptr;
            }

            lendConnectionToResult(result, std::move(connection), stopToken);
            return result;
        }
    } // namespace

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

    PooledConnection::PooledConnection(const PoolAcquireFailure failure) noexcept : m_failure(failure)
    {
    }

    PooledConnection::~PooledConnection()
    {
        doReturnToPool();
    }

    PooledConnection::PooledConnection(PooledConnection &&other) noexcept :
        m_connection(std::exchange(other.m_connection, nullptr)), m_pool(std::exchange(other.m_pool, nullptr)), m_poolLiveness(std::move(other.m_poolLiveness)),
        m_borrowSlot(std::exchange(other.m_borrowSlot, PoolLiveness::kNoBorrowSlot)), m_borrowedAt(std::exchange(other.m_borrowedAt, {})),
        m_failure(std::exchange(other.m_failure, PoolAcquireFailure::None))
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
            m_failure      = std::exchange(other.m_failure, PoolAcquireFailure::None);
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

    PoolAcquireFailure PooledConnection::failureReason() const noexcept
    {
        return m_failure;
    }

    void PooledConnection::release()
    {
        doReturnToPool();
    }

    void PooledConnection::discard()
    {
        doReturnToPool(true);
    }

    std::unique_ptr<DatabaseResult> PooledConnection::execute(const std::string_view command, const std::stop_token stopToken) &&
    {
        if (m_connection == nullptr)
        {
            return nullptr;
        }
        return executeWithCancellation(*this, *m_connection, stopToken, [this, command] { return m_connection->execute(command); });
    }

    std::unique_ptr<DatabaseResult> PooledConnection::execute(const std::string_view command, const std::span<const DatabaseValue> parameters, const std::stop_token stopToken) &&
    {
        if (m_connection == nullptr)
        {
            return nullptr;
        }
        return executeWithCancellation(*this, *m_connection, stopToken, [this, command, parameters] { return m_connection->execute(command, parameters); });
    }

    std::unique_ptr<DatabaseResult> PooledConnection::executeStreaming(const std::string_view command, const std::stop_token stopToken) &&
    {
        if (m_connection == nullptr)
        {
            return nullptr;
        }
        return executeWithCancellation(*this, *m_connection, stopToken, [this, command] { return m_connection->executeStreaming(command); });
    }

    std::unique_ptr<DatabaseResult> PooledConnection::executeStreaming(const std::string_view command, const std::span<const DatabaseValue> parameters,
                                                                       const std::stop_token stopToken) &&
    {
        if (m_connection == nullptr)
        {
            return nullptr;
        }
        return executeWithCancellation(*this, *m_connection, stopToken, [this, command, parameters] { return m_connection->executeStreaming(command, parameters); });
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
