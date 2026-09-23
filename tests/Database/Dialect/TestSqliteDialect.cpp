// SQLite 方言翻译单元测试（不需要数据库连接）。
// 只验证「查询树 → 参数化 SQL」的纯文本翻译结果与参数收集顺序，不打开任何数据库文件，也不经过任何驱动。
// 覆盖场景：
// - 标识符引用与内部引号转义
// - 占位符文本、方言类型、LIMIT/OFFSET 支持能力
// - SELECT 列展开（通配符 / 显式列 / 表达式列 / 限定名 / 含空段的点号文本 / 越过短字符串缓冲的长名 / 含引号列名）
// - 空字段名与空表名在翻译阶段就被拒绝（残缺 SQL 不该留给服务端去报）
// - FROM 与表别名
// - WHERE：单条件、AND/OR/NOT 递归、IS NULL / IS NOT NULL、IN / NOT IN、列-列比较
// - ORDER BY、GROUP BY、HAVING、LIMIT / OFFSET
// - JOIN：INNER/LEFT/RIGHT/CROSS 与 ON 条件；被连接的表名与主表同一条引用规则
//   （「schema.table」逐段引用，其余字节由引用字符兜住；表名为空或点号留空在翻译期被拒）
// - 参数顺序、数量、类型与 uint64 降级
// - 参数上限：判定按实际产出的占位符数（SQLite 内联的分页不占额度），写方向同样受上限保护
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

#include "DatabaseTestSupport.h"

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

    using AsynGyanis::Database::TestSupport::countPlaceholders;
    using AsynGyanis::Database::TestSupport::makeColumnComparison;
    using AsynGyanis::Database::TestSupport::makeComparison;
    using AsynGyanis::Database::TestSupport::makeComposite;
    using AsynGyanis::Database::TestSupport::makeField;
    using AsynGyanis::Database::TestSupport::makeInCondition;

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
 * @brief 验证含空段的点号文本按表达式原样输出，不生成空标识符引用
 */
TEST(SqliteDialectSelect, QualifiedNameWithEmptySegmentIsPassedThrough)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {"users.", ".id", "a..b"};

    const SqlStatement statement = dialect.translate(node);

    // 空段不是合法标识符：把它引用成 "" 会造出一个无名列，与「这压根不是限定名」是两回事。
    // 整段按表达式交给数据库报错，比在这一层替它猜一个意思更可靠
    EXPECT_EQ(statement.sql, "SELECT users., .id, a..b FROM \"users\"");
}

/**
 * @brief 钉住「整个字段名是空串」与「表名是空串」在翻译阶段就被拒绝
 * @details 这与上一条不同：含空段的点号文本还是一个表达式，而空文本什么都不是——
 *          按表达式原样输出等于渲染出一个缺项（SELECT  FROM "users"、WHERE  = ?），
 *          错误要到服务端才报出来且指不出是哪一项空了。反向对照一并钉住：
 *          通配符与表达式列仍按原样通过，这条判据不能扩成「凡形状特别就拒」。
 */
TEST(SqliteDialectSelect, EmptyFieldNameAndEmptyTableNameAreRejected)
{
    const SqliteDialect dialect;

    const auto makeNode = [](const std::string_view tableName)
    {
        QueryNode node;
        node.tableName = std::string(tableName);
        return node;
    };

    // SELECT 列表里的空项
    {
        QueryNode node        = makeNode("users");
        node.selectColumns    = {"id", ""};
        EXPECT_THROW(static_cast<void>(dialect.translate(node)), AsynGyanis::Base::InvalidArgumentException);
    }

    // GROUP BY 里的空字段
    {
        QueryNode node    = makeNode("users");
        node.groupBy      = {FieldReference{""}};
        EXPECT_THROW(static_cast<void>(dialect.translate(node)), AsynGyanis::Base::InvalidArgumentException);
    }

    // 条件左值是空字段
    {
        QueryNode node            = makeNode("users");
        node.whereConditions.push_back(WhereCondition{
                .left  = FieldReference{""},
                .op    = SqlOperator::Eq,
                .right = ParameterValue{static_cast<std::int64_t>(1)}
        });
        EXPECT_THROW(static_cast<void>(dialect.translate(node)), AsynGyanis::Base::InvalidArgumentException);
    }

    // 空表名：引用成 "" 是一个合法但必定不存在的名字，报出的 "no such table" 指不到真因
    {
        const QueryNode node = makeNode("");
        EXPECT_THROW(static_cast<void>(dialect.translate(node)), AsynGyanis::Base::InvalidArgumentException);
    }

    // 反向对照：通配符与表达式列照常通过
    {
        QueryNode node        = makeNode("users");
        node.selectColumns    = {"*", "COUNT(*)"};
        node.groupBy.push_back(FieldReference{"users.name"});
        const SqlStatement statement = dialect.translate(node);
        EXPECT_NE(statement.sql.find("SELECT *, COUNT(*)"), std::string::npos) << statement.sql;
    }
}

