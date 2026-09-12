/**
 * @file TestPostgresDialect.cpp
 * @brief PostgreSQL 方言翻译单元测试（不需要数据库连接、不需要 libpq）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 只验证「查询树 → 参数化 SQL」的纯文本翻译结果与参数收集顺序，
 *          不打开任何连接，也不经过任何驱动。与 TestSqliteDialect.cpp / TestMySqlDialect.cpp
 *          同口径覆盖 PostgresDialect 的引擎知识；凡与那两个方言共用的渲染行为
 *          （条件递归、空 IN 集合、表达式列、写语句校验等）已由 StandardSqlDialect
 *          的测试口径覆盖，本文件只覆盖 PostgreSQL 独有的差异点，并顺带验证
 *          「基类的参数顺序契约在 $n 方言上真正生效」这一条。
 *
 * 覆盖场景：
 * - 方言类型、占位符为 $1/$2（从 1 开始、随参数个数连续递增）、参数上限 65535
 * - 标识符双引号引用与内部双引号翻倍（与 SQLite 相同、与 MySQL 反引号不同）
 * - SELECT 列展开、FROM 与表别名（引用字符为双引号）
 * - WHERE 条件中的 $n 序号：单条件、IN 展开、多条件组合
 * - UPDATE 的 SET 参数排在 WHERE 参数之前（$1 在前、条件序号顺延）
 * - LIMIT $n / OFFSET $n，以及**只给 OFFSET 时不补任何常量**（与 SQLite 的 "LIMIT -1"、
 *   MySQL 的无符号上界常量形成对照）
 * - 综合场景下 $1..$n 的连续性与占位符个数 = 参数个数
 * - 事务控制语句：BEGIN / COMMIT / ROLLBACK
 * - DDL 支撑：六个 ColumnType 的物理类型名（含 NUMERIC(20) 承接无符号 64 位）
 *   与表存在性元数据语句（current_schema() 限定 + 表名走 $1 绑定参数）
 * - 错误文本带「PostgreSQL 方言」前缀（dialectName() 的用途）
 * - DialectRegistry：PostgreSQL 可取得、与 SQLite / MySQL 是不同实例、supports() 为真
 */
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/PostgresDialect.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace
{
    using AsynGyanis::Database::ColumnType;
    using AsynGyanis::Database::DatabaseType;
    using AsynGyanis::Database::DatabaseValue;
    using AsynGyanis::Database::DialectRegistry;
    using AsynGyanis::Database::PostgresDialect;
    using AsynGyanis::Database::SqlDialect;
    using AsynGyanis::Database::SqlStatement;
    using AsynGyanis::Database::Queryable::FieldReference;
    using AsynGyanis::Database::Queryable::JoinClause;
    using AsynGyanis::Database::Queryable::JoinType;
    using AsynGyanis::Database::Queryable::OrderByClause;
    using AsynGyanis::Database::Queryable::ParameterValue;
    using AsynGyanis::Database::Queryable::QueryNode;
    using AsynGyanis::Database::Queryable::SqlOperator;
    using AsynGyanis::Database::Queryable::WhereCondition;

    /**
     * @brief 统计 SQL 文本里的 PostgreSQL 占位符个数
     * @details PostgreSQL 的占位符是 "$1" "$2" 这样的文本，不能像 SQLite / MySQL 那样数 '?'：
     *          这里数「'$' 后面紧跟一个十进制数字」的出现次数。$ 后面不带数字的文本
     *          （现实中不会由本方言生成）不计入，避免把普通文本误判成占位符。
     * @param sql 待统计的 SQL 文本
     * @return std::size_t 形如 $n 的占位符出现次数
     */
    std::size_t countPlaceholders(const std::string &sql)
    {
        std::size_t count = 0;
        for (std::size_t index = 0; index + 1 < sql.size(); ++index)
        {
            if (sql[index] == '$' && sql[index + 1] >= '0' && sql[index + 1] <= '9')
            {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief 构造字段引用
     * @param name 列名或表达式文本
     * @return FieldReference 字段引用
     */
    FieldReference makeField(const std::string &name)
    {
        return FieldReference{.name = name};
    }

    /**
     * @brief 构造 列 与 参数值 的比较条件
     * @param columnName 列名
     * @param sqlOperator 比较操作符
     * @param value 右操作数参数值
     * @return WhereCondition 条件节点
     */
    WhereCondition makeComparison(const std::string &columnName, const SqlOperator sqlOperator, const ParameterValue &value)
    {
        return WhereCondition{
            .left  = makeField(columnName),
            .op    = sqlOperator,
            .right = value
        };
    }

    /**
     * @brief 构造 列 与 列 的比较条件（右操作数是字段引用）
     * @param columnName 左列名
     * @param sqlOperator 比较操作符
     * @param rightColumnName 右列名
     * @return WhereCondition 条件节点
     */
    WhereCondition makeColumnComparison(const std::string &columnName,
                                        const SqlOperator sqlOperator,
                                        const std::string &rightColumnName)
    {
        return WhereCondition{
            .left  = makeField(columnName),
            .op    = sqlOperator,
            .right = makeField(rightColumnName)
        };
    }

    /**
     * @brief 构造由 children 组成的复合条件
     * @param sqlOperator And / Or / Not
     * @param children 子条件列表
     * @return WhereCondition 复合条件节点
     */
    WhereCondition makeComposite(const SqlOperator sqlOperator, std::vector<WhereCondition> children)
    {
        WhereCondition condition;
        condition.op       = sqlOperator;
        condition.right    = ParameterValue{nullptr};
        condition.children = std::move(children);
        return condition;
    }

    /**
     * @brief 构造 IN / NOT IN 条件
     * @param columnName 列名
     * @param sqlOperator In 或 NotIn
     * @param values 值集合
     * @return WhereCondition 条件节点
     */
    WhereCondition makeInCondition(const std::string &columnName,
                                   const SqlOperator sqlOperator,
                                   std::vector<ParameterValue> values)
    {
        WhereCondition condition;
        condition.left     = makeField(columnName);
        condition.op       = sqlOperator;
        condition.right    = ParameterValue{static_cast<std::int64_t>(0)};
        condition.inValues = std::move(values);
        return condition;
    }

    /**
     * @brief 执行一个必然抛 std::invalid_argument 的翻译并取回异常文本
     * @param action 待执行的翻译动作
     * @return std::string 异常文本；若没有抛出则返回空串
     */
    std::string captureInvalidArgument(const std::function<void()> &action)
    {
        try
        {
            action();
        }
        catch (const std::invalid_argument &error)
        {
            return error.what();
        }

        return {};
    }

} // namespace

// ========================================================================
// 标识符引用与占位符
// ========================================================================

/**
 * @brief 验证标识符被双引号包裹，保留字同样安全
 */
TEST(PostgresDialectIdentifier, QuoteIdentifierWrapsWithDoubleQuotes)
{
    const PostgresDialect dialect;

    // PostgreSQL 与 SQLite 同用 SQL 标准的双引号，MySQL 才是反引号
    EXPECT_EQ(dialect.quoteIdentifier("name"), "\"name\"");
    EXPECT_EQ(dialect.quoteIdentifier("order"), "\"order\"");
    EXPECT_EQ(dialect.quoteIdentifier("group"), "\"group\"");
}

/**
 * @brief 验证标识符内部的双引号被翻倍转义
 */
TEST(PostgresDialectIdentifier, QuoteIdentifierDoublesEmbeddedQuote)
{
    const PostgresDialect dialect;

    // PostgreSQL 用「引号翻倍」表示标识符内部的引号；反斜杠在这里只是普通字符
    EXPECT_EQ(dialect.quoteIdentifier("weird\"name"), "\"weird\"\"name\"");
    EXPECT_EQ(dialect.quoteIdentifier("a\"b\"c"), "\"a\"\"b\"\"c\"");
    EXPECT_EQ(dialect.quoteIdentifier(""), "\"\"");
}

/**
 * @brief 验证占位符是 $1 / $2（从 1 开始，随序号递增）
 */
TEST(PostgresDialectIdentifier, PlaceholderIsDollarNumberedFromOne)
{
    const PostgresDialect dialect;

    // 这是三种引擎里唯一真正使用 placeholder() 序号的实现：
    // 基类传入的 index 是 parameters 的下标（从 0 开始），文本里必须加一
    EXPECT_EQ(dialect.placeholder(0), "$1");
    EXPECT_EQ(dialect.placeholder(1), "$2");
    EXPECT_EQ(dialect.placeholder(9), "$10");
    EXPECT_EQ(dialect.placeholder(65534), "$65535");
}

/**
 * @brief 验证方言类型、分页能力与参数上限
 */
TEST(PostgresDialectIdentifier, DialectMetadataAndParameterLimit)
{
    const PostgresDialect dialect;

    EXPECT_EQ(dialect.type(), DatabaseType::PostgreSql);
    EXPECT_TRUE(dialect.supportsLimitOffset());

    // 上限来自扩展查询协议 Bind 报文中 Int16 的参数个数上界，与 SQLite 的 999 是两回事
    EXPECT_EQ(dialect.maximumStatementParameters(), PostgresDialect::kMaximumStatementParameters);
    EXPECT_EQ(dialect.maximumStatementParameters(), 65535U);
}

// ========================================================================
// SELECT 列与 FROM
// ========================================================================

/**
 * @brief 验证 selectColumns 为空时退化为通配符
 */
TEST(PostgresDialectSelect, EmptySelectColumnsFallsBackToWildcard)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\"");
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证显式列按给定顺序加双引号输出
 */
TEST(PostgresDialectSelect, ExplicitColumnsAreQuotedInOrder)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name", "age"};

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT \"id\", \"name\", \"age\" FROM \"users\"");
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证表达式列原样输出而不被加引号
 */
TEST(PostgresDialectSelect, ExpressionColumnIsPassedThrough)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"COUNT(*)", "COALESCE(age, 0)"};

    // 加双引号会把函数调用降级成列名，因此必须原样输出
    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT COUNT(*), COALESCE(age, 0) FROM \"users\"");
}

