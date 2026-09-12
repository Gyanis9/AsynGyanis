/**
 * @file TestSchemaMigrator.cpp
 * @brief 建表迁移工具测试 —— 离线 DDL 文本断言 + 内存 SQLite 端到端
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 两个层次：
 * - 离线：只用 createTableStatement() / dropTableStatement() 生成文本并逐字断言，
 *   覆盖类型映射（含两种等价的二进制成员拼法都落到 BLOB/LONGBLOB）、可空规则、主键、
 *   标识符引用（表名与列名都含空格）与 IF NOT EXISTS/IF EXISTS；
 * - 端到端（内存 SQLite）：SchemaMigrator 建表 → ORM 写入/读回 → tableExists → dropTable →
 *   tableExists 变假，并验证重复建表幂等、不带 IF NOT EXISTS 时对已存在表如实失败；
 *   另有二进制列的专项用例：用 typeof() 断言存储类确实是 blob（声明成 BLOB 但按文本绑定时
 *   存储类会是 text，只看 DDL 发现不了）、两种拼法逐字节往返、零长载荷与 NULL 可区分、
 *   以及按二进制列做参数化条件查询（走的是与写入不同的那套参数变体）。
 *
 * 真实 MySQL 服务端的同类验证放在 tests/Database/MySql/TestMySqlIntegration.cpp
 * （沿用既有的环境变量门控：未设置 ASYN_MYSQL_TEST_PASSWORD 即跳过）。
 */
#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Dialect/MySqlDialect.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// ========================================================================
// 测试用数据结构与 TableSchema 特化
// ========================================================================

namespace
{
    // 结构体在文件的 using 块之前定义，因此这里单独引入二进制载荷的别名，
    // 让下面两个结构体的成员声明能直接用 BinaryBytes
    using AsynGyanis::Database::BinaryBytes;

    /**
     * @brief 覆盖全部逻辑列类型的结构体（表名与列名都含空格，顺带验证标识符引用）
     */
    struct MigratedAccountRow
    {
        std::int64_t               id;         ///< 主键（Int64 → NOT NULL + PRIMARY KEY）
        std::string                name;       ///< 文本列 → NOT NULL
        std::optional<std::string> note;       ///< 可空文本列 → 不加 NOT NULL
        double                     balance;    ///< 浮点列 → NOT NULL
        bool                       active;     ///< 布尔列 → NOT NULL（SQLite 用 INTEGER，MySQL 用 TINYINT(1)）
        std::uint64_t              sequence;   ///< 无符号整型列 → NOT NULL
        BinaryBytes                payload;    ///< 二进制列（规范拼法）→ NOT NULL
        std::vector<std::byte>     rawPayload; ///< 二进制列（等价拼法）→ 落到同一个列类型
    };

    /**
     * @brief 端到端用例的结构体：三列，够验证「建表 → ORM 读写 → 删表」这条链路
     */
    struct MigratedUserRow
    {
        std::int64_t               id;   ///< 主键
        std::string                name; ///< 户名（含中文）
        std::optional<std::string> note; ///< 备注，可空
    };

    /**
     * @brief 二进制列端到端用例的结构体：两种拼法各占一列，另加一列可空二进制
     */
    struct MigratedBinaryRow
    {
        std::int64_t               id;         ///< 主键
        BinaryBytes                payload;    ///< 二进制列（规范拼法，NOT NULL）
        std::vector<std::byte>     rawPayload; ///< 二进制列（等价拼法，NOT NULL）
        std::optional<BinaryBytes> note;       ///< 可空二进制列：用于区分「零长载荷」与「NULL」
    };

    /**
     * @brief 主键列名与 kColumns 中的列名拼写不一致的结构体
     */
    struct BrokenPrimaryKeyRow
    {
        std::int64_t id; ///< 列名是 "id"
    };