/**
 * @brief 验证超出短字符串缓冲的长标识符仍逐字加引用并转义
 */
TEST(SqliteDialectSelect, LongIdentifierWithQuoteIsQuotedVerbatim)
{
    const SqliteDialect dialect;

    // 长度越过短字符串优化的名字才会走堆缓冲；这里同时带一个内部引用符，两个分支一起验
    const std::string longColumnName = "veryLongColumnNameThatWillNotFitSmallBuffer\"tail";

    QueryNode node;
    node.tableName     = "users";
    node.selectColumns = {longColumnName};

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT \"veryLongColumnNameThatWillNotFitSmallBuffer\"\"tail\" FROM \"users\"");
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
 * @brief 验证字面量匹配（LikeLiteral）补出 ESCAPE 子句
 *
 * @details SQLite 的 LIKE 没有默认转义符：不写 ESCAPE 时模式里的 '!' 只是普通字符，
 *          contains()/startsWith()/endsWith() 加上去的转义会全部失效、'%_' 仍按通配符解释。
 *          因此「有 ESCAPE 子句」本身就是这条链能不能用起来的判据，必须钉在 SQL 文本上。
 */
TEST(SqliteDialectWhere, LikeLiteralConditionCarriesEscapeClause)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(
        makeComparison("name", SqlOperator::LikeLiteral, ParameterValue{std::string("100!%")}));

    const SqlStatement statement = dialect.translate(node);

    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE \"name\" LIKE ? ESCAPE '!'");
    // 取值仍走绑定，转义后的模式不进 SQL 文本
    ASSERT_EQ(statement.parameters.size(), 1U);
    EXPECT_EQ(std::get<std::string>(statement.parameters[0]), "100!%");
}

/**
 * @brief 验证多于一个子条件的 NOT 被拒绝，而不是只渲染第一个子条件
 *
 * @details NOT (a AND b) 与 (NOT a) AND (NOT b) 结果不同，替调用方挑一个就是静默改谓词。
 *          公开的 ! 运算符只会放一个子条件，多个只可能来自手搓的查询树。
 *          对照组钉住单子条件的 NOT 仍正常渲染（含括号），否则本用例会连同正确路径一起红。
 */
TEST(SqliteDialectWhere, NotWithMultipleChildrenIsRejectedInsteadOfSilentlyDropped)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(WhereCondition{
            .left     = FieldReference{"placeholder"},
            .op       = SqlOperator::Not,
            .right    = ParameterValue{nullptr},
            .children = {
                    makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)}),
                    makeComparison("score", SqlOperator::Lt, ParameterValue{static_cast<std::int64_t>(60)})
            }
    });

    EXPECT_THROW(static_cast<void>(dialect.translate(node)), AsynGyanis::Base::InvalidArgumentException);

    // 对照组：单个子条件的 NOT 是合法形态，必须仍按 "NOT (...)" 渲染
    QueryNode singleNode;
    singleNode.tableName = "users";
    singleNode.whereConditions.push_back(WhereCondition{
            .left     = FieldReference{"placeholder"},
            .op       = SqlOperator::Not,
            .right    = ParameterValue{nullptr},
            .children = {makeComparison("age", SqlOperator::Ge, ParameterValue{static_cast<std::int64_t>(18)})}
    });

    const SqlStatement statement = dialect.translate(singleNode);
    EXPECT_EQ(statement.sql, "SELECT * FROM \"users\" WHERE NOT (\"age\" >= ?)");
    EXPECT_EQ(statement.parameters.size(), 1U);
}

/**
 * @brief 验证 IN 列表超过引擎单条语句参数上限时在翻译阶段就被拒绝
 *
 * @details 不拦的话要等到执行阶段，由驱动回一句引擎原文（"too many SQL variables"），
 *          既指不出是哪一段条件撑爆的也不说怎么改。上限两侧各钉一条：恰好等于上限必须放行，
 *          否则这条判据就退化成「把所有大 IN 都拒了」。
 */
