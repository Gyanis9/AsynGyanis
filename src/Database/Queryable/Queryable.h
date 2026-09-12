/**
 * @file Queryable.h
 * @brief 查询构建器 —— 类型安全的 ORM 查询入口，构建 QueryNode 查询树
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Queryable<T> 是 ORM 查询的门面类，提供流式接口构建查询树，
 *          并负责把查询树送到数据库执行。两条使用路径：
 *          - 离线：默认构造，只用 toSql() 生成近似 SQL 文本，不接触连接池；
 *          - 在线：构造时绑定 ConnectionPool，执行器方法走
 *            「连接池 acquire() → 方言 translate() → execute(sql, 参数) → 行映射」这条链路。
 *          方言由池中连接的真实 DatabaseType 推导（也可在构造时显式指定）。
 *
 * ## 使用范例
 * @code
 *   // 离线 SQL 生成（测试用，无需连接池）
 *   Queryable<User> query;
 *   query.where(Column(&User::age, "age") >= 18)
 *        .orderBy(asc("name"))
 *        .limit(10);
 *   std::string sql = query.toSql();
 *   // => "SELECT * FROM users WHERE age >= ? ORDER BY name ASC LIMIT 10"
 *
 *   // 在线查询（需要连接池）
 *   ConnectionPool pool(...);
 *   Queryable<User> query(pool);
 *   auto users = query.where(Column(&User::age, "age") >= 18).toList();
 *
 *   // 写入
 *   query.insert(user);
 *   query.where(...).executeNonQuery();   // 按条件删除
 * @endcode
 */
#pragma once

#include "Database/Common/DatabaseType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/QueryNode.h"
#include "Database/Queryable/RowMapper.h"
#include "Database/Queryable/TableSchema.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    namespace Queryable::Detail
    {
        /**
         * @brief 取执行结果的影响行数
         *
         * @details DatabaseResult 基类不提供影响行数（该接口已冻结），只有具体驱动的结果集知道
         *          自己改了多少行，因此这里按数据库类型做一次受控的向下转型。
         *          定义在 Queryable.cpp 中，避免 ORM 的公开头文件依赖某个具体驱动。
         *
         * @param result 语句执行结果
         * @param databaseType 执行该语句的连接所属的数据库类型
         * @return int 影响行数；该驱动无法提供时返回 0（宁可不报也不误报）
         */
        [[nodiscard]] int affectedRowCountOf(const DatabaseResult &result, DatabaseType databaseType);
    }
}

namespace AsynGyanis::Database::Queryable
{