/**
 * @brief 验证限定名逐段加双引号，通配符段保持裸写
 */
TEST(PostgresDialectSelect, QualifiedNameQuotesEachSegment)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"users.id", "users.*"};

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT \"users\".\"id\", \"users\".* FROM \"users\"");
}

/**
 * @brief 验证表别名被引用（PostgreSQL 会把未引用的标识符折成小写，别名必须引用）
 */
TEST(PostgresDialectSelect, TableAliasIsQuoted)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName  = "users";
    node.tableAlias = "u";

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" AS \"u\"");
}

// ========================================================================
// WHERE：$n 序号与条件渲染
// ========================================================================

/**
 * @brief 验证单条件比较产生 $1 与对应参数
 */
TEST(PostgresDialectWhere, SingleComparisonUsesFirstPlaceholder)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"age\" >= $1");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 18);
}

/**
 * @brief 验证字符串参数按原文本绑定，SQL 文本里不出现数据
 */
TEST(PostgresDialectWhere, StringParameterIsBoundWithoutQuoting)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("name", SqlOperator::Eq, ParameterValue{std::string("O'Brien -- DROP")}));

    const SqlStatement statement = dialect.translate(node);

    // 数据一律走 $n 绑定，含单引号与注释符的取值不改变语句结构
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"name\" = $1");
    EXPECT_EQ(statement.sql.find("O'Brien"), std::string::npos);
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "O'Brien -- DROP");
}