TEST(SqliteDialectWhere, InListBeyondTheEngineParameterBudgetIsRejectedWithReadableReason)
{
    const SqliteDialect dialect;

    const auto makeInNode = [](const std::size_t valueCount)
    {
        std::vector<ParameterValue> values;
        values.reserve(valueCount);
        for (std::size_t index = 0; index < valueCount; ++index)
        {
            values.push_back(static_cast<std::int64_t>(index));
        }

        QueryNode node;
        node.tableName = "users";
        node.whereConditions.push_back(WhereCondition{
                .left     = FieldReference{"id"},
                .op       = SqlOperator::In,
                .right    = ParameterValue{static_cast<std::int64_t>(0)},
                .inValues = std::move(values)
        });
        return node;
    };

    const std::size_t budget = dialect.maximumStatementParameters();

    // 越界一格：拒绝，且文案给出实际个数、上限与替代做法
    try
    {
        static_cast<void>(dialect.translate(makeInNode(budget + 1U)));
        FAIL() << "超过引擎单条语句参数上限的 IN 列表应当被拒绝";
    }
    catch (const AsynGyanis::Base::InvalidArgumentException &failure)
    {
        const std::string message = failure.what();
        EXPECT_NE(message.find(std::to_string(budget)), std::string::npos) << message;
        EXPECT_NE(message.find(std::to_string(budget + 1U)), std::string::npos) << message;
        EXPECT_NE(message.find("拆成多条语句"), std::string::npos) << message;
    }

    // 恰好等于上限：放行，一条参数都不少
    const SqlStatement atBudget = dialect.translate(makeInNode(budget));
    EXPECT_EQ(atBudget.parameters.size(), budget);
}

/**
 * @brief 钉住参数上限按**实际产出的占位符数**判定：SQLite 内联的分页不许把查询挤出去
 * @details 本方言把 limit/offset 直接写进文本、不产出参数，而翻译前的估算按「各占一个」算。
 *          拿估计数当判据时，一条恰好贴着上限的大 IN 只要再带一个 LIMIT 就被误拒——
 *          而它实际只有 999 个占位符，是完全可执行的语句（MySQL 侧分页确实绑定，估计数即实际数，
 *          因此这条判据在两个方言上都会给出同一份正确结论）。
 */
TEST(SqliteDialectWhere, InlinedPaginationDoesNotConsumeTheParameterBudget)
{
    const SqliteDialect dialect;

    std::vector<ParameterValue> inValues;
    inValues.reserve(dialect.maximumStatementParameters());
    for (std::size_t index = 0; index < dialect.maximumStatementParameters(); ++index)
    {
        inValues.push_back(static_cast<std::int64_t>(index));
    }

    QueryNode node;
    node.tableName = "users";
    node.whereConditions.push_back(WhereCondition{
            .left     = FieldReference{"id"},
            .op       = SqlOperator::In,
            .right    = ParameterValue{static_cast<std::int64_t>(0)},
            .inValues = std::move(inValues)
    });
    node.limit  = 10U;
    node.offset = 5U;

    // 估算会数出 1001 个（999 + 分页各一），实际产出仍是 999：必须放行
    const SqlStatement statement = dialect.translate(node);
    EXPECT_EQ(statement.parameters.size(), dialect.maximumStatementParameters());
    EXPECT_NE(statement.sql.find("LIMIT 10"), std::string::npos) << "分页应内联进文本而不是占位符";
    EXPECT_NE(statement.sql.find("OFFSET 5"), std::string::npos);
}

/**
 * @brief 钉住写方向同样受参数上限保护，且给出中文可操作原因
 * @details 分块是 ORM 的职责，但直接调方言的调用方（以及列数本身就已超限的宽表）也要在翻译阶段
 *          拿到「哪个数撑爆了、上限多少、怎么办」，而不是驱动在 execute 阶段回一句引擎原文。
 */
