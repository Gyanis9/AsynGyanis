// 覆盖场景（内存 SQLite 建连接池，建表后**完全通过 ORM** 完成插入/查询/排序/分页/计数/更新/删除）：
// - ToSqlStaysOfflineGenerator / OfflineModeThrowsOnExecution
// - OrmInsertThenQueryWithWhereOrderAndLimit
// - HostileTextRoundTripsThroughParameterBinding
// - NullColumnMapsToEmptyOptional / EmptyStringStaysDistinctFromNull
// - FirstReturnsEmptyWhenNoRowMatches
// - CountMatchesFilteredRows
// - UpdateByPrimaryKeyChangesOnlyTargetRow
// - ExecuteNonQueryDeletesMatchingRows
// - DeleteWithLimitIsRejectedInsteadOfDeletingEverything（有界删除不能静默做成全表删除）
// - UnregisteredTypeNamesTheMissingSchemaSpecialization（空 kColumns 要说成真因，不推给自增主键）
// - SpacedIdentifiersSurviveCreateInsertAndQuery（表名与列名含空格的建表 + 读写全链路）
// - JoinThroughBuilderNarrowsRowsByTheJoinedTable / GroupByThroughBuilderAggregatesAndMapsAliasColumn
//   （join() 与 groupBy() 这两个公开写入口从 ORM 这头跑通，不是只喂手工搭的查询树）
// - HavingThroughBuilderFiltersGroups（having() 入口真的在分组后筛组，对照组是全量三组）
// - MissingColumnThrowsReadableError / TypeMismatchThrowsReadableError
// - DuplicateColumnNamesDoNotAliasTwoMembersOntoOneColumn（两个成员撞同一列名必须报错）
// - LiteralMatchHelpersTreatWildcardsAsLiteralText（contains/startsWith/endsWith 把 % _ ! 按字面量匹配，
//   并保留 like() 的通配符语义作为对照组）
// 断言映射回的结构体字段值正确（含 NULL 列、字符串、浮点、负数、中文），并验证取值确实以绑定方式传入：
// 含单引号与 "--" 的文本能原样查回、注入残留的表仍存在，证明没有拼接 SQL。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// ========================================================================
// 测试用数据结构
// ========================================================================

namespace
{
    /**
     * @brief 账户表对应的结构体（字段顺序与建表语句一致，便于人工对照）
     */
    struct AccountRow
    {
        std::int64_t               id;      ///< 主键
        std::string                name;    ///< 户名（含中文与特殊字符测试）
        double                     balance; ///< 余额（含负数测试）
        std::optional<std::string> note;    ///< 备注，可空（用于验证 NULL ↔ std::optional）
        bool                       active;  ///< 是否启用（SQLite 用 INTEGER 的 0/1 存）
    };

    /**
     * @brief 列缺失测试用结构体：声明了表里不存在的列
     */
    struct BrokenColumnRow
    {
        std::int64_t id;            ///< 存在
        std::string  missingColumn; ///< accounts 表里没有这一列
    };

    /**
     * @brief 类型不匹配测试用结构体：把 INTEGER 列映射成 std::string
     */
    struct TypeMismatchRow
    {
        std::string id; ///< accounts.id 是 INTEGER，映射到 std::string 应当失败
    };

    /**
     * @brief 撞名测试用结构体：两个成员声明了同一个列名
     *
     * @details 真实场景是 JOIN 两张都有 name 列的表、或 struct 改字段时漏改列名。结果集里出现两个
     *          同名列时按名解析都指向同一列，两个成员会静默拿到同一个值——必须报错而不是照收。
     */
    struct DuplicatedColumnNameRow
    {
        std::int64_t id;         ///< 主键
        std::string  firstName;  ///< 列名写成 "name"
        std::string  secondName; ///< 列名同样写成 "name"，与上一个成员撞名
    };

    /**
     * @brief 含空格标识符测试用结构体：表名与列名都带空格
     *
     * @details 含空格的标识符在真实库里确实存在（被引号包住的标识符允许含空格）。
     *          空格既不能出现在裸标识符里，也不会改变表达式结构，因此方言必须把它
     *          判为标识符并整段加引号；一旦漏引号，SQLite 会把它当成语法错误而整条语句失败。
     */
    struct SpacedIdentifierRow
    {
        std::int64_t               id;          ///< 主键
        std::string                fullName;    ///< 列名含空格（"full name"）
        std::optional<std::string> homeAddress; ///< 列名含空格且可空（"home address"）
    };

    /**
     * @brief 无符号 64 位列测试用结构体：覆盖 int64 之内与之外两段取值
     *
     * @details UInt64 是唯一在三个引擎上都「没有原生对应类型」的成员类型：SQLite 只有 64 位有符号整数，MySQL 有
     *          BIGINT UNSIGNED 但它的上界超过 int64，驱动只能以文本返回。这个结构体用于验证两侧边界：int64 能表达的取值必须
     *          无损往返，表达不了的取值必须**明确失败**而不是悄悄换个数值。
     */
    struct UnsignedCounterRow
    {
        std::int64_t  id;       ///< 主键
        std::uint64_t sequence; ///< 无符号 64 位计数，取值域 0 .. 2^64-1
    };

