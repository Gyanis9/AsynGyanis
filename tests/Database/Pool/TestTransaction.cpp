// 事务端到端测试 —— 文件 SQLite + 连接池 + ORM 的事务语义。
// 用真实 SQLite 钉住事务的三条核心语义：提交前别的连接看不到本次写入、提交后才可见；回滚后写入不可见（含析构自动回滚
// 与异常穿越两条路径）；重复 commit / rollback 幂等且不会误撤销已提交的工作。另覆盖「事务持有连接」这一前提
// （池满时第二个事务拿不到连接）与批量插入的分块执行。
// 用文件库而不是 ":memory:"：内存库不跨连接共享，只有一个连接时根本观察不到「提交前不可见」，事务语义无从验证。
// 覆盖场景：
// - CommitMakesChangesVisibleToOtherConnections
// - RollbackDiscardsChanges / DestructorRollsBackUncommittedWork
// - DestructorRollsBackEvenWhenPoolResetDoesNot（回滚归属：池的会话复位会替事务析构擦屁股）
// - PoolResetRollsBackTransactionOpenedByRawSql（绕过 beginTransaction() 的手工 BEGIN 也按引擎真值滚掉）
// - ExceptionPathRollsBackAndKeepsDatabaseClean
// - RepeatedCommitAndRollbackAreIdempotent / CommitThenRollbackKeepsCommittedData
// - TransactionHoldsItsConnectionUntilItEnds
// - BatchInsertChunksAutomatically / BatchInsertOnTransactionRollsBackWithIt / BatchInsertEdgeCases
// - StatementsComeFromTheDialect（事务控制语句来自方言）
// - StatementsOnOneTransactionWaitForTheConnection（同一事务上的语句互斥：ORM 查询与 COMMIT 都排队）
// - ChunkedBatchInsertHoldsTheConnectionForTheWholeBatch（分块批量插入整批占住使用权，块间不插进别的语句）
// - SequentialStatementsFromDifferentThreadsBothRun（互斥不等于绑死线程，先后换线程仍可用）
// - SavepointRollbackKeepsEarlierWorkAndLeavesTransactionOpen（保存点回退只退那一段，事务照旧开着）
// - ReleaseSavepointKeepsWorkAfterIt（丢弃名字不回滚也不提交）
// - RollbackToUnknownSavepointFailsWithoutEndingTransaction（回退不存在的点是失败而不是结束事务）
// - SavepointAfterCommitRefusesToSendAnything（已结束的事务上一条都不发，尤其不能顺手起一个新事务）
// - SavepointNameRejectsBlankAndNul / SavepointNameIsQuotedNotSpliced（名字是数据不是 SQL 片段）

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/TableSchema.h"
#include "Database/Sqlite/SqliteConnection.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ========================================================================
// 测试用数据结构
// ========================================================================

namespace
{
    /**
     * @brief 账目表对应的结构体
     */
    struct LedgerRow
    {
        std::int64_t               id;     ///< 主键，重复即触发唯一约束冲突
        std::string                name;   ///< 名称（含中文与特殊字符测试）
        double                     amount; ///< 金额（含负数测试）
        std::optional<std::string> note;   ///< 备注，可空
    };

