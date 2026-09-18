// SQLite 方言翻译单元测试（不需要数据库连接）。
// 只验证「查询树 → 参数化 SQL」的纯文本翻译结果与参数收集顺序，不打开任何数据库文件，也不经过任何驱动。
// 覆盖场景：
// - 标识符引用与内部引号转义
// - 占位符文本、方言类型、LIMIT/OFFSET 支持能力
// - SELECT 列展开（通配符 / 显式列 / 表达式列 / 限定名 / 含引号列名）
// - FROM 与表别名
// - WHERE：单条件、AND/OR/NOT 递归、IS NULL / IS NOT NULL、IN / NOT IN、列-列比较
// - ORDER BY、GROUP BY、HAVING、LIMIT / OFFSET
// - JOIN：INNER/LEFT/RIGHT/CROSS 与 ON 条件
// - 参数顺序、数量、类型与 uint64 降级
// - 写语句：INSERT / UPDATE / DELETE / 多行 INSERT 的文本、参数顺序与个数校验
// - 事务控制语句文本与单条语句的参数上限
// - DDL 支撑：逻辑列类型到 SQLite 存储类的映射、表存在性元数据语句（表名走绑定）
// - DialectRegistry：SQLite 可取得，MySQL 另有方言且是不同实例（供 SQLite 测试确认两者不会互相顶替），
//   Redis 抛出中文异常

#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/Queryable/QueryNode.h"

#include <gtest/gtest.h>

#include <cstdint>
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
    using AsynGyanis::Database::SqlDialect;
    using AsynGyanis::Database::SqliteDialect;
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
     * @brief 统计 SQL 文本里的占位符个数
     * @param sql 待统计的 SQL 文本
     * @return std::size_t '?' 出现的次数
     */
    std::size_t countPlaceholders(const std::string &sql)
    {
        std::size_t count = 0;
        for (const char character: sql)
        {
            if (character == '?')
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

} // namespace

// ========================================================================
// 标识符引用
// ========================================================================

/**
 * @brief 验证标识符被双引号包裹，保留字同样安全
 */
TEST(SqliteDialectIdentifier, QuoteIdentifierWrapsWithDoubleQuotes)
{
    const SqliteDialect dialect;

    EXPECT_EQ(dialect.quoteIdentifier("name"), "\"name\"");
    // order / group 是 SQL 保留字，加引号后可以安全作为列名使用
    EXPECT_EQ(dialect.quoteIdentifier("order"), "\"order\"");
    EXPECT_EQ(dialect.quoteIdentifier("group"), "\"group\"");
}

/**
 * @brief 验证标识符内部的双引号被翻倍转义
 */
TEST(SqliteDialectIdentifier, QuoteIdentifierDoublesEmbeddedQuote)
{
    const SqliteDialect dialect;

    EXPECT_EQ(dialect.quoteIdentifier("weird\"name"), "\"weird\"\"name\"");
    EXPECT_EQ(dialect.quoteIdentifier("a\"b\"c"), "\"a\"\"b\"\"c\"");
    EXPECT_EQ(dialect.quoteIdentifier(""), "\"\"");
}

/**
 * @brief 验证占位符文本恒为 '?'（接口不含序号信息）
 */
TEST(SqliteDialectIdentifier, PlaceholderIsAlwaysQuestionMark)
{
    const SqliteDialect dialect;

    // 同一方言无论调用几次都返回同一个文本：参数与占位符的对应靠压入顺序，不靠序号
    EXPECT_EQ(dialect.placeholder(), "?");
    EXPECT_EQ(dialect.placeholder(), "?");
}

/**
 * @brief 验证方言类型与分页能力声明
 */
TEST(SqliteDialectIdentifier, DialectMetadata)
{
    const SqliteDialect dialect;

    EXPECT_EQ(dialect.type(), DatabaseType::Sqlite);
    EXPECT_TRUE(dialect.supportsLimitOffset());
}

// ========================================================================
// SELECT 列与 FROM
// ========================================================================

/**
 * @brief 验证 selectColumns 为空时退化为通配符
 */
TEST(SqliteDialectSelect, EmptySelectColumnsFallsBackToWildcard)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\"");
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证显式列按给定顺序加引号输出
 */