    /**
     * @brief ORM 查询构建器模板
     *
     * @tparam T 表数据结构类型，需有对应的 TableSchema<T> 特化
     *
     * @details 提供流式接口构建类型安全的数据库查询。
     *          三种构造方式：
     *          - 默认构造：离线模式，仅用于 SQL 生成和测试
     *          - 带连接池构造：在线模式，方言由池中连接的真实类型推导
     *          - 带连接池与数据库类型构造：在线模式，方言类型由调用方直接指定
     *
     * @note 执行器方法（toList/first/count/insert/update/executeNonQuery）必须在绑定连接池
     *       的在线模式下调用，默认构造的离线模式调用它们会抛 std::logic_error。
     */
    template<typename T>
    class Queryable
    {
    public:
        /**
         * @brief 默认构造（离线模式）
         *
         * @details 用于仅生成 SQL 或测试场景，不需要连接池。
         *          表名从 TableSchema<T>::kTableName 自动获取。
         *          若 TableSchema 未特化或 kTableName 为空，则表名需要在首次构建前通过 table() 设置。
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
        explicit Queryable(ConnectionPool &pool)
            : m_pool(&pool)
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        /**
         * @brief 构造并绑定连接池与数据库类型（在线模式，显式指定方言）
         *
         * @details 与上一个构造函数的区别：方言类型由调用方直接给出，不再从池中连接推导。
         *          适用于希望避免「推导时借出连接」这一副作用，或连接工厂封装较复杂、
         *          类型已知的场景。
         *
         * @param pool 数据库连接池
         * @param databaseType 池中连接的数据库类型，决定使用哪个 SqlDialect
         */
        Queryable(ConnectionPool &pool, DatabaseType databaseType)
            : m_pool(&pool), m_databaseType(databaseType)
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
         *
         * @code
         *   query.where(Column(&User::age, "age") >= 18)
         *        .where(Column(&User::name, "name") == "Alice");
         * @endcode
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
         *
         * @param columns 列名列表
         * @return Queryable& 自身引用，支持链式调用
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
         * @param fields 分组列名列表
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &groupBy(std::vector<std::string> fields)
        {
            std::vector<FieldReference> fieldRefs;
            fieldRefs.reserve(fields.size());
            for (auto &field : fields)
            {
                fieldRefs.push_back(FieldReference{.name = std::move(field)});
            }
            m_queryNode.groupBy = std::move(fieldRefs);
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
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::runtime_error 取连接失败、SQL 执行失败或行映射失败，原因见异常文本
         * @throws std::invalid_argument 该数据库类型尚无方言实现（如 MySQL / Redis）
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
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::runtime_error 取连接失败、SQL 执行失败或行映射失败
         * @throws std::invalid_argument 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::optional<T> first()
        {
            requireOnline("first()");

            QueryNode limitedNode = resolvedQueryNode();
            // 只取一行：LIMIT 1 让数据库侧提前停止扫描，比取回全部再取首元素高效得多
            limitedNode.limit = 1U;

            std::vector<T> rows = fetchRows(limitedNode);
            if (rows.empty())
            {
                return std::nullopt;
            }
            return std::move(rows.front());
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
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::runtime_error 取连接失败或 SQL 执行失败
         * @throws std::invalid_argument 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t count()
        {
            requireOnline("count()");

            QueryNode countingNode   = resolvedQueryNode();
            countingNode.selectColumns = {"COUNT(*)"};
            countingNode.orderBy.clear();
            countingNode.limit.reset();
            countingNode.offset.reset();

            const SqlStatement   statement  = requireDialect().translate(countingNode);
            PooledConnection     connection = acquireConnection();
            std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                throw std::runtime_error("Queryable: 统计行数失败：" + connection->lastError());
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

            // NULL（例如 GROUP BY 后没有任何分组）按 0 处理，语义上「没有行」与 0 行等价
            return 0;
        }

        /**
         * @brief 执行非查询操作：按当前查询树的 WHERE 条件删除数据
         *
         * @details 生成并执行 "DELETE FROM 表 [WHERE 条件]"。条件部分复用方言的 WHERE 渲染
         *          （递归展开逻辑条件、IN 集合与参数顺序的规则只在方言层实现一次），
         *          因此参数同样以绑定方式送入，不会拼接进 SQL 文本。
         *          INSERT / UPDATE 带赋值语义，无法由查询树表达，请改用 insert() / update()。
         *
         * @return int 受影响的行数
         *
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::runtime_error 取连接失败或语句执行失败
         * @throws std::invalid_argument 该数据库类型尚无方言实现
         * @warning 查询树不含任何条件时生成的语句是 "DELETE FROM 表"，会清空全表；
         *          需要限定范围请先调用 where()
         */
        int executeNonQuery()
        {
            requireOnline("executeNonQuery()");
            return executeStatement(buildDeleteStatement(resolvedQueryNode()));
        }

        /**
         * @brief 按结构体字段插入一行
         *
         * @details 按 TableSchema<T>::kColumns 的顺序生成
         *          "INSERT INTO 表 (列…) VALUES (?, …)"，字段值全部以绑定参数传入。
         *          std::optional 成员为空时绑定为 SQL NULL。
         *
         * @param row 待插入的结构体（主键等字段由调用方填好，本方法不做自增处理）
         * @return int 受影响的行数（成功插入一行时为 1）
         *
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::runtime_error 取连接失败或语句执行失败（如唯一约束冲突）
         * @throws std::invalid_argument 该数据库类型尚无方言实现
         */
        int insert(const T &row)
        {
            requireOnline("insert()");
            return executeStatement(buildInsertStatement(row));
        }

        /**
         * @brief 按结构体主键更新一行
         *
         * @details 生成 "UPDATE 表 SET 非主键列 = ? WHERE 主键 = ?"：主键列不进 SET
         *          （更新主键会破坏行标识），它的值改作 WHERE 条件；所有取值仍然走参数绑定。
         *
         * @param row 待更新的结构体，主键字段用于定位目标行
         * @return int 受影响的行数；0 表示没有匹配的行（无此主键）
         *
         * @throws std::logic_error 当前为离线模式（无连接池）
         * @throws std::logic_error TableSchema<T>::kPrimaryKey 未在 kColumns 中声明，
         *         或表中只有主键列（没有可更新的列），无法生成 UPDATE
         * @throws std::runtime_error 取连接失败或语句执行失败
         * @throws std::invalid_argument 该数据库类型尚无方言实现
         */
        int update(const T &row)
        {
            requireOnline("update()");
            return executeStatement(buildUpdateStatement(row));
        }

        // ========================================================================
        // SQL 生成
        // ========================================================================

        /**
         * @brief 生成 SQL 文本
         *
         * @details 将当前 QueryNode 查询树转换为近似 SQL 字符串，用于调试与离线测试。
         *          这条路径不参与真实执行：它不给标识符加引号，SELECT 列为空时固定输出 "*"，
         *          分页也不做方言补全。真正执行的 SQL 由 SqlDialect::translate() 生成
         *          （带标识符引用、按 TableSchema 展开列、按方言补全分页），两者文本可能不同；
         *          参数占位符风格与方言一致，排查问题时可直接对照。
         *
         * @return std::string 生成的 SQL 文本
         *
         * @code
         *   Queryable<User> query;
         *   query.where(Column(&User::age, "age") >= 18)
         *        .orderBy(asc("name"));
         *   std::string sql = query.toSql();
         *   // => "SELECT * FROM users WHERE age >= ? ORDER BY name ASC"
         * @endcode
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
         * @brief 校验当前处于在线模式（已绑定连接池）
         * @param operationName 调用方方法名，用于拼出可定位的错误文本
         * @throws std::logic_error 离线模式（默认构造）下调用执行器方法
         */
        void requireOnline(const std::string_view operationName) const
        {
            if (m_pool != nullptr)
            {
                return;
            }
            throw std::logic_error("Queryable: " + std::string(operationName) + " 需要连接池，当前为离线模式");
        }

        /**
         * @brief 取得本查询要使用的方言，首次调用时解析并缓存
         * @details 方言类型优先用构造时显式指定的值；未指定时从池中借一条连接读取其真实
         *          DatabaseType（池配置里没有类型信息，直接问连接最可靠），读完立刻归还。
         * @return const SqlDialect& 方言实例引用，生命周期由本对象缓存持有
         * @throws std::runtime_error 无法从池中取得连接以推导类型
         * @throws std::invalid_argument 该数据库类型尚无方言实现（MySQL / Redis）
         */
        [[nodiscard]] const SqlDialect &requireDialect()
        {
            if (m_dialect != nullptr)
            {
                return *m_dialect;
            }

            DatabaseType resolvedType = DatabaseType::Sqlite;
            if (m_databaseType.has_value())
            {
                resolvedType = m_databaseType.value();
            }
            else
            {
                // 借出即读、读完归还：RAII 包装在作用域结束时会自动把连接还给池，
                // 因此这里不会额外占用池容量，也不会泄漏连接
                PooledConnection probeConnection = m_pool->acquire();
                if (!probeConnection)
                {
                    throw std::runtime_error("Queryable: 无法从连接池获取连接以推导数据库类型，请检查连接池配置");
                }
                resolvedType = probeConnection->databaseType();
            }

            // 未实现的类型由注册表抛出带中文提示的异常，这里不吞掉，让调用方明确知道缺什么
            m_dialect = DialectRegistry::dialectFor(resolvedType);
            return *m_dialect;
        }

        /**
         * @brief 从连接池取一条连接，取不到直接抛异常
         * @return PooledConnection 有效连接，作用域结束时自动归还
         * @throws std::runtime_error 池已达上限且等待超时，或连接工厂创建失败
         */
        [[nodiscard]] PooledConnection acquireConnection()
        {
            PooledConnection connection = m_pool->acquire();
            if (!connection)
            {
                throw std::runtime_error("Queryable: 从连接池获取连接失败，可能是池已达上限或连接创建失败");
            }
            return connection;
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
                std::apply(
                    [&resolvedNode](const auto &...columnDescriptors)
                    {
                        (resolvedNode.selectColumns.emplace_back(columnDescriptors.columnName), ...);
                    },
                    TableSchema<T>::kColumns);
            }

            return resolvedNode;
        }

        /**
         * @brief 执行一次 SELECT 并把结果映射成结构体列表
         * @param queryNode 已展开列的查询树
         * @return std::vector<T> 映射后的行列表
         * @throws std::runtime_error 取连接失败、SQL 执行失败或行映射失败
         */
        [[nodiscard]] std::vector<T> fetchRows(const QueryNode &queryNode)
        {
            const SqlStatement statement = requireDialect().translate(queryNode);

            PooledConnection                connection = acquireConnection();
            std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                throw std::runtime_error("Queryable: 查询执行失败：" + connection->lastError());
            }

            // SQLite 的结果集持有连接句柄的非拥有指针，因此 connection 必须比 result 活得久：
            // 二者在同一作用域内按声明逆序析构，result 先销毁，约束天然满足
            return mapResultRows<T>(*result);
        }