    /**
     * @brief 构造一行账目数据
     * @param id 主键
     * @param name 名称
     * @param amount 金额
     * @param note 备注，可为空
     * @return LedgerRow 结构体
     */
    [[nodiscard]] LedgerRow makeLedgerRow(const std::int64_t id, std::string name, const double amount, std::optional<std::string> note)
    {
        return LedgerRow{.id = id, .name = std::move(name), .amount = amount, .note = std::move(note)};
    }

} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<LedgerRow>
{
    static constexpr std::string_view kTableName = "ledger";
    static constexpr auto             kColumns   = std::tuple{
            Column(&LedgerRow::id, "id"),
            Column(&LedgerRow::name, "name"),
            Column(&LedgerRow::amount, "amount"),
            Column(&LedgerRow::note, "note"),
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
    using AsynGyanis::Database::DatabaseConnection;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::SqliteConnection;
    using AsynGyanis::Database::SqliteDialect;
    using AsynGyanis::Database::Transaction;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::TestSupport::TemporaryDatabaseFile;

    /**
     * @brief 归还时不做任何会话复位的 SQLite 连接，只用于把「兜底回滚」从池手里摘掉
     *
     * @details SqliteConnection 的实现在归还路径上会按引擎真值滚掉未结束的事务，因此「未提交就析构」
     *          这一条性质同时被事务析构与池复位两道防线保护。要用例能指认是哪一道在起作用，
     *          就得先让其中一道失效；除复位之外本类型不改任何行为。
     */
    class NoSessionResetSqliteConnection final : public SqliteConnection
    {
    public:
        using SqliteConnection::SqliteConnection;

        /**
         * @brief 什么都不做，把连接带着原有会话状态交还池
         * @details 重写 SqliteConnection::resetSessionState()：去掉归还路径上的兜底回滚，其余与基类一致。
         *          刻意不转发给基类——转发一次，用例就又看不到未结束事务的后果了。
         *          交回 true 是有意为之：本用例要考的是「没人在复位事务」而不是「复位失败要丢连接」，
         *          后者由池侧那条 sessionResetFails 的用例守着。
         */
        bool resetSessionState() noexcept override
        {
            return true;
        }
    };

    /**
     * @brief 事务测试夹具
     *
     * @details 每个用例独占一份临时文件库，池上限为 2：一条给事务持有，
     *          另一条用于「站在旁观连接上」观察提交前后的可见性差异。
     */
    class TransactionTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_pool = std::make_unique<ConnectionPool>(
                    [this]()
                    {
                        auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                        // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                        connection->connect();
                        return connection;
                    },
                    makePoolConfiguration(2));

            PooledConnection connection = m_pool->acquire();
            ASSERT_TRUE(connection);
            // 建表走原生 SQL：DDL 不由 ORM 生成
            const auto createResult = connection->execute("CREATE TABLE ledger ("
                                                          "id INTEGER PRIMARY KEY, "
                                                          "name TEXT NOT NULL, "
                                                          "amount REAL, "
                                                          "note TEXT)");
            ASSERT_TRUE(createResult != nullptr) << connection->lastError();
        }

        /**
         * @brief 构造连接池配置
         * @param maximumPoolSize 连接数上限
         * @return PoolConfig 配置对象
         */
        [[nodiscard]] static PoolConfig makePoolConfiguration(std::size_t maximumPoolSize)
        {
            PoolConfig poolConfiguration;
            poolConfiguration.maximumPoolSize = maximumPoolSize;
            // 上限被占满时不要等默认的 5 秒：用例只关心「取不到连接」这一结果
            poolConfiguration.acquireTimeoutMilliseconds = 50;
            return poolConfiguration;
        }

        /**
         * @brief 用旁观连接统计已提交的行数
         * @return std::int64_t 行数
         */
        [[nodiscard]] std::int64_t countCommittedRows() const
        {
            Queryable<LedgerRow> query(*m_pool);
            return query.count();
        }

        /**
         * @brief 用旁观连接按主键取一行
         * @param id 主键
         * @return std::optional<LedgerRow> 命中时返回该行
         */
        [[nodiscard]] std::optional<LedgerRow> findCommittedRow(const std::int64_t id) const
        {
            Queryable<LedgerRow> query(*m_pool);
            return query.where(AsynGyanis::Database::Queryable::Column(&LedgerRow::id, "id") == id).first();
        }

        TemporaryDatabaseFile           m_databaseFile{"Transaction"}; ///< 临时数据库文件，必须先于池析构
        std::unique_ptr<ConnectionPool> m_pool;                        ///< 用例独占的连接池
    };

} // namespace

// ========================================================================
// 提交与回滚的可见性
// ========================================================================

/**
 * @brief 验证提交前别的连接看不到本次写入，提交后才可见
 */
TEST_F(TransactionTest, CommitMakesChangesVisibleToOtherConnections)
{
    {
        Transaction transaction(*m_pool);
        EXPECT_TRUE(transaction.isActive());

        // 走事务的 Queryable：全部语句都落在事务持有的那条连接上
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "张三", 1234.56, std::string("普通备注"))), 1);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(2, "李四", -99.5, std::nullopt)), 1);

        // 提交前：旁观连接（另一条连接）什么都看不到，证明写入还在事务里
        EXPECT_EQ(countCommittedRows(), 0);

        ASSERT_TRUE(transaction.commit()) << transaction.lastError();
        EXPECT_FALSE(transaction.isActive());
    }

    // 提交后：数据对别的连接可见，字段值也能完整回读
    ASSERT_EQ(countCommittedRows(), 2);

    const std::optional<LedgerRow> committedRow = findCommittedRow(1);
    ASSERT_TRUE(committedRow.has_value());
    EXPECT_EQ(committedRow->name, "张三");
    EXPECT_DOUBLE_EQ(committedRow->amount, 1234.56);
    ASSERT_TRUE(committedRow->note.has_value());
    EXPECT_EQ(committedRow->note.value(), "普通备注");
}