TEST(SqliteDialectSelect, ExplicitColumnsAreQuotedInOrder)
{
    const SqliteDialect dialect;

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
TEST(SqliteDialectSelect, ExpressionColumnIsPassedThrough)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"COUNT(*)", "COALESCE(age, 0)"};

    const SqlStatement statement = dialect.translate(node);

    // 加引号会把函数调用降级成列名，因此必须原样输出
    EXPECT_EQ(statement.sql, "SELECT COUNT(*), COALESCE(age, 0) FROM \"users\"");
}

/**
 * @brief 验证限定名逐段加引号，通配符段保持裸写
 */
TEST(SqliteDialectSelect, QualifiedNameQuotesEachSegment)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"users.id", "users.*"};

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT \"users\".\"id\", \"users\".* FROM \"users\"");
}

/**
 * @brief 验证含双引号的列名在翻译时被正确转义
 */
TEST(SqliteDialectSelect, ColumnNameWithQuoteIsEscaped)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"weird\"name"};

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT \"weird\"\"name\" FROM \"users\"");
}

/**
 * @brief 验证表别名被引用
 */
TEST(SqliteDialectSelect, TableAliasIsQuoted)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName  = "users";
    node.tableAlias = "u";

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" AS \"u\"");
}

// ========================================================================
// WHERE：叶子条件
// ========================================================================

/**
 * @brief 验证单条件比较产生的 SQL 与参数
 */
TEST(SqliteDialectWhere, SingleComparisonBindsParameter)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"age\" >= ?");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 18);
}

/**
 * @brief 验证字符串参数按原文本绑定，不加引号拼接
 */
TEST(SqliteDialectWhere, StringParameterIsBoundWithoutQuoting)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("name", SqlOperator::Eq, ParameterValue{std::string("O'Brien -- DROP")}));

    const SqlStatement statement = dialect.translate(node);

    // SQL 文本里绝不出现用户数据，参数原样保留
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"name\" = ?");
    EXPECT_EQ(statement.sql.find("O'Brien"), std::string::npos);
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "O'Brien -- DROP");
}

/**
 * @brief 验证所有比较操作符的文本
 */
TEST(SqliteDialectWhere, AllComparisonOperators)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComparison("age", SqlOperator::Eq, ParameterValue{static_cast<std::int64_t>(1)}));
    node.whereConditions.push_back(makeComparison("age", SqlOperator::Neq, ParameterValue{static_cast<std::int64_t>(2)}));
    node.whereConditions.push_back(makeComparison("age", SqlOperator::Gt, ParameterValue{static_cast<std::int64_t>(3)}));
    node.whereConditions.push_back(makeComparison("age", SqlOperator::Lt, ParameterValue{static_cast<std::int64_t>(4)}));
    node.whereConditions.push_back(makeComparison("age", SqlOperator::Le, ParameterValue{static_cast<std::int64_t>(5)}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_NE(statement.sql.find("\"age\" = ?"), std::string::npos);
    EXPECT_NE(statement.sql.find("\"age\" != ?"), std::string::npos);
    EXPECT_NE(statement.sql.find("\"age\" > ?"), std::string::npos);
    EXPECT_NE(statement.sql.find("\"age\" < ?"), std::string::npos);
    EXPECT_NE(statement.sql.find("\"age\" <= ?"), std::string::npos);
    EXPECT_EQ(statement.parameters.size(), 5U);
}

/**
 * @brief 验证 LIKE 条件
 */
TEST(SqliteDialectWhere, LikeCondition)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("name", SqlOperator::Like, ParameterValue{std::string("%张%")}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"name\" LIKE ?");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "%张%");
}

/**
 * @brief 验证 IS NULL / IS NOT NULL 不产生参数
 */
TEST(SqliteDialectWhere, NullChecksProduceNoParameter)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("deleted_at", SqlOperator::IsNull, ParameterValue{nullptr}));
    node.whereConditions.push_back(
        makeComparison("created_at", SqlOperator::IsNotNull, ParameterValue{nullptr}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" WHERE \"deleted_at\" IS NULL AND \"created_at\" IS NOT NULL");
    // NULL 判断用 IS 而不是 "= NULL"，也不占用任何绑定参数
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证列-列比较不占用参数
 */
TEST(SqliteDialectWhere, ColumnToColumnComparisonBindsNoParameter)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "trades";
    node.whereConditions.push_back(makeColumnComparison("close_price", SqlOperator::Gt, "open_price"));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"trades\" WHERE \"close_price\" > \"open_price\"");
    EXPECT_TRUE(statement.parameters.empty());
    EXPECT_EQ(countPlaceholders(statement.sql), 0U);
}

