/**
 * @file Queryable.h
 * @brief 查询构建器 —— 类型安全的 ORM 查询入口，构建 QueryNode 查询树
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 三条使用路径：离线（默认构造，只用 toSql() 输出近似 SQL 文本）、绑定连接池（借连接 → 方言
 *          translate*() → execute(sql, 参数) → 行映射）、绑定事务（全部语句走事务持有的那一条连接）。
 *          本类另有离线调试渲染器 buildSelectSql() 等：不加标识符引号、SELECT 列为空时输出 "*"，只供调试
 *          与离线测试，不参与执行；真正的执行 SQL 一律由方言 translate*() 生成，每个执行器都有 Async 版本。
 */
#pragma once

#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/QueryNode.h"
#include "Database/Queryable/RowMapper.h"
#include "Database/Queryable/TableSchema.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Database::Queryable
{

    /**
     * @brief ORM 查询构建器模板
     *
     * @tparam T 表数据结构类型，需有对应的 TableSchema<T> 特化
     *
     * @details 提供流式接口构建类型安全的数据库查询；四种构造方式分别对应离线模式与
     *          绑定连接池（可显式指定方言）、绑定事务三种在线模式。
     *
     * @note 全部执行器方法（含异步版本）均要求在线模式：未绑定连接池或事务时抛 Base::LogicException。
     * @note 各方法文档里的 `@throws DatabaseException` 是家族根类型，具体子类按失败原因对应：
     *       取连接失败 → ConnectionUnavailableException（可重试）；语句执行失败 → QueryExecutionException
     *       （重试无意义，应记日志让请求失败）；结果集映射失败 → RowMappingException（表结构与结构体声明不一致）。
     * @note 本类不是线程安全的：异步方法只保证阻塞执行发生在工作线程上，调用方仍应避免在
     *       同一个查询对象上并发地构建查询与发起执行。
     */
    template<typename T>
    class Queryable
    {
    public:
        /**
         * @brief 默认构造（离线模式）
         *
         * @details 用于仅生成 SQL 或测试场景。表名与列名一律取自 TableSchema<T>（kTableName / kColumns）：
         *          表结构只有一个真值来源，本类刻意不提供单独设置表名的入口。
         */
        Queryable()
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        /**
         * @brief 构造并绑定连接池（在线模式）
         *
         * @details 使用连接池获取数据库连接执行查询，方言在首次执行时从池中连接的真实
         *          DatabaseType 推导并缓存。适用于池只服务单一数据库类型的场景。
         *
         * @param pool 数据库连接池
         * @note 只想构造、不立刻执行查询时不会产生任何数据库往返；
         *       推导类型会借出一条连接并立刻归还（不占用池容量），若池为空则创建一条新连接
         */
        explicit Queryable(ConnectionPool &pool) :
            m_pool(&pool)
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        /**
         * @brief 构造并绑定连接池与数据库类型（在线模式，显式指定方言）
         *
         * @details 方言类型由调用方直接给出，不从池中连接推导：适用于希望避免「推导时借出连接」
         *          这一副作用，或连接工厂封装较复杂、类型已知的场景。
         *
         * @param pool 数据库连接池
         * @param databaseType 池中连接的数据库类型，决定使用哪个 SqlDialect
         */
        Queryable(ConnectionPool &pool, DatabaseType databaseType) :
            m_pool(&pool), m_databaseType(databaseType)
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        /**
         * @brief 构造并绑定事务（在线模式，语句走事务连接）
         *
         * @details 与绑定连接池的唯一区别是本对象的全部语句都在事务持有的那条连接上执行，绝不各自
         *          从池里再取一条：否则 BEGIN 落在一个连接上、写语句落在别的连接上，那些写语句实际运行在
         *          自动提交模式下，回滚只能回滚一个空事务而数据已在库里，且不会报任何错。
         *          提交与回滚仍由调用方通过事务对象决定，本对象不代劳。
         *
         * @param transaction 事务对象，其生命周期必须覆盖本对象的所有执行调用
         * @note 方言类型直接取自事务连接，不需要像绑定连接池那样借出一条连接来探测
         */
        explicit Queryable(Transaction &transaction) :
            m_transaction(&transaction)
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        // ========================================================================
        // 构建器方法
        // ========================================================================

        /**
         * @brief 添加 WHERE 条件
         *
         * @details 使用 Expression.h 中的运算符构建条件。
         *          支持链式多次调用，条件之间用 AND 连接。
         *
         * @param condition 通过列比较或逻辑组合构建的条件
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &where(WhereCondition condition)
        {
            // 如果条件是 AND/OR/NOT 复合节点或普通比较，直接压入列表
            m_queryNode.whereConditions.push_back(std::move(condition));
            return *this;
        }

        /**
         * @brief 添加 ORDER BY 子句
         *
         * @param order 使用 asc()/desc() 创建的排序子句
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &orderBy(OrderByClause order)
        {
            m_queryNode.orderBy.push_back(std::move(order));
            return *this;
        }

        /**
         * @brief 设置 LIMIT 子句
         *
         * @param count 返回行数上限
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &limit(std::size_t count)
        {
            m_queryNode.limit = count;
            return *this;
        }

        /**
         * @brief 设置 OFFSET 子句
         *
         * @param skip 跳过的行数
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &offset(std::size_t skip)
        {
            m_queryNode.offset = skip;
            return *this;
        }

        /**
         * @brief 设置 SELECT 列
         *
         * @details 默认（不调用此方法或传入空列表）使用 TableSchema 中 kColumns 的顺序。
         *          列名之外也接受表达式文本（如 COUNT(*)、COALESCE(age, 0)），表达式按原样拼进 SQL。
         *
         * @param columns 列名或表达式文本列表
         * @return Queryable& 自身引用，支持链式调用
         * @warning 表达式文本**按原样拼进 SQL 语句**，不是参数绑定的对象：
         *          只允许传入编译期常量或可信文本，任何来自用户输入的片段都必须走参数绑定
         *          （值用占位符），否则就是 SQL 注入面
         */
        Queryable &select(std::vector<std::string> columns)
        {
            m_queryNode.selectColumns = std::move(columns);
            return *this;
        }

        /**
         * @brief 添加 JOIN 子句
         *
         * @param join 连接子句
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &join(JoinClause join)
        {
            m_queryNode.joins.push_back(std::move(join));
            return *this;
        }

        /**
         * @brief 设置 GROUP BY 字段
         *
         * @param fields 分组列名列表（也接受表达式文本，规则同 select()）
         * @return Queryable& 自身引用，支持链式调用
         * @warning 与 select() 同一条信任边界：文本按原样拼进 SQL，只允许编译期常量或可信文本
         */
        Queryable &groupBy(std::vector<std::string> fields)
        {
            std::vector<FieldReference> fieldRefs;
            fieldRefs.reserve(fields.size());
            for (auto &field: fields)
            {
                fieldRefs.push_back(FieldReference{.name = std::move(field)});
            }
            m_queryNode.groupBy = std::move(fieldRefs);
            return *this;
        }

        /**
         * @brief 指定异步执行器（阻塞任务的工作线程池）
         *
         * @details 只影响异步方法（toListAsync / firstAsync / countAsync / insertAsync /
         *          insertBatchAsync / updateAsync / executeNonQueryAsync）：
         *          未调用本方法时这些方法使用进程级共享的 AsyncExecutor::shared()，
         *          需要控制工作线程数或让执行器与连接池成对管理时用本方法注入自己的实例。
         *
         * @param executor 异步执行器，本对象只保存指针，其生命周期必须覆盖所有异步调用
         * @return Queryable& 自身引用，支持链式调用
         * @note 同步方法不使用执行器，调用本方法不会改变它们的行为
         */
        Queryable &useAsyncExecutor(Core::AsyncExecutor &executor)
        {
            m_asyncExecutor = &executor;
            return *this;
        }

        // ========================================================================
        // 执行器方法（在线模式）
        // ========================================================================

        /**
         * @brief 执行查询并返回所有结果行
         *
         * @details 执行链路：从连接池取连接 → 用方言把查询树翻译成参数化 SQL →
         *          带参数执行 → 按 TableSchema<T> 把结果集逐行映射成 T。
         *          SELECT 列在未显式指定时按 TableSchema<T>::kColumns 的顺序展开。
         *
         * @return std::vector<T> 查询结果列表，无匹配行时为空向量
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败，原因见异常文本
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（如 MySQL / Redis）
         */
        [[nodiscard]] std::vector<T> toList()
        {
            requireOnline("toList()");
            return fetchRows(resolvedQueryNode());
        }

        /**
         * @brief 执行查询并返回第一行结果
         *
         * @details 在查询树副本上把行数上限压到 1 后再执行，不会把整表读进内存再丢弃。
         *
         * @return std::optional<T> 第一行；没有任何匹配行时返回空
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::optional<T> first()
        {
            requireOnline("first()");

            QueryNode limitedNode = resolvedQueryNode();
            // 只取一行：LIMIT 1 让数据库侧提前停止扫描，比取回全部再取首元素高效得多
            limitedNode.limit     = 1U;

            return fetchFirst(limitedNode);
        }

        /**
         * @brief 统计匹配行数
         *
         * @details 把 SELECT 列表替换成 COUNT(*)、并清掉 ORDER BY 与分页后执行
         *          （聚合结果只有一行，排序与分页没有意义，某些数据库还会直接报错）。
         *          手动设置的 SELECT 列会被本方法忽略；GROUP BY 保留，此时返回第一组的计数。
         *
         * @return std::int64_t 匹配的行数；结果为空或计数列为 NULL 时返回 0
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或 SQL 执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t count()
        {
            requireOnline("count()");

            // 计数方向整段 SELECT 列表被 COUNT(*) 顶掉，因此不走 resolvedQueryNode()：
            // 先把全部列名展开再覆盖一遍是白做（列名展开要建一个 vector<string>）
            QueryNode countingNode     = m_queryNode;
            countingNode.selectColumns = {"COUNT(*)"};
            countingNode.orderBy.clear();
            countingNode.limit.reset();
            countingNode.offset.reset();

            const SqlDialect &dialect = requireDialect();
            ConnectionLease   lease   = acquireConnection(m_pool, m_transaction);
            return countOn(*lease.connection, dialect, countingNode);
        }

        /**
         * @brief 执行非查询操作：按当前查询树的 WHERE 条件删除数据
         *
         * @details 生成并执行 "DELETE FROM 表 [WHERE 条件]"。SQL 文本完全由方言生成
         *          （SqlDialect::translateDelete()），条件渲染、参数收集顺序与 SELECT / UPDATE
         *          共用方言层的那一份实现，因此参数同样以绑定方式送入，不会拼接进 SQL 文本。
         *          INSERT / UPDATE 带赋值语义，无法由查询树表达，请改用 insert() / update()。
         *
         * @return std::int64_t 受影响的行数；驱动不提供该信息时返回 0
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @warning 查询树不含任何条件时生成的语句是 "DELETE FROM 表"，会清空全表；
         *          需要限定范围请先调用 where()
         */
        [[nodiscard]] std::int64_t executeNonQuery()
        {
            requireOnline("executeNonQuery()");
            return executeStatement(requireDialect().translateDelete(m_queryNode));
        }

        /**
         * @brief 按结构体字段插入一行
         *
         * @details 按 TableSchema<T>::kColumns 的顺序取出列名与取值，交给方言生成
         *          "INSERT INTO 表 (列…) VALUES (?, …)"；字段值全部以绑定参数传入，
         *          本类不拼接任何 SQL 文本。std::optional 成员为空时绑定为 SQL NULL。
         *
         * @param row 待插入的结构体（主键等字段由调用方填好，本方法不做自增处理）
         * @return std::int64_t 受影响的行数（成功插入一行时为 1；驱动不提供该信息时为 0）
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或语句执行失败（如唯一约束冲突）
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t insert(const T &row)
        {
            requireOnline("insert()");
            return executeStatement(buildInsertStatement(row));
        }

        /**
         * @brief 批量插入多行，一次生成多行 VALUES
         *
         * @details 走方言的 SqlDialect::translateInsertBatch()，把 rows 一次写成
         *          "INSERT INTO 表 (列…) VALUES (?, …), (?, …), …"。参数总数受引擎上限约束（SQLite 为 999），
         *          因此按「每行占用的参数个数 = 列数」换算出每批行数并自动分块；
         *          分块后必须处于同一个事务，否则中途失败会留下「前几批已提交、后几批没写」的半成品。
         *
         * @param rows 待插入的行集合，允许为空（空集合直接返回 0，不产生任何语句）
         * @return std::int64_t 累计受影响的行数（正常等于 rows.size()；驱动不提供时为 0）
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、事务开启失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note 已绑定事务时不会自行提交或回滚：分块共用事务的连接，提交与否由调用方决定
         */
        [[nodiscard]] std::int64_t insertBatch(std::span<const T> rows)
        {
            requireOnline("insertBatch()");

            // 空集合不生成语句：SQL 里 "VALUES" 后面必须有至少一组括号，没有可写的内容就没有语句
            if (rows.empty())
            {
                return 0;
            }

            // 分块判定与执行整体交给静态实现：异步版本在工作线程上调用同一份实现，
            // 两条路径的每批行数换算与事务覆盖范围因此不可能出现分歧
            return insertBatchOn(m_pool, m_transaction, requireDialect(), rows);
        }

        /**
         * @brief 按结构体主键更新一行
         *
         * @details SET 的列与取值来自非主键列，WHERE 条件由主键列构成，两者一起交给方言生成
         *          "UPDATE 表 SET 非主键列 = ?, … WHERE 主键 = ?"：主键列不进 SET
         *          （更新主键会破坏行标识），它的值改作 WHERE 条件；所有取值仍然走参数绑定，
         *          条件渲染与 SELECT / DELETE 共用方言层的那一份实现。
         *
         * @param row 待更新的结构体，主键字段用于定位目标行
         * @return std::int64_t 受影响的行数；0 表示没有匹配的行（无此主键）
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws Base::LogicException TableSchema<T>::kPrimaryKey 未在 kColumns 中声明，
         *         或表中只有主键列（没有可更新的列），无法生成 UPDATE
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t update(const T &row)
        {
            requireOnline("update()");
            return executeStatement(buildUpdateStatement(row));
        }

        // ========================================================================
        // 执行器方法（在线模式，异步：阻塞链路挪到 AsyncExecutor 的工作线程）
        // ========================================================================

        /**
         * @brief 异步执行查询并返回所有结果行
         *
         * @details 与 toList() 语义完全一致，唯一差别是**不阻塞调用线程**：方言解析与 SQL 生成
         *          在提交前完成（纯文本变换，不访问数据库），随后「取连接 → 执行 → 行映射」
         *          整段阻塞链路交给 AsyncExecutor 的工作线程，任务完成后协程在 completionLoop
         *          所在线程上恢复并交出结果。
         *
         * @param completionLoop 恢复本协程用的事件循环；其 run() 必须正在运行（或即将运行），
         *        且对象生命周期要覆盖到任务完成之后，否则协程永远得不到恢复
         * @return Core::Task<std::vector<T> > 惰性启动的协程，co_await 后得到结果行列表
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败；异常原样穿过工作线程
         *         与调度投递，在 co_await 处重新抛出（类型与消息都不变）
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note 提交动作只做入队，因此 co_await 之前的耗时与数据库无关；真正的等待发生在协程挂起之后
         */
        [[nodiscard]] Core::Task<std::vector<T> > toListAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("toListAsync()");

            // 方言与查询树在提交前定型：二者都是值语义的纯数据，捕获进任务后工作线程
            // 不再触碰本对象，因此「查询构建器被并发使用」这类问题不会借异步路径被放大
            std::shared_ptr<SqlDialect> dialect      = resolveDialect();
            QueryNode                   resolvedNode = resolvedQueryNode();
            ConnectionPool *            pool         = m_pool;
            Transaction *               transaction  = m_transaction;

            // 类模板内经由成员函数返回值访问成员模板时，GCC 要求名字前写 template 关键字，
            // 否则 '<' 会被当成小于号解析；MSVC 接受缺省写法，这里按最严的编译器来写
            std::vector<T> rows = co_await asyncExecutor().template submit<std::vector<T> >(
                    completionLoop,
                    [dialect, resolvedNode = std::move(resolvedNode), pool, transaction]() -> std::vector<T>
                    {
                        // 整段阻塞链路在工作线程上执行：取连接、执行语句、逐行映射成结构体
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return fetchRowsOn(*lease.connection, *dialect, resolvedNode);
                    });

            co_return rows;
        }

        /**
         * @brief 异步执行查询并返回第一行结果
         *
         * @details 与 first() 语义完全一致：先在查询树副本上把行数上限压到 1，再交给工作线程执行，
         *          因此不会把整表读进内存。完成后的恢复时机与 toListAsync() 相同。
         *
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::optional<T> > 惰性启动的协程；无匹配行时结果为空 optional
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] Core::Task<std::optional<T> > firstAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("firstAsync()");

            std::shared_ptr<SqlDialect> dialect     = resolveDialect();
            QueryNode                   limitedNode = resolvedQueryNode();
            // 只取一行：让数据库侧提前停止扫描，与同步版 first() 的取舍一致
            limitedNode.limit                       = 1U;
            ConnectionPool *pool                    = m_pool;
            Transaction *   transaction             = m_transaction;

            std::optional<T> firstRow = co_await asyncExecutor().template submit<std::optional<T> >(
                    completionLoop,
                    [dialect, limitedNode = std::move(limitedNode), pool, transaction]() -> std::optional<T>
                    {
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return fetchFirstOn(*lease.connection, *dialect, limitedNode);
                    });

            co_return firstRow;
        }

        /**
         * @brief 异步统计匹配行数
         *
         * @details 与 count() 语义完全一致：把 SELECT 列表换成 COUNT(*)、清掉 ORDER BY 与分页，
         *          再由工作线程执行。完成后的恢复时机与 toListAsync() 相同。
         *
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；结果为空或计数列为 NULL 时为 0
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或 SQL 执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] Core::Task<std::int64_t> countAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("countAsync()");

            std::shared_ptr<SqlDialect> dialect = resolveDialect();
            // 与同步的 count() 同一条理由：列名展开的结果马上被 COUNT(*) 顶掉，不必先做一遍
            QueryNode countingNode     = m_queryNode;
            countingNode.selectColumns = {"COUNT(*)"};
            countingNode.orderBy.clear();
            countingNode.limit.reset();
            countingNode.offset.reset();
            ConnectionPool *pool        = m_pool;
            Transaction *   transaction = m_transaction;

            std::int64_t countedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop,
                    [dialect, countingNode = std::move(countingNode), pool, transaction]() -> std::int64_t
                    {
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return countOn(*lease.connection, *dialect, countingNode);
                    });

            co_return countedRows;
        }

        /**
         * @brief 异步执行非查询操作：按当前查询树的 WHERE 条件删除数据
         *
         * @details 与 executeNonQuery() 语义完全一致：SQL 文本由方言生成（translateDelete），
         *          语句与参数在提交前定型，执行交给工作线程。完成后的恢复时机与 toListAsync() 相同。
         *
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；受影响行数（驱动不提供时为 0）
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @warning 查询树不含任何条件时生成的语句是 "DELETE FROM 表"，会清空全表
         */
        [[nodiscard]] Core::Task<std::int64_t> executeNonQueryAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("executeNonQueryAsync()");

            // 语句翻译是纯文本且不依赖连接，放在提交前做；参数已经收集在 statement 里
            SqlStatement    statement   = resolveDialect()->translateDelete(m_queryNode);
            ConnectionPool *pool        = m_pool;
            Transaction *   transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop,
                    [statement = std::move(statement), pool, transaction]() -> std::int64_t
                    {
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return executeOn(*lease.connection, statement);
                    });

            co_return affectedRows;
        }

        /**
         * @brief 异步插入一行
         *
         * @details 与 insert() 语义完全一致：语句与参数在提交前由方言定型（纯文本变换），
         *          「取连接 → 执行」这段阻塞链路交给 AsyncExecutor 的工作线程，
         *          完成后协程在 completionLoop 所在线程上恢复。
         *
         * @param row 待插入的结构体（主键等字段由调用方填好，本方法不做自增处理）
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；受影响行数（驱动不提供时为 0）
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或语句执行失败（如唯一约束冲突）
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note **row 按值接收**，与另几个异步方法的引用/视图入参刻意不同：本方法是惰性启动的
         *       协程，函数体（包括把 row 的取值转成绑定参数）要到**首次 resume** 才执行，
         *       若按引用接收，调用方写「先拿 Task 再 resume」就会让引用指向已销毁的临时对象，
         *       而且不报错、只静默读到垃圾值。按值接收让协程帧自己持有一份副本，
         *       传临时对象也安全（拷贝成本与后面必然发生的参数拷贝同量级）
         */
        [[nodiscard]] Core::Task<std::int64_t> insertAsync(T row, Core::EventLoop &completionLoop)
        {
            requireOnline("insertAsync()");

            // 语句生成要读 row 并访问本对象的查询树，必须在提交前完成；之后按值捕获交给工作线程
            SqlStatement    statement   = buildInsertStatement(row);
            ConnectionPool *pool        = m_pool;
            Transaction *   transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop,
                    [statement = std::move(statement), pool, transaction]() -> std::int64_t
                    {
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return executeOn(*lease.connection, statement);
                    });

            co_return affectedRows;
        }

        /**
         * @brief 异步批量插入多行
         *
         * @details 与 insertBatch() 语义完全一致：分块、共连接、事务覆盖全部批次这些规则
         *          由同步与异步共用的静态实现决定（见 insertBatchOn），本方法只负责把
         *          「全部行」交给工作线程执行，因此不会阻塞调用线程。
         *
         * @param rows 待插入的行集合，允许为空（空集合直接得到 0，不产生任何语句）
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；累计受影响行数
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、事务开启失败、语句执行失败或本地事务提交失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note **rows 按值接收**（理由同 insertAsync）：惰性协程要到首次 resume 才读入参，
         *       视图（span）不持有所指数据，按视图接收会让「先拿 Task 再 resume」静默读到
         *       已销毁的容器。需要传已有容器时用 std::move 转交，避免多一次拷贝；
         *       传入 std::span 的调用方需自行构造 vector（这是让协程帧拥有数据的必要代价）
         */
        [[nodiscard]] Core::Task<std::int64_t> insertBatchAsync(std::vector<T> rows, Core::EventLoop &completionLoop)
        {
            requireOnline("insertBatchAsync()");

            // 方言解析可能借出连接探测类型，放在提交前（与其它异步方法一致）
            std::shared_ptr<SqlDialect> dialect     = resolveDialect();
            ConnectionPool *            pool        = m_pool;
            Transaction *               transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop,
                    [dialect, rows = std::move(rows), pool, transaction]() -> std::int64_t
                    {
                        // 空集合的判断放在任务内部而不是提交前：本方法只 co_await 一次，
                        // 不需要依赖「协程会在未挂起的情况下直接完成」这种额外前提
                        if (rows.empty())
                        {
                            return 0;
                        }
                        return insertBatchOn(pool, transaction, *dialect, rows);
                    });

            co_return affectedRows;
        }

        /**
         * @brief 异步按主键更新一行
         *
         * @details 与 update() 语义完全一致：SET 列与主键 WHERE 条件由方言生成，
         *          语句与参数在提交前定型，执行交给工作线程。完成后的恢复时机与 toListAsync() 相同。
         *
         * @param row 待更新的结构体，主键字段用于定位目标行
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；受影响行数（0 表示没有匹配的行）
         * @throws Base::LogicException 当前为离线模式；或主键未在 kColumns 声明、表中只有主键列
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note 主键缺失这类编程错误在**首次 resume** 时就会抛出（语句生成阶段），
         *       不会变成工作线程上的异常；row 按值接收的理由见 insertAsync()
         */
        [[nodiscard]] Core::Task<std::int64_t> updateAsync(T row, Core::EventLoop &completionLoop)
        {
            requireOnline("updateAsync()");

            SqlStatement    statement   = buildUpdateStatement(row);
            ConnectionPool *pool        = m_pool;
            Transaction *   transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop,
                    [statement = std::move(statement), pool, transaction]() -> std::int64_t
                    {
                        ConnectionLease lease = acquireConnection(pool, transaction);
                        return executeOn(*lease.connection, statement);
                    });

            co_return affectedRows;
        }

        // ========================================================================
        // SQL 生成
        // ========================================================================

        /**
         * @brief 生成 SQL 文本
         *
         * @details 将当前 QueryNode 查询树转换为近似 SQL 字符串，用于调试与离线测试：不给标识符加引号、
         *          SELECT 列为空时固定输出 "*"、分页不做方言补全。真正执行的 SQL 由 SqlDialect::translate()
         *          生成（带标识符引用、按 TableSchema 展开列、按方言补全分页），两者文本可能不同；
         *          参数占位符风格与方言一致，排查问题时可直接对照。
         *
         * @return std::string 生成的 SQL 文本
         */
        [[nodiscard]] std::string toSql() const
        {
            return buildSelectSql();
        }

    private:
        // ========================================================================
        // 在线执行支撑
        // ========================================================================

        /**
         * @brief 本次执行所用的连接租约
         *
         * @details 绑定事务时 pooled 为空、connection 指向事务持有的那条连接（所有权在事务手里，
         *          本对象析构不会归还它）；绑定连接池时 pooled 持有借出的 RAII 包装，
         *          租约析构即把连接还给池。两种情形对上层调用代码完全一致。
         */
        struct ConnectionLease
        {
            PooledConnection    pooled;               ///< 池借出的连接；绑定事务时为空
            DatabaseConnection *connection = nullptr; ///< 本次真正使用的连接，恒非空

            // 事务连接的使用权：租约活着的整段时间里独占那一条连接，离开作用域自动交还。
            // 池连接不必取锁——每个借用者各拿一条，本来就互不相干
            std::unique_lock<std::mutex> statementLock;
        };

        /**
         * @brief 校验当前处于在线模式（已绑定连接池或事务）
         * @param operationName 调用方方法名，用于拼出可定位的错误文本
         * @throws Base::LogicException 离线模式（默认构造）下调用执行器方法
         */
        void requireOnline(const std::string_view operationName) const
        {
            if (m_pool != nullptr || m_transaction != nullptr)
            {
                return;
            }
            throw Base::LogicException("Queryable: " + std::string(operationName) + " 需要连接池或事务，当前为离线模式");
        }

        /**
         * @brief 取得本查询要使用的方言，首次调用时解析并缓存（返回共享指针）
         * @details 方言类型优先用构造时显式指定的值；未指定时从实际要用的连接读取其真实 DatabaseType：
         *          绑定事务时直接问事务连接（零成本且必然准确），绑定池时才借一条连接探测，读完立刻归还。
         *          返回共享指针而不是引用，是为了让异步路径能把方言按值捕获进工作线程的任务：
         *          工作线程不得再触碰本对象，而引用无法脱离本对象的生命周期独立存在。
         * @return std::shared_ptr<SqlDialect> 方言实例，恒非空
         * @throws DatabaseException 无法从池中取得连接以推导类型
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（MySQL / Redis）
         * @warning 必须在**持有租约之前**调用：既未显式指定方言类型也未绑定事务时，它要自己再借一条
         *          连接去探测类型。调用方若已经占着池里唯一的连接（maximumPoolSize 为 1 是文件型
         *          SQLite 的常见配法），这次嵌套借用会白等满 acquireTimeoutMilliseconds，然后抛
         *          「无法获取连接以推导类型」——一条离真实原因很远的错误。各执行入口都按
         *          「先 requireDialect()、后 acquireConnection()」的顺序写，改动时请保持。
         */
        [[nodiscard]] std::shared_ptr<SqlDialect> resolveDialect()
        {
            if (m_dialect != nullptr)
            {
                return m_dialect;
            }

            DatabaseType resolvedType = DatabaseType::Sqlite;
            if (m_databaseType.has_value())
            {
                resolvedType = m_databaseType.value();
            } else if (m_transaction != nullptr)
            {
                // 事务已经握着一条确定的连接，直接问它即可，不需要再借出/归还一次
                resolvedType = m_transaction->connection().databaseType();
            } else
            {
                // 借出即读、读完归还：RAII 包装在作用域结束时会自动把连接还给池，
                // 因此这里不会额外占用池容量，也不会泄漏连接
                PooledConnection probeConnection = m_pool->acquire();
                if (!probeConnection)
                {
                    throw ConnectionUnavailableException("Queryable: 无法从连接池获取连接以推导数据库类型，请检查连接池配置");
                }
                resolvedType = probeConnection->databaseType();
            }

            // 未实现的类型由注册表抛出带中文提示的异常，这里不吞掉，让调用方明确知道缺什么
            m_dialect = DialectRegistry::dialectFor(resolvedType);
            return m_dialect;
        }

        /**
         * @brief 取得本查询要使用的方言引用
         * @details 同步路径的便捷入口：内部就是 resolveDialect() 的解引用，
         *          缓存的共享指针由本对象长期持有，引用在对象存活期间始终有效。
         * @return const SqlDialect& 方言实例引用
         * @throws DatabaseException 无法从池中取得连接以推导类型
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（MySQL / Redis）
         */
        [[nodiscard]] const SqlDialect &requireDialect()
        {
            return *resolveDialect();
        }

        /**
         * @brief 取得本查询异步执行时要用的执行器
         * @details 未注入时使用进程级共享实例：零配置即可用，且多个 Queryable 共享同一组
         *          工作线程，不会因为「每个查询各建一个执行器」而把线程数乘起来。
         * @return Core::AsyncExecutor& 执行器引用
         */
        [[nodiscard]] Core::AsyncExecutor &asyncExecutor() const
        {
            return m_asyncExecutor != nullptr ? *m_asyncExecutor : Core::AsyncExecutor::shared();
        }

        /**
         * @brief 取得本次执行要用的连接租约
         *
         * @details 绑定事务时直接引用事务持有的那一条连接——这是事务正确性的根本：
         *          若每个语句各自从池里取连接，BEGIN 落在 A 连接、写语句落在 B 连接，
         *          那些写语句实际运行在自动提交模式下，回滚只能回滚一个空事务，
         *          数据却已经落库，而且整个过程不会报任何错。
         * @param pool 连接池，与 transaction 必须有一个非空（由 requireOnline() 保证）
         * @param transaction 事务指针，非空时全部语句走事务连接
         * @return ConnectionLease 连接租约；池连接在租约析构时自动归还，事务连接不归还
         * @throws DatabaseException 池已达上限且等待超时，或连接工厂创建失败
         */
        [[nodiscard]] static ConnectionLease acquireConnection(ConnectionPool *pool, const Transaction *transaction)
        {
            ConnectionLease lease;
            if (transaction != nullptr)
            {
                // 先取连接的使用权再交出连接：异步路径会把语句投到工作线程上，两条并发语句
                // 不能同时踩同一条驱动连接（一个句柄一条协议流，交错发送即协议错乱）
                lease.statementLock = transaction->acquireStatementLock();
                lease.connection    = std::addressof(transaction->connection());
                return lease;
            }

            lease.pooled = pool->acquire();
            if (!lease.pooled)
            {
                throw ConnectionUnavailableException("Queryable: 从连接池获取连接失败，可能是池已达上限或连接创建失败");
            }
            lease.connection = lease.pooled.operator->();
            return lease;
        }

        /**
         * @brief 生成真正交给数据库执行的查询树副本
         * @details 与 m_queryNode 的唯一差别是：SELECT 列为空时按 TableSchema<T>::kColumns
         *          的声明顺序展开成显式列名。方言层不认识模板参数，只能把空列列表翻译成 '*'
         *          （顺序由数据库决定）；ORM 层知道结构体，因此在这里补上顺序。
         * @return QueryNode 展开列之后的查询树副本
         */
        [[nodiscard]] QueryNode resolvedQueryNode() const
        {
            QueryNode resolvedNode = m_queryNode;

            if (resolvedNode.selectColumns.empty())
            {
                resolvedNode.selectColumns = allColumnNames();
            }

            return resolvedNode;
        }

        /**
         * @brief 执行一次 SELECT 并把结果映射成结构体列表（同步路径）
         * @param queryNode 已展开列的查询树
         * @return std::vector<T> 映射后的行列表
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         */
        [[nodiscard]] std::vector<T> fetchRows(const QueryNode &queryNode)
        {
            const SqlDialect &dialect = requireDialect();
            ConnectionLease   lease   = acquireConnection(m_pool, m_transaction);
            // 连接在 result 之前声明、之后析构，二者按声明逆序销毁，因此连接必定比结果集活得久
            return fetchRowsOn(*lease.connection, dialect, queryNode);
        }

        /**
         * @brief 执行一次 SELECT 并只映射出第一行（同步路径）
         * @param queryNode 已展开列的查询树
         * @return std::optional<T> 第一行；没有匹配行时为空值
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         */
        [[nodiscard]] std::optional<T> fetchFirst(const QueryNode &queryNode)
        {
            const SqlDialect &dialect = requireDialect();
            ConnectionLease   lease   = acquireConnection(m_pool, m_transaction);
            // 连接在 result 之前声明、之后析构，因此结果集必定比连接短命
            return fetchFirstOn(*lease.connection, dialect, queryNode);
        }

        /**
         * @brief 在指定连接上执行 SELECT 并映射结果（同步与异步路径共用）
         * @details 与「用哪条连接、在哪个线程执行」无关：调用方负责提供一条可用的连接
         *          （同步路径在调用线程上取，异步路径在工作线程上取），本函数只做翻译、执行与映射。
         * @param connection 目标连接，必须在结果集存活期间保持有效
         * @param dialect 方言，提供 translate()
         * @param queryNode 已展开列的查询树
         * @return std::vector<T> 映射后的行列表
         * @throws DatabaseException SQL 执行失败或行映射失败
         * @note SQLite 的结果集持有连接句柄的非拥有指针，因此 connection 必须比结果集活得久：
         *       这一点由调用方持有连接租约、且租约比本函数返回值活得更久来保证
         */
        [[nodiscard]] static std::vector<T> fetchRowsOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode)
        {
            const auto [sql, parameters] = dialect.translate(queryNode);

            const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
            if (result == nullptr)
            {
                throw QueryExecutionException("Queryable: 查询执行失败：" + connection.lastError());
            }

            return mapResultRows<T>(*result);
        }

        /**
         * @brief 在指定连接上执行 SELECT 并只映射第一行（同步与异步路径共用）
         *
         * @details 与 fetchRowsOn() 的差别只在「要不要为剩余行准备向量」：取一行时先分配一个
         *          std::vector 再交出首元素，等于为这次调用白付一次堆分配与一次搬移。
         *          列下标解析与逐列赋值仍走 Detail 里的那一份实现，两条路径的映射规则不会分岔。
         *
         * @param connection 目标连接，必须在结果集存活期间保持有效
         * @param dialect 方言，提供 translate()
         * @param queryNode 已展开列的查询树
         * @return std::optional<T> 第一行；结果集为空时为空值
         * @throws QueryExecutionException SQL 执行失败
         * @throws RowMappingException 首行的列缺失、类型不符或收窄会改变数值
         * @note 空结果集不去解析列下标，因此「列缺失」只在真有一行时才报错——与列表路径同一条语义
         */
        [[nodiscard]] static std::optional<T> fetchFirstOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode)
        {
            const auto [sql, parameters] = dialect.translate(queryNode);

            const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
            if (result == nullptr)
            {
                throw QueryExecutionException("Queryable: 查询执行失败：" + connection.lastError());
            }

            // mapResultRow 的契约要求游标已停在有效行上；0 行是「没有匹配」而不是映射失败
            if (!result->next())
            {
                return std::nullopt;
            }
            return mapResultRow<T>(*result);
        }

        /**
         * @brief 在指定连接上执行 COUNT(*) 查询并解读结果（同步与异步路径共用）
         * @param connection 目标连接
         * @param dialect 方言，提供 translate()
         * @param countingNode 已把 SELECT 改成 COUNT(*)、并清掉排序与分页的查询树
         * @return std::int64_t 匹配行数；结果集没有计数行时为 0
         * @throws QueryExecutionException SQL 执行失败
         * @throws RowMappingException 计数列不是整数（语句被改坏或列位置漂移）
         */
        [[nodiscard]] static std::int64_t countOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &countingNode)
        {
            const auto [sql, parameters] = dialect.translate(countingNode);

            const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
            if (result == nullptr)
            {
                throw QueryExecutionException("Queryable: 统计行数失败：" + connection.lastError());
            }

            // COUNT(*) 恒返回一行一列；游标推进失败说明语句没有产出任何行，按 0 计
            if (!result->next())
            {
                return 0;
            }

            const DatabaseValue countValue = result->getValue(0);
            if (const auto *countedRows = std::get_if<std::int64_t>(&countValue))
            {
                return *countedRows;
            }

            // COUNT 恒返回整数：NULL 或其它类型说明结果集与预期不符（语句被改坏或列错位），
            // 静默返回 0 会让调用方以为「一行都没有」
            throw RowMappingException("Queryable: 计数列不是整数（实际类型 " + std::string(databaseValueTypeName(countValue)) +
                                      "，第 0 列列名 " + std::string(result->columnName(0).value_or("?")) +
                                      "），请检查计数语句是否仍是 COUNT(*)");
        }

        /**
         * @brief 执行一条写语句并返回受影响行数（连接由内部按当前模式决定）
         * @param statement 待执行的参数化语句
         * @return std::int64_t 受影响行数；驱动不提供该信息时为 0
         * @throws DatabaseException 取连接失败或语句执行失败
         */
        [[nodiscard]] std::int64_t executeStatement(const SqlStatement &statement)
        {
            ConnectionLease lease = acquireConnection(m_pool, m_transaction);
            return executeOn(*lease.connection, statement);
        }

        /**
         * @brief 在指定连接上执行一条写语句并返回受影响行数
         * @details 批量插入分块时必须让每一块都落在同一条连接上（否则事务覆盖不到全部块），
         *          因此这里把「用哪条连接」显式参数化，供 executeStatement() 与本类内部
         *          的批量执行共用同一份执行与取数逻辑。
         * @param connection 目标连接，生命周期由调用方保证
         * @param statement 待执行的参数化语句
         * @return std::int64_t 受影响行数；驱动不提供该信息时为 0
         * @throws DatabaseException 语句执行失败，原因见连接的错误文本
         */
        [[nodiscard]] static std::int64_t executeOn(DatabaseConnection &connection, const SqlStatement &statement)
        {
            const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                throw QueryExecutionException("Queryable: 语句执行失败：" + connection.lastError());
            }

            // 影响行数由结果集自己回答：DatabaseResult::affectedRowCount() 带默认实现
            // （不提供该信息的驱动返回 0），SQLite 覆盖它返回真实的 sqlite3_changes 快照。
            // ORM 侧因此不需要按 DatabaseType 向下转型，也不依赖任何具体驱动
            return result->affectedRowCount();
        }

        /**
         * @brief 生成 INSERT 语句
         * @details ORM 只负责把结构体整理成「列名 + 取值」两个等长的序列：列名放进查询树的
         *          selectColumns，取值转成 DatabaseValue 后按同序排列，SQL 文本、标识符引用、
         *          占位符写法全部由方言的 translateInsert() 决定，本类不拼任何 SQL 片段。
         * @param row 待插入的结构体
         * @return SqlStatement "INSERT INTO 表 (列…) VALUES (?, …)"
         */
        [[nodiscard]] SqlStatement buildInsertStatement(const T &row)
        {
            QueryNode insertNode     = makeWriteQueryNode();
            insertNode.selectColumns = allColumnNames();

            // 取值向量是临时对象，但它活到整条表达式结束，方言在本次调用内完成读取，不存在悬垂
            return requireDialect().translateInsert(insertNode, rowValuesOf(row));
        }

        /**
         * @brief 生成按主键更新一行的 UPDATE 语句
         * @details SET 子句覆盖除主键外的全部列，WHERE 子句用主键定位，主键值同样以参数绑定送出。
         *          条件渲染交回方言：ORM 只提供「SET 列 + 取值」与一条主键等值条件，
         *          条件树的递归、IN 展开、参数顺序因此与 SELECT / DELETE 完全一致。
         * @param row 待更新的结构体
         * @return SqlStatement "UPDATE 表 SET 列 = ?, … WHERE 主键 = ?"
         * @throws Base::LogicException 主键未在 kColumns 中声明，或表中只有主键列
         */
        [[nodiscard]] SqlStatement buildUpdateStatement(const T &row)
        {
            const std::string_view primaryKeyName = TableSchema<T>::kPrimaryKey;

            std::vector<std::string>      assignmentColumns;
            std::vector<DatabaseValue>    assignmentValues;
            std::optional<WhereCondition> primaryKeyCondition;

            std::apply(
                    [&](const auto &... columnDescriptors)
                    {
                        (appendUpdateColumn(columnDescriptors, row, primaryKeyName, assignmentColumns,
                                            assignmentValues, primaryKeyCondition), ...);
                    },
                    TableSchema<T>::kColumns);

            if (!primaryKeyCondition.has_value())
            {
                throw Base::LogicException("Queryable: 无法生成 UPDATE，TableSchema<" +
                                           std::string(TableSchema<T>::kTableName) + ">::kPrimaryKey（" +
                                           std::string(primaryKeyName) + "）未在 kColumns 中声明");
            }

            if (assignmentColumns.empty())
            {
                throw Base::LogicException("Queryable: 无法生成 UPDATE，表 " +
                                           std::string(TableSchema<T>::kTableName) + " 只有主键列，没有可更新的列");
            }

            QueryNode updateNode     = makeWriteQueryNode();
            updateNode.selectColumns = std::move(assignmentColumns);
            updateNode.whereConditions.push_back(std::move(primaryKeyCondition.value()));

            // SET 参数在前、主键条件参数在后，与方言输出文本中占位符的先后顺序一致
            return requireDialect().translateUpdate(updateNode, assignmentValues);
        }

        /**
         * @brief 构建一个只在写方向使用的查询树
         * @details 只填表名：写语句不涉及 JOIN / 排序 / 分页，也不应继承 m_queryNode 上
         *          调用方为查询设置的条件（例如 query.where(...).insert(row) 里的条件
         *          对 INSERT 毫无意义，带进写语句只会误导）。
         * @return QueryNode 空的写查询树
         */
        [[nodiscard]] static QueryNode makeWriteQueryNode()
        {
            QueryNode writeNode;
            writeNode.tableName = std::string(TableSchema<T>::kTableName);
            return writeNode;
        }

        /**
         * @brief 取 TableSchema<T> 声明的全部列名，顺序与 kColumns 一致
         * @return std::vector<std::string> 列名列表
         */
        [[nodiscard]] static std::vector<std::string> allColumnNames()
        {
            std::vector<std::string> columnNames;

            std::apply(
                    [&columnNames](const auto &... columnDescriptors)
                    {
                        // 折叠表达式从左到右执行（逗号运算符），列序与 kColumns 声明顺序严格一致
                        (columnNames.emplace_back(columnDescriptors.columnName), ...);
                    },
                    TableSchema<T>::kColumns);

            return columnNames;
        }

        /**
         * @brief 把一行的全部字段转成绑定参数，顺序与 kColumns 一致
         * @param row 待转换的结构体
         * @return std::vector<DatabaseValue> 与列名一一对应的取值列表
         */
        [[nodiscard]] static std::vector<DatabaseValue> rowValuesOf(const T &row)
        {
            std::vector<DatabaseValue> rowValues;

            std::apply(
                    [&rowValues, &row](const auto &... columnDescriptors)
                    {
                        // 成员值 → 绑定参数：optional 空值绑定为 SQL NULL，无符号超范围降级为十进制文本
                        (rowValues.push_back(Detail::toDatabaseValue(row.*(columnDescriptors.memberPointer))), ...);
                    },
                    TableSchema<T>::kColumns);

            return rowValues;
        }

        /**
         * @brief 按方言的参数上限分块执行批量插入，必要时用本地事务覆盖全部块
         *
         * @details 同步与异步两条路径的唯一实现：每行的参数个数就是列数，上限由方言的
         *          maximumStatementParameters() 回答；不分块时单条多行 INSERT 自身就是原子的，不额外开事务；
         *          需要分块时全部块必须落在同一条连接的同一个事务里，否则中途失败会让已提交的批次无法回滚。
         *
         * @param pool 连接池，transaction 为空时由它取连接（并可能起一个本地事务）
         * @param transaction 已绑定的事务，非空时全部块共用它的连接且不自行提交或回滚
         * @param dialect 目标方言，提供 maximumStatementParameters() 与 translateInsertBatch()
         * @param rows 待插入的全部行（非空，空集合由调用方提前返回）
         * @return std::int64_t 累计受影响行数
         * @throws DatabaseException 取连接失败、任意一块执行失败，或本地事务提交失败
         */
        [[nodiscard]] static std::int64_t insertBatchOn(ConnectionPool *pool, const Transaction *transaction, const SqlDialect &dialect, const std::span<const T> rows)
        {
            QueryNode batchNode     = makeWriteQueryNode();
            batchNode.selectColumns = allColumnNames();

            // 列数不可能为 0（TableSchema 的列已在编译期校验过），因此除法不会除零
            const std::size_t columnCount      = batchNode.selectColumns.size();
            const std::size_t parameterLimit   = dialect.maximumStatementParameters();
            const std::size_t rowsPerStatement = std::max<std::size_t>(1, parameterLimit / columnCount);

            if (rows.size() <= rowsPerStatement)
            {
                // 单条多行 INSERT 自身就是原子的，不需要额外开事务
                std::vector<std::vector<DatabaseValue> > batchRows;
                batchRows.reserve(rows.size());
                for (const T &row: rows)
                {
                    batchRows.push_back(rowValuesOf(row));
                }

                ConnectionLease lease = acquireConnection(pool, transaction);
                return executeOn(*lease.connection, dialect.translateInsertBatch(batchNode, batchRows));
            }

            if (transaction != nullptr)
            {
                // 已绑定事务：块之间共用事务连接，提交/回滚的决定权仍在调用方手里
                return executeBatchOn(transaction->connection(), dialect, batchNode, rows, rowsPerStatement);
            }

            Transaction        localTransaction(*pool);
            const std::int64_t affectedRows =
                    executeBatchOn(localTransaction.connection(), dialect, batchNode, rows, rowsPerStatement);
            // 全部批次写成功才提交；中途抛出异常时事务析构会回滚，已写入的批次一并撤销。
            // 提交本身也可能失败（磁盘写满、锁冲突），如实抛错而不是吞掉返回值：
            // 此时事务仍未结束，析构阶段还会再补一次 ROLLBACK
            if (!localTransaction.commit())
            {
                throw QueryExecutionException("Queryable: 批量插入提交失败：" + localTransaction.lastError());
            }
            return affectedRows;
        }

        /**
         * @brief 在指定连接上按行数上限分块执行批量插入
         * @details 每块生成一条多行 VALUES 语句，全部块都落在同一个 connection 上：
         *          只有共用一条连接，调用方（或本类内部的本地事务）才能用一个事务覆盖全部块。
         * @param connection 目标连接
         * @param dialect 方言语义，提供 translateInsertBatch()
         * @param batchNode 提供表名与待写列的查询树
         * @param rows 待插入的全部行
         * @param rowsPerStatement 每块最多容纳的行数（由方言的参数上限换算而来）
         * @return std::int64_t 累计受影响行数
         * @throws DatabaseException 任意一块执行失败（此时整个事务由调用方回滚）
         */
        [[nodiscard]] static std::int64_t executeBatchOn(DatabaseConnection &     connection,
                                                         const SqlDialect &       dialect,
                                                         const QueryNode &        batchNode,
                                                         const std::span<const T> rows,
                                                         const std::size_t        rowsPerStatement)
        {
            std::int64_t totalAffectedRows = 0;

            for (std::size_t firstRow = 0; firstRow < rows.size(); firstRow += rowsPerStatement)
            {
                // 最后一块可能不足一整批，因此每一块都要重新算上界，不能按固定步长假定满行
                const std::size_t lastRow = std::min(firstRow + rowsPerStatement, rows.size());

                std::vector<std::vector<DatabaseValue> > chunkRows;
                chunkRows.reserve(lastRow - firstRow);
                for (std::size_t rowIndex = firstRow; rowIndex < lastRow; ++rowIndex)
                {
                    chunkRows.push_back(rowValuesOf(rows[rowIndex]));
                }

                // 参数顺序为「行优先、行内按列序」，与方言生成的占位符顺序一一对应
                totalAffectedRows += executeOn(connection, dialect.translateInsertBatch(batchNode, chunkRows));
            }

            return totalAffectedRows;
        }

        /**
         * @brief 把 UPDATE 的一列拆进 SET 或 WHERE
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param row 提供字段值的结构体
         * @param primaryKeyName 主键列名，命中该列的成员不进 SET
         * @param assignmentColumns 出参：进入 SET 的列名列表，按 kColumns 顺序
         * @param assignmentValues 出参：与 assignmentColumns 同序的取值列表
         * @param primaryKeyCondition 出参：主键等值条件；未找到主键列时保持空
         */
        template<typename ColumnDescriptorType>
        static void appendUpdateColumn(const ColumnDescriptorType &   columnDescriptor,
                                       const T &                      row,
                                       const std::string_view         primaryKeyName,
                                       std::vector<std::string> &     assignmentColumns,
                                       std::vector<DatabaseValue> &   assignmentValues,
                                       std::optional<WhereCondition> &primaryKeyCondition)
        {
            // 主键列不进 SET：更新主键会破坏行标识（其它表的外键、上层缓存都指向旧值）。
            // 它的值改作 WHERE 条件，仍然是参数绑定而不是拼进 SQL 文本
            if (columnDescriptor.columnName == primaryKeyName)
            {
                primaryKeyCondition = WhereCondition{
                        .left = FieldReference{.name = std::string(columnDescriptor.columnName)},
                        .op = SqlOperator::Eq,
                        .right = makeParameterValue(row.*(columnDescriptor.memberPointer))
                };
                return;
            }

            assignmentColumns.emplace_back(columnDescriptor.columnName);
            assignmentValues.push_back(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer)));
        }

        /**
         * @brief 把结构体成员值转成查询树使用的参数值
         * @details 写语句的条件（例如 UPDATE 的主键等值条件）要放进 QueryNode，而 QueryNode
         *          的参数类型是 ParameterValue；本函数负责这最后一步转换，
         *          标量部分直接复用 Expression.h 里既有的重载，保证 ORM 里「值 → ParameterValue」
         *          只有一套规则。
         * @tparam MemberType 成员类型（可为 std::optional 包装）
         * @param value 成员值
         * @return ParameterValue 条件可直接使用的参数值
         */
        template<typename MemberType>
        [[nodiscard]] static ParameterValue makeParameterValue(const MemberType &value)
        {
            using BareType = std::remove_cvref_t<MemberType>;

            if constexpr (Detail::IsOptional<BareType>::value)
            {
                // 空 optional 表达 SQL NULL：主键为 NULL 时条件退化为 "主键 = NULL"（恒不成立），
                // 与 SQL 语义一致，不会误伤任何行，也不会静默匹配到别的行
                if (!value.has_value())
                {
                    return nullptr;
                }
                return makeParameterValue(value.value());
            } else
            {
                return Detail::toParameterValue(value);
            }
        }

        // ========================================================================
        // SQL 生成实现
        // ========================================================================

        /**
         * @brief 构建 SELECT SQL 语句
         * @return std::string 完整的 SELECT 语句
         */
        [[nodiscard]] std::string buildSelectSql() const
        {
            std::string sql;

            // SELECT 子句
            sql += "SELECT ";
            if (m_queryNode.selectColumns.empty())
            {
                sql += '*';
            } else
            {
                for (std::size_t i = 0; i < m_queryNode.selectColumns.size(); ++i)
                {
                    if (i > 0)
                        sql += ", ";
                    sql += m_queryNode.selectColumns[i];
                }
            }

            // FROM 子句
            sql += " FROM ";
            sql += m_queryNode.tableName;
            if (!m_queryNode.tableAlias.empty())
            {
                sql += " AS ";
                sql += m_queryNode.tableAlias;
            }

            // JOIN 子句
            for (const auto &[type, tableName, tableAlias, conditions]: m_queryNode.joins)
            {
                sql += ' ';
                sql += joinTypeToString(type);
                sql += " JOIN ";
                sql += tableName;
                if (!tableAlias.empty())
                {
                    sql += " AS ";
                    sql += tableAlias;
                }
                if (!conditions.empty())
                {
                    sql += " ON ";
                    for (std::size_t i = 0; i < conditions.size(); ++i)
                    {
                        if (i > 0)
                            sql += " AND ";
                        sql += buildConditionString(conditions[i]);
                    }
                }
            }

            // WHERE 子句
            if (!m_queryNode.whereConditions.empty())
            {
                sql += " WHERE ";
                for (std::size_t i = 0; i < m_queryNode.whereConditions.size(); ++i)
                {
                    if (i > 0)
                        sql += " AND ";
                    sql += buildConditionString(m_queryNode.whereConditions[i]);
                }
            }

            // GROUP BY 子句
            if (!m_queryNode.groupBy.empty())
            {
                sql += " GROUP BY ";
                for (std::size_t i = 0; i < m_queryNode.groupBy.size(); ++i)
                {
                    if (i > 0)
                        sql += ", ";
                    sql += fieldReferenceToString(m_queryNode.groupBy[i]);
                }
            }

            // HAVING 子句
            if (m_queryNode.having.has_value())
            {
                sql += " HAVING ";
                sql += buildConditionString(m_queryNode.having.value());
            }

            // ORDER BY 子句
            if (!m_queryNode.orderBy.empty())
            {
                sql += " ORDER BY ";
                for (std::size_t i = 0; i < m_queryNode.orderBy.size(); ++i)
                {
                    if (i > 0)
                        sql += ", ";
                    sql += orderByToString(m_queryNode.orderBy[i]);
                }
            }

            // LIMIT 子句
            if (m_queryNode.limit.has_value())
            {
                sql += " LIMIT ";
                sql += std::to_string(m_queryNode.limit.value());
            }

            // OFFSET 子句
            if (m_queryNode.offset.has_value())
            {
                sql += " OFFSET ";
                sql += std::to_string(m_queryNode.offset.value());
            }

            return sql;
        }

        /**
         * @brief 构建单个条件的 SQL 字符串
         * @param condition 待转换的条件
         * @return std::string 条件对应的 SQL 片段
         */
        [[nodiscard]] static std::string buildConditionString(const WhereCondition &condition)
        {
            // 复合节点（AND/OR/NOT）：递归展开 children
            if (condition.op == SqlOperator::And)
            {
                if (condition.children.empty())
                {
                    return "(1=1)";
                }
                std::string result = "(";
                for (std::size_t i = 0; i < condition.children.size(); ++i)
                {
                    if (i > 0)
                        result += " AND ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ')';
                return result;
            }

            if (condition.op == SqlOperator::Or)
            {
                if (condition.children.empty())
                {
                    return "(1=0)";
                }
                std::string result = "(";
                for (std::size_t i = 0; i < condition.children.size(); ++i)
                {
                    if (i > 0)
                        result += " OR ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ')';
                return result;
            }

            if (condition.op == SqlOperator::Not)
            {
                if (condition.children.empty())
                {
                    return "NOT (1=1)";
                }
                if (condition.children.size() > 1U)
                {
                    // 与方言同一判据：多个子条件取非的语义不确定，静默只渲染第一个会给出
                    // 一条「看起来对」的语句，离线文本也不能替调用方猜
                    throw Base::LogicException("Queryable: toSql 遇到带 " + std::to_string(condition.children.size()) +
                                               " 个子条件的 NOT，取非含义不确定，请先用 && 或 || 合成一个节点再取非");
                }
                return "NOT " + buildConditionString(condition.children[0]);
            }

            // 叶子节点：left op right
            std::string result;
            result += fieldReferenceToString(condition.left);
            result += ' ';
            result += operatorToString(condition.op);
            result += ' ';

            if (condition.op == SqlOperator::IsNull || condition.op == SqlOperator::IsNotNull)
            {
                // IS NULL / IS NOT NULL 不需要右操作数
                return result;
            }

            if (condition.op == SqlOperator::In || condition.op == SqlOperator::NotIn)
            {
                if (condition.inValues.empty())
                {
                    // 与方言同一写法：空集合在集合语义下「一个都不匹配」，渲染成恒假/恒真而不是
                    // 给一个不存在的元素挂上占位符——后者会让人对着 "IN (?)" 去数参数而对不上
                    result += (condition.op == SqlOperator::In) ? "(1 = 0)" : "(1 = 1)";
                    return result;
                }

                result += '(';
                for (std::size_t i = 0; i < condition.inValues.size(); ++i)
                {
                    if (i > 0)
                        result += ", ";
                    result += '?';
                }
                result += ')';
                return result;
            }

            result += '?';
            // 字面量匹配要连 ESCAPE 一起给出，否则对着这段文本改一改就会把转义过的 % 当成通配符。
            // 子句文本与方言渲染共用同一个常量，两处的「近似 SQL」才有对照价值
            if (condition.op == SqlOperator::LikeLiteral)
            {
                result += kLikeEscapeClauseText;
            }
            return result;
        }

        /**
         * @brief 将 SqlOperator 转为 SQL 操作符字符串
         */
        [[nodiscard]] static std::string_view operatorToString(const SqlOperator op)
        {
            using namespace std::string_view_literals;
            switch (op)
            {
                case SqlOperator::Eq:
                    return "="sv;
                case SqlOperator::Neq:
                    return "!="sv;
                case SqlOperator::Gt:
                    return ">"sv;
                case SqlOperator::Ge:
                    return ">="sv;
                case SqlOperator::Lt:
                    return "<"sv;
                case SqlOperator::Le:
                    return "<="sv;
                case SqlOperator::Like:
                    return "LIKE"sv;
                case SqlOperator::LikeLiteral:
                    // 与方言同形：操作符文本仍是 LIKE，转义语义由渲染分支补出的 ESCAPE 子句表达
                    return "LIKE"sv;
                case SqlOperator::In:
                    return "IN"sv;
                case SqlOperator::NotIn:
                    return "NOT IN"sv;
                case SqlOperator::IsNull:
                    return "IS NULL"sv;
                case SqlOperator::IsNotNull:
                    return "IS NOT NULL"sv;
                default:
                    // 与执行路径同一判据：复合节点由渲染分支提前分流，漏分支时不能静默给出 "="
                    throw Base::LogicException("Queryable: toSql 遇到没有比较文本的操作符（枚举值 " +
                                               std::to_string(std::to_underlying(op)) + "），请检查条件渲染分支");
            }
        }

        /**
         * @brief 将 JoinType 转为 SQL 连接类型字符串
         */
        [[nodiscard]] static std::string_view joinTypeToString(const JoinType type)
        {
            using namespace std::string_view_literals;
            switch (type)
            {
                case JoinType::Inner:
                    return "INNER"sv;
                case JoinType::Left:
                    return "LEFT"sv;
                case JoinType::Right:
                    return "RIGHT"sv;
                case JoinType::Cross:
                    return "CROSS"sv;
                default:
                    // 未知取值静默当成 INNER 会让调试看到的 SQL 比真实执行的少一张表
                    throw Base::LogicException("Queryable: toSql 遇到未知的连接类型（枚举值 " +
                                               std::to_string(std::to_underlying(type)) + "），请检查连接渲染分支");
            }
        }

        /**
         * @brief 将 FieldReference 转为字符串
         */
        [[nodiscard]] static const std::string &fieldReferenceToString(const FieldReference &field)
        {
            return field.name;
        }

        /**
         * @brief 将 OrderByClause 转为 SQL 排序字符串
         */
        [[nodiscard]] static std::string orderByToString(const OrderByClause &order)
        {
            std::string result = fieldReferenceToString(order.field);
            if (order.descending)
            {
                result += " DESC";
            } else
            {
                result += " ASC";
            }
            return result;
        }

        // ========================================================================
        // 数据成员
        // ========================================================================

        QueryNode                   m_queryNode;               ///< 查询树节点，存储所有查询构建信息
        ConnectionPool *            m_pool        = nullptr;   ///< 数据库连接池指针，离线模式或绑定事务时为 nullptr
        Transaction *               m_transaction = nullptr;   ///< 事务指针；非空时全部语句走事务持有的连接
        std::optional<DatabaseType> m_databaseType;            ///< 构造时显式指定的数据库类型；未指定时从连接推导
        std::shared_ptr<SqlDialect> m_dialect;                 ///< 缓存的方言实例，首次执行时解析并长期持有
        Core::AsyncExecutor *       m_asyncExecutor = nullptr; ///< 注入的异步执行器；为空时用进程级共享实例
    };

} // namespace AsynGyanis::Database::Queryable