TEST(SqliteDialectWrite, InsertAndBatchBeyondTheParameterBudgetAreRejected)
{
    const SqliteDialect dialect;
    const std::size_t   budget = dialect.maximumStatementParameters();

    const auto makeWideNode = [&dialect](const std::size_t columnCount)
    {
        QueryNode node;
        node.tableName = "wide";
        for (std::size_t index = 0; index < columnCount; ++index)
        {
            node.selectColumns.push_back("c" + std::to_string(index));
        }
        return node;
    };

    // 单行 INSERT：列数越界一格即拒绝
    {
        const QueryNode      overNode = makeWideNode(budget + 1U);
        const std::vector<DatabaseValue> values(budget + 1U, static_cast<std::int64_t>(1));
        EXPECT_THROW(static_cast<void>(dialect.translateInsert(overNode, values)),
                     AsynGyanis::Base::InvalidArgumentException);

        const QueryNode      atNode = makeWideNode(budget);
        const std::vector<DatabaseValue> atValues(budget, static_cast<std::int64_t>(1));
        EXPECT_EQ(dialect.translateInsert(atNode, atValues).parameters.size(), budget);
    }

    // 批量 INSERT：每行不越界，但列数 × 行数越界——一次 VALUES 多行就是本条语句的参数总数
    {
        const std::size_t columnsPerRow = budget / 2U + 1U;
        const QueryNode   batchNode       = makeWideNode(columnsPerRow);
        const std::vector<std::vector<DatabaseValue> > rows(2U, std::vector<DatabaseValue>(columnsPerRow, static_cast<std::int64_t>(7)));

        try
        {
            static_cast<void>(dialect.translateInsertBatch(batchNode, rows));
            FAIL() << "列数 × 行数超过引擎单条语句上限的批量插入应当被拒绝";
        }
        catch (const AsynGyanis::Base::InvalidArgumentException &failure)
        {
            const std::string message = failure.what();
            EXPECT_NE(message.find(std::to_string(budget)), std::string::npos) << message;
            EXPECT_NE(message.find("拆成多条语句"), std::string::npos) << message;
        }
    }
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
 * @details 期望：(("id" = ? AND "name" IS NOT NULL) OR NOT ("age" < ?))
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
 * @brief 验证被连接的表名一律加引用，不像字段引用那样为表达式「原样输出」让路
 *
 * @details 表名此前走的是字段引用那条通道，而它给 COUNT(*) 这类表达式留着原样拼出去的出口。
 *          表名位置没有任何合法表达式，那个出口等于把 SQL 结构送进语句。下面这个名字看着像
 *          注入串，加引用之后就只是一个名字很怪、但必定查不到的表。
 */
TEST(SqliteDialectJoin, JoinTargetTableNameIsAlwaysQuoted)
{
    const SqliteDialect dialect;

    JoinClause joinClause;
    joinClause.type       = JoinType::Inner;
    joinClause.tableName  = "orders; DROP TABLE users; --";
    joinClause.conditions.push_back(makeColumnComparison("id", SqlOperator::Eq, "user_id"));

    QueryNode node;
    node.tableName = "users";
    node.joins.push_back(std::move(joinClause));

    const SqlStatement statement = dialect.translate(node);
    // 整串在两个双引号之间：语句里那个分号只是名字的一部分，没有第二条语句被拼出来
    EXPECT_EQ(statement.sql,
              "SELECT * FROM \"users\" "
              "INNER JOIN \"orders; DROP TABLE users; --\" ON \"id\" = \"user_id\"");
}

/**
 * @brief 验证主表与被连接的表都把「schema.table」渲染成逐段引用的限定名
 *
 * @details 点号在 SQL 里是层级分隔符：整块包成一个标识符（"shop.users"）会得到一张名叫
 *          shop.users 的表，而不是 shop 库下的 users。两侧必须同规则，否则同一个字符串
 *          放进 FROM 与放进 JOIN 会指向两张不同的表。
 */
TEST(SqliteDialectJoin, SchemaPrefixedTableNamesAreQuotedSegmentBySegment)
{
    const SqliteDialect dialect;

    JoinClause joinClause;
    joinClause.type       = JoinType::Inner;
    joinClause.tableName  = "shop.orders";
    joinClause.conditions.push_back(makeColumnComparison("id", SqlOperator::Eq, "user_id"));

    QueryNode node;
    node.tableName = "shop.users";
    node.joins.push_back(std::move(joinClause));

    const SqlStatement statement = dialect.translate(node);
    EXPECT_NE(statement.sql.find("\"shop\".\"users\""), std::string::npos) << statement.sql;
    EXPECT_NE(statement.sql.find("\"shop\".\"orders\""), std::string::npos) << statement.sql;
}

/**
 * @brief 验证点号两侧留空的表名在翻译期就被拒，而不是产出一条语法不合法的引用
 *
 * @details "shop." 逐段引用会拼出 "shop"."",  服务端只报一句语法错，指不到「哪一段是空的」。
 */
TEST(SqliteDialectJoin, TableNameWithEmptySegmentAroundDotIsRejected)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName = "shop.";

    try
    {
        static_cast<void>(dialect.translate(node));
        FAIL() << "点号后留空的表名应当在翻译阶段就被拒绝";
    }
    catch (const AsynGyanis::Base::InvalidArgumentException &failure)
    {
        EXPECT_NE(std::string_view(failure.what()).find("空段"), std::string_view::npos) << failure.what();
    }
}