// ========================================================================
// WHERE：集合与递归逻辑
// ========================================================================

/**
 * @brief 验证 IN 展开成多个占位符且参数顺序一致
 */
TEST(SqliteDialectWhere, InConditionExpandsPlaceholdersInOrder)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeInCondition("id",
                                                   SqlOperator::In,
                                                   {ParameterValue{static_cast<std::int64_t>(1)},
                                                    ParameterValue{static_cast<std::int64_t>(2)},
                                                    ParameterValue{static_cast<std::int64_t>(3)}}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"id\" IN (?, ?, ?)");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 1);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 2);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 3);
}

/**
 * @brief 验证 NOT IN 与字符串集合
 */
TEST(SqliteDialectWhere, NotInConditionWithStrings)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeInCondition("name",
                                                   SqlOperator::NotIn,
                                                   {ParameterValue{std::string("张三")},
                                                    ParameterValue{std::string("李四")}}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"name\" NOT IN (?, ?)");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "张三");
    EXPECT_EQ(std::get<std::string>(statement.parameters[1]), "李四");
}

/**
 * @brief 验证空 IN 集合生成恒假常量而不是非法的 "IN ()"
 */
TEST(SqliteDialectWhere, EmptyInSetBecomesConstantPredicate)
{
    const SqliteDialect dialect;

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
 * @brief 验证 AND 递归展开并加括号
 */
TEST(SqliteDialectWhere, AndRecursionAddsParentheses)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComposite(
        SqlOperator::And,
        {makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}),
         makeComparison("age", SqlOperator::Le, ParameterValue{static_cast<std::int64_t>(60)})}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE (\"age\" >= ? AND \"age\" <= ?)");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 18);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 60);
}

/**
 * @brief 验证三层嵌套 AND/OR/NOT 的参数顺序严格按 SQL 出现顺序
 *
 * 期望：(("id" = ? AND "name" IS NOT NULL) OR NOT ("age" < ?))
 */
TEST(SqliteDialectWhere, NestedLogicCollectsParametersInSqlOrder)
{
    const SqliteDialect dialect;

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
              "SELECT * FROM \"users\" WHERE ((\"id\" = ? AND \"name\" IS NOT NULL) OR NOT (\"age\" < ?))");
    ASSERT_EQ(statement.parameters.size(), 2U);
    // 第一个参数来自第一个占位符（id = ?），第二个来自 NOT 内的 age < ?
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 7);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 30);
}

/**
 * @brief 验证空 children 的 AND / OR / NOT 退化为常量谓词
 */
TEST(SqliteDialectWhere, EmptyCompositeBecomesConstantPredicate)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComposite(SqlOperator::And, {}));
    node.whereConditions.push_back(makeComposite(SqlOperator::Or, {}));
    node.whereConditions.push_back(makeComposite(SqlOperator::Not, {}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE (1 = 1) AND (1 = 0) AND NOT (1 = 1)");
    EXPECT_TRUE(statement.parameters.empty());
}

// ========================================================================
// ORDER BY / GROUP BY / HAVING / 分页
// ========================================================================

/**
 * @brief 验证排序方向显式输出且多字段按序拼接
 */
TEST(SqliteDialectClauses, OrderByRendersDirections)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.orderBy.push_back(OrderByClause{.field = makeField("name"), .descending = false});
    node.orderBy.push_back(OrderByClause{.field = makeField("age"), .descending = true});

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" ORDER BY \"name\" ASC, \"age\" DESC");
}

/**
 * @brief 验证 GROUP BY 与 HAVING 的参数顺序排在 WHERE 参数之后
 */