    /**
     * @brief 未填表名的结构体：kTableName 保持主模板的空串
     */
    struct MissingTableNameRow
    {
        std::int64_t id; ///< 唯一一列
    };

} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MigratedAccountRow>
{
    // 表名与列名都含空格：DDL 与 DML 的引用规则必须一致，否则建出来的表根本查不动
    static constexpr std::string_view kTableName = "migrated accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&MigratedAccountRow::id,         "id"),
        Column(&MigratedAccountRow::name,       "full name"),
        Column(&MigratedAccountRow::note,       "note text"),
        Column(&MigratedAccountRow::balance,    "balance"),
        Column(&MigratedAccountRow::active,     "active"),
        Column(&MigratedAccountRow::sequence,   "sequence"),
        Column(&MigratedAccountRow::payload,    "payload bytes"),
        Column(&MigratedAccountRow::rawPayload, "raw payload"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MigratedUserRow>
{
    static constexpr std::string_view kTableName = "migrated users";
    static constexpr auto kColumns = std::tuple{
        Column(&MigratedUserRow::id,   "id"),
        Column(&MigratedUserRow::name, "name"),
        Column(&MigratedUserRow::note, "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MigratedBinaryRow>
{
    static constexpr std::string_view kTableName = "migrated binary";
    static constexpr auto kColumns = std::tuple{
        Column(&MigratedBinaryRow::id,         "id"),
        Column(&MigratedBinaryRow::payload,    "payload"),
        Column(&MigratedBinaryRow::rawPayload, "raw_payload"),
        Column(&MigratedBinaryRow::note,       "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<BrokenPrimaryKeyRow>
{
    static constexpr std::string_view kTableName = "broken primary key";
    static constexpr auto kColumns = std::tuple{
        Column(&BrokenPrimaryKeyRow::id, "id"),
    };
    // 大小写不一致：kColumns 里是 "id"，这里写成 "Id"
    static constexpr std::string_view kPrimaryKey = "Id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MissingTableNameRow>
{
    // 显式留空：完全特化不会继承主模板的默认值，忘填表名时建表必须失败而不是生成 "CREATE TABLE """
    static constexpr std::string_view kTableName = "";
    static constexpr auto kColumns = std::tuple{
        Column(&MissingTableNameRow::id, "id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

// ========================================================================
// 离线：DDL 文本断言
// ========================================================================

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::DatabaseResult;
    using AsynGyanis::Database::MySqlDialect;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::SqlStatement;
    using AsynGyanis::Database::SqliteDialect;
    using AsynGyanis::Database::Queryable::asc;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::Queryable::SchemaMigrator;
} // namespace

/**
 * @brief 验证 SQLite 建表语句的类型映射、可空规则、主键与标识符引用
 */
TEST(SchemaMigratorOffline, SqliteCreateTableStatementMapsTypesAndConstraints)
{
    const SqliteDialect dialect;
    const SqlStatement   statement = SchemaMigrator::createTableStatement<MigratedAccountRow>(dialect);

    // 逐项对照：optional 列不加 NOT NULL，其余列 NOT NULL；主键列加 PRIMARY KEY；
    // 布尔与无符号整数在 SQLite 上都落在 INTEGER（没有对应的独立存储类）；
    // 两种二进制成员拼法（vector<uint8_t> / vector<byte>）都落到 BLOB
    EXPECT_EQ(statement.sql,
              "CREATE TABLE IF NOT EXISTS \"migrated accounts\" ("
              "\"id\" INTEGER NOT NULL PRIMARY KEY, "
              "\"full name\" TEXT NOT NULL, "
              "\"note text\" TEXT, "
              "\"balance\" REAL NOT NULL, "
              "\"active\" INTEGER NOT NULL, "
              "\"sequence\" INTEGER NOT NULL, "
              "\"payload bytes\" BLOB NOT NULL, "
              "\"raw payload\" BLOB NOT NULL)");

    // DDL 不含任何字段值，参数列表必须为空（列定义全是标识符与类型名，没有外部数据）
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证 MySQL 建表语句的类型映射与 SQLite 的差异（引用符、位宽、无符号、布尔）
 */
TEST(SchemaMigratorOffline, MySqlCreateTableStatementMapsTypesAndConstraints)
{
    const MySqlDialect dialect;
    const SqlStatement  statement = SchemaMigrator::createTableStatement<MigratedAccountRow>(dialect);

    // 与 SQLite 的三处关键差异：反引号引用、整数按位宽分家、无符号整数用 BIGINT UNSIGNED、
    // 布尔用官方惯例的 TINYINT(1)；二进制用 LONGBLOB（上限 4 GiB）
    EXPECT_EQ(statement.sql,
              "CREATE TABLE IF NOT EXISTS `migrated accounts` ("
              "`id` BIGINT NOT NULL PRIMARY KEY, "
              "`full name` TEXT NOT NULL, "
              "`note text` TEXT, "
              "`balance` DOUBLE NOT NULL, "
              "`active` TINYINT(1) NOT NULL, "
              "`sequence` BIGINT UNSIGNED NOT NULL, "
              "`payload bytes` LONGBLOB NOT NULL, "
              "`raw payload` LONGBLOB NOT NULL)");
    EXPECT_TRUE(statement.parameters.empty());
}

/**
 * @brief 验证 ifNotExists / ifExists 开关能关掉对应的可选子句
 */
TEST(SchemaMigratorOffline, OptionalClausesRespectTheirFlags)
{
    const SqliteDialect dialect;

    const SqlStatement plainCreate =
        SchemaMigrator::createTableStatement<MigratedUserRow>(dialect, false);
    EXPECT_EQ(plainCreate.sql,
              "CREATE TABLE \"migrated users\" (\"id\" INTEGER NOT NULL PRIMARY KEY, "
              "\"name\" TEXT NOT NULL, \"note\" TEXT)");

    const SqlStatement guardedDrop = SchemaMigrator::dropTableStatement<MigratedUserRow>(dialect);
    EXPECT_EQ(guardedDrop.sql, "DROP TABLE IF EXISTS \"migrated users\"");
    EXPECT_TRUE(guardedDrop.parameters.empty());

    const SqlStatement plainDrop = SchemaMigrator::dropTableStatement<MigratedUserRow>(dialect, false);
    EXPECT_EQ(plainDrop.sql, "DROP TABLE \"migrated users\"");

    // MySQL 侧的删表语句同样只差引用符
    const MySqlDialect mySqlDialect;
    EXPECT_EQ(SchemaMigrator::dropTableStatement<MigratedUserRow>(mySqlDialect).sql,
              "DROP TABLE IF EXISTS `migrated users`");
}

/**
 * @brief 验证表结构不合法时抛出可读的中文逻辑错误
 */
TEST(SchemaMigratorOffline, InvalidSchemaIsRejectedWithLocalizedReason)
{
    const SqliteDialect dialect;

    // 主键列名在 kColumns 里不存在：静默建出无主键表会让「按主键更新/删除」在运行期才暴露
    try
    {
        static_cast<void>(SchemaMigrator::createTableStatement<BrokenPrimaryKeyRow>(dialect));
        FAIL() << "主键列名与任何列名都不一致，应当抛出 std::logic_error";
    }
    catch (const std::logic_error &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("Id"), std::string::npos) << message;
        EXPECT_NE(message.find("主键"), std::string::npos) << message;
    }

    // 未特化 kTableName：生成 "CREATE TABLE """ 毫无意义，必须在生成之前就失败
    EXPECT_THROW(static_cast<void>(SchemaMigrator::createTableStatement<MissingTableNameRow>(dialect)),
                 std::logic_error);
    EXPECT_THROW(static_cast<void>(SchemaMigrator::dropTableStatement<MissingTableNameRow>(dialect)),
                 std::logic_error);
}

// ========================================================================
// 端到端：内存 SQLite
// ========================================================================

namespace
{
    /**
     * @brief 建表迁移的端到端测试夹具
     *
     * @details 内存库不跨连接共享，因此把池上限压到 1，保证整个用例复用同一条连接
     *          （即同一份内存库）。夹具不预建任何表：建表由 SchemaMigrator 负责。
     */
    class SchemaMigratorSqliteTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            PoolConfig poolConfiguration;
            poolConfiguration.maximumPoolSize = 1;

            m_pool = std::make_unique<ConnectionPool>(
                []()
                {
                    auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                    // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                    connection->connect();
                    return connection;
                },
                poolConfiguration);
        }

        std::unique_ptr<ConnectionPool> m_pool; ///< 用例独占的连接池
    };
} // namespace

/**
 * @brief 验证 createTable → ORM 读写 → tableExists → dropTable 全链路
 */
TEST_F(SchemaMigratorSqliteTest, CreateTableThenOrmRoundTripThenDrop)
{
    std::string errorText;

    // 第一步：建表（表名与列名都含空格，DDL 的引用规则与 DML 共用同一份实现）
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, true, &errorText)) << errorText;
    EXPECT_TRUE(errorText.empty()) << errorText;
    ASSERT_TRUE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText)) << errorText;

    // 第二步：SchemaMigrator 建出来的表必须能被 ORM 直接读写（列名、类型、可空全部对齐）
    {
        Queryable<MigratedUserRow> insertQuery(*m_pool);
        ASSERT_EQ(1, insertQuery.insert(MigratedUserRow{1, "张三", std::string("首条")}));
        ASSERT_EQ(1, insertQuery.insert(MigratedUserRow{2, "Li Si", std::nullopt}));
    }

    {
        Queryable<MigratedUserRow> query(*m_pool);
        const std::vector<MigratedUserRow> rows = query.orderBy(asc("id")).toList();

        ASSERT_EQ(rows.size(), 2U);
        EXPECT_EQ(rows[0].name, "张三");
        ASSERT_TRUE(rows[0].note.has_value());
        EXPECT_EQ(rows[0].note.value(), "首条");
        EXPECT_EQ(rows[1].name, "Li Si");
        EXPECT_FALSE(rows[1].note.has_value());
    }

    // 第三步：重复建表（IF NOT EXISTS）幂等——返回 true 且已写入的数据不受影响
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool)) << errorText;
    {
        Queryable<MigratedUserRow> countQuery(*m_pool);
        EXPECT_EQ(countQuery.count(), 2);
    }

    // 第四步：删表后表不存在；errorText 必须保持为空，表示「确实不存在」而不是查询失败
    ASSERT_TRUE(SchemaMigrator::dropTable<MigratedUserRow>(*m_pool, true, &errorText)) << errorText;
    EXPECT_FALSE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText));
    EXPECT_TRUE(errorText.empty()) << errorText;

    // 再删一次（IF EXISTS）依旧算成功：目标状态已经达成
    EXPECT_TRUE(SchemaMigrator::dropTable<MigratedUserRow>(*m_pool)) << errorText;
}

/**
 * @brief 验证二进制列的端到端链路：真 BLOB 落库、两种拼法往返、空载荷与 NULL 可区分、可作条件
 *
 * @details 这条用例是二进制成员链路的验收核心。DDL 侧声明成 BLOB 只决定「列怎么声明」，
 *          值是否真的以 BLOB 存储取决于**绑定方式**（sqlite3_bind_blob）。因此这里用
 *          typeof() 断言存储类：列声明为 BLOB 却按文本绑定时，列的类型仍是 BLOB，
 *          但存储类会是 text——只看 DDL 是发现不了的。
 */
TEST_F(SchemaMigratorSqliteTest, BinaryColumnsRoundTripAndAreStoredAsRealBlob)
{
    std::string errorText;
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedBinaryRow>(*m_pool, true, &errorText)) << errorText;

    // 载荷里同时有内嵌 '\0' 与单字节上界 0xFF，覆盖最容易在搬运中被截断或改写的两种形态
    const BinaryBytes            payload{0x5C, 0x00, 0x41, 0xFF};
    const std::vector<std::byte> rawPayload{std::byte{0x01}, std::byte{0x80}};

    {
        Queryable<MigratedBinaryRow> insertQuery(*m_pool);
        // 第 1 行：两个二进制成员都有值，note 是「有值且长度为 0」
        ASSERT_EQ(1, insertQuery.insert(MigratedBinaryRow{1, payload, rawPayload, BinaryBytes{}}));
        // 第 2 行：两个二进制成员都是零长，note 是 NULL——与上一行构成对照
        ASSERT_EQ(1, insertQuery.insert(MigratedBinaryRow{2, BinaryBytes{}, std::vector<std::byte>{}, std::nullopt}));
    }

    // 存储类必须是 blob：这是「按 BLOB 绑定」与「按文本绑定」的分水岭。
    // 顺带验证零长载荷没有被绑成 SQL NULL——列是 NOT NULL，绑成 NULL 会直接插入失败
    {
        PooledConnection                      connection  = m_pool->acquire();
        const std::unique_ptr<DatabaseResult> storedTypes = connection->execute(
                "SELECT typeof(payload), typeof(raw_payload) FROM \"migrated binary\" WHERE id = 2");
        ASSERT_NE(storedTypes, nullptr) << connection->lastError();
        ASSERT_TRUE(storedTypes->next());

        const auto payloadTypeValue = storedTypes->getValue(std::size_t{0});
        const auto rawTypeValue     = storedTypes->getValue(std::size_t{1});
        const auto *payloadType = std::get_if<std::string>(&payloadTypeValue);
        const auto *rawType     = std::get_if<std::string>(&rawTypeValue);
        ASSERT_NE(payloadType, nullptr) << "typeof(payload) 不是文本类型";
        ASSERT_NE(rawType, nullptr) << "typeof(raw_payload) 不是文本类型";
        EXPECT_EQ(*payloadType, "blob");
        EXPECT_EQ(*rawType, "blob");
    }

    Queryable<MigratedBinaryRow>         query(*m_pool);
    const std::vector<MigratedBinaryRow> rows = query.orderBy(asc("id")).toList();
    ASSERT_EQ(rows.size(), 2U);

    // 两种拼法都逐字节无损往返
    EXPECT_EQ(rows[0].payload, payload);
    EXPECT_EQ(rows[0].rawPayload, rawPayload);
    EXPECT_EQ(rows[1].payload, BinaryBytes{});
    EXPECT_EQ(rows[1].rawPayload, std::vector<std::byte>{});

    // 零长 BLOB 与 NULL 必须区分：前者是「有值且长度为 0」，后者是「没有值」
    ASSERT_TRUE(rows[0].note.has_value());
    EXPECT_TRUE(rows[0].note->empty());
    EXPECT_FALSE(rows[1].note.has_value());

    // 按二进制列做条件查询：这条路径走的是 QueryNode::ParameterValue → convertParameter，
    // 与写入路径是两套变体，只接通其中一条不会让这里通过
    const std::vector<MigratedBinaryRow> matched =
        query.where(Column(&MigratedBinaryRow::payload, "payload") == payload).toList();
    ASSERT_EQ(matched.size(), 1U);
    EXPECT_EQ(matched[0].id, 1);
    EXPECT_EQ(matched[0].payload, payload);
}