/**
 * @brief 验证显式回滚后写入不可见
 */
TEST_F(TransactionTest, RollbackDiscardsChanges)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "会被回滚", 1.0, std::nullopt)), 1);

        ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
        EXPECT_FALSE(transaction.isActive());
    }

    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 验证事务对象析构而未提交时自动回滚
 */
TEST_F(TransactionTest, DestructorRollsBackUncommittedWork)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "未提交", 1.0, std::nullopt)), 1);

        // 故意不提交：离开作用域时由析构补一次 ROLLBACK
    }

    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 验证上一条用例里的回滚确实来自事务析构，而不是池归还时的会话复位
 *
 * @details 池在把连接放回空闲栈前会调用 resetSessionState()，SQLite 的实现顺手滚掉未结束的事务——
 *          两道防线叠着时，把事务析构里的 ROLLBACK 删掉也只会留下一条绿的用例。这里换成一把
 *          「复位空操作」的连接（上限 1，因此下一个借用者拿到的就是同一条），当场问这条连接
 *          「还有事务可收尾吗」：析构真滚过就答「没有」。
 */
TEST_F(TransactionTest, DestructorRollsBackEvenWhenPoolResetDoesNot)
{
    ConnectionPool unresettingPool(
            [this]() -> std::unique_ptr<DatabaseConnection>
            {
                auto connection = std::make_unique<NoSessionResetSqliteConnection>(ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                static_cast<void>(connection->connect());
                return connection;
            },
            makePoolConfiguration(1));

    {
        Transaction          transaction(unresettingPool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "只能由析构回滚", 1.0, std::nullopt)), 1);
        // 刻意不提交也不回滚，且这个池的复位路径什么都不做：回滚只剩事务析构一个来源
    }

    PooledConnection borrowed = unresettingPool.acquire();
    ASSERT_TRUE(borrowed) << "上一条连接没有归还：池里已经无可借的连接";
    // 事务入口在具体驱动上而不是基类：基类只承诺「能不能收尾会话」，收尾动作本身按引擎区分
    auto *borrowedSqliteConnection = dynamic_cast<SqliteConnection *>(borrowed.operator->());
    ASSERT_TRUE(borrowedSqliteConnection != nullptr) << "池交出的不是 SQLite 连接，本用例的判据无从落地";

    // 自动提交模式下没有事务可收尾，因此「回滚失败」恰恰证明事务已经在析构里结束掉了；
    // 若析构漏掉 ROLLBACK，这里会成功回滚并把别人的未结束事务交到手上传给下一个借用者
    EXPECT_FALSE(borrowedSqliteConnection->rollback()) << "析构没有补上 ROLLBACK：未结束的事务串给了下一个借用者";
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 验证归还路径按引擎自报认得出「手工开启的事务」，并把它滚掉
 * @details 上面两条钉住的是事务析构这一来源；本条钉住调用方绕过 `beginTransaction()`、直接
 *          `execute("BEGIN")` 这一来源。SQLite 的复位判据取自 `sqlite3_get_autocommit`（引擎真值）
 *          而不是本类记账，因此这条也滚得掉；MySQL 侧的同一判据由真机用例
 *          `ResetSessionStateRollsBackTransactionOpenedByRawSql` 钉住。池上限取 1，
 *          于是「下一个借用者」必然就是同一条连接，判据不靠调度运气。
 */
TEST_F(TransactionTest, PoolResetRollsBackTransactionOpenedByRawSql)
{
    ConnectionPool singleConnectionPool(
            [this]() -> std::unique_ptr<DatabaseConnection>
            {
                auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                static_cast<void>(connection->connect());
                return connection;
            },
            makePoolConfiguration(1));

    {
        PooledConnection borrowed = singleConnectionPool.acquire();
        ASSERT_TRUE(borrowed);
        ASSERT_TRUE(borrowed->execute("BEGIN") != nullptr) << borrowed->lastError();
        ASSERT_TRUE(borrowed->execute("INSERT INTO ledger (id, name, amount) VALUES (1, '手工开启的事务', 1.0)") != nullptr) << borrowed->lastError();
    }

    // 归还时读引擎真值的那道判据把事务滚掉了：同一连接现在看不到那一行
    Queryable<LedgerRow> observer(singleConnectionPool);
    EXPECT_EQ(observer.count(), 0) << "手工 BEGIN 开出的事务没有滚掉，它正串给下一个借用者";
}