TEST(SqliteDialectClauses, GroupByHavingParameterOrderFollowsSql)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "orders";
    node.whereConditions.push_back(
        makeComparison("status", SqlOperator::Eq, ParameterValue{std::string("paid")}));
    node.groupBy.push_back(makeField("customer_id"));
    node.having = makeComparison("total", SqlOperator::Gt, ParameterValue{100.5});
    node.orderBy.push_back(OrderByClause{.field = makeField("customer_id"), .descending = false});

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"orders\" WHERE \"status\" = ? GROUP BY \"customer_id\" "
              "HAVING \"total\" > ? ORDER BY \"customer_id\" ASC");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "paid");
    EXPECT_DOUBLE_EQ(std::get<double>(statement.parameters[1]), 100.5);
}

/**
 * @brief 验证 LIMIT / OFFSET 内联为十进制整数
 */
TEST(SqliteDialectClauses, LimitAndOffsetAreInlined)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.limit     = 10U;
    node.offset    = 20U;

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" LIMIT 10 OFFSET 20");
    // 分页值不占占位符，参数列表保持为空
    EXPECT_TRUE(statement.parameters.empty());
    EXPECT_EQ(countPlaceholders(statement.sql), 0U);
}

/**
 * @brief 验证只有 OFFSET 时补出 "LIMIT -1"
 */
TEST(SqliteDialectClauses, OffsetWithoutLimitAddsLimitMinusOne)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.offset    = 5U;

    const SqlStatement statement = dialect.translate(node);

    // SQLite 要求 OFFSET 必须跟在 LIMIT 之后，单独出现是语法错误
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" LIMIT -1 OFFSET 5");
}

// ========================================================================
// JOIN
// ========================================================================

/**
 * @brief 验证 INNER JOIN 与 ON 条件
 */
TEST(SqliteDialectJoin, InnerJoinWithOnCondition)
{
    const SqliteDialect dialect;

    JoinClause joinClause;
    joinClause.type      = JoinType::Inner;
    joinClause.tableName = "orders";
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
 * @brief 验证 ON 条件中的参数与 WHERE 参数按出现顺序收集
 */
TEST(SqliteDialectJoin, JoinOnParametersPrecedeWhereParameters)
{
    const SqliteDialect dialect;

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
              "SELECT * FROM \"users\" LEFT JOIN \"orders\" ON \"status\" = ? WHERE \"age\" >= ?");
    ASSERT_EQ(statement.parameters.size(), 2U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "已支付");
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 18);
}

/**
 * @brief 验证 RIGHT JOIN 与 CROSS JOIN 的关键字
 */
TEST(SqliteDialectJoin, RightAndCrossJoinKeywords)
{
    const SqliteDialect dialect;

    JoinClause rightJoin;
    rightJoin.type      = JoinType::Right;
    rightJoin.tableName = "b";

    JoinClause crossJoin;
    crossJoin.type      = JoinType::Cross;
    crossJoin.tableName = "c";

    QueryNode node;
    node.tableName = "a";
    node.joins.push_back(std::move(rightJoin));
    node.joins.push_back(std::move(crossJoin));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"a\" RIGHT JOIN \"b\" CROSS JOIN \"c\"");
}

// ========================================================================
// 参数类型转换
// ========================================================================

/**
 * @brief 验证各类参数值按备选转换到 DatabaseValue
 */
TEST(SqliteDialectParameter, ParameterTypesAreConverted)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "mixed";
    node.whereConditions.push_back(
        makeComparison("flag", SqlOperator::Eq, ParameterValue{true}));
    node.whereConditions.push_back(
        makeComparison("score", SqlOperator::Eq, ParameterValue{1.5}));
    node.whereConditions.push_back(
        makeComparison("deleted", SqlOperator::Eq, ParameterValue{nullptr}));

    const SqlStatement statement = dialect.translate(node);

    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<bool>(statement.parameters[0]));
    EXPECT_TRUE(std::get<bool>(statement.parameters[0]));
    EXPECT_TRUE(std::holds_alternative<double>(statement.parameters[1]));
    EXPECT_DOUBLE_EQ(std::get<double>(statement.parameters[1]), 1.5);
    // nullptr 在驱动层用 monostate 表达
    EXPECT_TRUE(std::holds_alternative<std::monostate>(statement.parameters[2]));
}

/**
 * @brief 验证 uint64 放得进 int64 时按有符号整数绑定
 */