/**
 * @brief 验证不带 IF NOT EXISTS 时对已存在的表如实失败并给出可读原因
 */
TEST_F(SchemaMigratorSqliteTest, CreateTableWithoutIfNotExistsFailsOnExistingTable)
{
    std::string errorText;
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, false, &errorText)) << errorText;

    // 表已存在：SQLite 会直接报 "table ... already exists"，这里转成 false + 中文原因
    errorText.clear();
    EXPECT_FALSE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, false, &errorText));
    EXPECT_FALSE(errorText.empty());
    EXPECT_NE(errorText.find("DDL 执行失败"), std::string::npos) << errorText;
}

/**
 * @brief 验证 tableExists 对不存在的表返回 false 且不写入错误原因
 */
TEST_F(SchemaMigratorSqliteTest, TableExistsIsFalseForUnknownTable)
{
    std::string errorText;

    // 表从未建过：false 表示「确实不存在」，因此 errorText 必须留空
    EXPECT_FALSE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText));
    EXPECT_TRUE(errorText.empty()) << errorText;

    // 建表后立刻变真，删表后立刻变假（同一连接、同一份内存库，不存在可见性延迟）
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, true, &errorText)) << errorText;
    EXPECT_TRUE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText)) << errorText;

    ASSERT_TRUE(SchemaMigrator::dropTable<MigratedUserRow>(*m_pool, true, &errorText)) << errorText;
    EXPECT_FALSE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText));
    EXPECT_TRUE(errorText.empty()) << errorText;
}