/**
 * @brief 验证 IS NULL / IS NOT NULL 不产生参数，也不消耗占位符序号
 */
TEST(PostgresDialectWhere, NullChecksProduceNoParameter)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("deleted_at", SqlOperator::IsNull, ParameterValue{nullptr}));
    node.whereConditions.push_back(
        makeComparison("created_at", SqlOperator::IsNotNull, ParameterValue{nullptr}));
    // 两个 NULL 判断之后才出现真正的参数：它必须是 $1，而不是 $3
    node.whereConditions.push_back(
        makeComparison("age", SqlOperator::Gt, ParameterValue{static_cast<std::int64_t>(0)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" WHERE \"deleted_at\" IS NULL "
              "AND \"created_at\" IS NOT NULL AND \"age\" > $1");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 0);
}

/**
 * @brief 验证列-列比较不占用参数与占位符序号
 */
TEST(PostgresDialectWhere, ColumnToColumnComparisonBindsNoParameter)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "trades";
    node.whereConditions.push_back(makeColumnComparison("close_price", SqlOperator::Gt, "open_price"));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"trades\" WHERE \"close_price\" > \"open_price\"");
    EXPECT_TRUE(statement.parameters.empty());
    EXPECT_EQ(countPlaceholders(statement.sql), 0U);
}