TEST(SqliteDialectParameter, UnsignedParameterWithinInt64RangeBecomesInt64)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "counters";
    node.whereConditions.push_back(
        makeComparison("value", SqlOperator::Eq, ParameterValue{static_cast<std::uint64_t>(42)}));

    const SqlStatement statement = dialect.translate(node);

    ASSERT_EQ(statement.parameters.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 42);
}

/**
 * @brief 验证超出 int64 的 uint64 降级为十进制文本，而不是回绕成负数
 */
TEST(SqliteDialectParameter, UnsignedParameterBeyondInt64RangeBecomesDecimalText)
{
    const SqliteDialect dialect;

    // UINT64_MAX 转成 int64 会变成 -1，静默出错；这里必须走文本降级
    constexpr std::uint64_t kBeyondInt64 = 18446744073709551615ULL;

    QueryNode node;
    node.tableName = "counters";
    node.whereConditions.push_back(
        makeComparison("value", SqlOperator::Eq, ParameterValue{kBeyondInt64}));

    const SqlStatement statement = dialect.translate(node);

    ASSERT_EQ(statement.parameters.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<std::string>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "18446744073709551615");
}

/**
 * @brief 验证占位符个数与参数个数始终一致（综合场景）
 */
TEST(SqliteDialectParameter, PlaceholderCountMatchesParameterCount)
{
    const SqliteDialect dialect;

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

    const SqlStatement statement = dialect.translate(node);

    // JOIN 1 个 + WHERE 里 3 个 + HAVING 1 个 = 5 个
    EXPECT_EQ(countPlaceholders(statement.sql), 5U);
    EXPECT_EQ(statement.parameters.size(), countPlaceholders(statement.sql));
}

// ========================================================================
// 写语句：INSERT / UPDATE / DELETE / 批量 INSERT
// ========================================================================

/**
 * @brief 验证单行 INSERT 的列名引用、占位符顺序与参数绑定
 */
TEST(SqliteDialectWrite, InsertRendersQuotedColumnsAndBindsValuesInOrder)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name", "note"};

    const std::vector<DatabaseValue> values{
        std::int64_t{7},
        std::string("O'Brien -- 中文"),
        std::monostate{}
    };

    const SqlStatement statement = dialect.translateInsert(node, values);

    EXPECT_EQ(statement.sql, "INSERT INTO \"users\" (\"id\", \"name\", \"note\") VALUES (?, ?, ?)");
    ASSERT_EQ(statement.parameters.size(), values.size());
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 7);
    EXPECT_EQ(std::get<std::string>(statement.parameters[1]), "O'Brien -- 中文");
    // NULL 必须以绑定参数的形式送出，而不是被拼成 SQL 文本里的 NULL 关键字
    EXPECT_TRUE(std::holds_alternative<std::monostate>(statement.parameters[2]));
    // 任何取值都不得出现在 SQL 文本里
    EXPECT_EQ(statement.sql.find("O'Brien"), std::string::npos);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
}

/**
 * @brief 验证列与取值个数不一致时抛异常，而不是生成写错列的语句
 */
TEST(SqliteDialectWrite, InsertRejectsColumnAndValueCountMismatch)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"id", "name"};

    // 少给值
    EXPECT_THROW(static_cast<void>(dialect.translateInsert(node, std::vector<DatabaseValue>{std::int64_t{1}})),
                 std::invalid_argument);

    // 多给值
    EXPECT_THROW(static_cast<void>(dialect.translateInsert(
                     node, std::vector<DatabaseValue>{std::int64_t{1}, std::string("a"), std::string("b")})),
                 std::invalid_argument);

    // 没有任何待写列
    QueryNode emptyColumnNode;
    emptyColumnNode.tableName = "users";
    EXPECT_THROW(static_cast<void>(dialect.translateInsert(emptyColumnNode, std::vector<DatabaseValue>{})),
                 std::invalid_argument);
}

/**
 * @brief 验证 UPDATE 的 SET 参数排在 WHERE 参数之前，且条件复用 SELECT 的渲染规则
 */