/**
 * @brief 验证异常穿过事务作用域时自动回滚，且不留下半成品数据
 */
TEST_F(TransactionTest, ExceptionPathRollsBackAndKeepsDatabaseClean)
{
    // 先提交一行，作为「事务之外的数据不受影响」的对照
    {
        Queryable<LedgerRow> query(*m_pool);
        EXPECT_EQ(query.insert(makeLedgerRow(1, "已有数据", 5.0, std::nullopt)), 1);
    }

    const auto failingInserts = [this]()
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);

        // 第一条是全新行，第二条与已提交的主键 1 冲突 → 驱动报错 → Queryable 抛异常
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(2, "回滚掉的新行", 7.0, std::nullopt)), 1);
        // 返回值只在成功路径上有意义：本语句注定失败，显式丢弃以表达这一意图
        static_cast<void>(transactionalQuery.insert(makeLedgerRow(1, "重复主键", 8.0, std::nullopt)));

        // 正常流程走不到这里；即便走到，析构也会因为未提交而回滚
    };

    EXPECT_THROW(failingInserts(), std::runtime_error);

    // 异常路径同样完成了回滚：只留下事务之前提交的那一行
    EXPECT_EQ(countCommittedRows(), 1);
    const std::optional<LedgerRow> survivingRow = findCommittedRow(1);
    ASSERT_TRUE(survivingRow.has_value());
    EXPECT_EQ(survivingRow->name, "已有数据");
    EXPECT_FALSE(findCommittedRow(2).has_value());
}

// ========================================================================
// 幂等性
// ========================================================================

/**
 * @brief 验证重复 commit / rollback 都是安全的无操作
 */
TEST_F(TransactionTest, RepeatedCommitAndRollbackAreIdempotent)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "提交一次", 1.0, std::nullopt)), 1);

        ASSERT_TRUE(transaction.commit()) << transaction.lastError();
        // 已结束的事务再次提交/回滚都返回成功，且不会发送多余的语句
        EXPECT_TRUE(transaction.commit());
        EXPECT_TRUE(transaction.rollback());
        EXPECT_FALSE(transaction.isActive());
    }

    EXPECT_EQ(countCommittedRows(), 1);

    {
        Transaction transaction(*m_pool);
        ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
        EXPECT_TRUE(transaction.rollback());
        EXPECT_TRUE(transaction.commit());
        EXPECT_FALSE(transaction.isActive());
    }

    // 已提交的那一行不受后续空事务的提交/回滚影响
    EXPECT_EQ(countCommittedRows(), 1);
}

/**
 * @brief 验证提交之后再调用 rollback 不会撤销已提交的数据
 */
TEST_F(TransactionTest, CommitThenRollbackKeepsCommittedData)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "已提交", 2.0, std::string("备注"))), 1);
        ASSERT_TRUE(transaction.commit()) << transaction.lastError();

        // 提交之后即使再喊回滚，也不该把已经落库的数据撤掉
        EXPECT_TRUE(transaction.rollback());
    }

    EXPECT_EQ(countCommittedRows(), 1);
}

// ========================================================================
// 连接归属
// ========================================================================

/**
 * @brief 验证事务一直占用自己的连接：池上限为 1 时第二个事务取不到连接，第一个仍然可用
 */
TEST_F(TransactionTest, TransactionHoldsItsConnectionUntilItEnds)
{
    // 独立的单连接池：事务一旦借走，池里就没有第二条连接可给
    ConnectionPool singleConnectionPool(
            [this]()
            {
                auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                connection->connect();
                return connection;
            },
            makePoolConfiguration(1));

    Transaction          transaction(singleConnectionPool);
    Queryable<LedgerRow> transactionalQuery(transaction);
    EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "单连接事务", 3.0, std::nullopt)), 1);

    // 第二个事务拿不到连接：绝不允许它复用第一条连接（那会把两个事务混在一条会话上）
    EXPECT_THROW(static_cast<void>(Transaction(singleConnectionPool)), std::runtime_error);

    // 第一个事务不受影响：仍然可以继续写并提交
    EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(2, "仍然可用", 4.0, std::nullopt)), 1);
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 2);
}

// ========================================================================
// 批量插入
// ========================================================================

/**
 * @brief 验证行数超过参数上限时自动分块，且全部行都写入成功
 */
