// ORM 乐观锁的判据。两半：
//   ① 真机两会话：两个独立会话（池里两条连接）读同一行、都改、都写回——后写的那个必须拿到
//      OptimisticLockException，且先写的那份数据没有被抹掉。单这一条要一个文件库：
//      SQLite 的 ":memory:" 不跨连接共享，池里两条连接会各持一份空库，那练的不是并发而是错觉。
//   ② 三处声明错误在生成语句时当场拒（版本列与主键同列、声明了却不在 kColumns 里），
//      以及「没声明版本列的表一切照旧（0 行照旧不抛）」这条对照。
//
// 临时库文件落在系统临时目录，TearDown 即删；用例之间用不同文件名，互不影响。

#include "Base/Exception/LogicException.h"

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/OptimisticLockException.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/TableSchema.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

// ========================================================================
// 测试用数据结构
// ========================================================================

namespace
{
    using AsynGyanis::Database::Queryable::Column;

    /// 带版本列的表：乐观锁的常规形状（id 主键 + 业务列 + version）
    struct LockedAccount
    {
        std::int64_t id;
        std::string  name;
        std::int64_t version;
    };

    /// 不带版本列的表：用来钉「没声明即一切照旧」
    struct UnlockedAccount
    {
        std::int64_t id;
        std::string  name;
    };

    /// 版本列与主键同列的错误声明：生成语句时该当场被拒
    struct SelfKeyedAccount
    {
        std::int64_t id;
        std::string  name;
    };

    /// 声明了版本列却忘了把它列进 kColumns：乐观锁会静默失效，也必须当场被拒
    struct MissingVersionColumnAccount
    {
        std::int64_t id;
        std::string  name;
        std::int64_t version;
    };

    /// 只有主键与版本列的表：更新只做版本自增加一
    struct TouchOnlyRow
    {
        std::int64_t id;
        std::int64_t version;
    };

    /// 版本列声明成文本：生成 UPDATE 时必须当场拒绝
    struct TextVersionedRow
    {
        std::int64_t id;
        std::string  version;
    };
} // namespace