/**
 * @brief 验证 IN 展开成连续递增的 $1/$2/$3 且参数顺序一致
 */
TEST(PostgresDialectWhere, InConditionExpandsPlaceholdersWithIncreasingNumbers)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeInCondition("id",
                                                   SqlOperator::In,
                                                   {ParameterValue{static_cast<std::int64_t>(1)},
                                                    ParameterValue{static_cast<std::int64_t>(2)},
                                                    ParameterValue{static_cast<std::int64_t>(3)}}));

    const SqlStatement statement = dialect.translate(node);

    // 序号必须严格连续：$1、$2、$3 与 parameters 的下标一一对应
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"id\" IN ($1, $2, $3)");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 1);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 2);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 3);
}

/**
 * @brief 验证空 IN 集合生成恒假常量，不产出非法 "IN ()" 也不消耗序号
 */
TEST(PostgresDialectWhere, EmptyInSetBecomesConstantPredicate)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeInCondition("id", SqlOperator::In, {}));
    node.whereConditions.push_back(makeInCondition("id", SqlOperator::NotIn, {}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"id\" IN (1 = 0) AND \"id\" NOT IN (1 = 1)");
    EXPECT_EQ(statement.sql.find("()"), std::string::npos);
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证三层嵌套 AND/OR/NOT 的 $n 序号严格按 SQL 出现顺序
 */
TEST(PostgresDialectWhere, NestedLogicNumbersPlaceholdersInSqlOrder)
{
    const PostgresDialect dialect;

    WhereCondition innerAnd = makeComposite(
        SqlOperator::And,
        {makeComparison("id", SqlOperator::Eq, ParameterValue{static_cast<std::int64_t>(7)}),
         makeComparison("name", SqlOperator::IsNotNull, ParameterValue{nullptr})});

    WhereCondition innerNot = makeComposite(
        SqlOperator::Not,
        {makeComparison("age", SqlOperator::Lt, ParameterValue{static_cast<std::int64_t>(30)})});

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComposite(SqlOperator::Or, {std::move(innerAnd), std::move(innerNot)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" WHERE ((\"id\" = $1 AND \"name\" IS NOT NULL) "
              "OR NOT (\"age\" < $2))");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 7);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 30);
}

// ========================================================================
// ORDER BY / GROUP BY / HAVING / 分页
// ========================================================================

/**
 * @brief 验证排序方向显式输出且多字段按序拼接
 */
TEST(PostgresDialectClauses, OrderByRendersDirections)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.orderBy.push_back(OrderByClause{.field = makeField("name"), .descending = false});
    node.orderBy.push_back(OrderByClause{.field = makeField("age"), .descending = true});

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" ORDER BY \"name\" ASC, \"age\" DESC");
}

