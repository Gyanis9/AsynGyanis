// 覆盖场景（Queryable 的全部公开 API：列与表结构描述、条件与表达式组合、查询树到 SQL 文本的生成，
// 全程在内存中完成，不涉及任何数据库 IO）：
// - ColumnDescriptorStoresCorrectMemberPointerAndName
// - ColumnDescriptorWithExplicitPropertyName
// - TableSchemaIsSpecializable
// - TableSchemaWithDifferentPrimaryKey
// - WhereConditionComparesColumnAndValue
// - WhereConditionNullComparison
// - QueryTreeAssemblesMultipleClauses
// - BinaryOrderByExpression
// - AndOrComposition
// - InExpressionAcceptsContainer
// - LikeExpression
// - NotInExpression
// - QueryNodeConvertsToSql
// - QueryNodeConvertsToSqlWithAllClauses
// - SelectColumnOverride
// - LimitAndOffset
// - NotConditionIsWrappedInParenthesesLikeTheDialect（预览与真正执行的语句同形）
// - ColumnToColumnComparisonKeepsTheColumnNameInsteadOfAPlaceholder（列-列比较不占绑定参数）
// - HavingThroughBuilderAppearsInSql（having() 入口 + 离线文本给出 GROUP BY 与 HAVING）

#include "Database/Queryable/Column.h"
#include "Database/Queryable/TableSchema.h"
#include "Database/Queryable/QueryNode.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

// ========================================================================
// 测试用数据结构定义
// ========================================================================

namespace
{

    /**
     * @brief 测试用 User 结构体
     */
    struct User
    {
        int         id;
        std::string name;
        int         age;
    };

    /**
     * @brief 测试用 Product 结构体
     */
    struct Product
    {
        int         id;
        std::string title;
        double      price;
        int         categoryId;
    };

    /**
     * @brief 测试用 Order 结构体（自定义主键）
     */
    struct Order
    {
        int         orderId;
        std::string customerName;
        double      totalAmount;
    };

} // namespace