    /**
     * @brief 构造一行账户示例数据
     * @param id 主键
     * @param name 户名
     * @param balance 余额
     * @param note 备注，可为空
     * @param active 是否启用
     * @return AccountRow 结构体
     */
    [[nodiscard]] AccountRow makeRow(const std::int64_t id,
                                     std::string name,
                                     const double balance,
                                     std::optional<std::string> note,
                                     const bool active)
    {
        return AccountRow{
            .id      = id,
            .name    = std::move(name),
            .balance = balance,
            .note    = std::move(note),
            .active  = active
        };
    }

    /**
     * @brief GROUP BY 的结果行：户名 + 该户名下的账户数
     *
     * @details 分组查询的投影与表结构不同构，因此需要一个自己的行类型；结果列靠 "cnt" 这个
     *          别名对上成员，正好把「表达式列 + 别名」这条映射路径也走一遍。
     */
    struct NameCountRow
    {
        std::string  name;     ///< 分组列
        std::int64_t rowCount; ///< COUNT(*) AS cnt 的结果
    };

} // namespace

// ========================================================================
// TableSchema 特化
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AccountRow>
{
    static constexpr std::string_view kTableName = "accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&AccountRow::id,   "id"),
        Column(&AccountRow::name, "name"),
        Column(&AccountRow::balance, "balance"),
        Column(&AccountRow::note, "note"),
        Column(&AccountRow::active, "active"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<NameCountRow>
{
    // 分组结果仍然来自 accounts 表，只是投影换成了「户名 + 该户名的行数」
    static constexpr std::string_view kTableName = "accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&NameCountRow::name,     "name"),
        Column(&NameCountRow::rowCount, "cnt"),
    };
    static constexpr std::string_view kPrimaryKey = "name";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<BrokenColumnRow>
{
    // 故意复用 accounts 表：表存在、但结构体声明了一个表里没有的列
    static constexpr std::string_view kTableName = "accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&BrokenColumnRow::id,            "id"),
        Column(&BrokenColumnRow::missingColumn, "missing_column"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<TypeMismatchRow>
{
    static constexpr std::string_view kTableName = "accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&TypeMismatchRow::id, "id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<DuplicatedColumnNameRow>
{
    // 两个成员写同一个列名：结果集里若有两个同名列，按名解析会让二者落到同一列
    static constexpr std::string_view kTableName = "accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&DuplicatedColumnNameRow::id,         "id"),
        Column(&DuplicatedColumnNameRow::firstName,  "name"),
        Column(&DuplicatedColumnNameRow::secondName, "name"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<SpacedIdentifierRow>
{
    // 表名与列名一律含空格：只有把标识符整段引用起来，SQLite 才会把它们当成名字而不是语法
    static constexpr std::string_view kTableName = "spaced accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&SpacedIdentifierRow::id,          "id"),
        Column(&SpacedIdentifierRow::fullName,    "full name"),
        Column(&SpacedIdentifierRow::homeAddress, "home address"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<UnsignedCounterRow>
{
    static constexpr std::string_view kTableName = "unsigned counters";
    static constexpr auto kColumns = std::tuple{
        Column(&UnsignedCounterRow::id,       "id"),
        Column(&UnsignedCounterRow::sequence, "sequence"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

// ========================================================================
// 夹具
// ========================================================================

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::Queryable::asc;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::contains;
    using AsynGyanis::Database::Queryable::desc;
    using AsynGyanis::Database::Queryable::endsWith;
    using AsynGyanis::Database::Queryable::FieldReference;
    using AsynGyanis::Database::Queryable::in;
    using AsynGyanis::Database::Queryable::JoinClause;
    using AsynGyanis::Database::Queryable::JoinType;
    using AsynGyanis::Database::Queryable::like;
    using AsynGyanis::Database::Queryable::mapResultRows;
    using AsynGyanis::Database::Queryable::ParameterValue;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::Queryable::SchemaMigrator;
    using AsynGyanis::Database::Queryable::SqlOperator;
    using AsynGyanis::Database::Queryable::startsWith;
    using AsynGyanis::Database::Queryable::WhereCondition;

    /**
     * @brief ORM 端到端测试夹具
     *
     * @details 每个用例构造一份独立的内存库：SQLite 的 ":memory:" 数据库随连接生命周期存在，
     *          因此把池上限压到 1，保证整个用例复用同一条连接（同一份内存库），
     *          既避免多条连接各持一份内存库的错觉，也让用例之间完全隔离。
     */
    class QueryableExecutionTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            PoolConfig poolConfiguration;
            // 上限 1：内存库不跨连接共享，必须保证用例内只有一条连接
            poolConfiguration.maximumPoolSize = 1;

            m_pool = std::make_unique<ConnectionPool>(
                []()
                {
                    auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                    // 连接池的工厂契约要求交出「已经 connect() 完成」的连接，池不会替调用方连接
                    connection->connect();
                    return connection;
                },
                poolConfiguration);

            // 建表走原生 SQL：DDL 不由 ORM 生成，这一步只负责准备好被 ORM 操作的表
            PooledConnection connection = m_pool->acquire();
            ASSERT_TRUE(connection);
            const auto createResult = connection->execute(
                "CREATE TABLE accounts ("
                "id INTEGER PRIMARY KEY, "
                "name TEXT NOT NULL, "
                "balance REAL, "
                "note TEXT, "
                "active INTEGER NOT NULL)");
            ASSERT_TRUE(createResult != nullptr) << connection->lastError();
        }

        /**
         * @brief 新建一个绑定本夹具连接池的 ORM 查询对象
         * @return Queryable<AccountRow> 干净状态的查询对象（不复用条件）
         */
        [[nodiscard]] Queryable<AccountRow> newQuery() const
        {
            return Queryable<AccountRow>(*m_pool);
        }

        /**
         * @brief 通过 ORM 插入三行固定数据（含 NULL 列、负数、中文、特殊字符）
         */
        void insertSampleRows()
        {
            Queryable<AccountRow> insertQuery = newQuery();
            ASSERT_EQ(1, insertQuery.insert(makeRow(1, "张三", 1234.56, std::string("普通备注"), true)));
            ASSERT_EQ(1, insertQuery.insert(makeRow(2, "O'Brien -- DROP TABLE accounts; --", -99.5, std::nullopt, false)));
            ASSERT_EQ(1, insertQuery.insert(makeRow(3, "李四", 0.0, std::string("中文备注"), true)));
        }

        std::unique_ptr<ConnectionPool> m_pool; ///< 用例独占的连接池
    };

} // namespace

// ========================================================================
// 离线模式回归
// ========================================================================

/**
 * @brief 验证默认构造的离线模式仍只做 SQL 生成，不触碰连接池
 */
TEST(QueryableOfflineMode, ToSqlStaysOfflineGenerator)
{
    const Queryable<AccountRow> offlineQuery;

    // 既有契约：离线 toSql 仍生成近似 SQL（'*'、不加引号），不受方言层接入影响
    EXPECT_EQ(offlineQuery.toSql(), "SELECT * FROM accounts");
}

/**
 * @brief 验证离线模式调用执行器方法抛出逻辑错误
 */
TEST(QueryableOfflineMode, OfflineModeThrowsOnExecution)
{
    Queryable<AccountRow> offlineQuery;

    EXPECT_THROW(static_cast<void>(offlineQuery.toList()), std::logic_error);
    EXPECT_THROW(static_cast<void>(offlineQuery.first()), std::logic_error);
    EXPECT_THROW(static_cast<void>(offlineQuery.count()), std::logic_error);
    EXPECT_THROW(static_cast<void>(offlineQuery.executeNonQuery()), std::logic_error);
    EXPECT_THROW(static_cast<void>(offlineQuery.insert(makeRow(9, "离线", 0.0, std::nullopt, true))), std::logic_error);
}

// ========================================================================
// 插入 / 查询 / 映射
// ========================================================================

/**
 * @brief 验证 ORM 插入后按 WHERE + ORDER BY + LIMIT 查询并正确映射各类型字段
 */
TEST_F(QueryableExecutionTest, OrmInsertThenQueryWithWhereOrderAndLimit)
{
    insertSampleRows();

    // 升序全量查询：验证三种字段类型、中文与负数都能正确往返
    {
        Queryable<AccountRow> query = newQuery();
        const std::vector<AccountRow> rows = query.orderBy(asc("id")).toList();

        ASSERT_EQ(rows.size(), 3U);

        EXPECT_EQ(rows[0].id, 1);
        EXPECT_EQ(rows[0].name, "张三");
        EXPECT_DOUBLE_EQ(rows[0].balance, 1234.56);
        ASSERT_TRUE(rows[0].note.has_value());
        EXPECT_EQ(rows[0].note.value(), "普通备注");
        EXPECT_TRUE(rows[0].active);

        // 第二行的备注是 NULL → std::optional 为空
        EXPECT_EQ(rows[1].id, 2);
        EXPECT_FALSE(rows[1].note.has_value());
        EXPECT_DOUBLE_EQ(rows[1].balance, -99.5);
        EXPECT_FALSE(rows[1].active);

        EXPECT_EQ(rows[2].id, 3);
        EXPECT_EQ(rows[2].name, "李四");
        EXPECT_DOUBLE_EQ(rows[2].balance, 0.0);
        EXPECT_TRUE(rows[2].active);
    }

    // 条件 + 降序 + 分页：id >= 1 共三行，降序取第一行应当是 id = 3
    {
        Queryable<AccountRow> query = newQuery();
        const std::vector<AccountRow> rows =
            query.where(Column(&AccountRow::id, "id") >= std::int64_t{1})
                 .orderBy(desc("id"))
                 .limit(1)
                 .toList();

        ASSERT_EQ(rows.size(), 1U);
        EXPECT_EQ(rows[0].id, 3);
        EXPECT_EQ(rows[0].note.value(), "中文备注");
    }

    // 复合逻辑条件 + IN + LIKE：验证递归条件与集合参数都能正确绑定
    {
        Queryable<AccountRow> query = newQuery();
        const std::vector<AccountRow> rows =
            query.where(in(Column(&AccountRow::id, "id"), std::vector<int>{1, 3})
                        && like(Column(&AccountRow::name, "name"), "%张%"))
                 .toList();

        ASSERT_EQ(rows.size(), 1U);
        EXPECT_EQ(rows[0].id, 1);
    }
}

/**
 * @brief 验证含单引号、"--" 与分号的文本能原样往返，且未影响表结构
 *
 * @details 若实现是拼接 SQL 而不是参数绑定，这段文本会提前闭合字符串字面量并把
 *          后半段变成 SQL 注释/新语句：轻则插入失败，重则表被删掉。
 *          这里既断言文本能原样查回，也断言注入残留（DROP TABLE）没有生效。
 */
TEST_F(QueryableExecutionTest, HostileTextRoundTripsThroughParameterBinding)
{
    insertSampleRows();

    const std::string hostileName = "O'Brien -- DROP TABLE accounts; --";

    // 按名精确查询：WHERE 的取值同样走绑定
    {
        Queryable<AccountRow> query = newQuery();
        const std::optional<AccountRow> found = query.where(Column(&AccountRow::name, "name") == hostileName).first();

        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->name, hostileName);
        EXPECT_EQ(found->id, 2);
    }

    // 表还在、数据还是三行：证明 "--" 没有把后续内容注释掉、分号也没有开启新语句
    {
        Queryable<AccountRow> countQuery = newQuery();
        EXPECT_EQ(countQuery.count(), 3);

        // 备注里同样写入恶意文本，验证可空列上的绑定与回读
        Queryable<AccountRow> updateQuery = newQuery();
        const AccountRow updated = makeRow(2, hostileName, -99.5, std::string("x'); DROP TABLE accounts; --"), false);
        EXPECT_EQ(updateQuery.update(updated), 1);
    }

    {
        Queryable<AccountRow> query = newQuery();
        const std::optional<AccountRow> found = query.where(Column(&AccountRow::id, "id") == std::int64_t{2}).first();

        ASSERT_TRUE(found.has_value());
        ASSERT_TRUE(found->note.has_value());
        EXPECT_EQ(found->note.value(), "x'); DROP TABLE accounts; --");
    }

    {
        Queryable<AccountRow> countQuery = newQuery();
        EXPECT_EQ(countQuery.count(), 3);
    }
}

/**
 * @brief 验证空字符串与 SQL NULL 在往返后保持语义区分
 */
TEST_F(QueryableExecutionTest, EmptyStringStaysDistinctFromNull)
{
    Queryable<AccountRow> insertQuery = newQuery();
    ASSERT_EQ(1, insertQuery.insert(makeRow(1, "空串备注", 1.0, std::string(""), true)));
    ASSERT_EQ(1, insertQuery.insert(makeRow(2, "空备注", 2.0, std::nullopt, true)));

    Queryable<AccountRow> query = newQuery();
    const std::vector<AccountRow> rows = query.orderBy(asc("id")).toList();

    ASSERT_EQ(rows.size(), 2U);
    // 空串是「有值且为空」，NULL 是「没有值」，二者不能混为一谈
    ASSERT_TRUE(rows[0].note.has_value());
    EXPECT_TRUE(rows[0].note->empty());
    EXPECT_FALSE(rows[1].note.has_value());
}

/**
 * @brief 验证 first() 在无匹配行时返回空
 */
TEST_F(QueryableExecutionTest, FirstReturnsEmptyWhenNoRowMatches)
{
    insertSampleRows();

    Queryable<AccountRow> query = newQuery();
    const std::optional<AccountRow> missing = query.where(Column(&AccountRow::id, "id") == std::int64_t{999}).first();

    EXPECT_FALSE(missing.has_value());
}

/**
 * @brief 验证 count() 在无条件与带条件时的结果
 */
TEST_F(QueryableExecutionTest, CountMatchesFilteredRows)
{
    insertSampleRows();

    {
        Queryable<AccountRow> query = newQuery();
        EXPECT_EQ(query.count(), 3);
    }

    {
        Queryable<AccountRow> query = newQuery();
        EXPECT_EQ(query.where(Column(&AccountRow::active, "active") == true).count(), 2);
    }

    {
        Queryable<AccountRow> query = newQuery();
        EXPECT_EQ(query.where(Column(&AccountRow::balance, "balance") < 0.0).count(), 1);
    }
}

/**
 * @brief 验证按主键更新只改动目标行
 */
TEST_F(QueryableExecutionTest, UpdateByPrimaryKeyChangesOnlyTargetRow)
{
    insertSampleRows();

    Queryable<AccountRow> updateQuery = newQuery();
    // 主键 2 的备注是 NULL，这里更新为有值；其余字段一并改写
    EXPECT_EQ(updateQuery.update(makeRow(2, "王五", 888.25, std::string("已更新"), true)), 1);

    Queryable<AccountRow> query = newQuery();
    const std::vector<AccountRow> rows = query.orderBy(asc("id")).toList();

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[1].id, 2);
    EXPECT_EQ(rows[1].name, "王五");
    EXPECT_DOUBLE_EQ(rows[1].balance, 888.25);
    ASSERT_TRUE(rows[1].note.has_value());
    EXPECT_EQ(rows[1].note.value(), "已更新");
    EXPECT_TRUE(rows[1].active);

    // 其它行不受影响
    EXPECT_EQ(rows[0].name, "张三");
    EXPECT_EQ(rows[2].name, "李四");
}

/**
 * @brief 验证 executeNonQuery() 按条件删除并回报受影响行数
 */
TEST_F(QueryableExecutionTest, ExecuteNonQueryDeletesMatchingRows)
{
    insertSampleRows();

    {
        Queryable<AccountRow> deleteQuery = newQuery();
        // executeNonQuery 返回 int64_t：接成 int 会在 /W4 下报可能丢数据的转换
        const std::int64_t deletedRows = deleteQuery.where(Column(&AccountRow::id, "id") >= std::int64_t{2}).executeNonQuery();
        EXPECT_EQ(deletedRows, 2);
    }

    Queryable<AccountRow> query = newQuery();
    const std::vector<AccountRow> remainingRows = query.toList();

    ASSERT_EQ(remainingRows.size(), 1U);
    EXPECT_EQ(remainingRows[0].id, 1);
}

/**
 * @brief 钉住「DELETE 带 LIMIT 被当场拒绝」，而不是把有界删除静默做成全表删除
 * @details 方言层不输出 DELETE 的 LIMIT / OFFSET（MySQL 单表删除支持，SQLite 要编译期开关
 *          SQLITE_ENABLE_UPDATE_DELETE_LIMIT），跨引擎给不出同一语义。但「不支持」不能变成
 *          「照发一句无界的同条件删除」：调用方要的是只删 1 行，实际删掉全部匹配行，且不可逆。
 *          对照组同时钉住「去掉 limit 的删除照常工作」，拒绝面不能把删除能力一起关掉。
 */
TEST_F(QueryableExecutionTest, DeleteWithLimitIsRejectedInsteadOfDeletingEverything)
{
    insertSampleRows();

    std::string rejectionMessage;
    try
    {
        Queryable<AccountRow> deleteQuery = newQuery();
        static_cast<void>(deleteQuery.where(Column(&AccountRow::id, "id") >= std::int64_t{2}).limit(1).executeNonQuery());
        FAIL() << "带 LIMIT 的删除必须被拒绝：方言不输出 LIMIT，静默放行就是把有界删除做成全表删除";
    } catch (const std::logic_error &exception)
    {
        rejectionMessage = exception.what();
    }

    // 文案要指名被拒的子句，只回一句「参数非法」等于把排查推回调用方
    EXPECT_NE(rejectionMessage.find("LIMIT"), std::string::npos) << rejectionMessage;
    EXPECT_NE(rejectionMessage.find("不支持"), std::string::npos) << rejectionMessage;

    // 判据是行数：一条都不该被删掉，「多删了却报成功」正是这条缺陷的表现
    Queryable<AccountRow> countQuery = newQuery();
    EXPECT_EQ(countQuery.count(), 3);

    Queryable<AccountRow> plainDelete = newQuery();
    EXPECT_EQ(plainDelete.where(Column(&AccountRow::id, "id") >= std::int64_t{2}).executeNonQuery(), 2);
}

/**
 * @brief 钉住「没有 TableSchema 特化」报出真正的成因，而不是推给一个没人写过的自增主键
 * @details kColumns 为空有两种来路：类型压根没特化 TableSchema，或者确实只声明了一个自增主键列。
 *          前者说成后者会把人指到一个自己没写过的声明上（此时表名也是空串，读起来更像笔误）。
 */
TEST_F(QueryableExecutionTest, UnregisteredTypeNamesTheMissingSchemaSpecialization)
{
    struct UnregisteredRow
    {
        std::int64_t id{};
        std::string  name;
    };

    std::string message;
    try
    {
        Queryable<UnregisteredRow> query(*m_pool);
        static_cast<void>(query.insert(UnregisteredRow{}));
        FAIL() << "没有列可写却生成了语句，说明空 kColumns 没有被拒";
    } catch (const std::logic_error &exception)
    {
        message = exception.what();
    }

    EXPECT_NE(message.find("TableSchema"), std::string::npos) << message;
    EXPECT_EQ(message.find("自增主键"), std::string::npos)
        << "把「没特化 TableSchema」报成「声明了自增主键」，调用方会去改一个自己没写过的声明：" << message;
}

/**
 * @brief 验证表名与列名含空格时 CREATE TABLE + ORM insert + toList 全链路可跑通
 *
 * @details 标识符引用实现（StandardSqlDialect 的可引用标识符判定）放行了空格，
 *          因此含空格的列名会被渲染成 "full name" 而不是被误判成表达式原样输出。
 *          若退化成不加引号，SQLite 会把 `full name` 解析成语法错误，
 *          甚至连建表那一步都会失败——所以这条用例同时覆盖 DDL 与 DML 两侧。
 */
TEST_F(QueryableExecutionTest, SpacedIdentifiersSurviveCreateInsertAndQuery)
{
    // 建表：表名与列名整段加双引号，交给 SQLite 当作标识符而不是语法片段。
    // 连接必须先取、用完立刻归还：本夹具的池上限为 1，持有连接期间 ORM 无法再取连接
    {
        PooledConnection connection = m_pool->acquire();
        ASSERT_TRUE(connection);
        const auto createResult = connection->execute(
            "CREATE TABLE \"spaced accounts\" ("
            "\"id\" INTEGER PRIMARY KEY, "
            "\"full name\" TEXT NOT NULL, "
            "\"home address\" TEXT)");
        ASSERT_TRUE(createResult != nullptr) << connection->lastError();
    }

    // ORM 写：列名 "full name" / "home address" 必须被方言正确引用，否则 INSERT 语法错误
    {
        Queryable<SpacedIdentifierRow> insertQuery(*m_pool);
        ASSERT_EQ(1, insertQuery.insert(SpacedIdentifierRow{1, "张三", std::string("北京 朝阳")}));
        // 第二行的可空列给 NULL，验证含空格的列同样能绑定 NULL
        ASSERT_EQ(1, insertQuery.insert(SpacedIdentifierRow{2, "Li Si", std::nullopt}));
    }

    // ORM 读：SELECT 列名逐个引用，映射回结构体时按列名查找下标
    {
        Queryable<SpacedIdentifierRow> query(*m_pool);
        const std::vector<SpacedIdentifierRow> rows = query.orderBy(asc("id")).toList();

        ASSERT_EQ(rows.size(), 2U);
        EXPECT_EQ(rows[0].id, 1);
        EXPECT_EQ(rows[0].fullName, "张三");
        ASSERT_TRUE(rows[0].homeAddress.has_value());
        EXPECT_EQ(rows[0].homeAddress.value(), "北京 朝阳");
        EXPECT_EQ(rows[1].id, 2);
        EXPECT_EQ(rows[1].fullName, "Li Si");
        EXPECT_FALSE(rows[1].homeAddress.has_value());
    }

    // 含空格的列名用于 WHERE：条件渲染与 SELECT 列表共用同一套引用规则
    {
        Queryable<SpacedIdentifierRow> filteredQuery(*m_pool);
        const std::optional<SpacedIdentifierRow> found =
            filteredQuery.where(Column(&SpacedIdentifierRow::fullName, "full name") == std::string("Li Si")).first();

        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->id, 2);
        EXPECT_FALSE(found->homeAddress.has_value());
    }

    // 含空格的列名用于 UPDATE 的 SET 与主键定位，证明写语句方向同样引用正确
    {
        Queryable<SpacedIdentifierRow> updateQuery(*m_pool);
        EXPECT_EQ(1, updateQuery.update(SpacedIdentifierRow{1, "张三", std::nullopt}));

        Queryable<SpacedIdentifierRow> verifyQuery(*m_pool);
        const std::optional<SpacedIdentifierRow> updated =
            verifyQuery.where(Column(&SpacedIdentifierRow::id, "id") == std::int64_t{1}).first();
        ASSERT_TRUE(updated.has_value());
        EXPECT_FALSE(updated->homeAddress.has_value());
    }
}

/**
 * @brief 验证 UInt64 列在 int64 能表达的范围内无损往返（表由 SchemaMigrator 建出）
 *
 * @details 走的是真实链路：建表类型由 SqliteDialect::columnTypeName() 给出（INTEGER），
 *          插入时 RowMapper 把它按整数绑定，读回时走 int64 支路。两端边界都要覆盖：
 *          0 与 INT64_MAX 分别对应 int64 支路的下界与上界，上界是最容易在别处被误写成
 *          「有符号溢出」的位置。
 */
TEST_F(QueryableExecutionTest, UnsignedColumnRoundTripsWithinInt64Range)
{
    ASSERT_TRUE(SchemaMigrator::createTable<UnsignedCounterRow>(*m_pool));

    const auto maximumSignedValue = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    Queryable<UnsignedCounterRow> insertQuery(*m_pool);
    ASSERT_EQ(1, insertQuery.insert(UnsignedCounterRow{.id = 1, .sequence = 0U}));
    ASSERT_EQ(1, insertQuery.insert(UnsignedCounterRow{.id = 2, .sequence = maximumSignedValue}));

    Queryable<UnsignedCounterRow> query(*m_pool);
    const std::vector<UnsignedCounterRow> rows = query.orderBy(asc("id")).toList();

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].sequence, 0U);
    EXPECT_EQ(rows[1].sequence, maximumSignedValue);
}

/**
 * @brief 验证超出 int64 的 UInt64 取值在 SQLite 上明确失败，而不是悄悄换个数值
 *
 * @details SQLite 只有 64 位有符号整数：INTEGER 亲和性会把「装不下的十进制文本」转成 REAL，于是这一列读回来是浮点
 *          而不是整数。这是引擎的存储能力边界，ORM 的职责是**如实报错**——静默取整或回绕都会给出一个看起来正常、实际错误的
 *          数值，那比失败难查得多。需要精确承载 2^63 以上取值时应改用 MySQL 的 BIGINT UNSIGNED，那条真机路径由 MySQL 集成用例覆盖。
 */
TEST_F(QueryableExecutionTest, UnsignedValueBeyondInt64FailsLoudlyOnSqlite)
{
    ASSERT_TRUE(SchemaMigrator::createTable<UnsignedCounterRow>(*m_pool));

    Queryable<UnsignedCounterRow> insertQuery(*m_pool);
    // 写入本身会成功：绑定的是十进制文本，SQLite 接受它并按列亲和性转成 REAL
    ASSERT_EQ(1, insertQuery.insert(
                     UnsignedCounterRow{.id = 1, .sequence = std::numeric_limits<std::uint64_t>::max()}));

    // 读回时列值已是浮点，与无符号整型成员类型不符：必须抛错，且原因指向该列
    Queryable<UnsignedCounterRow> query(*m_pool);
    try
    {
        static_cast<void>(query.toList());
        FAIL() << "SQLite 存不下超过 int64 的 UInt64，读回本应抛异常";
    } catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("sequence"), std::string::npos) << message;
        EXPECT_NE(message.find("无符号整型"), std::string::npos) << message;
    }
}

// ========================================================================
// 错误路径
// ========================================================================

/**
 * @brief 验证结构体声明的列在结果集中缺失时给出可读的中文错误
 */
TEST_F(QueryableExecutionTest, MissingColumnThrowsReadableError)
{
    insertSampleRows();

    Queryable<BrokenColumnRow> query(*m_pool);
    // 只查询存在的 id 列，让 SELECT 本身能成功执行，从而走到行映射阶段
    query.select({"id"});

    try
    {
        static_cast<void>(query.toList());
        FAIL() << "结构体声明了结果集中不存在的列，应当抛出异常";
    }
    catch (const std::runtime_error &exception)
    {
        const std::string message = exception.what();
        EXPECT_NE(message.find("missing_column"), std::string::npos);
        // 错误信息面向使用者，必须是中文
        EXPECT_NE(message.find("行映射失败"), std::string::npos);
    }
}

/**
 * @brief 验证列类型与成员类型不匹配时给出可读的中文错误
 */
TEST_F(QueryableExecutionTest, DuplicateColumnNamesDoNotAliasTwoMembersOntoOneColumn)
{
    insertSampleRows();

    PooledConnection connection = m_pool->acquire();
    ASSERT_TRUE(connection);
    // 同一列选两次：驱动按名解析时两个 "name" 都落到第一个的下标（该契约见 TestSqliteResult 的
    // DuplicateColumnNamesResolveToFirstIndex），因此结构体里第二个 std::string 成员会拿到前一列的数据
    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
            connection->execute("SELECT id, name, name FROM accounts ORDER BY id");
    ASSERT_NE(result, nullptr) << connection->lastError();

    try
    {
        static_cast<void>(mapResultRows<DuplicatedColumnNameRow>(*result));
        FAIL() << "两个成员解析到了同一列，应当抛出异常而不是给出两份相同的值";
    }
    catch (const std::runtime_error &exception)
    {
        const std::string message = exception.what();
        EXPECT_NE(message.find("都解析到结果集的第"), std::string::npos) << message;
        EXPECT_NE(message.find("第 2 个与第 3 个成员"), std::string::npos) << message;
        EXPECT_NE(message.find("同一个值"), std::string::npos) << message;
        EXPECT_NE(message.find("行映射失败"), std::string::npos) << message;
    }
}

/**
 * @brief 验证 contains / startsWith / endsWith 把用户输入里的 % _ ! 当字面量匹配
 *
 * @details like() 的入参是**模式**（% 与 _ 是通配符），把外部输入直接递给它的后果是：一个恰好
 *          含 % 的搜索词会放宽成「匹配任意内容」，选择性归零并退化成整表扫描。字面量三兄弟必须
 *          连 ESCAPE 子句一起产出才成立——SQLite 的 LIKE 没有默认转义符，缺了那句转义就是摆设。
 *          同一用例里保留 like() 的对照组，用来区分「修好了字面量」与「把通配符语义弄坏了」。
 */
TEST_F(QueryableExecutionTest, LiteralMatchHelpersTreatWildcardsAsLiteralText)
{
    const std::vector<std::string> sampleNames{"100%", "100percent", "a_b", "axb", "50!"};
    std::int64_t rowId = 1;
    for (const std::string &name: sampleNames)
    {
        Queryable<AccountRow> insertQuery = newQuery();
        ASSERT_EQ(1, insertQuery.insert(makeRow(rowId++, name, 1.0, std::nullopt, true)));
    }

    const auto namesMatching = [this](const AsynGyanis::Database::Queryable::WhereCondition &condition)
    {
        Queryable<AccountRow> query = newQuery();
        query.where(condition);
        query.orderBy(asc("id"));

        std::vector<std::string> matchedNames;
        for (const AccountRow &row: query.toList())
        {
            matchedNames.push_back(row.name);
        }
        return matchedNames;
    };

    const auto nameColumn = Column(&AccountRow::name, "name");

    // 含通配符字面量的输入只匹配那一行；换作 like() 这三条都会命中全部行
    EXPECT_EQ(namesMatching(contains(nameColumn, "100%")), (std::vector<std::string>{"100%"}));
    EXPECT_EQ(namesMatching(contains(nameColumn, "%")), (std::vector<std::string>{"100%"}));
    EXPECT_EQ(namesMatching(contains(nameColumn, "_")), (std::vector<std::string>{"a_b"}));
    // 转义符自身也要按字面量匹配，否则 "50!" 会被当成「50 加一个通配符前缀」
    EXPECT_EQ(namesMatching(contains(nameColumn, "!")), (std::vector<std::string>{"50!"}));

    EXPECT_EQ(namesMatching(startsWith(nameColumn, "10")), (std::vector<std::string>{"100%", "100percent"}));
    EXPECT_EQ(namesMatching(endsWith(nameColumn, "percent")), (std::vector<std::string>{"100percent"}));
    // 空文本退化成「匹配所有非 NULL 行」，这是 "%" 模式的既有语义
    EXPECT_EQ(namesMatching(contains(nameColumn, "")).size(), sampleNames.size());

    // 对照组：like() 仍是模式语义，% 匹配任意串——它没有被字面量改动牵连
    EXPECT_EQ(namesMatching(like(nameColumn, "100%")), (std::vector<std::string>{"100%", "100percent"}));
    EXPECT_EQ(namesMatching(like(nameColumn, "a_b")), (std::vector<std::string>{"a_b", "axb"}));
}

TEST_F(QueryableExecutionTest, TypeMismatchThrowsReadableError)
{
    insertSampleRows();

    Queryable<TypeMismatchRow> query(*m_pool);

    try
    {
        static_cast<void>(query.toList());
        FAIL() << "INTEGER 列映射到 std::string 应当抛出异常";
    }
    catch (const std::runtime_error &exception)
    {
        const std::string message = exception.what();
        EXPECT_NE(message.find("id"), std::string::npos);
        EXPECT_NE(message.find("std::string"), std::string::npos);
        // 提示里要给出「若可能为 NULL 请用 std::optional」这类可操作建议
        EXPECT_NE(message.find("std::optional"), std::string::npos);
    }
}

// ========================================================================
// 构建器入口：join() 与 groupBy()
// ========================================================================

/**
 * @brief 验证 join() 造出的 ON 子句确实能执行，并按被连接表把行筛掉
 *
 * @details join() 是 QueryNode::joins 唯一的公开写入口，此前所有 JOIN 断言都是手工搭查询树喂给
 *          方言层，「从 ORM 这头拼出来的语句可执行」这件事从没被走过。两张表都有 id 列，因此
 *          SELECT 列表必须按表限定——这顺带把「限定名投影 → 结果列名仍是不限定的 id → 映射回成员」
 *          这条路也钉住。
 */
TEST_F(QueryableExecutionTest, JoinThroughBuilderNarrowsRowsByTheJoinedTable)
{
    insertSampleRows();

    {
        const PooledConnection connection = m_pool->acquire();
        ASSERT_TRUE(connection);
        ASSERT_TRUE(connection->execute(
                        "CREATE TABLE orders (order_id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, amount REAL NOT NULL)") != nullptr)
            << connection->lastError();
        // 账户 1 两单、账户 3 一单，账户 2 一笔都没下
        ASSERT_TRUE(connection->execute(
                        "INSERT INTO orders (order_id, account_id, amount) VALUES (1, 1, 10.0), (2, 1, 20.0), (3, 3, 30.0)") != nullptr)
            << connection->lastError();
    }

    Queryable<AccountRow> query = newQuery();
    query.select({"accounts.id", "accounts.name", "accounts.balance", "accounts.note", "accounts.active"});
    query.join(JoinClause{
        .type       = JoinType::Inner,
        .tableName  = "orders",
        .tableAlias = {},
        .conditions = {WhereCondition{
            .left  = FieldReference{"orders.account_id"},
            .op    = SqlOperator::Eq,
            .right = FieldReference{"accounts.id"}}}});
    query.orderBy(asc("accounts.id"));

    const std::vector<AccountRow> rows = query.toList();
    // 三行而不是四行：INNER JOIN 把没下单的账户 2 筛掉，账户 1 因为两笔订单出现两次
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].id, 1);
    EXPECT_EQ(rows[0].name, "张三");
    EXPECT_EQ(rows[1].id, 1);
    EXPECT_EQ(rows[2].id, 3);
    EXPECT_EQ(rows[2].name, "李四");
}