        /**
         * @brief 执行一条写语句并返回受影响行数
         * @param statement 待执行的参数化语句
         * @return int 受影响行数
         * @throws std::runtime_error 取连接失败或语句执行失败
         */
        int executeStatement(const SqlStatement &statement)
        {
            const SqlDialect &dialect = requireDialect();

            PooledConnection                connection = acquireConnection();
            std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                throw std::runtime_error("Queryable: 语句执行失败：" + connection->lastError());
            }

            // 写语句的结果集不建立游标，影响行数由驱动构造结果集时快照；
            // DatabaseResult 基类不暴露该信息（接口已冻结），交由 Queryable.cpp 中的辅助函数取回
            return Detail::affectedRowCountOf(*result, dialect.type());
        }

        /**
         * @brief 生成 INSERT 语句
         * @details 表名与列名走方言的标识符引用、占位符走方言的 placeholder()，
         *          ORM 侧不写死任何方言符号；字段值全部作为绑定参数。
         * @param row 待插入的结构体
         * @return SqlStatement "INSERT INTO 表 (列…) VALUES (?, …)"
         */
        [[nodiscard]] SqlStatement buildInsertStatement(const T &row)
        {
            const SqlDialect &dialect = requireDialect();

            std::string                columnText;
            std::string                placeholderText;
            std::vector<DatabaseValue> parameters;

            std::apply(
                [&](const auto &...columnDescriptors)
                {
                    (appendInsertColumn(columnText, placeholderText, parameters, dialect, columnDescriptors, row), ...);
                },
                TableSchema<T>::kColumns);

            SqlStatement statement;
            statement.sql = "INSERT INTO " + dialect.quoteIdentifier(TableSchema<T>::kTableName) +
                            " (" + columnText + ") VALUES (" + placeholderText + ")";
            statement.parameters = std::move(parameters);
            return statement;
        }