TEST(SqliteDialectWrite, UpdateBindsAssignmentsBeforeWhereParameters)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"name", "balance"};
    node.whereConditions.push_back(
        makeComparison("id", SqlOperator::Eq, ParameterValue{static_cast<std::int64_t>(42)}));

    const std::vector<DatabaseValue> values{std::string("王五"), 888.25};

    const SqlStatement statement = dialect.translateUpdate(node, values);

    EXPECT_EQ(statement.sql, "UPDATE \"users\" SET \"name\" = ?, \"balance\" = ? WHERE \"id\" = ?");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "王五");
    EXPECT_DOUBLE_EQ(std::get<double>(statement.parameters[1]), 888.25);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 42);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());
}

/**
 * @brief 验证带表别名的 UPDATE 与复合条件（IN + IS NULL）的参数顺序
 */
TEST(SqliteDialectWrite, UpdateWithCompositeConditionKeepsParameterOrder)
{
    const SqliteDialect dialect;

    // NOT (deleted_at IS NULL) 不占参数；IN 展开成两个占位符
    WhereCondition notNull = makeComposite(
        SqlOperator::Not, {makeComparison("deleted_at", SqlOperator::IsNull, ParameterValue{nullptr})});

    QueryNode node;
    node.tableName     = "users";
    node.tableAlias    = "u";
    node.selectColumns = {"name"};
    node.whereConditions.push_back(
        makeInCondition("id",
                        SqlOperator::In,
                        {ParameterValue{static_cast<std::int64_t>(1)}, ParameterValue{static_cast<std::int64_t>(2)}}));
    node.whereConditions.push_back(std::move(notNull));

    const std::vector<DatabaseValue> values{std::string("李四")};

    const SqlStatement statement = dialect.translateUpdate(node, values);

    EXPECT_EQ(statement.sql,
              "UPDATE \"users\" AS \"u\" SET \"name\" = ? "
              "WHERE \"id\" IN (?, ?) AND NOT (\"deleted_at\" IS NULL)");
    ASSERT_EQ(statement.parameters.size(), 3U);
    // 赋值参数在前，随后是两个 IN 集合元素，顺序与文本中占位符的先后一致
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "李四");
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 1);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 2);
}

/**
 * @brief 验证没有 WHERE 条件的 UPDATE（整表更新）与空 WHERE 的 UPDATE
 */
TEST(SqliteDialectWrite, UpdateWithoutConditionOmitsWhereClause)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"active"};

    const SqlStatement statement = dialect.translateUpdate(node, std::vector<DatabaseValue>{std::int64_t{1}});

    // 无条件即整表更新，是 SQL 本身的语义，不额外补 "WHERE 1 = 1"
    EXPECT_EQ(statement.sql, "UPDATE \"users\" SET \"active\" = ?");
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 1);
}

/**
 * @brief 验证 DELETE 的条件渲染与参数收集
 */
TEST(SqliteDialectWrite, DeleteRendersWhereConditionAndBindsParameters)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(makeComposite(
        SqlOperator::And,
        {makeComparison("active", SqlOperator::Eq, ParameterValue{false}),
         makeInCondition("id",
                         SqlOperator::In,
                         {ParameterValue{static_cast<std::int64_t>(3)}, ParameterValue{static_cast<std::int64_t>(4)}})}));

    const SqlStatement statement = dialect.translateDelete(node);

    EXPECT_EQ(statement.sql, "DELETE FROM \"users\" WHERE (\"active\" = ? AND \"id\" IN (?, ?))");
    ASSERT_EQ(statement.parameters.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<bool>(statement.parameters[0]));
    EXPECT_FALSE(std::get<bool>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[1]), 3);
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[2]), 4);
}

/**
 * @brief 验证无条件 DELETE 直接退化为整表删除，且带别名时别名一并写出
 */
TEST(SqliteDialectWrite, DeleteWithoutConditionAndWithAlias)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";

    const SqlStatement wholeTable = dialect.translateDelete(node);
    EXPECT_EQ(wholeTable.sql, "DELETE FROM \"users\"");
    EXPECT_TRUE(wholeTable.parameters.empty());

    // 别名存在时必须写出：WHERE 里以别名限定的列名只有别名在场才能被解析
    QueryNode aliasedNode;
    aliasedNode.tableName  = "users";
    aliasedNode.tableAlias = "u";
    aliasedNode.whereConditions.push_back(
        makeComparison("u.id", SqlOperator::Gt, ParameterValue{static_cast<std::int64_t>(10)}));

    const SqlStatement aliased = dialect.translateDelete(aliasedNode);
    EXPECT_EQ(aliased.sql, "DELETE FROM \"users\" AS \"u\" WHERE \"u\".\"id\" > ?");
    ASSERT_EQ(aliased.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(aliased.parameters[0]), 10);
}