TEST_F(TransactionTest, BatchInsertChunksAutomatically)
{
    // LedgerRow 有 4 列，SQLite 单条语句上限 999 个参数 → 每批最多 249 行；
    // 500 行会拆成 249 + 249 + 2 三块，若不分块，SQLite 会直接以
    // "too many SQL variables" 拒绝执行
    std::vector<LedgerRow> rows;
    rows.reserve(500);
    for (std::int64_t id = 1; id <= 500; ++id)
    {
        rows.push_back(makeLedgerRow(id, "批量-" + std::to_string(id), static_cast<double>(id), std::nullopt));
    }

    Queryable<LedgerRow> query(*m_pool);
    EXPECT_EQ(query.insertBatch(rows), static_cast<std::int64_t>(rows.size()));
    EXPECT_EQ(countCommittedRows(), 500);

    // 分块后每一行都必须落在正确的列上：抽查跨越三个块的几个主键
    const std::optional<LedgerRow> firstOfSecondChunk = findCommittedRow(250);
    ASSERT_TRUE(firstOfSecondChunk.has_value());
    EXPECT_EQ(firstOfSecondChunk->name, "批量-250");
    EXPECT_DOUBLE_EQ(firstOfSecondChunk->amount, 250.0);

    const std::optional<LedgerRow> lastRow = findCommittedRow(500);
    ASSERT_TRUE(lastRow.has_value());
    EXPECT_EQ(lastRow->name, "批量-500");
}

/**
 * @brief 验证绑定事务时批量插入的分块共用事务连接，一起回滚
 */
TEST_F(TransactionTest, BatchInsertOnTransactionRollsBackWithIt)
{
    std::vector<LedgerRow> rows;
    rows.reserve(500);
    for (std::int64_t id = 1; id <= 500; ++id)
    {
        rows.push_back(makeLedgerRow(id, "事务批量", static_cast<double>(id), std::nullopt));
    }

    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);

        // 三块全部落在事务连接上，因此一次回滚就能把三块一起撤销
        EXPECT_EQ(transactionalQuery.insertBatch(rows), static_cast<std::int64_t>(rows.size()));
        ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
    }

    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 验证空行集合不产生任何语句，冲突主键则整体回滚
 */
TEST_F(TransactionTest, BatchInsertEdgeCases)
{
    {
        Queryable<LedgerRow> query(*m_pool);
        // 空集合：返回 0，且不生成语句
        EXPECT_EQ(query.insertBatch(std::vector<LedgerRow>{}), 0);
    }
    EXPECT_EQ(countCommittedRows(), 0);

    // 分块插入中途遇到唯一约束冲突：整个事务回滚，一行都不留
    std::vector<LedgerRow> rows;
    for (std::int64_t id = 1; id <= 500; ++id)
    {
        rows.push_back(makeLedgerRow(id, "冲突批量", static_cast<double>(id), std::nullopt));
    }
    rows.back() = makeLedgerRow(1, "重复主键", 0.0, std::nullopt); // 与第一行主键冲突，且落在最后一个分块

    {
        Queryable<LedgerRow> query(*m_pool);
        EXPECT_THROW(static_cast<void>(query.insertBatch(rows)), std::runtime_error);
    }

    // 前两块已经写入，但本地事务在异常路径上回滚，库里仍是 0 行
    EXPECT_EQ(countCommittedRows(), 0);
}

// ========================================================================
// 方言事务语句
// ========================================================================

/**
 * @brief 验证事务控制语句来自方言而不是写死在事务对象里
 */
TEST(TransactionDialectStatements, StatementsComeFromTheDialect)
{
    const SqliteDialect dialect;

    // SQLite 用 IMMEDIATE 立刻取写锁；换方言（如 MySQL 的 START TRANSACTION）时只需换实现
    EXPECT_EQ(dialect.beginTransactionStatement(), "BEGIN IMMEDIATE");
    EXPECT_EQ(dialect.commitStatement(), "COMMIT");
    EXPECT_EQ(dialect.rollbackStatement(), "ROLLBACK");
}

// ========================================================================
// 同一事务上的语句互斥
// ========================================================================

/**
 * @brief 钉住「走同一事务的语句互斥执行」：连接使用权被占时，ORM 查询与 COMMIT 都要排队
 * @details 一个驱动句柄只有一条协议流，而 Queryable 的异步接口会把语句投到 AsyncExecutor 的
 *          工作线程上执行——调用方全程待在自己的线程里，也可能同时有两条语句踩同一条连接。
 *          用例把前提构造成确定成立而非赌调度：主线程先占住使用权时，后台的查询与提交**不可能**
 *          完成（拿不到锁就到不了驱动），交还之后两者才依次跑完。
 */