/**
 * @brief 验证空的被连接表名给出的原因是「表名为空」而不是「字段名为空」
 *
 * @details 同一条语句里两种空值都会出现，指错位置会让人去查 SELECT 列表。
 */
TEST(SqliteDialectJoin, EmptyJoinTargetTableNameIsRejectedAsTableName)
{
    const SqliteDialect dialect;

    JoinClause joinClause;
    joinClause.type       = JoinType::Inner;
    joinClause.conditions.push_back(makeColumnComparison("id", SqlOperator::Eq, "user_id"));

    QueryNode node;
    node.tableName = "users";
    node.joins.push_back(std::move(joinClause));

    try
    {
        static_cast<void>(dialect.translate(node));
        FAIL() << "空的被连接表名应当在翻译阶段就被拒绝";
    }
    catch (const AsynGyanis::Base::InvalidArgumentException &failure)
    {
        const std::string message = failure.what();
        EXPECT_NE(message.find("表名为空"), std::string::npos) << message;
        EXPECT_EQ(message.find("字段名为空"), std::string::npos) << message;
    }
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
/**
 * @brief 验证插入方向（单行与批量）把限定表名渲染成与 SELECT 同一个形状
 *
 * @details 这两个方向曾把表名整块引用，于是同一个 "shop.users" 在 SELECT 里指 shop 库的 users 表、
 *          在 INSERT 里指一张名叫 "shop.users" 的表：写入与读取落在两张不同的表上，引擎还不报错。
 */
TEST(SqliteDialectWrite, InsertDirectionsQuoteQualifiedTableNamesSegmentBySegment)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.tableName     = "shop.users";
    node.selectColumns = {"id", "name"};

    const std::vector<DatabaseValue> values{std::int64_t{1}, std::string("Alice")};
    const SqlStatement               single = dialect.translateInsert(node, values);
    EXPECT_NE(single.sql.find("INSERT INTO \"shop\".\"users\""), std::string::npos) << single.sql;

    const std::vector<std::vector<DatabaseValue> > rows{values, values};
    const SqlStatement batch = dialect.translateInsertBatch(node, rows);
    EXPECT_NE(batch.sql.find("INSERT INTO \"shop\".\"users\""), std::string::npos) << batch.sql;

    // 插入方向不带别名："INSERT INTO 表 AS 别名" 是语法错误，别名只能出现在读侧
    EXPECT_EQ(single.sql.find(" AS "), std::string::npos) << single.sql;
}

/**
 * @brief 验证插入方向的空表名在翻译期就被拒，而不是拼出一条 "INSERT INTO \"\""
 * @details 读侧的空表名由 appendTableReference() 拦下并指明「查询树没填表名」；插入方向曾直接整块引用，
 *          失败于是挪到引擎侧，报出的话指不到该填哪个字段。
 */
TEST(SqliteDialectWrite, EmptyTableNameIsRejectedInInsertDirection)
{
    const SqliteDialect dialect;

    QueryNode node;
    node.selectColumns = {"id"};

    const std::vector<DatabaseValue>             values{std::int64_t{1}};
    const std::vector<std::vector<DatabaseValue> > rows{values};

    EXPECT_THROW(static_cast<void>(dialect.translateInsert(node, values)),
                 AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(static_cast<void>(dialect.translateInsertBatch(node, rows)),
                 AsynGyanis::Base::InvalidArgumentException);

    try
    {
        static_cast<void>(dialect.translateInsert(node, values));
        FAIL() << "空表名应当在翻译阶段就被拒绝";
    }
    catch (const AsynGyanis::Base::InvalidArgumentException &failure)
    {
        EXPECT_NE(std::string_view(failure.what()).find("表名为空"), std::string_view::npos) << failure.what();
    }
}

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