/**
 * @brief 验证 HAVING 参数序号排在 WHERE 参数之后
 */
TEST(PostgresDialectClauses, HavingPlaceholderFollowsWherePlaceholder)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "orders";
    node.whereConditions.push_back(
        makeComparison("status", SqlOperator::Eq, ParameterValue{std::string("paid")}));
    node.groupBy.push_back(makeField("customer_id"));
    node.having = makeComparison("total", SqlOperator::Gt, ParameterValue{100.5});

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"orders\" WHERE \"status\" = $1 GROUP BY \"customer_id\" "
              "HAVING \"total\" > $2");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "paid");
    EXPECT_DOUBLE_EQ(std::get<double>(statement.parameters[1]), 100.5);
}

/**
 * @brief 验证 LIMIT / OFFSET 走 $n 占位符，序号先 LIMIT 后 OFFSET
 */
TEST(PostgresDialectClauses, LimitAndOffsetAreBoundAsParameters)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.limit     = 10U;
    node.offset    = 20U;

    const SqlStatement statement = dialect.translate(node);

    // PostgreSQL 直接适用基类的标准关键字形式，分页值与其它取值一样走绑定参数
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" LIMIT $1 OFFSET $2");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 10);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 20);
}

/**
 * @brief 验证只有 LIMIT 时只占一个占位符
 */
TEST(PostgresDialectClauses, LimitWithoutOffsetBindsOneParameter)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.limit     = 7U;

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" LIMIT $1");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 7);
}

/**
 * @brief 验证只有 OFFSET 时 PostgreSQL 无需补任何常量（与 SQLite / MySQL 的关键差异）
 */
TEST(PostgresDialectClauses, OffsetWithoutLimitNeedsNoConstant)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.offset    = 5U;

    const SqlStatement statement = dialect.translate(node);

    // PostgreSQL 允许 "OFFSET n" 单独出现，因此既不用 SQLite 的 "LIMIT -1"，
    // 也不用 MySQL 的无符号上界常量；OFFSET 就是第一个出现的占位符，序号为 $1
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" OFFSET $1");
    EXPECT_EQ(statement.sql.find("LIMIT"), std::string::npos);
    EXPECT_EQ(statement.sql.find("-1"), std::string::npos);
    EXPECT_EQ(statement.sql.find("18446744073709551615"), std::string::npos);
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 5);
}

/**
 * @brief 验证既无 LIMIT 也无 OFFSET 时不产生分页子句与参数
 */
TEST(PostgresDialectClauses, NoPaginationProducesNoClause)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql.find("LIMIT"), std::string::npos);
    EXPECT_EQ(statement.sql.find("OFFSET"), std::string::npos);
    EXPECT_TRUE(statement.parameters.empty());
}

// ========================================================================
// JOIN 与 $n 序号在整条语句上的连续性
// ========================================================================

/**
 * @brief 验证 INNER JOIN 与 ON 条件（列-列比较不占参数）
 */
TEST(PostgresDialectJoin, InnerJoinWithColumnComparison)
{
    const PostgresDialect dialect;

    JoinClause joinClause;
    joinClause.type       = JoinType::Inner;
    joinClause.tableName  = "orders";
    joinClause.tableAlias = "o";
    joinClause.conditions.push_back(makeColumnComparison("id", SqlOperator::Eq, "user_id"));

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name"};
    node.joins.push_back(std::move(joinClause));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT \"id\", \"name\" FROM \"users\" "
              "INNER JOIN \"orders\" AS \"o\" ON \"id\" = \"user_id\"");
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证 ON 参数序号在 WHERE 参数之前
 */