TEST_F(TransactionTest, StatementsOnOneTransactionWaitForTheConnection)
{
    Transaction transaction(*m_pool);

    std::unique_lock<std::mutex> heldLock = transaction.acquireStatementLock();
    ASSERT_TRUE(heldLock.owns_lock());

    std::future<std::vector<LedgerRow>> rowsFuture   = std::async(std::launch::async,
                                                                  [&transaction]()
                                                                  {
                                                                    Queryable<LedgerRow> transactionalQuery(transaction);
                                                                    return transactionalQuery.toList();
                                                                  });
    std::future<bool>                   commitFuture = std::async(std::launch::async, [&transaction]() { return transaction.commit(); });

    // 使用权没交还之前两侧都到不了终点
    EXPECT_EQ(rowsFuture.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout) << "查询没等连接使用权：它会与占着连接的语句同时踩同一个驱动句柄";
    EXPECT_EQ(commitFuture.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout) << "COMMIT 没等连接使用权：它可能在语句执行到一半时落下";

    heldLock.unlock();

    std::vector<LedgerRow> rows = rowsFuture.get();
    EXPECT_TRUE(commitFuture.get());
    EXPECT_TRUE(rows.empty());
    EXPECT_FALSE(transaction.isActive());
}

/**
 * @brief 钉住分块批量插入整批占住连接使用权，而不只占住一条语句
 * @details 单条语句的路径经 acquireConnection() 取使用权，分块那条分支是「一条接一条地往同一条
 *          连接上发」：不整批占住，块与块之间就能插进别的语句，一个驱动句柄上的协议流随即交错。
 *          用例不赌调度：主线程占住使用权时批量插入**不可能**到达驱动，交还之后三块才依次写完。
 */
TEST_F(TransactionTest, ChunkedBatchInsertHoldsTheConnectionForTheWholeBatch)
{
    // 4 列 × 每块 249 行贴近 999 参数上限，500 行拆成三块：只有一条语句的粒度会漏掉这种交错
    std::vector<LedgerRow> rows;
    rows.reserve(500);
    for (std::int64_t id = 1; id <= 500; ++id)
    {
        rows.push_back(makeLedgerRow(id, "分块批量", static_cast<double>(id), std::nullopt));
    }

    Transaction transaction(*m_pool);

    std::unique_lock<std::mutex> heldLock = transaction.acquireStatementLock();
    ASSERT_TRUE(heldLock.owns_lock());

    std::future<std::int64_t> insertedRows = std::async(std::launch::async,
                                                        [&transaction, &rows]()
                                                        {
                                                            Queryable<LedgerRow> transactionalQuery(transaction);
                                                            return transactionalQuery.insertBatch(rows);
                                                        });

    EXPECT_EQ(insertedRows.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout) << "分块批量插入没等连接使用权：块语句会与并发语句交错在同一个驱动句柄上";

    heldLock.unlock();

    EXPECT_EQ(insertedRows.get(), static_cast<std::int64_t>(rows.size()));
    EXPECT_TRUE(transaction.commit()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 500);
}

/**
 * @brief 钉住互斥不等于「绑死线程」：先后两条语句可以来自不同线程
 * @details 异步路径每轮都可能换到一个不同的工作线程上，因此要排除的只有「同时」而不是「不同线程」。
 *          若这里改成记录线程号并拒绝，合法的异步用法会被整片误伤。
 */
TEST_F(TransactionTest, SequentialStatementsFromDifferentThreadsBothRun)
{
    Transaction transaction(*m_pool);

    static_cast<void>(std::async(std::launch::async,
                                 [&transaction]()
                                 {
                                     Queryable<LedgerRow> transactionalQuery(transaction);
                                     return static_cast<void>(transactionalQuery.insert(makeLedgerRow(1, "第一个线程", 1.5, std::nullopt)));
                                 })
                              .get());

    static_cast<void>(std::async(std::launch::async,
                                 [&transaction]()
                                 {
                                     Queryable<LedgerRow> transactionalQuery(transaction);
                                     return static_cast<void>(transactionalQuery.insert(makeLedgerRow(2, "第二个线程", 2.5, std::nullopt)));
                                 })
                              .get());

    EXPECT_TRUE(transaction.commit());
    EXPECT_EQ(countCommittedRows(), 2);
}

// ========================================================================
// 保存点：同一笔事务内部的可回退点
// ========================================================================