// ========================================================================
// TableSchema 特化
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<LockedAccount>
{
    static constexpr std::string_view kTableName = "locked_accounts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&LockedAccount::id, "id"),
            Column(&LockedAccount::name, "name"),
            Column(&LockedAccount::version, "version"),
    };
    static constexpr std::string_view kPrimaryKey    = "id";
    static constexpr std::string_view kVersionColumn = "version";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<UnlockedAccount>
{
    static constexpr std::string_view kTableName = "unlocked_accounts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&UnlockedAccount::id, "id"),
            Column(&UnlockedAccount::name, "name"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<SelfKeyedAccount>
{
    static constexpr std::string_view kTableName = "self_keyed_accounts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&SelfKeyedAccount::id, "id"),
            Column(&SelfKeyedAccount::name, "name"),
    };
    static constexpr std::string_view kPrimaryKey    = "id";
    static constexpr std::string_view kVersionColumn = "id"; ///< 与主键同列：这一格要当场被拒
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<MissingVersionColumnAccount>
{
    static constexpr std::string_view kTableName = "missing_version_accounts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&MissingVersionColumnAccount::id, "id"),
            Column(&MissingVersionColumnAccount::name, "name"),
            // 故意不列 version：kVersionColumn 指着它，生成语句时必须被拒
    };
    static constexpr std::string_view kPrimaryKey    = "id";
    static constexpr std::string_view kVersionColumn = "version";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<TouchOnlyRow>
{
    static constexpr std::string_view kTableName     = "touch_only";
    static constexpr auto             kColumns       = std::tuple{Column(&TouchOnlyRow::id, "id"), Column(&TouchOnlyRow::version, "version")};
    static constexpr std::string_view kPrimaryKey    = "id";
    static constexpr std::string_view kVersionColumn = "version";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<TextVersionedRow>
{
    static constexpr std::string_view kTableName     = "text_versioned";
    static constexpr auto             kColumns       = std::tuple{Column(&TextVersionedRow::id, "id"), Column(&TextVersionedRow::version, "version")};
    static constexpr std::string_view kPrimaryKey    = "id";
    static constexpr std::string_view kVersionColumn = "version";
};

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::OptimisticLockException;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::Queryable::Queryable;

    /**
     * @brief 乐观锁用例的夹具：一个文件库 + 池里两条连接（＝两个会话）
     * @details 文件库是这一组的前提：":memory:" 不跨连接共享，两条连接会各持一份空库，
     *          那样「两个会话同时改同一行」根本测不到
     */
    class QueryableOptimisticLockTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::random_device device;
            m_databasePath = std::filesystem::temp_directory_path() / ("asyn-orm-lock-" + std::to_string(device()) + ".db");

            PoolConfig poolConfiguration;
            // 上限 2：两个会话各占一条连接，且都指向同一个文件库
            poolConfiguration.maximumPoolSize = 2;

            const std::string databasePathText = m_databasePath.string();
            m_pool                             = std::make_unique<ConnectionPool>(
                    [databasePathText]()
                    {
                        auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(databasePathText));
                        connection->connect();
                        return connection;
                    },
                    poolConfiguration);

            PooledConnection connection = m_pool->acquire();
            ASSERT_TRUE(connection) << "取不到建表用的连接";
            ASSERT_NE(connection->execute("CREATE TABLE locked_accounts (id INTEGER PRIMARY KEY, name TEXT NOT NULL, version INTEGER NOT NULL)"), nullptr)
                    << connection->lastError();
            ASSERT_NE(connection->execute("CREATE TABLE unlocked_accounts (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"), nullptr) << connection->lastError();
            ASSERT_NE(connection->execute("CREATE TABLE touch_only (id INTEGER PRIMARY KEY, version INTEGER NOT NULL)"), nullptr) << connection->lastError();
        }

        void TearDown() override
        {
            m_pool.reset();
            std::error_code ignored;
            std::filesystem::remove(m_databasePath, ignored);
            // SQLite 的默认回滚日志在干净提交时就删掉了；这里再兜一次 -journal 与 -wal，避免残留
            std::filesystem::remove(std::filesystem::path(m_databasePath.string() + "-journal"), ignored);
            std::filesystem::remove(std::filesystem::path(m_databasePath.string() + "-wal"), ignored);
        }

        /// 建一个绑定本夹具连接池的 ORM 对象
        [[nodiscard]] Queryable<LockedAccount> lockedQuery() const
        {
            return Queryable<LockedAccount>(*m_pool);
        }

        std::filesystem::path           m_databasePath; ///< 本用例独占的临时库文件
        std::unique_ptr<ConnectionPool> m_pool;         ///< 上限 2：够两个会话各占一条
    };

    /// 把会话一/会话二读到的两份视图取回来（两处都要判前置，故收成一个助手）
    [[nodiscard]] std::pair<LockedAccount, LockedAccount> readFromTwoSessions(Queryable<LockedAccount> &firstSession, Queryable<LockedAccount> &secondSession)
    {
        const std::optional<LockedAccount> firstView  = firstSession.where(Column(&LockedAccount::id, "id") == std::int64_t{1}).first();
        const std::optional<LockedAccount> secondView = secondSession.where(Column(&LockedAccount::id, "id") == std::int64_t{1}).first();
        EXPECT_HAS_VALUE(firstView);
        EXPECT_HAS_VALUE(secondView);
        return {firstView.value_or(LockedAccount{}), secondView.value_or(LockedAccount{})};
    }
} // namespace

// ========================================================================
// 真机两会话
// ========================================================================

/**
 * @brief 钉住「两个会话同时改同一行」：后写的那个拿到冲突异常，先写的那份数据没被抹掉
 * @details 这是乐观锁的全部意义所在。两个会话各自读出行（版本都是 0），会话一先写成功
 *          （版本变 1），会话二拿着已经过期的版本 0 再写——它必须失败，而不是把会话一的
 *          改动覆盖掉。异常文案同时点名表、版本列、期望版本与主键值，三样缺一调用方就得回去翻代码。
 * @note 证伪：把 WHERE 里的版本条件撤掉（只在 SET 里加一），「会话二写成功」那条断言立刻红——
 *       那正是这条用例要防的事：先写的那份改动被静默覆盖
 */
