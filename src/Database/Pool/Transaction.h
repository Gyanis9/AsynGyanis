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
 *       回滚失败时事务状态不可知，此时主动断开连接让池的探活丢弃它。
 *       **嵌套不是再开一个 Transaction**（两个引擎都不吃嵌套 BEGIN：MySQL 的 START TRANSACTION 会
 *       隐式提交上一笔、SQLite 直接报错，而再借一条连接更是另一个事务），要的是同一笔事务里的
 *       保存点：savepoint() / rollbackToSavepoint() / releaseSavepoint()，见那三个方法的说明。
 *       走本事务的语句由 m_statementMutex 串行落在那一条连接上（驱动连接不是线程安全的）。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseConnection.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"

#include <memory>
#include <mutex>
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
     * @warning 事务对象本身不允许跨线程使用：commit() / rollback() / 析构要在同一个线程上按顺序发生。
     *          走本事务的**语句**则由 acquireStatementLock() 串行落在那一条连接上（异步接口会把语句
     *          投到工作线程，调用方无需自己保证不并发），对象生命周期必须覆盖所有走该事务执行的
     *          查询/写语句（Queryable(Transaction&) 只保存指针）。
     */
    class ASYN_DATABASE_API Transaction
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
         * @brief 在本事务里立一个保存点（事务内部的可回退点）
         *
         * @details 用途是「一笔事务里回退一小段而不要整笔作废」：批量写入里某一条坏了，可以只退到
         *          这一批之前的那个点，前面已经做的工作保住。两个引擎都**不支持嵌套 BEGIN**（MySQL 的
         *          START TRANSACTION 会隐式提交上一笔，SQLite 直接报错），所以嵌套这一层只能由保存点回答，
         *          而不是再构造一个 Transaction 对象——后者会去池里再借一条连接，那是另一个事务。
         *
         * @note 名称怎么进 SQL：交给方言的 `savepointStatement()`，那里一律用本引擎的引用符把名字包起来
         *       （内部的同字符翻倍），所以调用方给的串不会变成语句的一部分。名字本身只要求非空、
         *       不全是空白、不含 NUL——长度上限由各引擎回答（MySQL 标识符 64 字符），拒绝时原因走
         *       lastError() 而不是我们替它编一个数。
         * @note SQLite 的语句缓存按语句文本存：不同名字各占一格（上限 64 的 LRU），所以别把保存点名
         *       当循环计数器无限增长地起，复用几个稳定的名字即可。
         *
         * @param name 保存点名
         * @throws Base::InvalidArgumentException 名字为空、全是空白或含 NUL（是调用方的输入问题，
         *         不是运行时状态，因此当场抛而不留一个「返回 false 但没人知道为什么」的口子）
         * @return true 保存点已建立
         * @return false 本事务已结束（此时一条语句都不发）或引擎拒绝，原因见 lastError()
         */
        [[nodiscard]] bool savepoint(std::string_view name);

        /**
         * @brief 回退到本事务里的某个保存点，**事务仍然开着**
         *
         * @details 与 `rollback()` 的关键区别就在这里：这条语句撤掉的是「那个点之后」的写入，
         *          而那个点之前的工作照旧留在未提交状态，之后仍然可以 `commit()`。
         *          回退成功后 `isActive()` 为真——这是它能替代嵌套事务的前提。
         *
         * @param name 之前立过的保存点名
         * @throws Base::InvalidArgumentException 名字为空、全是空白或含 NUL（同 `savepoint()`）
         * @return true 已回退到该点
         * @return false 本事务已结束（不发语句）、名字没立过或引擎拒绝，原因见 lastError()；
         *         失败不会把事务结束掉（引擎报的是「没有这个保存点」而不是「事务没了」）
         */
        [[nodiscard]] bool rollbackToSavepoint(std::string_view name);

        /**
         * @brief 丢弃一个保存点（不回滚、也不提交它之后的工作）
         *
         * @details 语义只是「这个名字之后不能被回退了」。省略这一步没有正确性代价：
         *          保存点随事务结束一并消失，因此它只在需要早释放命名空间时有用。
         *
         * @param name 之前立过的保存点名
         * @throws Base::InvalidArgumentException 名字为空、全是空白或含 NUL（同 `savepoint()`）
         * @return true 已丢弃
         * @return false 本事务已结束（不发语句）、名字没立过或引擎拒绝，原因见 lastError()
         */
        [[nodiscard]] bool releaseSavepoint(std::string_view name);

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
         * @brief 取得「本事务这条连接」的独占使用权
         *
         * @details 驱动连接不是线程安全的（一个句柄一条协议流），而 Queryable 的异步接口会把语句
         *          投到 AsyncExecutor 的工作线程上执行——调用方即使全程待在自己的线程里，两条并发
         *          语句也会各自落到一个工作线程上，同时踩这同一条连接。取到这把锁即代表可以独占
         *          这条连接，返回的句柄可移动、离开作用域自动释放；已被占用时阻塞等待而不是并发进入。
         * @return std::unique_lock<std::mutex> 已持有的独占锁
         * @note 同一线程内嵌套取用会自锁死：语句执行期间不得再发第二条走同一事务的语句
         */
        [[nodiscard]] std::unique_lock<std::mutex> acquireStatementLock() const;

        /**
         * @brief 获取最后一次事务控制语句失败的原因
         * @return const std::string& 中文错误描述的常引用；无失败时为空串
         */
        [[nodiscard]] const std::string &lastError() const
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

        /**
         * @brief 三条保存点语句共用的两道前置检查
         *
         * @details 分两件事是因为它们的性质不同：**名字坏**是调用方的输入错误（空串、全是空白、
         *          含 NUL），当场抛，不留「返回 false 但没人知道为什么」的口子；**事务已经结束**是
         *          运行期状态（提交过了或已经回滚过），按本类的错误通道回 false 并写 lastError()，
         *          且此时一条语句都不发——在一个已结束的事务上立保存点，引擎给的是「没有这个保存点」
         *          之类的误导信息，而我们说得清是哪种。
         *
         * @param name 调用方给的保存点名
         * @param action 错误文本里的动作名（「建立」「回退到」「丢弃」），让报错指着实际那一步
         * @throws Base::InvalidArgumentException 名字为空、全是空白或含 NUL
         * @return true 名字可用且事务仍在进行，可以发语句
         * @return false 事务已结束，原因已写进 lastError()
         */
        [[nodiscard]] bool checkSavepointPremises(std::string_view name, std::string_view action);

        PooledConnection            m_connection;       ///< 事务独占的连接，析构时归还池
        std::shared_ptr<SqlDialect> m_dialect;          ///< 本连接的方言，构造时解析并缓存（提供事务语句文本）
        bool                        m_isActive = false; ///< 事务是否仍在进行，决定析构是否回滚
        std::string                 m_lastError;        ///< 最后一次事务控制语句的失败原因

        // 驱动连接不是线程安全的（一个句柄一条协议流），而 Queryable 的异步接口会把语句投到
        // 工作线程上执行，因此同一事务上的语句必须串行落在这条连接里。锁由 Queryable 取用。
        mutable std::mutex m_statementMutex; ///< 事务连接的使用权：同一时刻只让一条语句碰这条连接
    };

} // namespace AsynGyanis::Database