/**
 * @brief 验证批量 INSERT 生成多行 VALUES，参数按「行优先、行内按列序」排列
 */
TEST(SqliteDialectWrite, InsertBatchRendersMultipleValueRowsInOrder)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "accounts";
    node.selectColumns = {"id", "name", "note"};

    const std::vector<std::vector<DatabaseValue>> rows{
        {std::int64_t{1}, std::string("张三"), std::string("普通备注")},
        {std::int64_t{2}, std::string("O'Brien -- DROP"), std::monostate{}},
        {std::int64_t{3}, std::string("李四"), std::monostate{}}
    };

    const SqlStatement statement = dialect.translateInsertBatch(node, rows);

    EXPECT_EQ(statement.sql,
              "INSERT INTO \"accounts\" (\"id\", \"name\", \"note\") "
              "VALUES (?, ?, ?), (?, ?, ?), (?, ?, ?)");

    // 参数个数 = 行数 × 列数
    ASSERT_EQ(statement.parameters.size(), 9U);
    EXPECT_EQ(countPlaceholders(statement.sql), statement.parameters.size());

    // 逐位校验：第 i 个占位符必须绑定第 i 个参数（行优先、行内按列序）
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[0]), 1);
    EXPECT_EQ(std::get<std::string>(statement.parameters[1]), "张三");
    EXPECT_EQ(std::get<std::string>(statement.parameters[2]), "普通备注");
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[3]), 2);
    EXPECT_EQ(std::get<std::string>(statement.parameters[4]), "O'Brien -- DROP");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(statement.parameters[5]));
    EXPECT_EQ(std::get<std::int64_t>(statement.parameters[6]), 3);
    EXPECT_EQ(std::get<std::string>(statement.parameters[7]), "李四");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(statement.parameters[8]));
}

/**
 * @brief 验证批量 INSERT 拒绝空行集合与行列数不符的输入
 */
TEST(SqliteDialectWrite, InsertBatchRejectsEmptyRowsAndColumnMismatch)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "accounts";
    node.selectColumns = {"id", "name"};

    // 空行集合：SQL 里 "VALUES" 后面必须有至少一组括号，无法生成合法语句
    EXPECT_THROW(static_cast<void>(
                     dialect.translateInsertBatch(node, std::vector<std::vector<DatabaseValue>>{})),
                 std::invalid_argument);

    // 某行少给一列：列与值错位会让数据写进错误的列，必须当场失败
    const std::vector<std::vector<DatabaseValue>> mismatchedRows{
        {std::int64_t{1}, std::string("张三")},
        {std::int64_t{2}}
    };
    EXPECT_THROW(static_cast<void>(dialect.translateInsertBatch(node, mismatchedRows)), std::invalid_argument);
}

/**
 * @brief 验证事务控制语句文本与参数上限常量都由方言给出
 */
TEST(SqliteDialectWrite, TransactionStatementsAndParameterLimit)
{
    const SqliteDialect dialect;

    // SQLite 用 IMMEDIATE 立刻取写锁，避免 DEFERRED 事务升级锁时的 SQLITE_BUSY
    EXPECT_EQ(dialect.beginTransactionStatement(), "BEGIN IMMEDIATE");
    EXPECT_EQ(dialect.commitStatement(), "COMMIT");
    EXPECT_EQ(dialect.rollbackStatement(), "ROLLBACK");

    // 上限来自 SQLITE_MAX_VARIABLE_NUMBER 的默认值，批量写入据此分块
    EXPECT_EQ(dialect.maximumStatementParameters(), SqliteDialect::kMaximumStatementParameters);
    EXPECT_EQ(dialect.maximumStatementParameters(), 999U);
}

// ========================================================================
// DDL 支撑（类型名映射与表存在性元数据查询）
// ========================================================================