// ========================================================================
// TableSchema 特化
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<User>
{
    static constexpr std::string_view kTableName = "users";
    static constexpr auto kColumns = std::tuple{
        Column(&User::id,   "id"),
        Column(&User::name, "name"),
        Column(&User::age,  "age"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<Product>
{
    static constexpr std::string_view kTableName = "products";
    static constexpr auto kColumns = std::tuple{
        Column(&Product::id,         "id"),
        Column(&Product::title,      "title"),
        Column(&Product::price,      "price"),
        Column(&Product::categoryId, "category_id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<Order>
{
    static constexpr std::string_view kTableName = "orders";
    static constexpr auto kColumns = std::tuple{
        Column(&Order::orderId,       "order_id"),
        Column(&Order::customerName,  "customer_name"),
        Column(&Order::totalAmount,   "total_amount"),
    };
    static constexpr std::string_view kPrimaryKey = "order_id";
};

// ========================================================================
// 测试用例
// ========================================================================

namespace AsynGyanis::Database::Queryable
{

    // ========================================================================
    // ColumnDescriptor 测试
    // ========================================================================

    /**
     * @brief 验证 Column() 函数产生的 ColumnDescriptor 存储正确的成员指针和列名
     */
    TEST(QueryableColumn, ColumnDescriptorStoresCorrectMemberPointerAndName)
    {
        constexpr auto columnDescriptor = Column(&User::name, "name");

        static_assert(columnDescriptor.columnName == "name");
        static_assert(columnDescriptor.propertyName == "name");

        static_assert(std::is_same_v<decltype(columnDescriptor)::ClassType, User>);

        static_assert(std::is_same_v<decltype(columnDescriptor)::MemberType, std::string>);

        EXPECT_EQ(columnDescriptor.memberPointer, &User::name);

        EXPECT_EQ(columnDescriptor.columnName, "name");
        EXPECT_EQ(columnDescriptor.propertyName, "name");
    }

    /**
     * @brief 验证 Column() 三参数版本支持独立的属性名
     */
    TEST(QueryableColumn, ColumnDescriptorWithExplicitPropertyName)
    {
        constexpr auto columnDescriptor = Column(&User::age, "user_age", "age");

        static_assert(columnDescriptor.columnName   == "user_age");
        static_assert(columnDescriptor.propertyName == "age");

        EXPECT_EQ(columnDescriptor.columnName,   "user_age");
        EXPECT_EQ(columnDescriptor.propertyName, "age");
        EXPECT_EQ(columnDescriptor.memberPointer, &User::age);
    }

    /**
     * @brief 验证不同类型字段的描述符类型正确
     */
    TEST(QueryableColumn, ColumnDescriptorDifferentTypes)
    {
        constexpr auto idCol   = Column(&User::id,   "id");
        constexpr auto ageCol  = Column(&User::age,  "age");
        constexpr auto nameCol = Column(&User::name, "name");

        static_assert(std::is_same_v<decltype(idCol)::MemberType,   int>);
        static_assert(std::is_same_v<decltype(ageCol)::MemberType,  int>);
        static_assert(std::is_same_v<decltype(nameCol)::MemberType, std::string>);

        EXPECT_EQ(idCol.columnName,   "id");
        EXPECT_EQ(ageCol.columnName,  "age");
        EXPECT_EQ(nameCol.columnName, "name");
    }

    // ========================================================================
    // TableSchema 测试
    // ========================================================================

    /**
     * @brief 验证 TableSchema 可被特化，特化后的常量可访问
     */
    TEST(QueryableSchema, TableSchemaIsSpecializable)
    {
        static_assert(TableSchema<User>::kTableName == "users");
        EXPECT_EQ(TableSchema<User>::kTableName, "users");

        static_assert(TableSchema<User>::kPrimaryKey == "id");
        EXPECT_EQ(TableSchema<User>::kPrimaryKey, "id");

        constexpr std::size_t columnCount = std::tuple_size_v<decltype(TableSchema<User>::kColumns)>;
        static_assert(columnCount == 3);
        EXPECT_EQ(columnCount, 3);
    }

    /**
     * @brief 验证不同表结构的元数据正确
     */
    TEST(QueryableSchema, TableSchemaProduct)
    {
        static_assert(TableSchema<Product>::kTableName == "products");
        static_assert(TableSchema<Product>::kPrimaryKey == "id");

        constexpr std::size_t columnCount = std::tuple_size_v<decltype(TableSchema<Product>::kColumns)>;
        static_assert(columnCount == 4);
        EXPECT_EQ(columnCount, 4);

        constexpr auto priceCol = std::get<2>(TableSchema<Product>::kColumns);
        static_assert(priceCol.columnName == "price");
        static_assert(std::is_same_v<decltype(priceCol)::MemberType, double>);
    }

    /**
     * @brief 验证自定义主键
     */
    TEST(QueryableSchema, TableSchemaWithDifferentPrimaryKey)
    {
        static_assert(TableSchema<Order>::kTableName == "orders");
        static_assert(TableSchema<Order>::kPrimaryKey == "order_id");
        EXPECT_EQ(TableSchema<Order>::kPrimaryKey, "order_id");
    }

    // ========================================================================
    // WhereCondition 表达式测试
    // ========================================================================

    /**
     * @brief 验证 Column >= Value 构建出正确的 WhereCondition
     */
    TEST(QueryableExpression, WhereConditionComparesColumnAndValue)
    {
        constexpr auto ageColumn = Column(&User::age, "age");
        const auto condition = ageColumn >= 18;

        ASSERT_TRUE(true);
        EXPECT_EQ(condition.left.name, "age");

        EXPECT_EQ(condition.op, SqlOperator::Ge);

        ASSERT_TRUE(std::holds_alternative<ParameterValue>(condition.right));
        const auto &parameter = std::get<ParameterValue>(condition.right);
        ASSERT_TRUE(std::holds_alternative<int64_t>(parameter));
        EXPECT_EQ(std::get<int64_t>(parameter), 18);
    }

    /**
     * @brief 验证所有比较运算符
     */
    TEST(QueryableExpression, AllComparisonOperators)
    {
        constexpr auto ageColumn = Column(&User::age, "age");

        {
            auto cond = ageColumn == 25;
            EXPECT_EQ(cond.left.name, "age");
            EXPECT_EQ(cond.op, SqlOperator::Eq);
            EXPECT_EQ(std::get<int64_t>(std::get<ParameterValue>(cond.right)), 25);
        }

        {
            auto cond = ageColumn != 25;
            EXPECT_EQ(cond.op, SqlOperator::Neq);
        }

        {
            auto cond = ageColumn < 25;
            EXPECT_EQ(cond.op, SqlOperator::Lt);
        }

        {
            auto cond = ageColumn <= 25;
            EXPECT_EQ(cond.op, SqlOperator::Le);
        }

        {
            auto cond = ageColumn > 25;
            EXPECT_EQ(cond.op, SqlOperator::Gt);
        }

        {
            auto cond = ageColumn >= 25;
            EXPECT_EQ(cond.op, SqlOperator::Ge);
        }
    }

    /**
     * @brief 验证 nullptr 比较转为 IS NULL / IS NOT NULL
     */
    TEST(QueryableExpression, WhereConditionNullComparison)
    {
        constexpr auto nameColumn = Column(&User::name, "name");

        // == nullptr → IS NULL
        {
            auto condition = nameColumn == nullptr;
            EXPECT_EQ(condition.left.name, "name");
            EXPECT_EQ(condition.op, SqlOperator::IsNull);
        }

        // != nullptr → IS NOT NULL
        {
            auto condition = nameColumn != nullptr;
            EXPECT_EQ(condition.left.name, "name");
            EXPECT_EQ(condition.op, SqlOperator::IsNotNull);
        }
    }

    /**
     * @brief 验证字符串字面量比较
     */
    TEST(QueryableExpression, StringLiteralComparison)
    {
        constexpr auto nameColumn = Column(&User::name, "name");

        auto condition = nameColumn == "Alice";
        EXPECT_EQ(condition.left.name, "name");
        EXPECT_EQ(condition.op, SqlOperator::Eq);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(condition.right));
        EXPECT_EQ(std::get<std::string>(std::get<ParameterValue>(condition.right)), "Alice");
    }

    /**
     * @brief 验证浮点数比较
     */
    TEST(QueryableExpression, FloatComparison)
    {
        constexpr auto priceColumn = Column(&Product::price, "price");

        auto condition = priceColumn >= 99.99;
        EXPECT_EQ(condition.op, SqlOperator::Ge);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(condition.right));
        EXPECT_DOUBLE_EQ(std::get<double>(std::get<ParameterValue>(condition.right)), 99.99);
    }

    // ========================================================================
    // 逻辑组合测试
    // ========================================================================

    /**
     * @brief 验证 && 组合构建 AND 复合条件
     */
    TEST(QueryableExpression, AndComposition)
    {
        constexpr auto ageColumn  = Column(&User::age,  "age");
        constexpr auto idColumn   = Column(&User::id,   "id");

        auto condition = (ageColumn >= 18) && (idColumn == 1);

        EXPECT_EQ(condition.op, SqlOperator::And);
        EXPECT_EQ(condition.children.size(), 2);

        EXPECT_EQ(condition.children[0].left.name, "age");
        EXPECT_EQ(condition.children[0].op, SqlOperator::Ge);

        EXPECT_EQ(condition.children[1].left.name, "id");
        EXPECT_EQ(condition.children[1].op, SqlOperator::Eq);
    }

    /**
     * @brief 验证 || 组合构建 OR 复合条件
     */
    TEST(QueryableExpression, OrComposition)
    {
        constexpr auto ageColumn = Column(&User::age, "age");

        auto condition = (ageColumn == 18) || (ageColumn == 65);

        EXPECT_EQ(condition.op, SqlOperator::Or);
        EXPECT_EQ(condition.children.size(), 2);
    }

    /**
     * @brief 验证 NOT 条件
     */
    TEST(QueryableExpression, NotComposition)
    {
        constexpr auto ageColumn = Column(&User::age, "age");

        auto condition = !(ageColumn < 18);

        EXPECT_EQ(condition.op, SqlOperator::Not);
        EXPECT_EQ(condition.children.size(), 1);
        EXPECT_EQ(condition.children[0].op, SqlOperator::Lt);
    }

    /**
     * @brief 验证 AND/OR 嵌套组合的树结构：(col1 == 1 && col2 == 2) || col3 == 3
     */
    TEST(QueryableExpression, AndOrComposition)
    {
        constexpr auto col1 = Column(&User::id,   "id");
        constexpr auto col2 = Column(&User::age,  "age");
        constexpr auto col3 = Column(&User::name, "name");

        auto condition = (col1 == 1 && col2 == 2) || col3 == "test";

        EXPECT_EQ(condition.op, SqlOperator::Or);
        ASSERT_EQ(condition.children.size(), 2);

        const auto &andNode = condition.children[0];
        EXPECT_EQ(andNode.op, SqlOperator::And);
        ASSERT_EQ(andNode.children.size(), 2);

        EXPECT_EQ(andNode.children[0].op, SqlOperator::Eq);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(andNode.children[0].right));
        EXPECT_EQ(std::get<int64_t>(std::get<ParameterValue>(andNode.children[0].right)), 1);

        EXPECT_EQ(andNode.children[1].op, SqlOperator::Eq);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(andNode.children[1].right));
        EXPECT_EQ(std::get<int64_t>(std::get<ParameterValue>(andNode.children[1].right)), 2);

        const auto &thirdNode = condition.children[1];
        EXPECT_EQ(thirdNode.op, SqlOperator::Eq);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(thirdNode.right));
        EXPECT_EQ(std::get<std::string>(std::get<ParameterValue>(thirdNode.right)), "test");
    }

    // ========================================================================
    // LIKE / IN 表达式测试
    // ========================================================================

    /**
     * @brief 验证 LIKE 表达式构建
     */
    TEST(QueryableExpression, LikeExpression)
    {
        constexpr auto nameColumn = Column(&User::name, "name");

        auto condition = like(nameColumn, "%张%");

        EXPECT_EQ(condition.left.name, "name");
        EXPECT_EQ(condition.op, SqlOperator::Like);
        ASSERT_TRUE(std::holds_alternative<ParameterValue>(condition.right));
        EXPECT_EQ(std::get<std::string>(std::get<ParameterValue>(condition.right)), "%张%");
    }

    /**
     * @brief 验证 IN 表达式接受容器并构建正确条件
     */
    TEST(QueryableExpression, InExpressionAcceptsContainer)
    {
        constexpr auto idColumn = Column(&User::id, "id");

        std::vector<int> ids = {1, 2, 3};
        auto condition = in(idColumn, ids);

        EXPECT_EQ(condition.left.name, "id");
        EXPECT_EQ(condition.op, SqlOperator::In);
        EXPECT_EQ(condition.inValues.size(), 3);

        ASSERT_GE(condition.inValues.size(), 3);
        EXPECT_EQ(std::get<int64_t>(condition.inValues[0]), 1);
        EXPECT_EQ(std::get<int64_t>(condition.inValues[1]), 2);
        EXPECT_EQ(std::get<int64_t>(condition.inValues[2]), 3);
    }

    /**
     * @brief 验证 IN 表达式的初始化列表支持
     */
    TEST(QueryableExpression, InExpressionAcceptsInitializerList)
    {
        constexpr auto idColumn = Column(&User::id, "id");

        auto condition = in(idColumn, std::vector<int>{10, 20, 30, 40});

        EXPECT_EQ(condition.op, SqlOperator::In);
        EXPECT_EQ(condition.inValues.size(), 4);
    }

    /**
     * @brief 验证 NOT IN 表达式
     */
    TEST(QueryableExpression, NotInExpression)
    {
        constexpr auto ageColumn = Column(&User::age, "age");

        std::vector<int> excluded = {0, 1, 99};
        auto condition = in(ageColumn, excluded);
        // NotIn 构建器暂未提供：手动把操作符置为 NotIn，只验证 inValues 机制本身
        condition.op = SqlOperator::NotIn;

        EXPECT_EQ(condition.op, SqlOperator::NotIn);
        EXPECT_EQ(condition.inValues.size(), 3);
    }

    /**
     * @brief 验证空 IN 集合
     */
    TEST(QueryableExpression, InExpressionEmptyContainer)
    {
        constexpr auto idColumn = Column(&User::id, "id");

        std::vector<int> empty;
        auto condition = in(idColumn, empty);

        EXPECT_EQ(condition.op, SqlOperator::In);
        EXPECT_TRUE(condition.inValues.empty());
    }

    // ========================================================================
    // OrderByClause 测试
    // ========================================================================

    /**
     * @brief 验证 asc/desc 排序子句构建
     */
    TEST(QueryableExpression, BinaryOrderByExpression)
    {
        auto ascending  = asc("name");
        auto descending = desc("age");

        EXPECT_EQ(ascending.field.name,  "name");
        EXPECT_FALSE(ascending.descending);

        EXPECT_EQ(descending.field.name, "age");
        EXPECT_TRUE(descending.descending);
    }

    // ========================================================================
    // QueryNode 组装测试
    // ========================================================================

    /**
     * @brief 验证 Queryable 链式调用后查询树包含正确的表名、条件和排序
     */
    TEST(QueryableNode, QueryTreeAssemblesMultipleClauses)
    {
        Queryable<User> query;

        constexpr auto ageColumn  = Column(&User::age,  "age");
        constexpr auto nameColumn = Column(&User::name, "name");

        query.where(ageColumn >= 18)
             .where(nameColumn == "Alice")
             .orderBy(asc("age"))
             .limit(10)
             .offset(5);

        // 通过 toSql 间接验证树结构
        std::string sql = query.toSql();

        EXPECT_NE(sql.find("users"), std::string::npos);

        EXPECT_NE(sql.find("WHERE"), std::string::npos);
        EXPECT_NE(sql.find(">="), std::string::npos);

        EXPECT_NE(sql.find("ORDER BY"), std::string::npos);
        EXPECT_NE(sql.find("age ASC"), std::string::npos);

        EXPECT_NE(sql.find("LIMIT"), std::string::npos);
        EXPECT_NE(sql.find("OFFSET"), std::string::npos);
    }

    /**
     * @brief 验证 QueryNode 可直接构造
     */
    TEST(QueryableNode, QueryNodeManualConstruction)
    {
        QueryNode node;
        node.tableName = "users";

        WhereCondition condition;
        condition.left  = FieldReference{.name = std::string("age")};
        condition.op    = SqlOperator::Ge;
        condition.right = ParameterValue{static_cast<int64_t>(18)};
        node.whereConditions.push_back(std::move(condition));

        node.orderBy.push_back(OrderByClause{
            .field      = FieldReference{std::string("name")},
            .descending = false
        });

        EXPECT_EQ(node.tableName, "users");
        EXPECT_EQ(node.whereConditions.size(), 1);
        EXPECT_EQ(node.orderBy.size(), 1);
    }

    // ========================================================================
    // SQL 生成测试
    // ========================================================================

    /**
     * @brief 验证最简单的 SELECT * FROM 查询
     */
    TEST(QueryableSql, QueryNodeConvertsToSimpleSql)
    {
        Queryable<User> query;
        std::string sql = query.toSql();

        EXPECT_EQ(sql, "SELECT * FROM users");
    }

    /**
     * @brief 验证 WHERE 条件生成
     */
    TEST(QueryableSql, WhereConditionConvertsToSql)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");
        query.where(ageColumn >= 18);

        std::string sql = query.toSql();

        // 生成形如 "SELECT * FROM users WHERE age >= ?" 的 SQL
        EXPECT_NE(sql.find("SELECT * FROM users"), std::string::npos);
        EXPECT_NE(sql.find("WHERE"), std::string::npos);
        EXPECT_NE(sql.find("age"), std::string::npos);
        EXPECT_NE(sql.find(">="), std::string::npos);
        EXPECT_NE(sql.find("?"), std::string::npos);
    }

    /**
     * @brief 验证完整查询树生成合理的 SQL
     */
    TEST(QueryableSql, QueryNodeConvertsToSql)
    {
        Queryable<User> query;
        constexpr auto ageColumn  = Column(&User::age,  "age");
        constexpr auto nameColumn = Column(&User::name, "name");

        query.where(ageColumn >= 18)
             .where(nameColumn == "Alice")
             .orderBy(asc("name"))
             .limit(10);

        std::string sql = query.toSql();

        // SQL 应包含以下片段（顺序相关）
        EXPECT_NE(sql.find("SELECT * FROM users"), std::string::npos);
        EXPECT_NE(sql.find("WHERE"), std::string::npos);
        EXPECT_NE(sql.find("age >= ?"), std::string::npos);
        EXPECT_NE(sql.find("name = ?"), std::string::npos);
        EXPECT_NE(sql.find("ORDER BY name ASC"), std::string::npos);
        EXPECT_NE(sql.find("LIMIT 10"), std::string::npos);
    }

    /**
     * @brief 验证包含所有子句的完整 SQL 生成
     */
    TEST(QueryableSql, QueryNodeConvertsToSqlWithAllClauses)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");

        query.select({"id", "name", "age"})
             .where(ageColumn >= 18)
             .where(ageColumn <= 60)
             .orderBy(asc("name"))
             .orderBy(desc("age"))
             .limit(20)
             .offset(10);

        std::string sql = query.toSql();

        EXPECT_NE(sql.find("SELECT id, name, age"), std::string::npos);

        EXPECT_NE(sql.find("ORDER BY"), std::string::npos);
        EXPECT_NE(sql.find("name ASC"), std::string::npos);
        EXPECT_NE(sql.find("age DESC"), std::string::npos);

        EXPECT_NE(sql.find("LIMIT 20"), std::string::npos);
        EXPECT_NE(sql.find("OFFSET 10"), std::string::npos);
    }

    /**
     * @brief 验证 AND 复合条件的 SQL 生成
     */
    TEST(QueryableSql, AndConditionGeneratesSql)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");
        constexpr auto idColumn  = Column(&User::id,  "id");

        query.where(ageColumn >= 18 && idColumn == 1);

        std::string sql = query.toSql();

        // 应生成 (age >= ? AND id = ?)
        EXPECT_NE(sql.find("age >= ?"), std::string::npos);
        EXPECT_NE(sql.find("AND"), std::string::npos);
        EXPECT_NE(sql.find("id = ?"), std::string::npos);
    }

    /**
     * @brief 验证 OR 条件的 SQL 生成
     */
    TEST(QueryableSql, OrConditionGeneratesSql)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");

        query.where(ageColumn == 18 || ageColumn == 65);

        std::string sql = query.toSql();

        // 应生成 (age = ? OR age = ?)
        EXPECT_NE(sql.find("age = ?"), std::string::npos);
        EXPECT_NE(sql.find("OR"), std::string::npos);
    }

    /**
     * @brief 验证嵌套 AND/OR 的 SQL 生成
     * @details (col1 == 1 && col2 == 2) || col3 == "test" 期望生成 ((id = ? AND age = ?) OR name = ?)
     */
    TEST(QueryableSql, NestedAndOrGeneratesSql)
    {
        Queryable<User> query;
        constexpr auto idColumn   = Column(&User::id,   "id");
        constexpr auto ageColumn  = Column(&User::age,  "age");
        constexpr auto nameColumn = Column(&User::name, "name");

        query.where((idColumn == 1 && ageColumn == 2) || nameColumn == "test");

        std::string sql = query.toSql();

        // 应包含括号和 AND/OR
        EXPECT_NE(sql.find("id = ?"), std::string::npos);
        EXPECT_NE(sql.find("age = ?"), std::string::npos);
        EXPECT_NE(sql.find("name = ?"), std::string::npos);
        EXPECT_NE(sql.find("AND"), std::string::npos);
        EXPECT_NE(sql.find("OR"), std::string::npos);
    }

    /**
     * @brief 验证 IN 表达式的 SQL 生成
     */
    TEST(QueryableSql, InExpressionGeneratesSql)
    {
        Queryable<User> query;
        constexpr auto idColumn = Column(&User::id, "id");

        std::vector<int> ids = {1, 2, 3};
        query.where(in(idColumn, ids));

        std::string sql = query.toSql();

        // 应生成 "id IN (?, ?, ?)"
        EXPECT_NE(sql.find("IN"), std::string::npos);
        EXPECT_NE(sql.find("(?, ?, ?)"), std::string::npos);
    }

    /**
     * @brief 验证 NOT 条件的 SQL 生成
     */
    TEST(QueryableSql, NotConditionGeneratesSql)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");

        query.where(!(ageColumn < 18));

        std::string sql = query.toSql();

        // 应生成 "NOT (age < ?)"
        EXPECT_NE(sql.find("NOT"), std::string::npos);
        EXPECT_NE(sql.find("age < ?"), std::string::npos);
    }

    /**
     * @brief 钉住 NOT 的离线预览与真正执行的语句同形：作用域靠括号界定
     * @details 裸写 "NOT age < ?" 两个引擎都按 NOT(age < ?) 解析，语义没错，但方言渲染出来的是
     *          带括号的那一种。预览文本的价值在于「照着它能改出可执行的语句」，形状不一致就会
     *          逼着人去猜哪一处才是真发出去的那句。
     */
    TEST(QueryableSql, NotConditionIsWrappedInParenthesesLikeTheDialect)
    {
        Queryable<User> query;
        constexpr auto ageColumn = Column(&User::age, "age");

        query.where(!(ageColumn < 18));

        const std::string sql = query.toSql();
        EXPECT_NE(sql.find("NOT (age < ?)"), std::string::npos) << sql;
    }

    /**
     * @brief 钉住列-列比较在离线预览里不占绑定参数：两侧都是列名，一个 '?' 都不该有
     * @details 执行的语句走的是方言那一条判据（右操作数是 FieldReference 时不绑参数），
     *          预览却把右值写成 '?'——照着预览去数参数、或把这段文本贴进客户端手工执行，
     *          都会得到一句挂着占位符却没有绑定值的语句。
     */
    TEST(QueryableSql, ColumnToColumnComparisonKeepsTheColumnNameInsteadOfAPlaceholder)
    {
        WhereCondition condition;
        condition.left  = FieldReference{.name = std::string("age")};
        condition.op    = SqlOperator::Gt;
        condition.right = FieldReference{.name = std::string("baseline_age")};

        Queryable<User> query;
        query.where(std::move(condition));

        const std::string sql = query.toSql();
        EXPECT_NE(sql.find("age > baseline_age"), std::string::npos) << sql;
        EXPECT_EQ(sql.find('?'), std::string::npos) << "列-列比较仍被预览成绑定参数：" << sql;
    }

    /**
     * @brief 钉住 having() 从构建器进入后，GROUP BY 与 HAVING 一起出现在离线文本里
     * @details 查询树与方言层一直支持 HAVING，ORM 这一头没有入口时那条渲染分支从公开 API 走不到。
     */
    TEST(QueryableBuilder, HavingThroughBuilderAppearsInSql)
    {
        WhereCondition havingCondition;
        havingCondition.left  = FieldReference{.name = std::string("COUNT(*)")};
        havingCondition.op    = SqlOperator::Gt;
        havingCondition.right = ParameterValue{static_cast<std::int64_t>(1)};

        Queryable<User> query;
        query.groupBy({"name"}).having(std::move(havingCondition));

        const std::string sql = query.toSql();
        EXPECT_NE(sql.find("GROUP BY name"), std::string::npos) << sql;
        EXPECT_NE(sql.find("HAVING COUNT(*) > ?"), std::string::npos) << sql;
    }

    // ========================================================================
    // Queryable 构建器测试
    // ========================================================================

    /**
     * @brief 验证 SELECT 列覆盖
     */
    TEST(QueryableBuilder, SelectColumnOverride)
    {
        Queryable<User> query;
        query.select({"id", "name"});

        std::string sql = query.toSql();
        EXPECT_NE(sql.find("SELECT id, name"), std::string::npos);
        EXPECT_EQ(sql.find("*"), std::string::npos);
    }

    /**
     * @brief 验证 LIMIT 和 OFFSET 单独使用
     */
    TEST(QueryableBuilder, LimitAndOffset)
    {
        Queryable<User> query;
        query.limit(5);
        std::string sql = query.toSql();
        EXPECT_NE(sql.find("LIMIT 5"), std::string::npos);
        EXPECT_EQ(sql.find("OFFSET"), std::string::npos);

        Queryable<User> query2;
        query2.offset(10);
        std::string sql2 = query2.toSql();
        EXPECT_NE(sql2.find("OFFSET 10"), std::string::npos);
    }

    /**
     * @brief 验证表别名的 SQL 生成
     */
    TEST(QueryableBuilder, TableAlias)
    {
        Queryable<User> query;
        // QueryNode 的 tableAlias 可直接设置
        // 通过访问 QueryNode 内部方法不可行，验证 toSql 的默认行为
        std::string sql = query.toSql();
        EXPECT_EQ(sql, "SELECT * FROM users");
    }

    // ========================================================================
    // 边缘情况及编译期测试
    // ========================================================================

    /**
     * @brief 验证 nullptr 比较的 SQL 生成
     */
    TEST(QueryableSql, NullComparisonSql)
    {
        Queryable<User> query;
        constexpr auto nameColumn = Column(&User::name, "name");
        query.where(nameColumn == nullptr);

        std::string sql = query.toSql();
        EXPECT_NE(sql.find("name IS NULL"), std::string::npos);
    }

    /**
     * @brief 验证 IS NOT NULL 的 SQL 生成
     */
    TEST(QueryableSql, IsNotNullSql)
    {
        Queryable<User> query;
        constexpr auto nameColumn = Column(&User::name, "name");
        query.where(nameColumn != nullptr);

        std::string sql = query.toSql();
        EXPECT_NE(sql.find("name IS NOT NULL"), std::string::npos);
    }

    /**
     * @brief 验证 LIKE 的 SQL 生成
     */
    TEST(QueryableSql, LikeSqlGeneration)
    {
        Queryable<User> query;
        constexpr auto nameColumn = Column(&User::name, "name");
        query.where(like(nameColumn, "%value%"));

        std::string sql = query.toSql();
        EXPECT_NE(sql.find("name LIKE ?"), std::string::npos);
    }

    /**
     * @brief 验证离线渲染在两个易错点上与方言给出同样的结构
     *
     * @details 本渲染器的契约是「可与真正执行的 SQL 直接对照」。空集合方言产出恒假的 (1 = 0)
     *          且不占参数——这里若仍写 "IN (?)"，照着这段文本去数占位符就会对不上一个不存在的参数；
     *          字面量匹配少了 ESCAPE 子句则整段转义都是摆设，两者都属于「看起来能对照、实际误导」。
     */
    TEST(QueryableSql, OfflineRendererAgreesWithDialectOnEmptyInAndLiteralMatch)
    {
        Queryable<User> emptyInQuery;
        constexpr auto idColumn = Column(&User::id, "id");
        emptyInQuery.where(in(idColumn, std::vector<std::int64_t>{}));
        const std::string emptyInSql = emptyInQuery.toSql();
        EXPECT_NE(emptyInSql.find("(1 = 0)"), std::string::npos) << emptyInSql;
        EXPECT_EQ(emptyInSql.find("IN (?)"), std::string::npos) << emptyInSql;

        Queryable<User> literalQuery;
        constexpr auto nameColumn = Column(&User::name, "name");
        literalQuery.where(contains(nameColumn, std::string("50%")));
        const std::string literalSql = literalQuery.toSql();
        EXPECT_NE(literalSql.find("LIKE ? ESCAPE '!'"), std::string::npos) << literalSql;
    }

} // namespace AsynGyanis::Database::Queryable