/**
 * @brief 验证 groupBy() 真的按组聚合，且表达式投影的别名列能映射回成员
 *
 * @details groupBy() 此前只被手工赋值的查询树测过：从 ORM 传进来的是「文本列名」，
 *          转成 FieldReference 这一步（以及 "COUNT(*) AS cnt" 这种表达式列靠别名对上 rowCount
 *          成员）从没从这头走过。
 */
TEST_F(QueryableExecutionTest, GroupByThroughBuilderAggregatesAndMapsAliasColumn)
{
    insertSampleRows();
    Queryable<AccountRow> extra = newQuery();
    // 第二个同名账户：没有这一行，「分组」与「全表一行」的结果看不出差别
    ASSERT_EQ(extra.insert(makeRow(4, "张三", 5.0, std::nullopt, true)), 1);

    Queryable<NameCountRow> query(*m_pool);
    query.select({"name", "COUNT(*) AS cnt"});
    query.groupBy({"name"});
    query.orderBy(asc("name"));

    const std::vector<NameCountRow> groups = query.toList();
    ASSERT_EQ(groups.size(), 3U);
    // 排序按字节序：ASCII 开头的注入样本在前，两条中文按 UTF-8 字节排在后
    EXPECT_EQ(groups[0].name, "O'Brien -- DROP TABLE accounts; --");
    EXPECT_EQ(groups[0].rowCount, 1);
    EXPECT_EQ(groups[1].name, "张三");
    EXPECT_EQ(groups[1].rowCount, 2);
    EXPECT_EQ(groups[2].name, "李四");
    EXPECT_EQ(groups[2].rowCount, 1);
}