/**
 * @brief 回退到保存点只撤那一段，事务照旧开着，提交带走更早与更晚的工作
 * @details 这条钉的是「嵌套回滚」的整个卖点：`ROLLBACK TO SAVEPOINT` 之后
 *          `isActive()` 必须仍为真（否则调用方退一段就丢掉整笔），而退掉的只有那一点之后的写入。
 *          证伪：把 `rollbackToSavepoint` 发成 `ROLLBACK`（少写 TO SAVEPOINT 那截），
 *          事务当场结束、第三条写不进、`commit()` 失败——红在这一条上。
 */
TEST_F(TransactionTest, SavepointRollbackKeepsEarlierWorkAndLeavesTransactionOpen)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);

        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "保存点之前", 1.0, std::nullopt)), 1);
        ASSERT_TRUE(transaction.savepoint("keep")) << transaction.lastError();
        EXPECT_TRUE(transaction.isActive()) << "立保存点不该改变事务状态";

        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(2, "要被退掉", 2.0, std::nullopt)), 1);
        ASSERT_TRUE(transaction.rollbackToSavepoint("keep")) << transaction.lastError();
        EXPECT_TRUE(transaction.isActive()) << "回退到保存点把整笔事务结束了";

        // 退完之后还能接着在同一笔事务里写，并一次提交
        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(3, "回退之后又写的", 3.0, std::nullopt)), 1);
        // 三段工作都还在同一笔未提交的事务里：旁观连接什么都看不到
        EXPECT_EQ(countCommittedRows(), 0);

        ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    }

    ASSERT_EQ(countCommittedRows(), 2);
    ASSERT_TRUE(findCommittedRow(1).has_value()) << "保存点之前的工作被一起退掉了：回退范围写错";
    EXPECT_FALSE(findCommittedRow(2).has_value()) << "回退到保存点没有撤掉那之后的写入";
    ASSERT_TRUE(findCommittedRow(3).has_value()) << "回退之后的写入没被提交带走";
}

/**
 * @brief 丢弃保存点只让名字消失，不回滚也不提交它之后的工作
 * @details 证伪：把 `RELEASE SAVEPOINT x` 发成 `COMMIT`，第二条写入会在 `commit()` 之前就被
 *          旁观连接看到（那条断言正是本用例的判据之一）。
 */
TEST_F(TransactionTest, ReleaseSavepointKeepsWorkAfterIt)
{
    {
        Transaction          transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);

        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "第一段", 1.0, std::nullopt)), 1);
        ASSERT_TRUE(transaction.savepoint("step")) << transaction.lastError();
        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(2, "第二段", 2.0, std::nullopt)), 1);

        ASSERT_TRUE(transaction.releaseSavepoint("step")) << transaction.lastError();
        EXPECT_TRUE(transaction.isActive());
        // 丢弃之后名字不再可用，这条是「只让名字消失」的反面判据：引擎报的是没有这个保存点
        EXPECT_FALSE(transaction.rollbackToSavepoint("step")) << "名字丢弃后仍可回退：RELEASE 什么都没做";

        // 还没提交：丢弃保存点不能把工作一起落库
        EXPECT_EQ(countCommittedRows(), 0);
        ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    }

    EXPECT_EQ(countCommittedRows(), 2) << "丢弃保存点把已写入的行一起弄没了";
}

/**
 * @brief 回退一个没立过的名字是失败，而不是把事务结束掉
 * @details 两个引擎对未知保存点回的都是「没有这个保存点」这一类错误，事务照旧开着——
 *          调用方据此可以「试一下回退，不行就继续干活」。证伪：让失败路径也走 `rollback()`
 *          那条（把 `m_isActive` 一起置假），本用例里随后的 `commit()` 会红。
 */
TEST_F(TransactionTest, RollbackToUnknownSavepointFailsWithoutEndingTransaction)
{
    Transaction          transaction(*m_pool);
    Queryable<LedgerRow> transactionalQuery(transaction);

    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "留在事务里", 1.0, std::nullopt)), 1);
    ASSERT_TRUE(transaction.savepoint("known")) << transaction.lastError();

    EXPECT_FALSE(transaction.rollbackToSavepoint("ghost"));
    EXPECT_FALSE(transaction.lastError().empty()) << "失败没有给出可读原因";
    EXPECT_TRUE(transaction.isActive()) << "回退不存在的保存点把整笔事务结束了";

    // 失败之后事务仍然可用：继续写、继续提交
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(2, "失败之后写的", 2.0, std::nullopt)), 1);
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();

    ASSERT_EQ(countCommittedRows(), 2);
    ASSERT_TRUE(findCommittedRow(2).has_value()) << "失败的回退把已经写入的那行一起弄丢了";
}

