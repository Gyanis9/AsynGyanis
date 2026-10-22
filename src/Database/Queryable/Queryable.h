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

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Task.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/OptimisticLockException.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Common/TemporalText.h"
#include "Database/Common/TransactionTimeoutException.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/QueryNode.h"
#include "Database/Queryable/Relations.h"
#include "Database/Queryable/RowMapper.h"
#include "Database/Queryable/TableSchema.h"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
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
     *       （重试无意义，应记日志让请求失败）；结果集映射失败 → RowMappingException（表结构与结构体声明不一致）；
     *       绑定事务且整笔时限已到期 → TransactionTimeoutException（再发语句与提交都被拒，另起一笔）。
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
        explicit Queryable(ConnectionPool &pool) : m_pool(&pool)
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
        Queryable(ConnectionPool &pool, DatabaseType databaseType) : m_pool(&pool), m_databaseType(databaseType)
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
        explicit Queryable(Transaction &transaction) : m_transaction(&transaction)
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
            if (m_queryNode.strictIdentifiers)
            {
                requireStrictWhereCondition(condition, "where()");
            }
            // 如果条件是 AND/OR/NOT 复合节点或普通比较，直接压入列表
            m_queryNode.whereConditions.push_back(std::move(condition));
            return *this;
        }

        /**
         * @brief 开关严格标识符校验（默认关）
         * @details 开启后 `select()/orderBy()/groupBy()/where()` 里的文本字段只允许保守字符集
         *          （字母、数字、`_ . $ * ( ) ,` 与空白）；分号、引号、注释符、运算符等一律拒绝，
         *          并在异常里点名是哪个字段、哪个字符。默认关是硬要求：既要保证既有表达式不被收紧，
         *          也要让不开这一档的调用方保持逐字节相同的 SQL
         * @param enabled true 开启；false 关闭（关闭不回溯校验已有字段）
         * @return Queryable& 自身引用
         * @throws Base::InvalidArgumentException 开启时已有字段里存在不允许的文本
         */
        Queryable &strictIdentifiers(const bool enabled = true)
        {
            if (enabled)
            {
                for (std::size_t index = 0; index < m_queryNode.selectColumns.size(); ++index)
                {
                    requireStrictIdentifierText(m_queryNode.selectColumns[index], "select() 的第 " + std::to_string(index + 1U) + " 项");
                }
                for (std::size_t index = 0; index < m_queryNode.groupBy.size(); ++index)
                {
                    requireStrictIdentifierText(m_queryNode.groupBy[index].name, "groupBy() 的第 " + std::to_string(index + 1U) + " 项");
                }
                for (const OrderByClause &order: m_queryNode.orderBy)
                {
                    requireStrictIdentifierText(order.field.name, "orderBy() 的字段");
                }
                for (const WhereCondition &condition: m_queryNode.whereConditions)
                {
                    requireStrictWhereCondition(condition, "where()");
                }
                if (m_queryNode.having.has_value())
                {
                    requireStrictWhereCondition(*m_queryNode.having, "having()");
                }
            }
            m_queryNode.strictIdentifiers = enabled;
            return *this;
        }

        /**
         * @brief 添加 ORDER BY 子句
         *
         * @param order 使用 asc()/desc() 创建的排序子句
         * @return Queryable& 自身引用，支持链式调用
         * @warning 排序字段文本与 select() 走同一条信任边界（表达式按原样拼进 SQL）：只接受编译期常量
         *          或可信文本，任何来自用户输入的片段都必须走参数绑定，否则就是 SQL 注入面
         */
        Queryable &orderBy(OrderByClause order)
        {
            if (m_queryNode.strictIdentifiers)
            {
                requireStrictIdentifierText(order.field.name, "orderBy() 的字段");
            }
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
         * @brief 为这条 SELECT 请求行锁（悲观锁）
         *
         * @details 渲染成方言给的行锁子句（MySQL / PostgreSQL 是 `FOR UPDATE`），锁住本次结果集的行
         *          直到事务结束，因此必须与事务同用：autocommit 下语句一结束锁就释放，等于没锁。
         *          SQLite 没有行级锁，本方法在那一侧于渲染阶段抛 `Base::InvalidArgumentException`
         *          并点名引擎——「要锁」被静默做成「不锁」是最难查的一种失效。
         *
         * @return Queryable& 自身引用，支持链式调用
         * @throws Base::InvalidArgumentException 引擎表达不了行锁（SQLite）
         * @note 与 `count()` 同用时行锁被丢掉：`SELECT COUNT(*) … FOR UPDATE` 在 PostgreSQL 里被服务端
         *       直接拒绝（聚合语句不许带行锁），而计数本来也不锁行
         */
        Queryable &forUpdate()
        {
            m_queryNode.isForUpdate = true;
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
         * @warning 表达式文本按原样拼进 SQL 语句，不是参数绑定的对象：
         *          只允许传入编译期常量或可信文本，任何来自用户输入的片段都必须走参数绑定
         *          （值用占位符），否则就是 SQL 注入面
         */
        Queryable &select(std::vector<std::string> columns)
        {
            if (m_queryNode.strictIdentifiers)
            {
                for (std::size_t index = 0; index < columns.size(); ++index)
                {
                    requireStrictIdentifierText(columns[index], "select() 的第 " + std::to_string(index + 1U) + " 项");
                }
            }
            m_queryNode.selectColumns = std::move(columns);
            return *this;
        }

        /**
         * @brief 显式装载一条关系：把关联行按外键分批补取回来并挂到结果上
         *
         * @details 这是「显式装载」而不是懒加载：本仓不做会挂起属性访问的代理（那在协程运行时里
         *          是最容易咬人的形状）。补取是第二条查询（`SELECT … WHERE 外键 IN (…)`），
         *          按方言的参数上限分块，因此一对多也不会把父行乘成笛卡尔积——这正是它与 JOIN
         *          那条路的分工：子行多时用本方法，一对一又想省一趟往返时才用一次 JOIN 取回。
         *
         * @details 方向由成员类型决定（见 `RelationDescriptor`）：`std::vector<U>` 是一对多
         *          （外键列在 U 的表里、指向本行主键），`U` / `std::optional<U>` 是多对一
         *          （外键列在本表里、指向 U 的主键）。要装载的成员必须先在
         *          `TableSchema<T>::kRelations` 里用 `Relation()` 声明过，否则当场抛点名那个成员的
         *          `LogicException`——装配错误要在第一次查询时就出声，不要等某天悄悄少挂一半数据。
         *
         * @details 空结果集不发第二条查询；补取不到的行（对端已删）在可空成员上留空、
         *          在 `std::vector` 成员上留空列表、在必填的非 optional 成员上保持原值（不覆写、不抛）。
         *
         * @param memberPointer 要装载的成员（`&Post::author` 这类）
         * @return Queryable& 自身引用，支持链式调用
         * @throws Base::LogicException 当前为离线模式（要方言定参数上限）；或该成员没有在
         *         `kRelations` 里声明过
         * @note 对 `first()` / `firstAsync()` 同样生效：它们把单行包成一行列表走同一条装载路径
         */
        template<typename MemberType>
        Queryable &then(MemberType T::*memberPointer)
        {
            requireOnline("then()");
            m_relationLoaders.push_back(makeRelationLoader(memberPointer, m_pool, m_transaction, resolveDialect()));
            return *this;
        }

        /**
         * @brief 显式装载一条关系：用一次 LEFT JOIN 把关联行随主查询一起带回来
         *
         * @details 与 `then()` 同一条取向（显式点名装载，不做懒加载代理），差别只在取数方式：
         *          本方法把关联表连进同一条 SELECT（`LEFT JOIN … ON …`），并把它的列以
         *          `别名__列名` 的形状投影出来，因此不多一次往返；`then()` 走的是第二条
         *          `IN (…)` 补取查询。取舍：多对一（对端唯一，JOIN 不会把行乘开）用本方法划算；
         *          一对多走 JOIN 会把父行按子行乘开（结果行数 = 父行 × 子行数），需要在客户端
         *          折叠回一行——子行多时应当改用 `then()`。
         *
         * @details 两个方向的装载形状：多对一挂上 `U` / `std::optional<U>` 成员（LEFT JOIN
         *          配不上时留空、必填成员保持原值）；一对多把子行按基表主键折叠回父行，
         *          没有子行的父行留空列表（与 `then()` 同一条语义）。
         *
         * @param memberPointer 要装载的成员（`&Post::author` 这类）
         * @return Queryable& 自身引用，支持链式调用
         * @throws Base::LogicException 该成员没有在 `kRelations` 里声明过；关联行类型的主键列
         *         没有在它的 `kColumns` 里声明（多对一要靠它判「配不上」、一对多要靠它折叠）；
         *         或一对多的 include 与 LIMIT / OFFSET 同时用（在取数时拒，见 @warning）
         * @note `count()` / `countAsync()` 不接受 include：聚合方向整段 SELECT 列表被 COUNT(*)
         *       顶掉，而 JOIN 还会把一对多的行乘开让计数虚高——请另起一条查询统计
         * @note `first()` / `firstAsync()` 与一对多的 include 同用时会取回全部结果行再折叠
         *       （LIMIT 压到 1 会把父行的子行截掉），不能接受这一点时请用 `then()` 补取
         * @warning 一对多的 include 与 `limit()` / `offset()` 不能同时用：分页数的是结果行
         *          （父行被子行乘开之后的），不是父行数。要限制父行请先查出父行、再用 `then()` 补取
         */
        template<typename MemberType>
        Queryable &include(MemberType T::*memberPointer)
        {
            const RelationDescriptor<T, MemberType> *relation = findRelationOf(memberPointer);
            if (relation == nullptr)
            {
                throw Base::LogicException("Queryable: include() 要装载的成员没有在 TableSchema<" + std::string(TableSchema<T>::kTableName) +
                                           ">::kRelations 里声明过——请先加一条 Relation(&T::成员, \"外键列\")");
            }

            using Target     = Detail::RelationTarget<MemberType>;
            using RelatedRow = typename Target::RowType;
            // 成员类型必须是一个登记过的行类型：指向别的东西（比如 std::string）时整段不实例化
            // （上面那次 findRelationOf 已经先抛了，这里挡住的是编译期的实例化）。
            // 返回语句放进 else 支里：真支是 throw，之后不该再留下任何可达语句（MSVC 会报 C4702）
            if constexpr (!std::is_aggregate_v<RelatedRow>)
            {
                throw Base::LogicException("Queryable: include() 指向的成员不是一张登记过的行类型（关系装载只认 std::vector<行> 与 行/optional<行>）");
            } else
            {
                // 主键列必须在关联行类型里声明过：LEFT JOIN 配不上时整行都是 NULL，只有主键列
                // 保证非空——拿别的列判「配不上」会把「子行某列恰好是 NULL」误判成「没有子行」；
                // 一对多的折叠也按基表主键做，同样要求它可读
                if (!columnIsDeclared<RelatedRow>(TableSchema<RelatedRow>::kPrimaryKey))
                {
                    throw Base::LogicException("Queryable: include() 要求关联行类型 TableSchema<" + std::string(TableSchema<RelatedRow>::kTableName) + "> 把它的主键列（" +
                                               std::string(TableSchema<RelatedRow>::kPrimaryKey) + "）声明在 kColumns 里——装载要靠它判「有没有配上」");
                }

                // 前缀按声明顺序编号：r0、r1…。它与「__」一起把关联列与基表列分开（见 AliasColumn）
                const std::string prefix = "r" + std::to_string(m_includeProcessors.size());

                appendIncludeJoin<RelatedRow, MemberType>(*relation, prefix, Target::isCollection);
                appendIncludeProjection<RelatedRow>(prefix);
                m_includeProcessors.push_back(makeIncludeProcessor<RelatedRow, MemberType>(memberPointer, prefix));
                return *this;
            }
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
                if (m_queryNode.strictIdentifiers)
                {
                    requireStrictIdentifierText(field, "groupBy() 的第 " + std::to_string(fieldRefs.size() + 1U) + " 项");
                }
                fieldRefs.push_back(FieldReference{.name = std::move(field)});
            }
            m_queryNode.groupBy = std::move(fieldRefs);
            return *this;
        }

        /**
         * @brief 设置 HAVING 条件（对分组结果的过滤）
         *
         * @details 与 where() 的差别只在时机：where 在分组前筛行，having 在分组后筛组。条件本身
         *          与 where() 共用同一套 Expression/Column 组合与同一份方言渲染，因此不额外引入写法。
         *          GROUP BY 为空时仍然合法——两个引擎都把整份结果当成一组。
         * @param condition 分组过滤条件，可含聚合表达式（如 "COUNT(*)" 作为左值）
         * @return Queryable& 自身引用，支持链式调用
         * @warning 条件里的字段与表达式文本沿用 select()/groupBy() 的同一条信任边界（表达式按原样拼
         *          进 SQL）：只接受编译期常量或可信文本，用户输入一律走参数绑定，否则就是注入面
         */
        Queryable &having(WhereCondition condition)
        {
            if (m_queryNode.strictIdentifiers)
            {
                requireStrictWhereCondition(condition, "having()");
            }
            m_queryNode.having = std::move(condition);
            return *this;
        }

        /**
         * @brief 指定异步执行器（阻塞任务的工作线程池）
         *
         * @details 只影响异步方法（toListAsync / firstAsync / countAsync / insertAsync /
         *          insertAndGetGeneratedIdAsync / insertBatchAsync / updateAsync / executeNonQueryAsync）：
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
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（如 Redis）
         */
        [[nodiscard]] std::vector<T> toList()
        {
            requireOnline("toList()");

            std::vector<T> rows = fetchRows(resolvedQueryNode());
            // 装载在取数那条连接释放之后跑：补取会再借一条（池上限为 1 的部署里，握着第一条等第二条就是死等）
            runRelationLoaders(rows);
            return rows;
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
            if (!hasFoldingInclude())
            {
                // 只取一行：LIMIT 1 让数据库侧提前停止扫描，比取回全部再取首元素高效得多。
                // 带一对多的 include 时不压 LIMIT——分页数的是结果行，压到 1 会把父行的子行截掉，
                // 那种情况只能取回全部结果再折叠（见 include() 的 @note）
                limitedNode.limit = 1U;
            }

            if (m_relationLoaders.empty() && m_includeProcessors.empty())
            {
                return fetchFirst(limitedNode);
            }

            // 有装载器 / include 时走列表路径：装载器就地改一个 vector，单行包成一行即可（LIMIT 1 照样生效）
            std::vector<T> rows = fetchRows(limitedNode);
            runRelationLoaders(rows);
            if (rows.empty())
            {
                return std::nullopt;
            }
            return std::optional<T>(std::move(rows.front()));
        }

        /**
         * @brief 统计匹配行数
         *
         * @details 把 SELECT 列表替换成 COUNT(*)、并清掉 ORDER BY 与分页后执行
         *          （聚合结果只有一行，排序与分页没有意义，某些数据库还会直接报错）。
         *          手动设置的 SELECT 列会被本方法忽略；GROUP BY 保留，此时返回第一组的计数。
         *
         * @return std::int64_t 匹配的行数；带 GROUP BY 时是其中一组的计数（引擎按组各回一行，
         *         本方法只读第一行且组序不保证），要在乎每一组就改用 toList() 投影 COUNT(*)；
         *         结果为空或计数列为 NULL 时返回 0
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败或 SQL 执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t count()
        {
            requireOnline("count()");
            requireNoIncludesForAggregate("count()");

            // 计数方向整段 SELECT 列表被 COUNT(*) 顶掉，因此不走 resolvedQueryNode()：
            // 先把全部列名展开再覆盖一遍是白做（列名展开要建一个 vector<string>）
            QueryNode countingNode     = m_queryNode;
            countingNode.selectColumns = {"COUNT(*)"};
            countingNode.orderBy.clear();
            countingNode.limit.reset();
            countingNode.offset.reset();
            // 行锁一并丢掉：PostgreSQL 服务端拒绝「聚合 + FOR UPDATE」，而计数本来也不锁行
            countingNode.isForUpdate = false;

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
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现；或查询树带着 LIMIT / OFFSET——
         *         本层不生成带分页的删除语句，静默丢掉它就会把「只删 N 行」做成删掉全部匹配行
         * @throws DatabaseException 取连接失败或语句执行失败
         * @warning 查询树不含任何条件时生成的语句是 "DELETE FROM 表"，会清空全表；
         *          需要限定范围请先调用 where()，需要限定行数请改为「先按 limit 查出主键、再按主键删」
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
         * @brief 插入一行，并指定唯一键冲突时的处置
         *
         * @details 补的是「按业务键 create-or-update」这一格：只有 `insert(row)` 时，调用方要嘛
         *          先查后插（两条语句之间别人插进来就撞唯一键），要嘛自己写 `INSERT OR REPLACE` /
         *          `REPLACE INTO` 的原语 SQL——而那句是方言专有的，等于绕开 ORM 的参数化与取值转换。
         *          本方法只交意图，关键词由方言给（见 `SqlDialect::insertKeywordPhrase()`）。
         * @param row 待插入的结构体
         * @param conflict 冲突处置：`Fail` 与单参版完全等价；`Ignore` 跳过冲突行；`Replace` 换掉那一行；
         *        `Upsert` 只更新本次插入给出的列
         * @return std::int64_t 受影响的行数（引擎对 IGNORE 跳过的行不计入，MySQL 的 REPLACE 计 2）
         * @note `Replace` 在两家引擎上都是「先删同键旧行再插」而不是「只改这几列」：触发器多一次 DELETE、
         *       自增标识可能换值、新行没给的列回默认值。要只改指定列就用 `Upsert`（MySQL 写作
         *       ON DUPLICATE KEY UPDATE，PostgreSQL/SQLite 写作 ON CONFLICT DO UPDATE）
         * @throws Base::LogicException 当前为离线模式
         * @throws DatabaseException 取连接失败或语句执行失败（`Fail` 档的唯一约束冲突即在此列）
         */
        [[nodiscard]] std::int64_t insert(const T &row, const InsertConflict conflict)
        {
            requireOnline("insert(row, conflict)");
            return executeStatement(buildInsertStatement(row, conflict));
        }

        /**
         * @brief 插入一行并取回数据库生成的自增标识
         *
         * @details 与 insert() 走同一份语句生成与执行路径，只是读的是生成键而不是影响行数。三条 SQL
         *          引擎各按自己的通道在同一次往返里取回：SQLite 与 MySQL 在结果集构造时从连接句柄
         *          就地快照（见 DatabaseResult::lastInsertRowId()），PostgreSQL 由方言给这条 INSERT 追加
         *          `RETURNING 主键列`、值随该语句的回复一起交回（见
         *          PostgresDialect::appendGeneratedKeyReturningTail() 与 PostgresResult::lastInsertRowId()）。
         *          两条通道都不是「再补一条查询去问」：连接由池共享，先插入、再单独查
         *          last_insert_rowid() 或 lastval() 的两次取出可能落到两条连接上，而 lastval() 读的还是
         *          本会话最后一个序列——插入别的表也会串号。
         *
         * @param row 待插入的结构体；TableSchema<T>::kIsAutoIncrementPrimaryKey 为真时主键字段被忽略
         * @return std::int64_t 数据库生成的自增标识；0 表示这条语句没有产生可交回的生成键（未声明
         *         自增主键、表上没有自增列、带回的值不是整数，或值超出有符号 64 位——后两种由结果集的
         *         lastError() 说明，ORM 这一层拿不到那份文本）
         *
         * @throws Base::LogicException 当前为离线模式，或声明了自增主键却没有其它可写列
         * @throws DatabaseException 取连接失败或语句执行失败（如唯一约束冲突）
         * @note 批量插入不提供这个通道：一次写多行时「哪个标识属于哪一行」两家引擎口径不同
         *       （MySQL 给首行、SQLite 给末行），交出一个含义不明的值不如让调用方按业务键回查
         */
        [[nodiscard]] std::int64_t insertAndGetGeneratedId(const T &row)
        {
            requireOnline("insertAndGetGeneratedId()");

            // 语句必须先在锁外生成：方言解析要借一条连接探测数据库类型，握着租约再借一次就是把
            // 自己排在自己后面——池上限为 1 时（内存库的常见配置）直接等到 acquire 超时
            const SqlStatement    statement = buildInsertStatement(row, InsertConflict::Fail, true);
            const ConnectionLease lease     = acquireConnection(m_pool, m_transaction);
            return runOn(*lease.connection, statement)->lastInsertRowId();
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
         * @brief 批量插入多行，并指定唯一键冲突时的处置
         * @details 与 insertBatch(rows) 同一份分块与事务实现，只是把冲突处置写进查询树；
         *          `Ignore` 档下「受影响行数」会小于 rows.size()（跳过的行不计），这不是失败
         * @param rows 待插入的行集合，允许为空（空集合直接返回 0，不产生任何语句）
         * @param conflict 冲突处置，`Fail` 与不带该参数的版本完全等价
         * @return std::int64_t 累计受影响的行数
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws DatabaseException 取连接失败、事务开启失败或语句执行失败
         */
        [[nodiscard]] std::int64_t insertBatch(const std::span<const T> rows, const InsertConflict conflict)
        {
            requireOnline("insertBatch(rows, conflict)");

            if (rows.empty())
            {
                return 0;
            }

            return insertBatchOn(m_pool, m_transaction, requireDialect(), rows, conflict);
        }

        /**
         * @brief 按结构体主键批量更新（整批在同一事务里）
         * @details 逐行生成 `UPDATE … WHERE 主键 = ?`，共用一条连接与一个事务：中途失败整批回滚。
         *          没有绑定事务时本方法自己开一笔；已绑定事务时并入调用方那笔，提交仍由调用方负责
         * @param rows 待更新的行，允许为空（返回 0，不产生任何语句）
         * @return std::int64_t 各行受影响行数之和（口径同 update()）
         * @note 只按行主键更新，不叠加当前查询树的 where 条件（与 update() 同口径）
         * @throws Base::LogicException 当前为离线模式
         * @throws DatabaseException 事务开启或语句执行失败
         * @throws OptimisticLockException 任一行版本不匹配（整批回滚）
         */
        [[nodiscard]] std::int64_t updateBatch(const std::span<const T> rows)
        {
            requireOnline("updateBatch()");
            if (rows.empty())
            {
                return 0;
            }

            std::int64_t affectedRowCount = 0;
            if (m_transaction != nullptr)
            {
                for (const T &row: rows)
                {
                    affectedRowCount += update(row);
                }
                return affectedRowCount;
            }

            Transaction  transaction(*m_pool);
            Queryable<T> batchQuery(transaction);
            for (const T &row: rows)
            {
                affectedRowCount += batchQuery.update(row);
            }
            if (!transaction.commit())
            {
                throw QueryExecutionException("批量更新：提交事务失败：" + transaction.lastError());
            }
            return affectedRowCount;
        }

        /**
         * @brief 按结构体主键批量删除（整批在同一事务里）
         * @details 按引擎参数上限把主键分块，每块生成 `DELETE … WHERE 主键 IN (?, …)`；
         *          没有绑定事务时自己开一笔，已绑定时并入调用方那笔
         * @param rows 用于取主键的行，允许为空（返回 0）
         * @return std::int64_t 受影响的行数
         * @note 只按这些行的主键删，不叠加当前查询树上已设置的 where 条件——批量删的语义是
         *       「删掉我手上这些行」，混入别的过滤会让「传了 N 行却只删掉一部分」变成静默行为
         * @throws Base::LogicException 当前为离线模式，或主键列未在 kColumns 中声明
         * @throws DatabaseException 事务开启或语句执行失败
         */
        [[nodiscard]] std::int64_t deleteBatch(const std::span<const T> rows)
        {
            requireOnline("deleteBatch()");
            if (rows.empty())
            {
                return 0;
            }

            const SqlDialect          &dialect        = requireDialect();
            const std::size_t          parameterLimit = std::max<std::size_t>(1U, dialect.maximumStatementParameters());
            std::vector<DatabaseValue> primaryKeys;
            primaryKeys.reserve(rows.size());
            for (const T &row: rows)
            {
                const std::optional<DatabaseValue> key = readColumnValue(row, TableSchema<T>::kPrimaryKey);
                if (!key.has_value())
                {
                    throw Base::InvalidArgumentException("批量删除：TableSchema 的主键列「" + std::string(TableSchema<T>::kPrimaryKey) + "」不在 kColumns 中，无法从行里取出键值");
                }
                primaryKeys.push_back(*key);
            }

            const auto deleteChunks = [&](Transaction &transaction)
            {
                std::int64_t affectedRowCount = 0;
                for (std::size_t offset = 0; offset < primaryKeys.size(); offset += parameterLimit)
                {
                    const std::size_t chunkCount = std::min(parameterLimit, primaryKeys.size() - offset);
                    WhereCondition    condition;
                    condition.left  = FieldReference{.name = std::string(TableSchema<T>::kPrimaryKey)};
                    condition.op    = SqlOperator::In;
                    condition.right = ParameterValue{static_cast<std::int64_t>(0)};
                    condition.inValues.reserve(chunkCount);
                    for (std::size_t index = 0; index < chunkCount; ++index)
                    {
                        condition.inValues.push_back(Detail::toParameterValueFromKey(primaryKeys[offset + index], TableSchema<T>::kPrimaryKey));
                    }

                    // 每块独立构造：where() 是追加语义，复用同一个查询对象会让第二块起
                    // 带上前一块的 IN 条件——参数数累加越界被拒，或在不判上限的引擎上静默少删
                    Queryable<T> chunkQuery(transaction);
                    chunkQuery.where(std::move(condition));
                    affectedRowCount += chunkQuery.executeNonQuery();
                }
                return affectedRowCount;
            };

            if (m_transaction != nullptr)
            {
                return deleteChunks(*m_transaction);
            }

            Transaction        transaction(*m_pool);
            const std::int64_t affectedRowCount = deleteChunks(transaction);
            if (!transaction.commit())
            {
                throw QueryExecutionException("批量删除：提交事务失败：" + transaction.lastError());
            }
            return affectedRowCount;
        }

        /**
         * @brief keyset（游标）分页：取主键严格大于上一页最后一条的记录
         * @details 与 LIMIT/OFFSET 分页相比，翻页代价不随页码增长；本方法只追加 `键 > 上一页末值`
         *          并清掉 offset。调用方应同时 `orderBy(asc(同一个键))` 并设置 `limit()`——
         *          顺序与游标键不一致时结果仍是「键大于阈值」的子集，但不是稳定的逐页序列
         * @param keyColumn 游标列（通常是主键）
         * @param lastSeenValue 上一页最后一条在该列上的值；第一页不调用本方法
         * @return Queryable& 自身，便于链式调用
         */
        template<typename KeyType>
        Queryable &seekAfter(const ColumnDescriptor<T, KeyType> &keyColumn, const KeyType &lastSeenValue)
        {
            m_queryNode.offset.reset();
            return where(keyColumn > lastSeenValue);
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
         * @return std::int64_t 驱动口径的受影响行数
         * @warning 不要拿它判断「这行存在不存在」：SQLite 统计匹配到并被写入的行（写回原值也算 1），
         *          MySQL 只统计值真变了的行（写回原值是 0，与主键不存在无法区分）。两侧口径各由一条
         *          用例钉住，存在性请用 first() / count() 确认。
         *
         * @details `TableSchema<T>::kVersionColumn` 非空时（乐观锁）多两件事：WHERE 追加
         *          「版本 = ?」（问的是手里这份数据的版本），SET 把该列写成自增加一；更新到
         *          0 行即抛 `OptimisticLockException`，消息点名表、版本列、期望版本与主键值。
         *          这一档与 `InsertConflict` 三档正交：那一档管 INSERT 撞唯一键，本档管 UPDATE
         *          条件匹配不上，一次写入只可能落在其中一处。
         * @note 更新成功之后，这条结构体里的版本字段就成了旧值（服务端已经加过一，而本方法只读地
         *       接收结构体、拿不到回写）。要继续改这一行请先重新读出——这正是乐观锁的意思：
         *       手里那份数据一旦写出去就过期了，第二次拿旧值写会当场报冲突而不是悄悄覆盖别人
         *
         * @throws Base::LogicException 当前为离线模式（无连接池也未绑定事务）
         * @throws Base::LogicException TableSchema<T>::kPrimaryKey 未在 kColumns 中声明，
         *         或表中只有主键列（没有可更新的列），无法生成 UPDATE
         * @throws Base::LogicException 版本列与主键同列，或声明了版本列但它不在 kColumns 中
         * @throws OptimisticLockException 声明了版本列且这次 UPDATE 命中 0 行（别人先改过，或这一行已不在）
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         */
        [[nodiscard]] std::int64_t update(const T &row)
        {
            requireOnline("update()");

            const auto [statement, conflictReason] = buildUpdateStatement(row);
            const std::int64_t affectedRows        = executeStatement(statement);
            // 启用乐观锁时，「0 行」不是「没变化」而是「别人先改过」：见 requireNoOptimisticLockConflict
            requireNoOptimisticLockConflict(conflictReason, affectedRows);
            return affectedRows;
        }

        // ========================================================================
        // 执行器方法（在线模式，异步：阻塞链路挪到 AsyncExecutor 的工作线程）
        // ========================================================================

        /**
         * @brief 异步执行查询并返回所有结果行
         *
         * @details 与 toList() 语义完全一致，唯一差别是不阻塞调用线程：方言解析与 SQL 生成
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
        [[nodiscard]] Core::Task<std::vector<T>> toListAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("toListAsync()");

            // 方言与查询树在提交前定型：二者都是值语义的纯数据，捕获进任务后工作线程
            // 不再触碰本对象，因此「查询构建器被并发使用」这类问题不会借异步路径被放大
            std::shared_ptr<SqlDialect> dialect      = resolveDialect();
            QueryNode                   resolvedNode = resolvedQueryNode();
            ConnectionPool             *pool         = m_pool;
            Transaction                *transaction  = m_transaction;
            // 装载器按值拷进任务：它们捕获的是池/事务/方言，在工作线程上跑与在调用线程上一样安全；
            // include 处理器同样按值拷贝（闭包只捕获成员指针与前缀字符串，不触及本对象）
            std::vector<RelationLoader>   loaders  = m_relationLoaders;
            std::vector<IncludeProcessor> includes = m_includeProcessors;

            // 类模板内经由成员函数返回值访问成员模板时，GCC 要求名字前写 template 关键字，
            // 否则 '<' 会被当成小于号解析；MSVC 接受缺省写法，这里按最严的编译器来写
            std::vector<T> rows = co_await asyncExecutor().template submit<std::vector<T>>(
                    completionLoop,
                    [dialect, resolvedNode = std::move(resolvedNode), pool, transaction, loaders = std::move(loaders), includes = std::move(includes)]() -> std::vector<T>
                    {
                        std::vector<T> mappedRows;
                        {
                            // 整段阻塞链路在工作线程上执行：取连接、执行语句、逐行映射成结构体
                            ConnectionLease lease = acquireConnection(pool, transaction);
                            mappedRows            = fetchRowsOn(*lease.connection, *dialect, resolvedNode, includes);
                        }
                        // 补取排在取数连接释放之后：池上限为 1 时，握着第一条等第二条就是死等
                        for (const RelationLoader &relationLoader: loaders)
                        {
                            relationLoader(mappedRows);
                        }
                        return mappedRows;
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
        [[nodiscard]] Core::Task<std::optional<T>> firstAsync(Core::EventLoop &completionLoop)
        {
            requireOnline("firstAsync()");

            std::shared_ptr<SqlDialect> dialect     = resolveDialect();
            QueryNode                   limitedNode = resolvedQueryNode();
            if (!hasFoldingInclude())
            {
                // 只取一行：让数据库侧提前停止扫描，与同步版 first() 的取舍一致。
                // 带一对多的 include 时不压 LIMIT，理由见 first() 与 include() 的 @note
                limitedNode.limit = 1U;
            }
            ConnectionPool *pool        = m_pool;
            Transaction    *transaction = m_transaction;
            // 装载器与 include 处理器按值拷进任务（理由同 toListAsync()）
            std::vector<RelationLoader>   loaders  = m_relationLoaders;
            std::vector<IncludeProcessor> includes = m_includeProcessors;

            std::optional<T> firstRow = co_await asyncExecutor().template submit<std::optional<T>>(
                    completionLoop,
                    [dialect, limitedNode = std::move(limitedNode), pool, transaction, loaders = std::move(loaders), includes = std::move(includes)]() -> std::optional<T>
                    {
                        std::vector<T> mappedRows;
                        {
                            ConnectionLease lease = acquireConnection(pool, transaction);
                            mappedRows            = fetchRowsOn(*lease.connection, *dialect, limitedNode, includes);
                        }
                        for (const RelationLoader &relationLoader: loaders)
                        {
                            relationLoader(mappedRows);
                        }
                        if (mappedRows.empty())
                        {
                            return std::nullopt;
                        }
                        return std::optional<T>(std::move(mappedRows.front()));
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
            requireNoIncludesForAggregate("countAsync()");

            std::shared_ptr<SqlDialect> dialect = resolveDialect();
            // 与同步的 count() 同一条理由：列名展开的结果马上被 COUNT(*) 顶掉，不必先做一遍
            QueryNode countingNode     = m_queryNode;
            countingNode.selectColumns = {"COUNT(*)"};
            countingNode.orderBy.clear();
            countingNode.limit.reset();
            countingNode.offset.reset();
            // 行锁一并丢掉：PostgreSQL 服务端拒绝「聚合 + FOR UPDATE」，而计数本来也不锁行
            countingNode.isForUpdate    = false;
            ConnectionPool *pool        = m_pool;
            Transaction    *transaction = m_transaction;

            std::int64_t countedRows = co_await asyncExecutor().template submit<std::int64_t>(completionLoop,
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
            Transaction    *transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(completionLoop,
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
         * @note row 按值接收，与另几个异步方法的引用/视图入参刻意不同：本方法是惰性启动的
         *       协程，函数体（包括把 row 的取值转成绑定参数）要到首次 resume 才执行，
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
            Transaction    *transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(completionLoop,
                                                                                               [statement = std::move(statement), pool, transaction]() -> std::int64_t
                                                                                               {
                                                                                                   ConnectionLease lease = acquireConnection(pool, transaction);
                                                                                                   return executeOn(*lease.connection, statement);
                                                                                               });

            co_return affectedRows;
        }

        /**
         * @brief 异步插入一行并取回数据库生成的自增标识
         * @details 与 insertAndGetGeneratedId() 语义完全一致：语句在提交前由方言定型，
         *          「取连接 → 执行 → 读写回执上的自增标识」整段交给工作线程，快照因此就在那条
         *          连接上完成，回到事件循环之后再也没有别人能改写它。
         * @param row 待插入的结构体（声明为自增的主键字段被忽略）
         * @param completionLoop 恢复本协程用的事件循环，要求同 toListAsync()
         * @return Core::Task<std::int64_t> 惰性启动的协程；数据库生成的自增标识，未产生时为 0
         * @throws Base::LogicException 当前为离线模式，或声明了自增主键却没有其它可写列
         * @throws DatabaseException 取连接失败或语句执行失败
         * @note row 按值接收的理由见 insertAsync()
         */
        [[nodiscard]] Core::Task<std::int64_t> insertAndGetGeneratedIdAsync(T row, Core::EventLoop &completionLoop)
        {
            requireOnline("insertAndGetGeneratedIdAsync()");

            // 语句生成要读 row 并访问本对象的查询树，必须在提交前完成；之后按值捕获交给工作线程
            // 要求回读生成键这一点也写进语句里（理由与同步版相同，见 insertAndGetGeneratedId()）
            SqlStatement    statement   = buildInsertStatement(row, InsertConflict::Fail, true);
            ConnectionPool *pool        = m_pool;
            Transaction    *transaction = m_transaction;

            const std::int64_t generatedId = co_await asyncExecutor().template submit<std::int64_t>(completionLoop,
                                                                                                    [statement = std::move(statement), pool, transaction]() -> std::int64_t
                                                                                                    {
                                                                                                        ConnectionLease lease = acquireConnection(pool, transaction);
                                                                                                        return runOn(*lease.connection, statement)->lastInsertRowId();
                                                                                                    });

            co_return generatedId;
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
         * @note rows 按值接收（理由同 insertAsync）：惰性协程要到首次 resume 才读入参，
         *       视图（span）不持有所指数据，按视图接收会让「先拿 Task 再 resume」静默读到
         *       已销毁的容器。需要传已有容器时用 std::move 转交，避免多一次拷贝；
         *       传入 std::span 的调用方需自行构造 vector（这是让协程帧拥有数据的必要代价）
         */
        [[nodiscard]] Core::Task<std::int64_t> insertBatchAsync(std::vector<T> rows, Core::EventLoop &completionLoop)
        {
            requireOnline("insertBatchAsync()");

            // 空集合的判断排在方言探测之前：探测可能要借一条连接去问数据库类型，池被占满时那一次
            // 借出会等满 acquireTimeout 再抛 ConnectionUnavailableException，而契约是「空集合直接得到 0，
            // 不产生任何语句」。同步版也是先判空再取方言，两条方向同一口径
            if (rows.empty())
            {
                co_return 0;
            }

            // 方言解析可能借出连接探测类型，放在提交前（与其它异步方法一致）
            std::shared_ptr<SqlDialect> dialect     = resolveDialect();
            ConnectionPool             *pool        = m_pool;
            Transaction                *transaction = m_transaction;

            std::int64_t affectedRows = co_await asyncExecutor().template submit<std::int64_t>(
                    completionLoop, [dialect, rows = std::move(rows), pool, transaction]() -> std::int64_t { return insertBatchOn(pool, transaction, *dialect, rows); });

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
         * @return Core::Task<std::int64_t> 惰性启动的协程；驱动口径的受影响行数，含义与 update() 相同
         *         （不要拿它当存在性判据，理由见 update() 的 @warning）
         * @throws Base::LogicException 当前为离线模式；或主键未在 kColumns 声明、表中只有主键列；
         *         或版本列与主键同列、声明了版本列但不在 kColumns 中
         * @throws OptimisticLockException 声明了版本列且这次 UPDATE 命中 0 行（别人先改过，或这一行已不在）；
         *         抛在恢复本协程的那个线程上，判据与同步路径共用同一份实现
         * @throws DatabaseException 取连接失败或语句执行失败
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现
         * @note 主键缺失这类编程错误在首次 resume 时就会抛出（语句生成阶段），
         *       不会变成工作线程上的异常；row 按值接收的理由见 insertAsync()
         */
        [[nodiscard]] Core::Task<std::int64_t> updateAsync(T row, Core::EventLoop &completionLoop)
        {
            requireOnline("updateAsync()");

            auto [statement, conflictReason] = buildUpdateStatement(row);
            ConnectionPool *pool             = m_pool;
            Transaction    *transaction      = m_transaction;

            std::int64_t affectedRows =
                    co_await asyncExecutor().template submit<std::int64_t>(completionLoop,
                                                                           [statement = std::move(statement), conflictReason, pool, transaction]() -> std::int64_t
                                                                           {
                                                                               ConnectionLease lease = acquireConnection(pool, transaction);
                                                                               return executeOn(*lease.connection, statement);
                                                                           });

            // 判据与同步路径同一份实现（同一条口径：启用乐观锁时 0 行＝冲突），抛在恢复本协程的线程上
            requireNoOptimisticLockConflict(conflictReason, affectedRows);
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
        /// 关系装载器：拿着行列表去补取并把关联挂上。闭包捕获池/事务/方言，不捕获 this——
        /// 查询构建器在链式调用里会被移动，捕获 this 的闭包在移动之后指向旧对象
        using RelationLoader = std::function<void(std::vector<T> &)>;

        /// 本次查询要显式装载的关系（`then()` 登记）：见 RelationLoader 的说明
        std::vector<RelationLoader> m_relationLoaders;

        /// 一次 `include()` 的取数侧处理器：JOIN 的投影怎么拆、拆出来的行挂到哪个成员
        struct IncludeProcessor
        {
            /// 每行调用一次：把带前缀的关联列拆成一个对象挂到行上。
            /// resolvedIndices 是本次取数的下标槽（首次调用时解析并填充）——槽位由 fetchRowsOn 每次执行现造，
            /// 因此上一次执行的列下标不会被带进下一次（列结构变更后仍按名重解）
            std::function<void(T &row, DatabaseResult &result, std::vector<std::size_t> &resolvedIndices)> attach;
            /// 一对多折叠：把重复父行上挂到的子行并进首次出现的那一行（非折叠方向为空）
            std::function<void(T &first, T &duplicate)> absorbDuplicate;
            bool                                        foldsParentRows = false; ///< 该 include 是否会把父行乘开（一对多）
        };

        /// 本次查询用 `include()` 装载的关系（顺序与别名前缀 r0、r1… 一致）
        std::vector<IncludeProcessor> m_includeProcessors;

        /**
         * @brief 造一条关系装载器
         * @details 在这里（`then()` 被调用时）查一次「这条关系声明过没有」：查不到当场抛，
         *          而不是等到装载时才发现少了一个成员
         * @param memberPointer 目标成员
         * @param pool 连接池（可为空：只有事务时走事务）
         * @param transaction 绑定的事务（可为空）
         * @param dialect 目标引擎的方言（定补取语句的参数上限）
         * @return RelationLoader 装载器
         * @throws Base::LogicException TableSchema<T>::kRelations 里没有这个成员的声明
         */
        template<typename MemberType>
        [[nodiscard]] static RelationLoader makeRelationLoader(const MemberType T::*memberPointer, ConnectionPool *pool, Transaction *transaction,
                                                               std::shared_ptr<SqlDialect> dialect);

        /**
         * @brief 在 TableSchema<T>::kRelations 里按成员指针取那条声明
         * @param memberPointer 目标成员
         * @return 声明指针；未声明时为空
         */
        template<typename MemberType>
        [[nodiscard]] static const RelationDescriptor<T, MemberType> *findRelationOf(const MemberType T::*memberPointer);

        /**
         * @brief 判断某一格列名是否声明在行类型的 kColumns 里
         * @tparam RowType 行类型
         * @param columnName 列名
         * @return bool 声明过即真
         */
        template<typename RowType>
        [[nodiscard]] static constexpr bool columnIsDeclared(const std::string_view columnName);

        /**
         * @brief 取某一格列名在 kColumns 里的位置（不存在时返回列数）
         * @tparam RowType 行类型
         * @param columnName 列名
         * @return std::size_t 位置；未声明时为 kColumns 的列数
         */
        template<typename RowType, std::size_t... IndexPositions>
        [[nodiscard]] static consteval std::size_t findColumnPosition(const std::string_view columnName, std::index_sequence<IndexPositions...>);

        /**
         * @brief 给 include 的关系加一条 LEFT JOIN
         * @details 连接条件按方向取列：多对一（外键在本表）用「关联表主键 = 本表外键」，
         *          一对多（外键在对方表）用「关联表外键 = 本表主键」。本表一侧用表名的最后一段
         *          引用（`appendFieldReference` 逐段引用），因此与查询树自己的写法一致
         * @tparam RelatedRow 关联行类型
         * @tparam MemberType 目标成员类型
         * @param relation 关系声明
         * @param prefix 关联表别名（r0、r1…）
         * @param isCollection 是否一对多
         */
        template<typename RelatedRow, typename MemberType>
        void appendIncludeJoin(const RelationDescriptor<T, MemberType> &relation, const std::string_view prefix, const bool isCollection);

        /**
         * @brief 把关联行的全部列按「前缀__列名」加进 SELECT 投影
         * @tparam RelatedRow 关联行类型
         * @param prefix 别名前缀（r0、r1…）
         */
        template<typename RelatedRow>
        void appendIncludeProjection(const std::string_view prefix);

        /**
         * @brief 造一个 include 的取数侧处理器（拆投影、挂成员、折叠）
         * @tparam RelatedRow 关联行类型
         * @tparam MemberType 目标成员类型
         * @param memberPointer 目标成员
         * @param prefix 别名前缀（闭包按值持有一份字符串，不指向调用方的临时量）
         * @return IncludeProcessor 处理器
         */
        template<typename RelatedRow, typename MemberType>
        [[nodiscard]] static IncludeProcessor makeIncludeProcessor(MemberType T::*memberPointer, const std::string_view prefix);

        /**
         * @brief 把行键渲染成可比较的索引文本
         * @details `DatabaseValue` 的备选里有无序哈希表（没有 `<`），整个 variant 因此不可比较，
         *          不能直接当 `std::map` 的键。这里按「备选下标 + 取值文本」拼一个：下标保证
         *          「不同备选里的同一个文本」不会撞（例如整数 1 与文本 "1"）
         * @param key 键值（关系两端的主键/外键，都是标量）
         * @return std::string 索引键
         */
        [[nodiscard]] static std::string indexKeyOf(const DatabaseValue &key);

        /**
         * @brief 严格标识符模式的字符白名单校验
         * @param text 待校验文本
         * @param fieldDescription 出错时点名的字段描述（含 API 名与序号）
         * @throws Base::InvalidArgumentException 出现白名单外的字符
         */
        static void requireStrictIdentifierText(const std::string_view text, const std::string &fieldDescription);

        /**
         * @brief 递归校验条件树里的字段引用（参数值不校验：它们走绑定）
         */
        static void requireStrictWhereCondition(const WhereCondition &condition, const std::string &context);

        /**
         * @brief 从一行结构体里按列名取值
         * @details 借的是 kColumns 的元组：列名对得上就取那个成员的值——关系的两端（外键与主键）
         *          因此必须在各自的 TableSchema 里声明过，这是本层的契约
         * @tparam RowType 行类型
         * @param row 已映射的行
         * @param columnName 列名
         * @return std::optional<DatabaseValue> 取值；该行类型没有这一列时为空
         */
        template<typename RowType>
        [[nodiscard]] static std::optional<DatabaseValue> readColumnValue(const RowType &row, const std::string_view columnName);

        /**
         * @brief 多对一：按本表外键列的取值补取对端行，再逐个挂上去
         * @tparam RelatedRow 对端行类型
         * @tparam MemberType 目标成员类型（`RelatedRow` 或 `std::optional<RelatedRow>`）
         * @param relation 关系声明（外键列在本表）
         * @param dialect 方言
         * @param pool 连接池
         * @param transaction 事务（可为空）
         * @param rows 待装载的行
         */
        template<typename RelatedRow, typename MemberType>
        static void loadManyToOne(const RelationDescriptor<T, MemberType> &relation, const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction,
                                  std::vector<T> &rows);

        /**
         * @brief 一对多：按本行主键补取子行，按外键分组挂上
         * @tparam RelatedRow 子行类型
         * @tparam MemberType 目标成员类型（`std::vector<RelatedRow>`）
         * @param relation 关系声明（外键列在子表）
         * @param dialect 方言
         * @param pool 连接池
         * @param transaction 事务（可为空）
         * @param rows 待装载的行
         */
        template<typename RelatedRow, typename MemberType>
        static void loadOneToMany(const RelationDescriptor<T, MemberType> &relation, const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction,
                                  std::vector<T> &rows);

        /**
         * @brief 按 `列 IN (…)` 补取对端行，按方言参数上限分块
         * @details 分块的理由与 insertBatch 同源：一条语句的参数个数有上限而键可能很多。
         *          块之间不再排序（装载结果按分组建索引，与顺序无关）
         * @tparam RelatedRow 对端行类型
         * @param dialect 方言（给出参数上限）
         * @param pool 连接池（可为空：只有事务时走事务）
         * @param transaction 事务（可为空）
         * @param columnName IN 的列名（多对一时是对方主键列，一对多时是对方外键列）
         * @param keys 键集合（重复不影响结果）
         * @return std::vector<RelatedRow> 取回的对端行
         */
        template<typename RelatedRow>
        [[nodiscard]] static std::vector<RelatedRow> fetchRelatedByKeyIn(const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction,
                                                                         const std::string_view columnName, const std::span<const DatabaseValue> keys);

        /**
         * @brief 跑一遍已登记的关系装载器
         * @param rows 待装载的行（就地修改）
         */
        void runRelationLoaders(std::vector<T> &rows) const;

        /**
         * @brief 本次查询是否带「会把父行乘开」的 include（一对多）
         * @return bool 带折叠 include 即真
         */
        [[nodiscard]] bool hasFoldingInclude() const;

        /**
         * @brief 聚合方向不接受 include：整段 SELECT 列表被 COUNT(*) 顶掉，JOIN 还会把一对多的行乘开
         * @param methodName 调用方名字（count() / countAsync()），文案点名它
         * @throws Base::LogicException 本次查询带 include
         */
        void requireNoIncludesForAggregate(const char *methodName) const;


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
        void requireOnline(const std::string_view operationName) const;

        /**
         * @brief 取得本查询要使用的方言，首次调用时解析并缓存（返回共享指针）
         * @details 方言类型优先用构造时显式指定的值；未指定时从实际要用的连接读取其真实 DatabaseType：
         *          绑定事务时直接问事务连接（零成本且必然准确），绑定池时才借一条连接探测，读完立刻归还。
         *          返回共享指针而不是引用，是为了让异步路径能把方言按值捕获进工作线程的任务：
         *          工作线程不得再触碰本对象，而引用无法脱离本对象的生命周期独立存在。
         * @return std::shared_ptr<SqlDialect> 方言实例，恒非空
         * @throws DatabaseException 无法从池中取得连接以推导类型
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（MySQL / Redis）
         * @warning 必须在持有租约之前调用：既未显式指定方言类型也未绑定事务时，它要自己再借一条
         *          连接去探测类型。调用方若已经占着池里唯一的连接（maximumPoolSize 为 1 是文件型
         *          SQLite 的常见配法），这次嵌套借用会白等满 acquireTimeoutMilliseconds，然后抛
         *          「无法获取连接以推导类型」——一条离真实原因很远的错误。各执行入口都按
         *          「先 requireDialect()、后 acquireConnection()」的顺序写，改动时请保持。
         */
        [[nodiscard]] std::shared_ptr<SqlDialect> resolveDialect();

        /**
         * @brief 取得本查询要使用的方言引用
         * @details 同步路径的便捷入口：内部就是 resolveDialect() 的解引用，
         *          缓存的共享指针由本对象长期持有，引用在对象存活期间始终有效。
         * @return const SqlDialect& 方言实例引用
         * @throws DatabaseException 无法从池中取得连接以推导类型
         * @throws Base::InvalidArgumentException 该数据库类型尚无方言实现（MySQL / Redis）
         */
        [[nodiscard]] const SqlDialect &requireDialect();

        /**
         * @brief 取得本查询异步执行时要用的执行器
         * @details 未注入时使用进程级共享实例：零配置即可用，且多个 Queryable 共享同一组
         *          工作线程，不会因为「每个查询各建一个执行器」而把线程数乘起来。
         * @return Core::AsyncExecutor& 执行器引用
         */
        [[nodiscard]] Core::AsyncExecutor &asyncExecutor() const;

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
         * @throws TransactionTimeoutException 绑定了事务且整笔时限已到期——此时一条语句都不发，
         *         也不去排那把连接使用权锁；排队等锁本身可能等过时限，取到使用权后还会复查一次，
         *         复查不过同样在此拒绝（语句尚未发出）
         */
        [[nodiscard]] static ConnectionLease acquireConnection(ConnectionPool *pool, const Transaction *transaction);

        /**
         * @brief 生成真正交给数据库执行的查询树副本
         * @details 与 m_queryNode 的唯一差别是：SELECT 列为空时按 TableSchema<T>::kColumns
         *          的声明顺序展开成显式列名。方言层不认识模板参数，只能把空列列表翻译成 '*'
         *          （顺序由数据库决定）；ORM 层知道结构体，因此在这里补上顺序。
         * @return QueryNode 展开列之后的查询树副本
         */
        [[nodiscard]] QueryNode resolvedQueryNode() const;

        /**
         * @brief 判断一串列名文本是不是「纯列名」（只有标识符字节、不含点号与运算符）
         * @param columnText 待判断的文本
         * @return bool 是纯列名即真（表达式与已限定的名字都判否）
         */
        [[nodiscard]] static bool isPlainColumnName(const std::string_view columnText);

        /**
         * @brief 带 include 时把基表列限定成「表名.列名」
         * @details JOIN 一旦把两张表放进同一条语句，裸列名在同名列（几乎一定有的 `id`）上就是
         *          歧义，服务端直接报错。因此带 include 的查询里：纯列名一律限定到基表，
         *          表达式（`COUNT(*)` 之类）与已经带点的名字原样保留。条件树递归处理左右两侧，
         *          IN 的值列表不动（那是绑定的参数，不是列名）
         * @param node 待限定的查询树（就地修改）
         */
        static void qualifyBaseColumnsForIncludes(QueryNode &node);

        /**
         * @brief 执行一次 SELECT 并把结果映射成结构体列表（同步路径）
         * @param queryNode 已展开列的查询树
         * @return std::vector<T> 映射后的行列表
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         */
        [[nodiscard]] std::vector<T> fetchRows(const QueryNode &queryNode);

        /**
         * @brief 执行一次 SELECT 并只映射出第一行（同步路径）
         * @param queryNode 已展开列的查询树
         * @return std::optional<T> 第一行；没有匹配行时为空值
         * @throws DatabaseException 取连接失败、SQL 执行失败或行映射失败
         */
        [[nodiscard]] std::optional<T> fetchFirst(const QueryNode &queryNode);

        /**
         * @brief 在指定连接上执行 SELECT 并映射结果（同步与异步路径共用）
         * @details 与「用哪条连接、在哪个线程执行」无关：调用方负责提供一条可用的连接
         *          （同步路径在调用线程上取，异步路径在工作线程上取），本函数只做翻译、执行与映射。
         * @param connection 目标连接，必须在结果集存活期间保持有效
         * @param dialect 方言，提供 translate()
         * @param queryNode 已展开列的查询树
         * @param includes 本次查询的 include 处理器（为空即普通映射路径，一字不改）
         * @return std::vector<T> 映射后的行列表
         * @throws DatabaseException SQL 执行失败或行映射失败
         * @throws Base::LogicException 一对多的 include 与 LIMIT / OFFSET 同时出现（分页数的是结果行，不是父行数）
         * @note SQLite 的结果集持有连接句柄的非拥有指针，因此 connection 必须比结果集活得久：
         *       这一点由调用方持有连接租约、且租约比本函数返回值活得更久来保证
         */
        [[nodiscard]] static std::vector<T> fetchRowsOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode,
                                                        const std::span<const IncludeProcessor> includes = {});

        /**
         * @brief 校验 include 与分页的组合是合法的（在翻译与执行之前判，问题与数据库无关）
         * @param queryNode 查询树（读分页信息）
         * @param includes 本次查询的 include 处理器
         * @throws Base::LogicException 一对多的 include 与 LIMIT / OFFSET 同时出现
         */
        static void requireCompatibleIncludes(const QueryNode &queryNode, const std::span<const IncludeProcessor> includes);

        /**
         * @brief 带 include 的逐行映射：映射基表行之后把带前缀的关联列拆出来挂上，再折叠一对多
         * @details 与 mapResultRows 同一纪律：基表列下标只在第一行解析一次并复用、空结果集不解析列；
         *          include 各自的下标槽由本次调用现造（见 IncludeProcessor::attach）
         * @param result 结果集，会被推进
         * @param includes 本次查询的 include 处理器
         * @return std::vector<T> 映射（并折叠）后的行列表
         */
        [[nodiscard]] static std::vector<T> mapRowsWithIncludes(DatabaseResult &result, const std::span<const IncludeProcessor> includes);

        /**
         * @brief 把一对多 include 乘开的重复父行折叠回一行
         * @details 按基表主键认「同一个父行」：首次出现的行留在结果里并占住位置，后续重复行上的
         *          子行按结果行顺序并进首次出现的那一行，重复行被丢弃。父行的先后按首次出现定，
         *          因此调用方的 ORDER BY 仍然说了算
         * @param rows 待折叠的行列表（就地修改）
         * @param includes 本次查询的 include 处理器（折叠只走带 absorbDuplicate 的那些）
         * @throws Base::LogicException 基表主键列不在结果集里（include() 已在声明阶段挡过，这里兜住「行类型被换过」）
         */
        static void foldIncludedParentRows(std::vector<T> &rows, const std::span<const IncludeProcessor> includes);

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
        [[nodiscard]] static std::optional<T> fetchFirstOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode);

        /**
         * @brief 在指定连接上执行 COUNT(*) 查询并解读结果（同步与异步路径共用）
         * @param connection 目标连接
         * @param dialect 方言，提供 translate()
         * @param countingNode 已把 SELECT 改成 COUNT(*)、并清掉排序与分页的查询树
         * @return std::int64_t 匹配行数；结果集没有计数行时为 0
         * @throws QueryExecutionException SQL 执行失败
         * @throws RowMappingException 计数列不是整数（语句被改坏或列位置漂移）
         */
        [[nodiscard]] static std::int64_t countOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &countingNode);

        /**
         * @brief 执行一条写语句并返回受影响行数（连接由内部按当前模式决定）
         * @param statement 待执行的参数化语句
         * @return std::int64_t 受影响行数；驱动不提供该信息时为 0
         * @throws DatabaseException 取连接失败或语句执行失败
         */
        [[nodiscard]] std::int64_t executeStatement(const SqlStatement &statement);

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
        [[nodiscard]] static std::int64_t executeOn(DatabaseConnection &connection, const SqlStatement &statement);

        /**
         * @brief 在指定连接上执行一条写语句并把结果集交给调用方
         * @details 与 executeOn() 共用同一份「执行 + 判失败 + 抛异常」，区别只是把结果集原样交出
         *          而不是当场折算成影响行数：自增标识这类同样挂在写回执上的语句级信息要走这条路，
         *          ORM 侧仍然不向下转型到具体驱动。
         * @param connection 目标连接，生命周期由调用方保证
         * @param statement 待执行的参数化语句
         * @return std::unique_ptr<DatabaseResult> 结果集；写语句是 0 行 0 列的写回执
         * @throws QueryExecutionException 语句执行失败，原因见连接的错误文本
         */
        [[nodiscard]] static std::unique_ptr<DatabaseResult> runOn(DatabaseConnection &connection, const SqlStatement &statement);

        /**
         * @brief 生成 INSERT 语句
         * @details ORM 只负责把结构体整理成「列名 + 取值」两个等长的序列：列名放进查询树的
         *          selectColumns，取值转成 DatabaseValue 后按同序排列，SQL 文本、标识符引用、
         *          占位符写法全部由方言的 translateInsert() 决定，本类不拼任何 SQL 片段。
         * @param row 待插入的结构体
         * @param conflict 唯一键冲突的处置，默认 Fail 时关键词与既有写法逐字相同
         * @param withGeneratedKey 是否要求把生成的主键一起交回：为真时把 TableSchema 的主键写进查询树，
         *        回读通道由方言决定（SQLite/MySQL 读连接句柄上的快照、语句一分不变；PostgreSQL 追加
         *        RETURNING 主键列）。为假时渲染结果与加这一格之前逐字相同
         * @return SqlStatement "INSERT INTO 表 (列…) VALUES (?, …)"；声明为自增的主键不出现在列清单里，
         *         要求回读生成键时在 PostgreSQL 上末尾多出 RETURNING 子句
         */
        [[nodiscard]] SqlStatement buildInsertStatement(const T &row, const InsertConflict conflict = InsertConflict::Fail, const bool withGeneratedKey = false);

        /**
         * @brief 生成按主键更新一行的 UPDATE 语句，并在启用乐观锁时顺带给出冲突文案
         * @details SET 子句覆盖除主键外的全部列，WHERE 子句用主键定位，主键值同样以参数绑定送出。
         *          条件渲染交回方言：ORM 只提供「SET 列 + 取值」与一条主键等值条件，
         *          条件树的递归、IN 展开、参数顺序因此与 SELECT / DELETE 完全一致。
         *          `TableSchema<T>::kVersionColumn` 非空时（乐观锁）多两件事：WHERE 追加
         *          「版本 = ?」（问的是手里这份数据的版本），SET 里把该列写成「自己加一」而不是取结构体的值——
         *          加一由服务端在一次语句里完成，因此调用方忘不掉、两个会话也不可能都成功。
         * @param row 待更新的结构体
         * @return std::pair<SqlStatement, std::string> 语句；第二项是冲突文案（含表、版本列、期望版本与主键值），
         *         未启用乐观锁时为空串——调用方据此决定「0 行」要不要当冲突抛
         * @throws Base::LogicException 主键未在 kColumns 中声明；表里既没有可更新的列也没有版本列；
         *         版本列与主键同列；或声明了版本列但它不在 kColumns 里
         */
        [[nodiscard]] std::pair<SqlStatement, std::string> buildUpdateStatement(const T &row);

        /**
         * @brief 启用乐观锁时，UPDATE 命中 0 行就是并发冲突，当场抛
         * @details 「0 行」在关掉乐观锁时含义含糊（SQLite 把「写回原值」也算命中，MySQL 只算真变了的行，
         *          见 update() 的 @warning），但在启用版本列之后它是确定的：WHERE 里带着「版本 = 手里的值」，
         *          0 行只可能是「这一行被别人改过（版本已经不是这个数）或被删掉了」。因此同步与异步两条
         *          路径共用这一份判据，口径不会各说各话。
         * @param conflictReason 语句生成阶段留下的冲突文案；未启用乐观锁时为空串（此时 0 行照旧不抛）
         * @param affectedRows 驱动口径的受影响行数
         * @throws OptimisticLockException conflictReason 非空且受影响行数为 0 时
         */
        static void requireNoOptimisticLockConflict(const std::string &conflictReason, const std::int64_t affectedRows);

        /**
         * @brief 构建一个只在写方向使用的查询树
         * @details 只填表名：写语句不涉及 JOIN / 排序 / 分页，也不应继承 m_queryNode 上
         *          调用方为查询设置的条件（例如 query.where(...).insert(row) 里的条件
         *          对 INSERT 毫无意义，带进写语句只会误导）。
         * @return QueryNode 空的写查询树
         */
        [[nodiscard]] static QueryNode makeWriteQueryNode();

        /**
         * @brief 取 TableSchema<T> 声明的全部列名，顺序与 kColumns 一致
         * @return std::vector<std::string> 列名列表
         */
        [[nodiscard]] static std::vector<std::string> allColumnNames();

        /**
         * @brief 取 INSERT 要写的列名：全部列减去「由数据库生成的自增主键」
         * @details 自增主键不能出现在列清单里：显式给值会占掉号段（MySQL 甚至直接把它当业务值存下），
         *          与「让引擎生成」这个声明自相矛盾。SELECT 侧不受影响——读回来时那一列是有值的，
         *          仍须出现在结果映射的列清单里，因此这里只服务写入路径。
         * @return std::vector<std::string> 与 kColumns 同序的待写列名
         * @throws Base::LogicException 声明了自增主键却没有任何其它列可写（此时 INSERT 无列可生成）
         */
        [[nodiscard]] static std::vector<std::string> insertColumnNames();

        /**
         * @brief 把一行里要写入的字段转成绑定参数，顺序与 insertColumnNames() 一致
         * @details 与列名走的是同一个判定（同一个自增列被跳过），两个列表因此按构造对齐。
         * @param row 待转换的结构体
         * @return std::vector<DatabaseValue> 与待写列一一对应的取值列表
         */
        [[nodiscard]] static std::vector<DatabaseValue> insertValuesOf(const T &row);

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
         * @throws TransactionTimeoutException 绑定了事务且整笔时限已到期（分块与非分块两条路都在发语句前判）
         */
        [[nodiscard]] static std::int64_t insertBatchOn(ConnectionPool *pool, const Transaction *transaction, const SqlDialect &dialect, const std::span<const T> rows,
                                                        const InsertConflict conflict = InsertConflict::Fail);

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
        [[nodiscard]] static std::int64_t executeBatchOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &batchNode, const std::span<const T> rows,
                                                         const std::size_t rowsPerStatement);

        /**
         * @brief 把 UPDATE 的一列拆进 SET、WHERE 或「自增加一」
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param row 提供字段值的结构体
         * @param primaryKeyName 主键列名，命中该列的成员不进 SET
         * @param versionColumnName 乐观锁版本列名；为空表示未启用。命中该列的成员同样不进 SET
         *        （它走自增加一那一格），其取值改作 WHERE 里的版本条件
         * @param assignmentColumns 出参：进入 SET 的列名列表，按 kColumns 顺序
         * @param assignmentValues 出参：与 assignmentColumns 同序的取值列表
         * @param incrementColumns 出参：走「自己加一」的列名列表（乐观锁的版本列）
         * @param primaryKeyCondition 出参：主键等值条件；未找到主键列时保持空
         * @param versionCondition 出参：版本等值条件；未启用乐观锁或未找到版本列时保持空
         * @param versionExpectationText 出参：版本取值的可读文本，供冲突消息点名「期望值是多少」
         * @param primaryKeyValueText 出参：主键取值的可读文本，供冲突消息点名「是哪一行」
         */
        template<typename ColumnDescriptorType>
        static void appendUpdateColumn(const ColumnDescriptorType &columnDescriptor, const T &row, const std::string_view primaryKeyName, const std::string_view versionColumnName,
                                       std::vector<std::string> &assignmentColumns, std::vector<DatabaseValue> &assignmentValues, std::vector<std::string> &incrementColumns,
                                       std::optional<WhereCondition> &primaryKeyCondition, std::optional<WhereCondition> &versionCondition, std::string &versionExpectationText,
                                       std::string &primaryKeyValueText);

        /**
         * @brief 把一个取值渲染成异常消息里可读的一小段
         * @details 只服务于乐观锁冲突那一条消息（点名「期望版本是多少」），因此口径取「看得懂」而不是
         *          「可再解析」：文本原样、整数与浮点按通用写法、布尔写成 true/false；复合类型只报规模，
         *          把它们整个铺开会让一条异常消息长到没人读
         * @param value 待渲染的取值
         * @return std::string 可读文本
         */
        [[nodiscard]] static std::string describeValueForConflictMessage(const DatabaseValue &value);

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
        [[nodiscard]] static ParameterValue makeParameterValue(const MemberType &value);

        // ========================================================================
        // SQL 生成实现
        // ========================================================================

        /**
         * @brief 构建 SELECT SQL 语句
         * @return std::string 完整的 SELECT 语句
         * @note 带 include 的查询走已解析的查询树（基表列已限定、别名投影会列出）：这份调试文本
         *       才反映真正执行的形状；不带 include 时行为与之前逐字相同
         */
        [[nodiscard]] std::string buildSelectSql() const;

        /**
         * @brief 构建单个条件的 SQL 字符串
         * @param condition 待转换的条件
         * @return std::string 条件对应的 SQL 片段
         */
        [[nodiscard]] static std::string buildConditionString(const WhereCondition &condition);

        /**
         * @brief 将 SqlOperator 转为 SQL 操作符字符串
         */
        [[nodiscard]] static std::string_view operatorToString(const SqlOperator op);

        /**
         * @brief 将 JoinType 转为 SQL 连接类型字符串
         */
        [[nodiscard]] static std::string_view joinTypeToString(const JoinType type);

        /**
         * @brief 将 FieldReference 转为字符串
         */
        [[nodiscard]] static const std::string &fieldReferenceToString(const FieldReference &field);

        /**
         * @brief 将 OrderByClause 转为 SQL 排序字符串
         */
        [[nodiscard]] static std::string orderByToString(const OrderByClause &order);

        // ========================================================================
        // 数据成员
        // ========================================================================

        QueryNode                   m_queryNode;               ///< 查询树节点，存储所有查询构建信息
        ConnectionPool             *m_pool        = nullptr;   ///< 数据库连接池指针，离线模式或绑定事务时为 nullptr
        Transaction                *m_transaction = nullptr;   ///< 事务指针；非空时全部语句走事务持有的连接
        std::optional<DatabaseType> m_databaseType;            ///< 构造时显式指定的数据库类型；未指定时从连接推导
        std::shared_ptr<SqlDialect> m_dialect;                 ///< 缓存的方言实例，首次执行时解析并长期持有
        Core::AsyncExecutor        *m_asyncExecutor = nullptr; ///< 注入的异步执行器；为空时用进程级共享实例
    };

} // namespace AsynGyanis::Database::Queryable

#include "Database/Queryable/QueryableImpl.h"