/**
 * @brief 验证成功返回的调用会清空出参，不会把上一次失败的原因留给调用方
 *
 * @details 该场景最早在真机 MySQL 用例上暴露：调用方复用同一个字符串跨多次调用时，
 *          若成功路径不清空出参，tableExists() 赖以区分「表不存在」与「查询失败」的判据
 *          就会失效——一次建表失败留下的原因会被误读成本次查询失败。
 */
TEST_F(SchemaMigratorSqliteTest, SuccessfulCallClearsStaleErrorFromPreviousFailure)
{
    std::string errorText;

    // 先制造一次真实失败：不带 IF NOT EXISTS 建表两次，第二次必然失败并写入原因
    ASSERT_TRUE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, false, &errorText)) << errorText;
    EXPECT_FALSE(SchemaMigrator::createTable<MigratedUserRow>(*m_pool, false, &errorText));
    ASSERT_FALSE(errorText.empty());

    // 随后的成功调用必须把上一次的原因清掉，否则调用方无法凭出参判断本次结果
    ASSERT_TRUE(SchemaMigrator::dropTable<MigratedUserRow>(*m_pool, true, &errorText)) << errorText;
    EXPECT_TRUE(errorText.empty()) << errorText;

    // 于是「表确实不存在」这一路径上，出参为空才真正代表「查询成功但没有这张表」
    EXPECT_FALSE(SchemaMigrator::tableExists<MigratedUserRow>(*m_pool, &errorText));
    EXPECT_TRUE(errorText.empty()) << errorText;
}