/**
 * @brief 验证 having() 这个构建入口真的在分组之后筛组
 * @details 查询树与方言层一直支持 HAVING，ORM 这一头没有入口时那条渲染分支从公开 API 走不到。
 *          对照组就是上面那条 groupBy 用例：同一份数据分三组，加上 COUNT(*) > 1 之后只剩两行那一组。
 */
TEST_F(QueryableExecutionTest, HavingThroughBuilderFiltersGroups)
{
    insertSampleRows();
    Queryable<AccountRow> extra = newQuery();
    ASSERT_EQ(extra.insert(makeRow(4, "张三", 5.0, std::nullopt, true)), 1);

    WhereCondition duplicatedGroup;
    duplicatedGroup.left  = FieldReference{.name = std::string("COUNT(*)")};
    duplicatedGroup.op    = SqlOperator::Gt;
    duplicatedGroup.right = ParameterValue{static_cast<std::int64_t>(1)};

    Queryable<NameCountRow> query(*m_pool);
    query.select({"name", "COUNT(*) AS cnt"});
    query.groupBy({"name"});
    query.having(std::move(duplicatedGroup));

    const std::vector<NameCountRow> groups = query.toList();
    ASSERT_EQ(groups.size(), 1U) << "HAVING 没有筛掉只有一行的组，等于是把条件丢了";
    EXPECT_EQ(groups[0].name, "张三");
    EXPECT_EQ(groups[0].rowCount, 2);
}