        /**
         * @brief 生成按主键更新一行的 UPDATE 语句
         * @details SET 子句覆盖除主键外的全部列，WHERE 子句用主键定位；
         *          主键值作为最后一个绑定参数，与它在 SQL 中最后出现的位置一致。
         * @param row 待更新的结构体
         * @return SqlStatement "UPDATE 表 SET 列 = ?, … WHERE 主键 = ?"
         * @throws std::logic_error 主键未在 kColumns 中声明，或表中只有主键列
         */
        [[nodiscard]] SqlStatement buildUpdateStatement(const T &row)
        {
            const SqlDialect      &dialect        = requireDialect();
            const std::string_view primaryKeyName = TableSchema<T>::kPrimaryKey;

            std::string                assignmentText;
            std::vector<DatabaseValue> parameters;
            DatabaseValue              primaryKeyValue;
            bool                       primaryKeyFound = false;

            std::apply(
                [&](const auto &...columnDescriptors)
                {
                    (appendUpdateColumn(assignmentText, parameters, dialect, columnDescriptors, row,
                                        primaryKeyName, primaryKeyValue, primaryKeyFound), ...);
                },
                TableSchema<T>::kColumns);

            if (!primaryKeyFound)
            {
                throw std::logic_error("Queryable: 无法生成 UPDATE，TableSchema<" +
                                       std::string(TableSchema<T>::kTableName) + ">::kPrimaryKey（" +
                                       std::string(primaryKeyName) + "）未在 kColumns 中声明");
            }

            if (assignmentText.empty())
            {
                throw std::logic_error("Queryable: 无法生成 UPDATE，表 " +
                                       std::string(TableSchema<T>::kTableName) + " 只有主键列，没有可更新的列");
            }

            SqlStatement statement;
            statement.sql = "UPDATE " + dialect.quoteIdentifier(TableSchema<T>::kTableName) +
                            " SET " + assignmentText +
                            " WHERE " + dialect.quoteIdentifier(primaryKeyName) +
                            " = " + dialect.placeholder(parameters.size());

            // 主键参数最后压入：SET 里的占位符先出现，WHERE 的占位符最后出现
            parameters.push_back(std::move(primaryKeyValue));
            statement.parameters = std::move(parameters);
            return statement;
        }

