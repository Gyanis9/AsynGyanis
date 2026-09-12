/**
 * @file Transaction.h
 * @brief RAII 事务 —— 构造即取连接并开启事务，析构未提交时自动回滚
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本类把「一个事务」表达成栈对象：构造即从池里借出一条连接并执行方言的开启事务语句，
 *          析构时若仍未提交则自动回滚，连接随后归还池。事务不是独立的数据库句柄，而是某条连接上
 *          的会话状态，因此必须由本对象一直持有那条连接（中途归还或每条语句各取一条都会让
 *          BEGIN 落在 A 连接、写语句落在自动提交的 B 连接上：数据当场落库、回滚作用在空事务上）。
 *
 * @note 析构必须自动回滚：异常会跨过作用域跳走，显式 rollback() 常常来不及执行，而未结束的事务
 *       会污染下一个使用者。回滚是幂等的，已提交/已回滚的事务不会再发语句（m_isActive 为假）。
 *       回滚失败时事务状态不可知，此时主动断开连接让池的探活丢弃它。不支持嵌套事务与跨线程共享。
 */
#pragma once

#include "Database/Common/DatabaseConnection.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"

#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief RAII 事务对象
     *
     * @details 构造即从连接池借出连接并执行方言的开启事务语句；commit() / rollback() 幂等，
     *          析构时若仍未提交则自动回滚，随后把连接归还池。
     *
     * @warning 事务对象与它持有的连接都不允许跨线程使用；对象生命周期必须覆盖
     *          所有走该事务执行的查询/写语句（Queryable(Transaction&) 只保存指针）。
     */
    class Transaction
    {
    public:
        /**
         * @brief 从连接池借出连接并开启事务
         *
         * @param pool 提供连接的连接池，本对象在析构前一直占用其中一条连接
         *
         * @throws ConnectionUnavailableException 池中取不到连接（已达上限且等待超时，
         *         或连接工厂创建失败）
         * @throws QueryExecutionException 驱动拒绝 BEGIN（例如同一连接上已有未结束的事务）
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现，取不到开启事务的语句文本
         * @note 构造成功即代表数据库已经进入了事务；构造失败不会留下半开状态，
         *       已借出的连接由 PooledConnection 在栈展开时归还池
         */
        explicit Transaction(ConnectionPool &pool);

        /**
         * @brief 析构：未提交则自动回滚，随后把连接归还连接池
         *
         * @details 析构不抛异常：回滚失败只记录在 lastError() 中（此时对象即将消失），
         *          并把连接断开以便池丢弃它。提交/回滚的顺序与连接归属见文件头说明。
         */
        ~Transaction();

        // 事务同时独占「连接所有权」与「唯一的会话状态」，拷贝与移动都会让
        // 「谁负责提交/回滚、连接何时归还」变得含糊，还可能让两个对象操作同一事务；
        // 绑定事务的 Queryable 还保存着本对象的地址，移动会破坏该引用的有效性
        Transaction(const Transaction &) = delete;

        Transaction &operator=(const Transaction &) = delete;

        Transaction(Transaction &&) = delete;

        Transaction &operator=(Transaction &&) = delete;

        /**
         * @brief 提交事务
         *
         * @details 幂等：事务已经结束（已提交或已回滚）时直接返回 true，不重复发送 COMMIT。
         *          提交失败时事务仍被标记为活动，析构阶段还会尝试回滚，不会把失败静默成成功。
         *
         * @return true 提交成功，或事务已经结束
         * @return false 提交失败（如连接已断开），原因见 lastError()
         */
        [[nodiscard]] bool commit();

        /**
         * @brief 回滚事务
         *
         * @details 幂等：事务已经结束时直接返回 true。回滚失败说明事务状态不可知，
         *          本方法会断开底层连接，让连接池在归还时丢弃它，避免把带着未结束事务的
         *          连接交给下一个使用者。
         *
         * @return true 回滚成功，或事务已经结束
         * @return false 回滚失败，原因见 lastError()
         */
        [[nodiscard]] bool rollback();

        /**
         * @brief 判断事务是否仍在进行
         * @return true 事务已开启且尚未提交/回滚成功
         */
        [[nodiscard]] bool isActive() const noexcept
        {
            return m_isActive;
        }

        /**
         * @brief 获取事务持有的连接
         *
         * @details Queryable 通过本方法让全部语句都落在同一条连接上，这是事务正确性的前提。
         *          返回的引用在本对象析构前始终有效，调用方不得保存到本对象之后使用。
         * @return DatabaseConnection& 事务持有的连接
         * @throws Base::LogicException 对象未持有连接（构造失败后继续使用等，正常流程不可达）
         */
        [[nodiscard]] DatabaseConnection &connection() const;

        /**
         * @brief 获取最后一次事务控制语句失败的原因
         * @return std::string 中文错误描述；无失败时为空串
         */
        [[nodiscard]] std::string lastError() const
        {
            return m_lastError;
        }

    private:
        /**
         * @brief 在当前连接上执行一条事务控制语句
         * @details BEGIN / COMMIT / ROLLBACK 都走同一条路径：清空上一轮错误、执行、
         *          失败时拼出带语句文本的中文原因。
         * @param statement 事务控制语句文本，由方言提供
         * @return true 语句执行成功
         * @return false 执行失败，原因见 lastError()
         */
        [[nodiscard]] bool executeControlStatement(std::string_view statement);

        PooledConnection            m_connection;       ///< 事务独占的连接，析构时归还池
        std::shared_ptr<SqlDialect> m_dialect;          ///< 本连接的方言，构造时解析并缓存（提供事务语句文本）
        bool                        m_isActive = false; ///< 事务是否仍在进行，决定析构是否回滚
        std::string                 m_lastError;        ///< 最后一次事务控制语句的失败原因
    };

} // namespace AsynGyanis::Database