TEST(PostgresDialectJoin, JoinOnPlaceholderPrecedesWherePlaceholder)
{
    const PostgresDialect dialect;

    JoinClause joinClause;
    joinClause.type      = JoinType::Left;
    joinClause.tableName = "orders";
    joinClause.conditions.push_back(
        makeComparison("status", SqlOperator::Eq, ParameterValue{std::string("已支付")}));

    QueryNode node;
    node.tableName = "users";
    node.joins.push_back(std::move(joinClause));
    node.whereConditions.push_back(
        makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" LEFT JOIN \"orders\" ON \"status\" = $1 WHERE \"age\" >= $2");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "已支付");
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 18);
}

/**
 * @brief 验证综合场景下 $1..$n 连续、且占位符个数等于参数个数
 */
TEST(PostgresDialectJoin, PlaceholderNumbersAreContinuousAcrossAllClauses)
{
    const PostgresDialect dialect;

    JoinClause joinClause;
    joinClause.type      = JoinType::Inner;
    joinClause.tableName = "orders";
    joinClause.conditions.push_back(
        makeComparison("state", SqlOperator::Eq, ParameterValue{std::string("open")}));

    QueryNode node;
    node.tableName = "users";
    node.joins.push_back(std::move(joinClause));
    node.whereConditions.push_back(makeComposite(
        SqlOperator::And,
        {makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}),
         makeInCondition("id",
                         SqlOperator::In,
                         {ParameterValue{static_cast<std::int64_t>(1)},
                          ParameterValue{static_cast<std::int64_t>(2)}}),
         makeComparison("deleted_at", SqlOperator::IsNull, ParameterValue{nullptr})}));
    node.having = makeComparison("total", SqlOperator::Gt, ParameterValue{0.0});
    node.limit  = 5U;
    node.offset = 10U;

    const SqlStatement statement = dialect.translate(node);

    // JOIN 1 个 + WHERE 里 3 个（列-列与 IS NULL 均不占序号）+ HAVING 1 个 + 分页 2 个 = 7 个
    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" INNER JOIN \"orders\" ON \"state\" = $1 "
              "WHERE (\"age\" >= $2 AND \"id\" IN ($3, $4) AND \"deleted_at\" IS NULL) "
              "HAVING \"total\" > $5 LIMIT $6 OFFSET $7");
    EXPECT_EQ(countPlaceholders(statement.sql), 7U);
    EXPECT_EQ(statement.parameters.size(), countPlaceholders(statement.sql));
}

// ========================================================================
// 写语句：SET 参数序号在 WHERE 之前
// ========================================================================

/**
 * @brief 验证 INSERT 的列名双引号引用与 $n 取值
 */
TEST(PostgresDialectWrite, InsertUsesDollarPlaceholders)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name", "note"};

    const std::vector<DatabaseValue> values{
        std::int64_t{7},
        std::string("O'Brien -- 中文"),
        std::monostate{}
    };

    const SqlStatement statement = dialect.translateInsert(node, values);

    EXPECT_EQ(statement.sql, "INSERT INTO \"users\" (\"id\", \"name\", \"note\") VALUES ($1, $2, $3)");
    ASSERT_EQ(statement.parameters.size(), values.size());
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 7);
    EXPECT_EQ(std::get<std::string>(statement.parameters[1]), "O'Brien -- 中文");
    // NULL 必须以绑定参数的形式送出，而不是被拼成 SQL 文本里的 NULL 关键字
    EXPECT_TRUE(std::holds_alternative<std::monostate>(statement.parameters[2]));
    EXPECT_EQ(statement.sql.find("O'Brien"), std::string::npos);
}

/**
 * @brief 验证 UPDATE 的 SET 参数是 $1/$2，WHERE 参数顺延为 $3
 */