/**
 * @brief 事务结束后再要保存点：一条语句都不发
 * @details 这条不是「参数校验」那类洁癖，而是 SQLite 的真行为：没有活动事务时 `SAVEPOINT` 等价于
 *          `BEGIN DEFERRED`，会**另起一个事务**。而本对象的 `m_isActive` 已假，之后没人提交也没人回滚，
 *          那条连接就带着一个没人负责的事务回到池里（下一位借用者的语句悄悄并进它）。
 *          判据直接问这条连接「还有事务可收尾吗」：拒绝到位就答「没有」（回滚失败）。
 *          证伪：删掉 `checkSavepointPremises` 里 `m_isActive` 那一段，这里会红。
 */
TEST_F(TransactionTest, SavepointAfterCommitRefusesToSendAnything)
{
    Transaction          transaction(*m_pool);
    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "已提交", 1.0, std::nullopt)), 1);
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    ASSERT_FALSE(transaction.isActive());

    EXPECT_FALSE(transaction.savepoint("late"));
    // 本仓库的用例只链 gtest（没有 gmock），因此按「我们自己那句话里的关键词」判：
    // 这句话是本类写的，不跟着底层库的措辞变
    EXPECT_NE(transaction.lastError().find("已结束"), std::string::npos) << transaction.lastError();
    EXPECT_FALSE(transaction.rollbackToSavepoint("late"));
    EXPECT_FALSE(transaction.releaseSavepoint("late"));

    auto *sqliteConnection = dynamic_cast<SqliteConnection *>(&transaction.connection());
    ASSERT_TRUE(sqliteConnection != nullptr) << "池交出的不是 SQLite 连接，本用例的判据无从落地";
    EXPECT_FALSE(sqliteConnection->rollback()) << "拒绝路径照样发了 SAVEPOINT：它在 SQLite 上会另起一个没人负责的事务";

    EXPECT_EQ(countCommittedRows(), 1);
}

/**
 * @brief 名字为空、全是空白或含 NUL 时当场抛，而不是发出去让引擎报错
 * @details 抛完之后事务必须仍然完好——判据是名字检查发生在发语句之前：本用例随后照常写入并提交。
 */
TEST_F(TransactionTest, SavepointNameRejectsBlankAndNul)
{
    Transaction transaction(*m_pool);

    EXPECT_THROW(static_cast<void>(transaction.savepoint("")), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(static_cast<void>(transaction.savepoint("   \t\r\n ")), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(static_cast<void>(transaction.savepoint(std::string_view("a\0b", 3))), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(static_cast<void>(transaction.rollbackToSavepoint("")), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(static_cast<void>(transaction.releaseSavepoint("  ")), AsynGyanis::Base::InvalidArgumentException);

    // 三次拒绝都没碰过这条连接：事务仍在进行，写入与提交照常
    EXPECT_TRUE(transaction.isActive());
    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "名字被拒之后写的", 1.0, std::nullopt)), 1);
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 1);
}

/**
 * @brief 调用方给的保存点名是数据，不是 SQL 片段
 * @details 方言一律把名字包进本引擎的引用符（内部的同字符翻倍），因此带着引号、分号与注释符的串
 *          只能是一个名字。这条同时是「改天有人图省事把名字直接拼进文本」的反注入判据：
 *          那样一来 `;` 之后的内容就会变成第二条语句（SQLite 的 `execute()` 与 MySQL 的文本协议
 *          都刻意不执行多条语句，会当场拒——红在本用例的「表还在、行还在」这两句上）。
 */
TEST_F(TransactionTest, SavepointNameIsQuotedNotSpliced)
{
    constexpr std::string_view hostileName = R"(x"; DROP TABLE ledger; --)";

    Transaction          transaction(*m_pool);
    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "带着怪名字", 1.0, std::nullopt)), 1);

    ASSERT_TRUE(transaction.savepoint(hostileName)) << transaction.lastError();
    ASSERT_TRUE(transaction.rollbackToSavepoint(hostileName)) << transaction.lastError();
    ASSERT_TRUE(transaction.releaseSavepoint(hostileName)) << transaction.lastError();

    ASSERT_TRUE(transaction.commit()) << transaction.lastError();

    // 表没被 drop，行也没少：那句 SQL 片段自始至终只是个名字
    ASSERT_EQ(countCommittedRows(), 1);
    ASSERT_TRUE(findCommittedRow(1).has_value());
}