TEST_F(QueryableOptimisticLockTest, RejectsTheStaleWriteFromASecondSession)
{
    Queryable<LockedAccount> firstSession  = lockedQuery();
    Queryable<LockedAccount> secondSession = lockedQuery();

    ASSERT_EQ(firstSession.insert(LockedAccount{.id = 1, .name = "原值", .version = 0}), 1);

    const auto [firstView, secondView] = readFromTwoSessions(firstSession, secondSession);
    ASSERT_EQ(firstView.version, 0);
    ASSERT_EQ(secondView.version, 0);

    // 会话一先写：拿到的是自己读出来的那一行，改完写回
    LockedAccount firstRow = firstView;
    firstRow.name          = "会话一写的";
    EXPECT_EQ(firstSession.update(firstRow), 1) << "第一个写者不该失败";

    // 会话二拿旧版本再写：必须冲突
    LockedAccount secondRow = secondView;
    secondRow.name          = "会话二写的";
    try
    {
        static_cast<void>(secondSession.update(secondRow));
        FAIL() << "会话二用过期版本写成功了：会话一的改动已经被静默覆盖";
    } catch (const OptimisticLockException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("locked_accounts"), std::string::npos) << "冲突消息没点出表名：" << message;
        EXPECT_NE(message.find("version"), std::string::npos) << "冲突消息没点出版本列：" << message;
        EXPECT_NE(message.find("期望值 0"), std::string::npos) << "冲突消息没点出期望的版本值：" << message;
        EXPECT_NE(message.find("id = 1"), std::string::npos) << "冲突消息没点出主键与它的值：" << message;
    }

    // 会话一写的值还在，行也仍然只有一行——冲突不是「合并写入」，什么都没落
    const std::optional<LockedAccount> after = lockedQuery().where(Column(&LockedAccount::id, "id") == std::int64_t{1}).first();
    ASSERT_HAS_VALUE(after);
    EXPECT_EQ(after->name, "会话一写的") << "先写的那份数据被覆盖了";
    EXPECT_EQ(after->version, 1) << "成功的这次没有把版本加一";
    EXPECT_EQ(lockedQuery().count(), 1);
}

/**
 * @brief 钉住每次成功更新都把版本加一，且加一由服务端完成
 * @details 判据取「读回来的是几」：写成功之后版本必须是 1 而不是 0（若实现是把结构体里的值原样
 *          写回，这里会读到 0）。同时钉住「结构体里的那份仍是旧值」：不重新读、直接再写同一份
 *          必须冲突——这正是乐观锁的用法约束
 */
TEST_F(QueryableOptimisticLockTest, BumpsTheVersionOnTheServerSideAndLeavesTheStructStale)
{
    Queryable<LockedAccount> query = lockedQuery();
    ASSERT_EQ(query.insert(LockedAccount{.id = 7, .name = "初始", .version = 0}), 1);

    LockedAccount row = *query.where(Column(&LockedAccount::id, "id") == std::int64_t{7}).first();
    row.name          = "第一次改";
    EXPECT_EQ(query.update(row), 1);

    const std::optional<LockedAccount> afterFirst = query.where(Column(&LockedAccount::id, "id") == std::int64_t{7}).first();
    ASSERT_HAS_VALUE(afterFirst);
    EXPECT_EQ(afterFirst->version, 1) << "版本没加一，或加的是结构体里的那个旧值";

    // 手里那份结构体仍是 version = 0：再写一次必须冲突（「要继续改请先重新读出」）
    row.name = "拿旧版本再写";
    EXPECT_THROW(static_cast<void>(query.update(row)), OptimisticLockException);

    // 重新读出、改、再写：一路顺畅，版本走到 2
    LockedAccount fresh = *query.where(Column(&LockedAccount::id, "id") == std::int64_t{7}).first();
    fresh.name          = "重新读出后再改";
    EXPECT_EQ(query.update(fresh), 1);
    EXPECT_EQ(query.where(Column(&LockedAccount::id, "id") == std::int64_t{7}).first()->version, 2);
}

/**
 * @brief 钉住「行已经不在了」与「别人改过」走同一条冲突路径
 * @details 两种情形对调用方的处置是一样的（重新读出才发现行没了 / 重新读出拿到新值），
 *          分成两种异常只会让每个调用点各写一遍 if。文案里那一句「或已经被删掉了」就是这条的口径
 */