TEST(PostgresDialectWrite, UpdateBindsAssignmentsBeforeWhereParameters)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"name", "balance"};
    node.whereConditions.push_back(
        makeComparison("id", SqlOperator::Eq, ParameterValue{static_cast<std::int64_t>(42)}));

    const std::vector<DatabaseValue> values{std::string("王五"), 888.25};

    const SqlStatement statement = dialect.translateUpdate(node, values);

    // SET 子句先于 WHERE 输出，因此赋值占 $1/$2、条件占 $3，与文本里占位符的先后严格一致
    EXPECT_EQ(statement.sql, "UPDATE \"users\" SET \"name\" = $1, \"balance\" = $2 WHERE \"id\" = $3");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "王五");
    EXPECT_DOUBLE_EQ(std::get<double>(statement.parameters[1]), 888.25);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 42);
}

/**
 * @brief 验证 DELETE 的条件渲染与 $n 参数收集
 */
TEST(PostgresDialectWrite, DeleteRendersWhereConditionAndBindsParameters)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComposite(
        SqlOperator::And,
        {makeComparison("active", SqlOperator::Eq, ParameterValue{false}),
         makeInCondition("id",
                         SqlOperator::In,
                         {ParameterValue{static_cast<std::int64_t>(3)}, ParameterValue{static_cast<std::int64_t>(4)}})}));

    const SqlStatement statement = dialect.translateDelete(node);

    EXPECT_EQ(statement.sql, "DELETE FROM \"users\" WHERE (\"active\" = $1 AND \"id\" IN ($2, $3))");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<bool>(statement.parameters[0]));
    EXPECT_FALSE(std::get<bool>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 3);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 4);
}

/**
 * @brief 验证多行 INSERT 的 $n 按「行优先、行内按列序」连续编号
 */
TEST(PostgresDialectWrite, InsertBatchNumbersPlaceholdersRowMajor)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "accounts";
    node.selectColumns = {"id", "name"};

    const std::vector<std::vector<DatabaseValue>> rows{
        {std::int64_t{1}, std::string("张三")},
        {std::int64_t{2}, std::string("O'Brien -- DROP")}
    };

    const SqlStatement statement = dialect.translateInsertBatch(node, rows);

    EXPECT_EQ(statement.sql,
              "INSERT INTO \"accounts\" (\"id\", \"name\") VALUES ($1, $2), ($3, $4)");
    ASSERT_EQ(statement.parameters.size(), 4U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 1);
    EXPECT_EQ(std::get<std::string>(statement.parameters[1]), "张三");
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 2);
    EXPECT_EQ(std::get<std::string>(statement.parameters[3]), "O'Brien -- DROP");
}

/**
 * @brief 验证列数与取值个数不符时抛异常，且异常文本带 PostgreSQL 方言名前缀
 */
TEST(PostgresDialectWrite, ColumnCountMismatchReportsDialectName)
{
    const PostgresDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name"};

    // 少给值：必须当场失败，而不是生成会写错列的语句
    const std::string mismatchText = captureInvalidArgument(
        [&dialect, &node]
        {
            static_cast<void>(dialect.translateInsert(node, std::vector<DatabaseValue>{std::int64_t{1}}));
        });
    EXPECT_NE(mismatchText.find("PostgreSQL 方言"), std::string::npos);
    EXPECT_NE(mismatchText.find("取值个数（1）"), std::string::npos);

    // 没有任何待写列：同样是调用方输入错误，文本里带上表名便于定位
    QueryNode emptyColumnNode;
    emptyColumnNode.tableName = "users";
    const std::string emptyColumnText = captureInvalidArgument(
        [&dialect, &emptyColumnNode]
        {
            static_cast<void>(dialect.translateInsert(emptyColumnNode, std::vector<DatabaseValue>{}));
        });
    EXPECT_NE(emptyColumnText.find("PostgreSQL 方言"), std::string::npos);
    EXPECT_NE(emptyColumnText.find("users"), std::string::npos);
}

/**
 * @brief 验证事务控制语句文本由 PostgreSQL 方言给出
 */
