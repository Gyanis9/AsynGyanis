/**
 * @file Transaction.h
 * @brief RAII 事务 —— 构造即取连接并开启事务，析构未提交时自动回滚
 * @author Gyanis
 * @date 2026-09-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 连接池 + ORM 的标准三件套（连接、查询、事务）之一。本类把「一个事务」
 *          表达成一个栈对象：构造即从池里借出一条连接并执行方言给出的开启语句，
 *          析构时若仍未提交则自动回滚，连接随后归还池。
 *
 * ## 为什么事务对象必须持有连接（而不是每次执行时现取）
 * 事务不是数据库里的一个独立句柄，而是**某一条连接上的会话状态**：BEGIN 只在它被执行的那条
 * 连接上生效，提交与回滚同样只作用于那条连接。因此事务的正确性完全依赖「BEGIN、全部语句、
 * COMMIT/ROLLBACK 走同一条连接」：
 * - 若每条语句各自 pool.acquire() 一条连接，BEGIN 会落在 A 连接、INSERT 落在 B 连接，
 *   B 上的写语句运行在自动提交模式下当场落库；随后的 ROLLBACK 在 A 上回滚一个空事务，
 *   数据却已经写进库里——而且全程不报任何错，是极难定位的一类静默错误；
 * - 连接池对调用方是「借出/归还」的语义，同一时刻一条连接只能被一个持有者使用，
 *   所以事务期内必须一直握着这条连接，不能中途归还。
 * 因此本类在构造时就 acquire() 并持有 PooledConnection，直到对象析构才归还，
 * 期间无论执行多少条语句都复用同一条连接。
 *
 * ## 为什么析构要自动回滚
 * 事务最常见的收尾方式其实是「中途抛出异常」：C++ 里异常会跨过作用域直接跳走，
 * 显式 rollback() 往往来不及执行。若此时什么都不做，连接归还池时会带着一个未结束的事务，
 * 下一个使用者莫名其妙地落在别人的事务里；若反过来「未提交就提交」，则会把半成品数据写库。
 * 回滚是幂等的，且不会把未完成的中间状态留下，因此是唯一安全的默认动作：
 * - 已 commit() 成功的事务，m_isActive 为假，析构不会再发 ROLLBACK，已提交的工作不会被撤销；
 * - 已 rollback() 过的事务同理；
 * - 从未提交的事务，析构补一次 ROLLBACK，把整个事务的影响抹掉。
 *
 * ## 与连接池「归还连接」的关系
 * m_connection（PooledConnection）在析构函数体执行完之后才析构，因此顺序天然是
 * 「先回滚，再把连接还给池」。连接池在归还时会做健康检查：
 * - 回滚成功：连接是干净的，正常回到空闲栈，下次可以继续复用；
 * - 回滚失败：事务是否还在进行已不可知，此时本类会主动 disconnect() 断开该连接，
 *   池的 isConnected() 探活随即判定它不健康并丢弃它——宁可废掉一条连接，
 *   也不能把可能带着未结束事务的连接交给下一个使用者。
 *
 * ## 当前实现的边界
 * - 不支持嵌套事务（SAVEPOINT）：同一个连接上再次 BEGIN 会被引擎拒绝（SQLite 报
 *   "cannot start a transaction within a transaction"），本类如实把失败暴露成异常，
 *   不会静默把内层语句并入外层事务；
 * - 不支持跨线程共享：事务对象与被它保护的连接属于同一线程，本类不做任何加锁。
 *
 * @code
 *   ConnectionPool pool(...);
 *   try
 *   {
 *       Transaction transaction(pool);              // 取连接 + BEGIN
 *       Queryable<Account> account(pool);
 *       Queryable<Account> transactionalAccount(transaction);   // 走事务连接
 *       transactionalAccount.insert(first);
 *       transactionalAccount.insert(second);
 *       transaction.commit();                       // 提交后数据对所有连接可见
 *   }   // 中途抛异常时 transaction 析构自动 ROLLBACK
 *   catch (const std::exception &error)
 *   {
 *       // 事务已回滚，可以安全地重试或降级
 *   }
 * @endcode
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
        Transaction(const Transaction &)            = delete;
        Transaction &operator=(const Transaction &) = delete;
        Transaction(Transaction &&)                 = delete;
        Transaction &operator=(Transaction &&)      = delete;

        /**
         * @brief 提交事务
         *
         * @details 幂等：事务已经结束（已提交或已回滚）时直接返回 true，不重复发送 COMMIT。
         *          提交失败时事务仍被标记为活动，析构阶段还会尝试回滚，不会把失败静默成成功。
         *
         * @return true 提交成功，或事务此前已经结束
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
         * @return true 回滚成功，或事务此前已经结束
         * @return false 回滚失败，原因见 lastError()
         */
        [[nodiscard]] bool rollback();

        /**
         * @brief 判断事务是否仍在进行
         * @return true 事务已开启且尚未提交/回滚成功
         */
        [[nodiscard]] bool isActive() const noexcept { return m_isActive; }

        /**
         * @brief 获取事务持有的连接
         *
         * @details Queryable 通过本方法让全部语句都落在同一条连接上，这是事务正确性的前提。
         *          返回的引用在本对象析构前始终有效，调用方不得保存到本对象之后使用。
         * @return DatabaseConnection& 事务持有的连接
         * @throws Base::LogicException 对象未持有连接（构造失败后继续使用等，正常流程不可达）
         */
        [[nodiscard]] DatabaseConnection &connection();

        /**
         * @brief 获取最后一次事务控制语句失败的原因
         * @return std::string 中文错误描述；无失败时为空串
         */
        [[nodiscard]] std::string lastError() const { return m_lastError; }

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

        PooledConnection            m_connection;        ///< 事务独占的连接，析构时归还池
        std::shared_ptr<SqlDialect> m_dialect;           ///< 本连接的方言，构造时解析并缓存（提供事务语句文本）
        bool                        m_isActive = false;  ///< 事务是否仍在进行，决定析构是否回滚
        std::string                 m_lastError;         ///< 最后一次事务控制语句的失败原因
    };

} // namespace AsynGyanis::Database