        /**
         * @brief 生成按查询树条件删除的 DELETE 语句
         * @details 表名走方言引用；WHERE 子句复用方言对同一条件树的 SELECT 翻译结果并截取其
         *          条件部分——递归逻辑条件、IN 展开、参数收集顺序的规则只在方言层实现一次，
         *          ORM 侧不再维护第二套渲染器，两边也不会随方言演进而产生行为差异。
         * @param queryNode 提供条件与表名的查询树
         * @return SqlStatement "DELETE FROM 表 [WHERE 条件]"；无 WHERE 条件时省略该子句
         * @throws std::logic_error 方言生成的前缀与预期不符（说明方言实现被改动）
         */
        [[nodiscard]] SqlStatement buildDeleteStatement(const QueryNode &queryNode)
        {
            const SqlDialect &dialect = requireDialect();

            // 别名一并带上：WHERE 里的限定列名（"u"."id"）只有别名存在时才能被解析
            std::string deleteTarget = dialect.quoteIdentifier(queryNode.tableName);
            std::string selectPrefix = "SELECT * FROM " + deleteTarget;
            if (!queryNode.tableAlias.empty())
            {
                const std::string aliasText = " AS " + dialect.quoteIdentifier(queryNode.tableAlias);
                deleteTarget += aliasText;
                selectPrefix += aliasText;
            }

            SqlStatement statement;
            statement.sql = "DELETE FROM " + deleteTarget;

            if (queryNode.whereConditions.empty())
            {
                // 无条件即全表删除，行为与 SQL 语义一致；调用方已在方法声明处收到警告
                return statement;
            }

            QueryNode conditionOnlyNode;
            conditionOnlyNode.tableName       = queryNode.tableName;
            conditionOnlyNode.tableAlias      = queryNode.tableAlias;
            conditionOnlyNode.whereConditions = queryNode.whereConditions;

            const SqlStatement conditionStatement = dialect.translate(conditionOnlyNode);

            // 方言按固定顺序生成 SQL，翻译结果必然以该前缀开头；对不上说明方言实现被改动了，
            // 此时宁可报错也不能把条件拼错的 SQL 交给数据库
            if (conditionStatement.sql.rfind(selectPrefix, 0) != 0)
            {
                throw std::logic_error("Queryable: 方言生成的 SELECT 前缀与预期不一致，无法复用 WHERE 子句（实际：" +
                                       conditionStatement.sql + "）");
            }

            statement.sql += conditionStatement.sql.substr(selectPrefix.size());
            statement.parameters = conditionStatement.parameters;
            return statement;
        }