TEST_F(QueryableOptimisticLockTest, TreatsADeletedRowAsAConflictAsWell)
{
    Queryable<LockedAccount> query = lockedQuery();
    ASSERT_EQ(query.insert(LockedAccount{.id = 9, .name = "将被删掉", .version = 0}), 1);

    LockedAccount row = *query.where(Column(&LockedAccount::id, "id") == std::int64_t{9}).first();
    ASSERT_EQ(query.where(Column(&LockedAccount::id, "id") == std::int64_t{9}).executeNonQuery(), 1);

    row.name = "删了之后再写";
    EXPECT_THROW(static_cast<void>(query.update(row)), OptimisticLockException);
}

// ========================================================================
// 正交性与声明错误的拒绝面
// ========================================================================

/**
 * @brief 对照：没声明版本列的表一切照旧——命中 0 行既不抛也不改口径
 * @details 这条是「正交」的可证形式：乐观锁只对声明了 kVersionColumn 的表生效，
 *          既有表（绝大多数）在升级之后行为与之前逐字相同
 */
TEST_F(QueryableOptimisticLockTest, TablesWithoutAVersionColumnKeepTheOldBehaviour)
{
    Queryable<UnlockedAccount> query(*m_pool);
    EXPECT_EQ(query.update(UnlockedAccount{.id = 404, .name = "这一行不存在"}), 0) << "未声明版本列的表在 0 行时抛了异常：既有调用方会被这枚新异常打到";
    EXPECT_EQ(query.insert(UnlockedAccount{.id = 5, .name = "在的"}), 1);
    EXPECT_EQ(query.update(UnlockedAccount{.id = 5, .name = "改了"}), 1);
    EXPECT_EQ(query.where(Column(&UnlockedAccount::id, "id") == std::int64_t{5}).first()->name, "改了");
}

/**
 * @brief 钉住两处声明错误在生成语句时当场拒，而不是生成一条语义含糊（或让乐观锁静默失效）的语句
 * @details 两处都不碰数据库：判据是异常类型与文案里点出的列名。它们属「调用方把接口用错了」，
 *          因此走 Base::LogicException 那一支（不是 DatabaseException 家族）
 */
TEST_F(QueryableOptimisticLockTest, RejectsVersionColumnDeclarationsThatCannotWork)
{
    Queryable<SelfKeyedAccount> selfKeyed(*m_pool);
    try
    {
        static_cast<void>(selfKeyed.update(SelfKeyedAccount{.id = 1, .name = "x"}));
        FAIL() << "版本列与主键同列却没有被拒";
    } catch (const AsynGyanis::Base::LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("kVersionColumn"), std::string::npos) << message;
        EXPECT_NE(message.find("kPrimaryKey"), std::string::npos) << message;
    }

    Queryable<MissingVersionColumnAccount> missingColumn(*m_pool);
    try
    {
        static_cast<void>(missingColumn.update(MissingVersionColumnAccount{.id = 1, .name = "x", .version = 0}));
        FAIL() << "版本列没在 kColumns 里却没有被拒：乐观锁会静默失效";
    } catch (const AsynGyanis::Base::LogicException &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("version"), std::string::npos) << message;
        EXPECT_NE(message.find("乐观锁会静默失效"), std::string::npos) << message;
    }
}

TEST_F(QueryableOptimisticLockTest, UpdatesATableWithOnlyPrimaryKeyAndVersion)
{
    Queryable<TouchOnlyRow> query(*m_pool);
    ASSERT_EQ(query.insert(TouchOnlyRow{.id = 1, .version = 0}), 1);

    EXPECT_EQ(query.update(TouchOnlyRow{.id = 1, .version = 0}), 1);
    const std::optional<TouchOnlyRow> reloaded = query.where(Column(&TouchOnlyRow::id, "id") == std::int64_t{1}).first();
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->version, 1);
}

TEST_F(QueryableOptimisticLockTest, RejectsNonIntegralVersionColumnWhenGeneratingUpdate)
{
    Queryable<TextVersionedRow> query(*m_pool);
    try
    {
        static_cast<void>(query.update(TextVersionedRow{.id = 1, .version = "0"}));
        FAIL() << "文本版本列没有被拒绝";
    } catch (const AsynGyanis::Base::LogicException &error)
    {
        EXPECT_NE(std::string{error.what()}.find("整数"), std::string::npos) << error.what();
    }
}