TEST(PostgresDialectWrite, TransactionStatements)
{
    const PostgresDialect dialect;

    // PostgreSQL 的 BEGIN 与 START TRANSACTION 等价；COMMIT / ROLLBACK 与另两个引擎相同
    EXPECT_EQ(dialect.beginTransactionStatement(), "BEGIN");
    EXPECT_EQ(dialect.commitStatement(), "COMMIT");
    EXPECT_EQ(dialect.rollbackStatement(), "ROLLBACK");
}

// ========================================================================
// DDL 支撑（类型名映射与表存在性元数据查询）
// ========================================================================

/**
 * @brief 验证六个逻辑列类型被映射成 PostgreSQL 的物理类型名
 */
TEST(PostgresDialectDdl, ColumnTypeNamesCoverAllLogicalTypes)
{
    const PostgresDialect dialect;

    EXPECT_EQ(dialect.columnTypeName(ColumnType::Int64), "BIGINT");
    // PostgreSQL 没有无符号整数，NUMERIC(20) 的 20 位十进制正好覆盖 0 .. 2^64-1
    EXPECT_EQ(dialect.columnTypeName(ColumnType::UInt64), "NUMERIC(20)");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Double), "DOUBLE PRECISION");
    // 三个引擎里唯一有真布尔类型的
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Bool), "BOOLEAN");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Text), "TEXT");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Blob), "BYTEA");

    // 未知取值必须回落到一个可用类型名而不是崩溃：本方法 noexcept 且调用点没有回退分支
    EXPECT_EQ(dialect.columnTypeName(static_cast<ColumnType>(0xFF)), "TEXT");
}

/**
 * @brief 验证表存在性查询按 current_schema() 限定，且表名以 $1 绑定参数送出
 */
TEST(PostgresDialectDdl, TableExistsStatementScopesToCurrentSchema)
{
    const PostgresDialect dialect;

    const SqlStatement statement = dialect.tableExistsStatement("users");

    // information_schema.tables 覆盖当前库的所有模式：必须用 current_schema() 限定，
    // 否则其它模式里的同名表会让「表不存在却报告存在」
    EXPECT_EQ(statement.sql,
              "SELECT COUNT(*) FROM information_schema.tables "
              "WHERE table_schema = current_schema() AND table_name = $1");
    ASSERT_EQ(statement.parameters.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<std::string>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "users");

    // 恶意表名只是普通文本：它落在参数里，不会改变语句结构，也不会多出占位符
    const SqlStatement hostileStatement = dialect.tableExistsStatement("x\"; DROP TABLE users; --");
    EXPECT_EQ(hostileStatement.sql, statement.sql);
    EXPECT_EQ(countPlaceholders(hostileStatement.sql), 1U);
    ASSERT_EQ(hostileStatement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(hostileStatement.parameters[0]), "x\"; DROP TABLE users; --");
}

// ========================================================================
// DialectRegistry
// ========================================================================

/**
 * @brief 验证 PostgreSQL 方言可取得、类型正确、与 SQLite / MySQL 是不同实例且重复取得为同一对象
 */
TEST(DialectRegistryTest, PostgresDialectIsAvailable)
{
    const std::shared_ptr<SqlDialect> dialect = DialectRegistry::dialectFor(DatabaseType::PostgreSql);

    ASSERT_NE(dialect, nullptr);
    EXPECT_EQ(dialect->type(), DatabaseType::PostgreSql);
    EXPECT_TRUE(DialectRegistry::supports(DatabaseType::PostgreSql));

    // 无状态方言由注册表共享，两次取得应得到同一对象
    EXPECT_EQ(dialect.get(), DialectRegistry::dialectFor(DatabaseType::PostgreSql).get());

    // 与另两个方言必须是不同实例：占位符风格完全不同（$n vs ?），混用会生成非法 SQL
    EXPECT_NE(dialect.get(), DialectRegistry::dialectFor(DatabaseType::Sqlite).get());
    EXPECT_NE(dialect.get(), DialectRegistry::dialectFor(DatabaseType::MySql).get());
}