/**
 * @brief 验证逻辑列类型被映射成 SQLite 的存储类名
 */
TEST(SqliteDialectDdl, ColumnTypeNamesMapToStorageClasses)
{
    const SqliteDialect dialect;

    // 映射依据：SQLite 只有 INTEGER / REAL / TEXT / BLOB 四个可用存储类，
    // 无符号整数与布尔都没有独立类型，只能落在 INTEGER 上（见 SqliteDialect 的说明）
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Int64), "INTEGER");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::UInt64), "INTEGER");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Double), "REAL");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Bool), "INTEGER");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Text), "TEXT");
    EXPECT_EQ(dialect.columnTypeName(ColumnType::Blob), "BLOB");

    // 未知取值必须回落到一个可用类型名而不是崩溃：本方法 noexcept 且调用点没有回退分支
    EXPECT_EQ(dialect.columnTypeName(static_cast<ColumnType>(0xFF)), "TEXT");
}

/**
 * @brief 验证表存在性查询查 sqlite_master，且表名以绑定参数送出
 */
TEST(SqliteDialectDdl, TableExistsStatementBindsTableName)
{
    const SqliteDialect dialect;

    const SqlStatement statement = dialect.tableExistsStatement("users");

    // 表清单来自当前库文件的 sqlite_master：type='table' 过滤掉索引/视图/触发器
    EXPECT_EQ(statement.sql, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?");
    ASSERT_EQ(statement.parameters.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<std::string>(statement.parameters[0]));
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "users");

    // 恶意表名只是普通文本：它落在参数里，不会改变语句结构，也不会多出占位符
    const SqlStatement hostileStatement = dialect.tableExistsStatement("x'; DROP TABLE users; --");
    EXPECT_EQ(hostileStatement.sql, statement.sql);
    ASSERT_EQ(hostileStatement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(hostileStatement.parameters[0]), "x'; DROP TABLE users; --");
}

// ========================================================================
// DialectRegistry
// ========================================================================

/**
 * @brief 验证 SQLite 方言可取得且类型正确，重复取得为同一实例
 */
TEST(DialectRegistryTest, SqliteDialectIsAvailable)
{
    const std::shared_ptr<SqlDialect> dialect = DialectRegistry::dialectFor(DatabaseType::Sqlite);

    ASSERT_NE(dialect, nullptr);
    EXPECT_EQ(dialect->type(), DatabaseType::Sqlite);
    EXPECT_TRUE(DialectRegistry::supports(DatabaseType::Sqlite));

    // 无状态方言由注册表共享，两次取得应得到同一对象
    EXPECT_EQ(dialect.get(), DialectRegistry::dialectFor(DatabaseType::Sqlite).get());
}

/**
 * @brief 验证 MySQL 已有自己的方言实现，且与 SQLite 方言不是同一个实例
 */
TEST(DialectRegistryTest, MySqlDialectIsDistinctFromSqliteDialect)
{
    // MySQL 方言的完整行为由 TestMySqlDialect.cpp 覆盖，这里只确认注册表分派到了另一个实现：
    // 引用符与分页语法完全不同，若两者被混用会生成非法 SQL
    EXPECT_TRUE(DialectRegistry::supports(DatabaseType::MySql));

    const std::shared_ptr<SqlDialect> mySqlDialect = DialectRegistry::dialectFor(DatabaseType::MySql);

    ASSERT_NE(mySqlDialect, nullptr);
    EXPECT_EQ(mySqlDialect->type(), DatabaseType::MySql);
    EXPECT_NE(mySqlDialect.get(), DialectRegistry::dialectFor(DatabaseType::Sqlite).get());
}

/**
 * @brief 验证 Redis 不作为 SQL 方言注册
 */
TEST(DialectRegistryTest, RedisDialectThrowsWithChineseMessage)
{
    EXPECT_FALSE(DialectRegistry::supports(DatabaseType::Redis));

    try
    {
        static_cast<void>(DialectRegistry::dialectFor(DatabaseType::Redis));
        FAIL() << "Redis 不是 SQL 数据库，应当抛出异常";
    }
    catch (const std::invalid_argument &exception)
    {
        const std::string message = exception.what();
        EXPECT_NE(message.find("Redis"), std::string::npos);
    }
}