        /**
         * @brief 拼接 INSERT 的一列（列名 + 占位符 + 绑定参数）
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param columnText 列名列表缓冲区，非空时先补分隔符
         * @param placeholderText 占位符列表缓冲区，与 columnText 同步补分隔符
         * @param parameters 绑定参数列表，按列顺序追加
         * @param dialect 方言，提供标识符引用与占位符写法
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param row 提供字段值的结构体
         */
        template<typename ColumnDescriptorType>
        static void appendInsertColumn(std::string &columnText,
                                       std::string &placeholderText,
                                       std::vector<DatabaseValue> &parameters,
                                       const SqlDialect &dialect,
                                       const ColumnDescriptorType &columnDescriptor,
                                       const T &row)
        {
            // 用「缓冲区是否为空」判断是不是第一列：折叠表达式从左到右执行，
            // 因此列名与占位符的分隔符总能同步补上
            if (!columnText.empty())
            {
                columnText += ", ";
                placeholderText += ", ";
            }

            columnText += dialect.quoteIdentifier(columnDescriptor.columnName);
            placeholderText += dialect.placeholder(parameters.size());

            // 成员值 → 绑定参数：optional 空值绑定为 SQL NULL，无符号超范围降级为十进制文本
            parameters.push_back(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer)));
        }

        /**
         * @brief 拼接 UPDATE 的一列（主键进 WHERE，其余进 SET）
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param assignmentText SET 子句缓冲区
         * @param parameters 绑定参数列表，按 SET 列顺序追加
         * @param dialect 方言，提供标识符引用与占位符写法
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param row 提供字段值的结构体
         * @param primaryKeyName 主键列名，命中该列的成员不进入 SET
         * @param primaryKeyValue 出参：主键列的绑定参数
         * @param primaryKeyFound 出参：是否找到主键列
         */
        template<typename ColumnDescriptorType>
        static void appendUpdateColumn(std::string &assignmentText,
                                       std::vector<DatabaseValue> &parameters,
                                       const SqlDialect &dialect,
                                       const ColumnDescriptorType &columnDescriptor,
                                       const T &row,
                                       std::string_view primaryKeyName,
                                       DatabaseValue &primaryKeyValue,
                                       bool &primaryKeyFound)
        {
            // 主键列不进 SET：更新主键会破坏行标识（其它表的外键、上层缓存都指向旧值）
            if (columnDescriptor.columnName == primaryKeyName)
            {
                primaryKeyValue = Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer));
                primaryKeyFound = true;
                return;
            }

            if (!assignmentText.empty())
            {
                assignmentText += ", ";
            }
            assignmentText += dialect.quoteIdentifier(columnDescriptor.columnName);
            assignmentText += " = ";
            assignmentText += dialect.placeholder(parameters.size());
            parameters.push_back(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer)));
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
                sql += "*";
            }
            else
            {
                for (std::size_t i = 0; i < m_queryNode.selectColumns.size(); ++i)
                {
                    if (i > 0) sql += ", ";
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
            for (const auto &joinClause : m_queryNode.joins)
            {
                sql += " ";
                sql += joinTypeToString(joinClause.type);
                sql += " JOIN ";
                sql += joinClause.tableName;
                if (!joinClause.tableAlias.empty())
                {
                    sql += " AS ";
                    sql += joinClause.tableAlias;
                }
                if (!joinClause.conditions.empty())
                {
                    sql += " ON ";
                    for (std::size_t i = 0; i < joinClause.conditions.size(); ++i)
                    {
                        if (i > 0) sql += " AND ";
                        sql += buildConditionString(joinClause.conditions[i]);
                    }
                }
            }

            // WHERE 子句
            if (!m_queryNode.whereConditions.empty())
            {
                sql += " WHERE ";
                for (std::size_t i = 0; i < m_queryNode.whereConditions.size(); ++i)
                {
                    if (i > 0) sql += " AND ";
                    sql += buildConditionString(m_queryNode.whereConditions[i]);
                }
            }

            // GROUP BY 子句
            if (!m_queryNode.groupBy.empty())
            {
                sql += " GROUP BY ";
                for (std::size_t i = 0; i < m_queryNode.groupBy.size(); ++i)
                {
                    if (i > 0) sql += ", ";
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
                    if (i > 0) sql += ", ";
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
                    if (i > 0) result += " AND ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ")";
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
                    if (i > 0) result += " OR ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ")";
                return result;
            }

            if (condition.op == SqlOperator::Not)
            {
                if (condition.children.empty())
                {
                    return "NOT (1=1)";
                }
                return "NOT " + buildConditionString(condition.children[0]);
            }

            // 叶子节点：left op right
            std::string result;
            result += fieldReferenceToString(condition.left);
            result += " ";
            result += operatorToString(condition.op);
            result += " ";

            if (condition.op == SqlOperator::IsNull || condition.op == SqlOperator::IsNotNull)
            {
                // IS NULL / IS NOT NULL 不需要右操作数
                return result;
            }

            if (condition.op == SqlOperator::In || condition.op == SqlOperator::NotIn)
            {
                if (condition.inValues.empty())
                {
                    result += "(?)";
                }
                else
                {
                    result += "(";
                    for (std::size_t i = 0; i < condition.inValues.size(); ++i)
                    {
                        if (i > 0) result += ", ";
                        result += "?";
                    }
                    result += ")";
                }
                return result;
            }

            result += "?";
            return result;
        }

        /**
         * @brief 将 SqlOperator 转为 SQL 操作符字符串
         */
        [[nodiscard]] static std::string_view operatorToString(SqlOperator op)
        {
            using namespace std::string_view_literals;
            switch (op)
            {
                case SqlOperator::Eq:        return "="sv;
                case SqlOperator::Neq:       return "!="sv;
                case SqlOperator::Gt:        return ">"sv;
                case SqlOperator::Ge:        return ">="sv;
                case SqlOperator::Lt:        return "<"sv;
                case SqlOperator::Le:        return "<="sv;
                case SqlOperator::Like:      return "LIKE"sv;
                case SqlOperator::In:        return "IN"sv;
                case SqlOperator::NotIn:     return "NOT IN"sv;
                case SqlOperator::IsNull:    return "IS NULL"sv;
                case SqlOperator::IsNotNull: return "IS NOT NULL"sv;
                default:                     return "="sv;
            }
        }

        /**
         * @brief 将 JoinType 转为 SQL 连接类型字符串
         */
        [[nodiscard]] static std::string_view joinTypeToString(JoinType type)
        {
            using namespace std::string_view_literals;
            switch (type)
            {
                case JoinType::Inner: return "INNER"sv;
                case JoinType::Left:  return "LEFT"sv;
                case JoinType::Right: return "RIGHT"sv;
                case JoinType::Cross: return "CROSS"sv;
                default:              return "INNER"sv;
            }
        }

        /**
         * @brief 将 FieldReference 转为字符串
         */
        [[nodiscard]] static std::string fieldReferenceToString(const FieldReference &field)
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
            }
            else
            {
                result += " ASC";
            }
            return result;
        }

        // ========================================================================
        // 数据成员
        // ========================================================================

        QueryNode                   m_queryNode;      ///< 查询树节点，存储所有查询构建信息
        ConnectionPool             *m_pool = nullptr; ///< 数据库连接池指针，离线模式为 nullptr
        std::optional<DatabaseType> m_databaseType;   ///< 构造时显式指定的数据库类型；未指定时从池中连接推导
        std::shared_ptr<SqlDialect> m_dialect;        ///< 缓存的方言实例，首次执行时解析并长期持有
    };

} // namespace AsynGyanis::Database::Queryable