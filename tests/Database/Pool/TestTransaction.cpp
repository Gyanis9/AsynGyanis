// 事务端到端测试 —— 文件 SQLite + 连接池 + ORM 的事务语义。
// 用真实 SQLite 钉住事务的三条核心语义：提交前别的连接看不到本次写入、提交后才可见；回滚后写入不可见（含析构自动回滚
// 与异常穿越两条路径）；重复 commit / rollback 幂等且不会误撤销已提交的工作。另覆盖「事务持有连接」这一前提
// （池满时第二个事务拿不到连接）与批量插入的分块执行。
// 用文件库而不是 ":memory:"：内存库不跨连接共享，只有一个连接时根本观察不到「提交前不可见」，事务语义无从验证。
// 覆盖场景：
// CommitMakesChangesVisibleToOtherConnections
// RollbackDiscardsChanges / DestructorRollsBackUncommittedWork
// DestructorRollsBackEvenWhenPoolResetDoesNot（回滚归属：池的会话复位会替事务析构擦屁股）
// PoolResetRollsBackTransactionOpenedByRawSql（绕过 beginTransaction() 的手工 BEGIN 也按引擎真值滚掉）
// ExceptionPathRollsBackAndKeepsDatabaseClean
// RepeatedCommitAndRollbackAreIdempotent / CommitThenRollbackKeepsCommittedData
// TransactionHoldsItsConnectionUntilItEnds
// BatchInsertChunksAutomatically / BatchInsertOnTransactionRollsBackWithIt / BatchInsertEdgeCases
// StatementsComeFromTheDialect（事务控制语句来自方言）
// RefusesUnsupportedIsolationLevelWithoutLeakingTheConnection（SQLite 拒绝非默认级别并点名，被拒的连接还回池；Default 照常开局）
// StatementsOnOneTransactionWaitForTheConnection（同一事务上的语句互斥：ORM 查询与 COMMIT 都排队）
// ChunkedBatchInsertHoldsTheConnectionForTheWholeBatch（分块批量插入整批占住使用权，块间不插进别的语句）
// SequentialStatementsFromDifferentThreadsBothRun（互斥不等于绑死线程，先后换线程仍可用）
// SavepointRollbackKeepsEarlierWorkAndLeavesTransactionOpen（保存点回退只退那一段，事务照旧开着）
// ReleaseSavepointKeepsWorkAfterIt（丢弃名字不回滚也不提交）
// RollbackToUnknownSavepointFailsWithoutEndingTransaction（回退不存在的点是失败而不是结束事务）
// SavepointAfterCommitRefusesToSendAnything（已结束的事务上一条都不发，尤其不能顺手起一个新事务）
// SavepointNameRejectsBlankAndNul / SavepointNameIsQuotedNotSpliced（名字是数据不是 SQL 片段）
// StatementAfterTimeLimitIsRejected（整笔时限到期后续语句被拒，已写入的行没落库）
// CommitAfterTimeLimitIsRefused（时限把「跑飞的事务还能落库」变成不可能）
// RollbackAfterTimeLimitStillWorks（收尾永不被拒：拦住回滚等于把事务钉得更死）
// SavepointStatementsAfterTimeLimitAreRefused（保存点也是往前走的一步）
// ChunkedBatchInsertAfterTimeLimitSendsNoChunk（分块那条分支同样过闸）
// TransactionHasNoTimeLimitByDefault / NonPositiveTimeLimitMeansNoLimit（默认与 0／负数都是不设限）
// EndedTransactionIsNotReportedAsTimedOut（「已结束」与「超时」两条原因不混成一句）
// StatementExecutionForwardsRemainingBudgetToQueryTimeout（剩余预算透传到语句级超时的读数）
// ForwardingBudgetNeverLoosensTheConnectionsOwnTimeout（取更严者：预算比会话超时宽时不动它）
// ForwardingBudgetTightensAnUnlimitedSessionTimeout（会话超时 0＝不设限，透传不能把预算吞掉）
// NoTimeLimitLeavesTheConnectionsTimeoutAlone（不设限的事务不碰连接的会话超时）
// BudgetCheckedAgainAfterWaitingForTheConnectionLock（排队等锁期间越限，取到锁后又被拒）

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/ConnectionUnavailableException.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Common/TransactionTimeoutException.h"
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
#include "AssertionSupport.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    using AsynGyanis::Database::ConnectionUnavailableException;
    using AsynGyanis::Database::DatabaseConnection;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::QueryExecutionException;
    using AsynGyanis::Database::SqliteConnection;
    using AsynGyanis::Database::SqliteDialect;
    using AsynGyanis::Database::Transaction;
    using AsynGyanis::Database::TransactionIsolationLevel;
    using AsynGyanis::Database::TransactionTimeoutException;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::TestSupport::TemporaryDatabaseFile;

    /**
     * @brief 时限用例的额度：只用来「让一笔事务能写成、另一笔注定越限」
     * @details 单条本地 SQLite 语句实测只用亚毫秒，200 毫秒留了三个数量级的余量，
     *          因此额度内那次插入不会被满载的 runner 误判成越限
     */
    constexpr std::chrono::milliseconds kShortTransactionTimeLimit{200};

    /**
     * @brief 睡过时限用的时长：必须显著大于 kShortTransactionTimeLimit
     * @details `sleep_for` 只可能超时而不会不足，所以这个数只管「已经越限」这一侧成立
     */
    constexpr std::chrono::milliseconds kSleepPastTheLimit{500};

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
    ASSERT_HAS_VALUE(committedRow);
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

    const std::string failure = AsynGyanis::TestSupport::requireFailureText<std::runtime_error>([&] { failingInserts(); }, "重复主键");
    EXPECT_FALSE(failure.empty()) << "驱动失败没有带任何原因";

    // 异常路径同样完成了回滚：只留下事务之前提交的那一行
    EXPECT_EQ(countCommittedRows(), 1);
    const std::optional<LedgerRow> survivingRow = findCommittedRow(1);
    ASSERT_HAS_VALUE(survivingRow);
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
    try
    {
        static_cast<void>(Transaction(singleConnectionPool));
        FAIL() << "池里唯一一条连接已被占用，第二个事务却拿到了连接";
    } catch (const ConnectionUnavailableException &failure)
    {
        const std::string message = failure.what();
        EXPECT_NE(message.find("无法从连接池获取连接"), std::string::npos) << "原因没有点明取连接失败：" << message;
        EXPECT_NE(message.find("事务未开启"), std::string::npos) << "原因没有说明事务未开启：" << message;
    }

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
    ASSERT_HAS_VALUE(firstOfSecondChunk);
    EXPECT_EQ(firstOfSecondChunk->name, "批量-250");
    EXPECT_DOUBLE_EQ(firstOfSecondChunk->amount, 250.0);

    const std::optional<LedgerRow> lastRow = findCommittedRow(500);
    ASSERT_HAS_VALUE(lastRow);
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
        const std::string    failure =
                AsynGyanis::TestSupport::requireFailureText<std::runtime_error>([&query, &rows] { static_cast<void>(query.insertBatch(rows)); }, "批量中途冲突");
        EXPECT_FALSE(failure.empty()) << "驱动失败没有带任何原因";
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
// 隔离级别入口
// ========================================================================

/**
 * @brief 钉住隔离级别入口的两端：SQLite 拒绝非默认级别（点名引擎与级别、连接不泄漏），Default 照常开局
 * @details 拒绝发生在 BEGIN 之前，且已借出的连接要随构造失败还回池——用例把这一点变成可见事实：
 *          拒一次之后池仍能借满两条（夹具池上限为 2），说明那具半成品对象的栈展开把连接交了回来。
 *          能给的引擎照发设置语句（MySQL 侧文本由方言单测逐条钉死），给不出的当场拒，
 *          不静默降级成引擎默认
 */
TEST_F(TransactionTest, RefusesUnsupportedIsolationLevelWithoutLeakingTheConnection)
{
    try
    {
        Transaction unsupported(*m_pool, TransactionIsolationLevel::ReadCommitted);
        FAIL() << "SQLite 不支持按事务请求隔离级别，构造本该被拒";
    } catch (const AsynGyanis::Base::InvalidArgumentException &failure)
    {
        const std::string message = failure.what();
        EXPECT_NE(message.find("Sqlite"), std::string::npos) << "拒绝文案没点名引擎：" << message;
        EXPECT_NE(message.find("读已提交"), std::string::npos) << "拒绝文案没点名级别：" << message;
    }

    // 连接不泄漏：被拒一次之后，上限 2 的池仍能借满两条
    PooledConnection first  = m_pool->acquire();
    PooledConnection second = m_pool->acquire();
    ASSERT_TRUE(first) << "被拒构造把连接留在了手里：池少了一条";
    ASSERT_TRUE(second) << "被拒构造把连接留在了手里：池少了一条";
    first.release();
    second.release();

    // 正向对照：Default（单参构造的语义）照常开启，拒绝没有把整条路打死
    Transaction defaulted(*m_pool, TransactionIsolationLevel::Default);
    EXPECT_TRUE(defaulted.isActive()) << "默认级别的事务没能开启";
    EXPECT_TRUE(defaulted.commit());
}

// ========================================================================
// 同一事务上的语句互斥
// ========================================================================

/**
 * @brief 钉住「走同一事务的语句互斥执行」：连接使用权被占时，ORM 查询与 COMMIT 都要排队
 * @details 一个驱动句柄只有一条协议流，而 Queryable 的异步接口会把语句投到 AsyncExecutor 的
 *          工作线程上执行——调用方全程待在自己的线程里，也可能同时有两条语句踩同一条连接。
 *          用例把前提构造成确定成立而非赌调度：主线程先占住使用权时，后台的查询与提交不可能
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
 *          用例不赌调度：主线程占住使用权时批量插入不可能到达驱动，交还之后三块才依次写完。
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
 *          `BEGIN DEFERRED`，会另起一个事务。而本对象的 `m_isActive` 已假，之后没人提交也没人回滚，
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

    const auto requireRejected = [&transaction](const std::string_view expectedReason, const auto &action)
    {
        const std::string reason = AsynGyanis::TestSupport::requireFailureText<AsynGyanis::Base::InvalidArgumentException>(action, "保存点名");
        EXPECT_NE(reason.find(expectedReason), std::string::npos) << "原因没有点明拒绝的是哪一种形状：" << reason;
    };
    requireRejected("非空且不含 NUL", [&] { static_cast<void>(transaction.savepoint("")); });
    requireRejected("不能全是空白字符", [&] { static_cast<void>(transaction.savepoint("   \t\r\n ")); });
    requireRejected("非空且不含 NUL", [&] { static_cast<void>(transaction.savepoint(std::string_view("a\0b", 3))); });
    requireRejected("非空且不含 NUL", [&] { static_cast<void>(transaction.rollbackToSavepoint("")); });
    requireRejected("不能全是空白字符", [&] { static_cast<void>(transaction.releaseSavepoint("  ")); });

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

// ========================================================================
// 整笔事务时限：跑飞的事务不能再往前走
// ========================================================================

/**
 * @brief 时限到期后走这笔事务的下一条语句被拒，而额度内那次写入照旧有效
 * @details 钉住时限的卖点本身：拒的是「再往前发一条语句」，不是把已做过的工作悄悄落库。
 *          拒绝之后事务必须仍算活动（`isActive()` 为真），否则析构就不去补那次 ROLLBACK。
 *          证伪：删掉 `Queryable::acquireConnection()` 里 `isTimeLimitExceeded()` 那一段，
 *          越限后的插入会照常发出去，这里的 `EXPECT_TRUE(rejected)` 当场变红。
 */
TEST_F(TransactionTest, StatementAfterTimeLimitIsRejected)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "额度内写的", 1.0, std::nullopt)), 1) << transaction.lastError();

    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

    bool        rejectedWithTimeout = false;
    std::string failureReason;
    try
    {
        static_cast<void>(transactionalQuery.insert(makeLedgerRow(2, "越限之后写的", 2.0, std::nullopt)));
    } catch (const TransactionTimeoutException &failure)
    {
        rejectedWithTimeout = true;
        failureReason       = failure.what();
    }
    EXPECT_TRUE(rejectedWithTimeout) << "整笔时限已到期，这条语句却照样发给了数据库";
    EXPECT_NE(failureReason.find("时限"), std::string::npos) << failureReason;

    // 拒绝不改变事务状态：调用方还得靠收尾那一步把这条连接上的行锁放掉
    EXPECT_TRUE(transaction.isActive());
    EXPECT_EQ(countCommittedRows(), 0) << "被拒的语句还是把第二行写进了库";

    ASSERT_TRUE(transaction.rollback()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 整笔时限到期时 COMMIT 一条都不发：一笔跑飞的事务落不了库
 * @details 与上一条合起来才是完整语义——只挡语句不挡提交，等于允许「等到时限到了再一次性提交」。
 *          拒提交之后 `m_isActive` 必须仍为真，让析构去补那次 ROLLBACK。
 *          证伪：删掉 `Transaction::commit()` 里 `isTimeLimitExceeded()` 那一段，
 *          `EXPECT_FALSE` 与末尾那句「库里 0 行」会同时变红。
 */
TEST_F(TransactionTest, CommitAfterTimeLimitIsRefused)
{
    {
        Transaction transaction(*m_pool);
        transaction.setTimeLimit(kShortTransactionTimeLimit);

        Queryable<LedgerRow> transactionalQuery(transaction);
        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "越限前要提交的", 1.0, std::nullopt)), 1) << transaction.lastError();

        std::this_thread::sleep_for(kSleepPastTheLimit);
        ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

        EXPECT_FALSE(transaction.commit()) << "时限到期后 COMMIT 照样发了出去";
        EXPECT_NE(transaction.lastError().find("时限"), std::string::npos) << transaction.lastError();
        EXPECT_TRUE(transaction.isActive()) << "拒提交却把事务标成已结束：析构就不会补那次回滚了";
    }

    EXPECT_EQ(countCommittedRows(), 0) << "被拒的提交还是把数据落了库";
}

/**
 * @brief 时限只挡「往前走」：越限之后的 rollback() 永远放行
 * @details 三条拒绝出口之外的第四条判据，也是最容易写反的一条——把回滚也拒了，超时反而把事务钉得更死：
 *          行锁一直留到连接被池丢弃，调用方连放掉它的入口都没有。
 *          证伪：在 `Transaction::rollback()` 开头补一段与 `commit()` 相同的时限判断，
 *          这里的 `EXPECT_TRUE` 会红，且 `lastError()` 变成那句超时原因。
 */
TEST_F(TransactionTest, RollbackAfterTimeLimitStillWorks)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "等着被回滚", 1.0, std::nullopt)), 1) << transaction.lastError();

    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

    EXPECT_TRUE(transaction.rollback()) << transaction.lastError();
    EXPECT_FALSE(transaction.isActive());
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 三条保存点语句同样是「往前走」：越限之后一条都不发
 * @details 保存点容易被当成回退工具而以为该放行，但它同样是发往那笔事务的语句；而且越限之后
 *          唯一出路是整笔作废——退到一个点上再提交仍会被 `commit()` 那道闸拒掉。
 *          名字检查排在时限之前：坏名字是使用错误，无论越限与否都当场抛。
 *          证伪：删掉 `Transaction::checkSavepointPremises()` 里第三段时限判断，
 *          三个 `EXPECT_FALSE` 会同时变红（SQLite 上这些保存点语句是真能成功的）。
 */
TEST_F(TransactionTest, SavepointStatementsAfterTimeLimitAreRefused)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "额度内写的", 1.0, std::nullopt)), 1) << transaction.lastError();
    ASSERT_TRUE(transaction.savepoint("early")) << transaction.lastError();

    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

    EXPECT_FALSE(transaction.savepoint("late"));
    EXPECT_NE(transaction.lastError().find("时限"), std::string::npos) << transaction.lastError();
    EXPECT_FALSE(transaction.rollbackToSavepoint("early")) << transaction.lastError();
    EXPECT_FALSE(transaction.releaseSavepoint("early")) << transaction.lastError();

    // 坏名字仍按使用错误当场抛，不被「时限已到」那条回 false 的通道吃掉
    EXPECT_THROW(static_cast<void>(transaction.savepoint("")), AsynGyanis::Base::InvalidArgumentException);

    EXPECT_TRUE(transaction.isActive());
    EXPECT_TRUE(transaction.rollback()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 分块批量插入同样过闸：越限之后一块都不发
 * @details 专门盯分块那条分支：它原先不经 `acquireConnection()`（自己取锁、自己拿连接），
 *          整笔时限的闸会漏在它身上，于是一批语句悄悄落进已经超时的事务。
 *          500 行 × 4 列贴近 SQLite 的 999 参数上限，会拆成三块，单条语句的粒度盖不住这种形状。
 *          证伪：把该分支改回直接 `transaction->acquireStatementLock()` + `transaction->connection()`，
 *          这里的 `EXPECT_THROW` 变红（三块会照常写完，只是随后被回滚掉）。
 */
TEST_F(TransactionTest, ChunkedBatchInsertAfterTimeLimitSendsNoChunk)
{
    std::vector<LedgerRow> rows;
    rows.reserve(500);
    for (std::int64_t id = 1; id <= 500; ++id)
    {
        // 主键从 1001 起：额度内那条写入占着 1，越限批量不能与它撞主键——
        // 否则缺闸时失败会以「唯一约束冲突」的面目出现，把这条判据指向错的分支
        rows.push_back(makeLedgerRow(1000 + id, "越限批量", static_cast<double>(id), std::nullopt));
    }

    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "额度内写的", 1.0, std::nullopt)), 1) << transaction.lastError();

    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

    EXPECT_THROW(static_cast<void>(transactionalQuery.insertBatch(rows)), TransactionTimeoutException);

    EXPECT_TRUE(transaction.rollback()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 默认不设限：调用方没喊过时限的事务，行为与加这个功能之前逐字一致
 * @details 「加功能不许改变既有语义」的对照用例——同一个夹具、同样的睡眠时长，
 *          但一次都没调 `setTimeLimit()`，写入与提交都必须成功。
 *          证伪：把 `m_timeLimit` 的默认值改成任何正数，或让 `isTimeLimitExceeded()` 把 0 当成「立刻到期」。
 */
TEST_F(TransactionTest, TransactionHasNoTimeLimitByDefault)
{
    Transaction transaction(*m_pool);
    EXPECT_EQ(transaction.timeLimit(), std::chrono::milliseconds(0)) << "默认必须是 0（不设限）";

    std::this_thread::sleep_for(kSleepPastTheLimit);
    EXPECT_FALSE(transaction.isTimeLimitExceeded());

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "不设限照旧可写", 1.0, std::nullopt)), 1) << transaction.lastError();
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 1);
}

/**
 * @brief 0 与负数一律当作不设限，而不是「额度为零、当场到期」
 * @details `setTimeLimit()` 是 noexcept 的设值通道，把非正数解释成立即到期，
 *          「用 0 关掉时限」这种写法就会静默把整笔事务废掉。
 *          证伪：把 `isTimeLimitExceeded()` 的守卫从 `m_timeLimit <= 0` 改成 `m_timeLimit == 0`
 *          （让负数落到下面的时长比较），非正时限里的那一半会红。
 */
TEST_F(TransactionTest, NonPositiveTimeLimitMeansNoLimit)
{
    constexpr std::array<std::chrono::milliseconds, 2> kNonPositiveLimits{std::chrono::milliseconds(0), std::chrono::milliseconds(-1)};

    for (std::size_t index = 0; index < kNonPositiveLimits.size(); ++index)
    {
        const std::chrono::milliseconds limit = kNonPositiveLimits[index];
        const std::int64_t              rowId = static_cast<std::int64_t>(index) + 1;

        Transaction transaction(*m_pool);
        transaction.setTimeLimit(limit);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_FALSE(transaction.isTimeLimitExceeded()) << "时限设为 " << limit.count() << " 毫秒却被判成已到期";

        Queryable<LedgerRow> transactionalQuery(transaction);
        ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(rowId, "非正时限仍可写", 1.0, std::nullopt)), 1) << transaction.lastError();
        ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    }

    EXPECT_EQ(countCommittedRows(), 2) << "0 与负数没有都按「不设限」处理";
}

/**
 * @brief 到点之后把额度放宽，这笔事务随即恢复可用——错误文案里那条出路必须真能走
 * @details 时限每次都现算（steady_clock 与 `m_timeLimit` 比），不是一次性闩住的标志，因此抬额度就能继续。
 *          超时原因里写着「或把额度放宽到实际需要的时长」，本用例就是那句话的兑现。
 *          证伪：把越限状态闩成成员（首次判到期后立 flag、之后只在 flag 上判），抬额度后的插入仍会被拒。
 */
TEST_F(TransactionTest, QuotaWideningResumesTheTransaction)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";
    EXPECT_FALSE(transaction.commit()) << transaction.lastError();

    transaction.setTimeLimit(std::chrono::seconds(30));
    EXPECT_FALSE(transaction.isTimeLimitExceeded()) << "额度抬起来之后仍被判成越限：时限被闩住了";

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "放宽额度后写的", 1.0, std::nullopt)), 1) << transaction.lastError();
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();
    EXPECT_EQ(countCommittedRows(), 1);
}

/**
 * @brief 已经结束的事务不被报成「超时」：两条原因各归各的判据
 * @details `isTimeLimitExceeded()` 在 `m_isActive` 为假时恒回 false。这不是省事：时限到期与事务已结束
 *          在收尾上是两件事——已结束要回「幂等成功」，越限要回「被拒」。混起来之后，正常提交完的事务
 *          睡过时限会得到一个假的超时读数，调用方的重试与报警随即收到一条不存在的故障。
 *          证伪：删掉 `isTimeLimitExceeded()` 里 `!m_isActive` 那半截守卫，这里的 `EXPECT_FALSE` 变红。
 */
TEST_F(TransactionTest, EndedTransactionIsNotReportedAsTimedOut)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    Queryable<LedgerRow> transactionalQuery(transaction);
    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "按时提交的", 1.0, std::nullopt)), 1) << transaction.lastError();
    ASSERT_TRUE(transaction.commit()) << transaction.lastError();

    std::this_thread::sleep_for(kSleepPastTheLimit);

    EXPECT_FALSE(transaction.isTimeLimitExceeded()) << "已结束的事务被报成整笔超时";
    EXPECT_TRUE(transaction.commit()) << "已提交的事务再次提交该回幂等成功，而不是被时限拒掉";
    EXPECT_TRUE(transaction.rollback());
    EXPECT_EQ(countCommittedRows(), 1) << "已结束的事务再走一遍收尾，把已提交的数据撤掉了";
}

// ========================================================================
// 剩余预算透传：整笔时限收紧语句级超时，正在执行的语句也能被打断
// ========================================================================

/**
 * @brief 走事务的语句在下发前把剩余预算透传成连接的语句级超时（SQLite 面读数）
 * @details 「到期不中断正在执行的那条语句」只靠本端计时解决不了：语句已经发给引擎，本端拒的是
 *          下一条。透传把剩余预算（与连接自身的会话级超时取更严者）写成连接的 queryTimeout，
 *          各驱动再把它落到引擎（SQLite 是 busy_timeout 与执行时限两道界），正在执行的语句因此
 *          会在预算耗尽时被打断。本用例钉住「透传真的发生了」这一读数：10 秒的整笔预算比
 *          夹具连接的默认超时（30 秒）更严，语句执行后连接上的读数必须向预算收紧。
 *          证伪：让 Queryable 只复查时限而不调用透传，读数仍是 30 秒，断言当场变红。
 */
TEST_F(TransactionTest, StatementExecutionForwardsRemainingBudgetToQueryTimeout)
{
    constexpr std::chrono::milliseconds kForwardedTimeLimit{10000};

    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kForwardedTimeLimit);
    Queryable<LedgerRow> transactionalQuery(transaction);

    const int baselineTimeout = transaction.connection().queryTimeout();
    ASSERT_GT(baselineTimeout, static_cast<int>(kForwardedTimeLimit.count())) << "前提不成立：连接的默认超时不比整笔预算更宽，透传无从观察";

    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "透传读数", 1.0, std::nullopt)), 1) << transaction.lastError();

    const int forwardedTimeout = transaction.connection().queryTimeout();
    EXPECT_LE(forwardedTimeout, static_cast<int>(kForwardedTimeLimit.count())) << "语句执行后连接上的语句级超时仍比整笔预算宽：剩余预算没有透传";
    EXPECT_GT(forwardedTimeout, 0) << "透传把语句级超时设成了 0＝不设限";
    EXPECT_LT(forwardedTimeout, baselineTimeout) << "读数没有向预算收紧，恰是基线值";

    EXPECT_TRUE(transaction.commit()) << transaction.lastError();
}

/**
 * @brief 预算比连接自身的会话级超时更宽时取更严者：透传不放松既有约束
 * @details 调用方可以在借用期间把连接的语句级超时设得比整笔预算更严（例如这条会话上的查询
 *          本来就该在 1.5 秒内说话）。透传是「给正在执行的语句加一道界」，不是「用预算顶掉
 *          调用方设的界」——取两者更严的那个，相等时连 SET 都不重发。
 *          证伪：把透传改成无条件 `setQueryTimeout(预算)`，这里 1.5 秒的读数会被放大成 30 秒，变红。
 */
TEST_F(TransactionTest, ForwardingBudgetNeverLoosensTheConnectionsOwnTimeout)
{
    constexpr int kStricterSessionTimeout = 1500;

    Transaction transaction(*m_pool);
    transaction.connection().setQueryTimeout(kStricterSessionTimeout);
    // 预算（30 秒）比会话超时（1.5 秒）宽松：透传该不动声色的那一侧
    transaction.setTimeLimit(std::chrono::seconds(30));
    Queryable<LedgerRow> transactionalQuery(transaction);

    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "更严者保留", 1.0, std::nullopt)), 1) << transaction.lastError();

    EXPECT_EQ(transaction.connection().queryTimeout(), kStricterSessionTimeout) << "透传把调用方设的更严会话超时顶掉了";

    EXPECT_TRUE(transaction.commit()) << transaction.lastError();
}

/**
 * @brief 连接的会话级超时是 0（不设限）时，透传必须把预算装上去：0 不得被翻成「无限预算」
 * @details queryTimeout 的口径里 0 与负数是「不设限」，而整笔预算是有限的一笔钱——把两者取
 *          「更严者」时，0 绝不能参与比较（0 在整数意义上最严、在语义上最宽）。透传的正确行为
 *          是：会话级不设限时直接下发预算本身。写反的形状（`current <= 0` 时跳过、或把
 *          `min` 直接用在 0 上）会让整笔预算失去引擎侧的界，「越限被打断」当场落空。
 *          证伪：在透传里加一句「会话超时非正就返回」，这里读数停在 0，变红。
 */
TEST_F(TransactionTest, ForwardingBudgetTightensAnUnlimitedSessionTimeout)
{
    constexpr std::chrono::milliseconds kForwardedTimeLimit{10000};

    Transaction transaction(*m_pool);
    transaction.connection().setQueryTimeout(0);
    transaction.setTimeLimit(kForwardedTimeLimit);
    Queryable<LedgerRow> transactionalQuery(transaction);

    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "无界会话装上界", 1.0, std::nullopt)), 1) << transaction.lastError();

    const int forwardedTimeout = transaction.connection().queryTimeout();
    EXPECT_GT(forwardedTimeout, 0) << "会话级 0＝不设限，透传把整笔预算一起吞掉了";
    EXPECT_LE(forwardedTimeout, static_cast<int>(kForwardedTimeLimit.count()));

    EXPECT_TRUE(transaction.commit()) << transaction.lastError();
}

/**
 * @brief 不设限的事务不碰连接的会话超时：透传的守门条件写反会误伤所有既有用法
 * @details 没调过 `setTimeLimit()` 的事务走语句时，连接上的超时读数必须原样不动——包括调用方
 *          自己设过的更严值。这是「加功能不许改变既有语义」的对照判据：透传若把「不设限」
 *          误当成「预算 0」再按 0＝不设限的语义写回去，调用方设的 1.5 秒就被静默放松成无限。
 *          证伪：把透传的守门从 `m_timeLimit <= 0 时返回` 改成「无守门、预算按 0 算」，
 *          这里 1.5 秒的读数会被写回 0，变红。
 */
TEST_F(TransactionTest, NoTimeLimitLeavesTheConnectionsTimeoutAlone)
{
    constexpr int kStricterSessionTimeout = 1500;

    Transaction transaction(*m_pool);
    transaction.connection().setQueryTimeout(kStricterSessionTimeout);
    // 刻意不设整笔时限
    Queryable<LedgerRow> transactionalQuery(transaction);

    ASSERT_EQ(transactionalQuery.insert(makeLedgerRow(1, "不设限不动读数", 1.0, std::nullopt)), 1) << transaction.lastError();

    EXPECT_EQ(transaction.connection().queryTimeout(), kStricterSessionTimeout) << "没设整笔时限的事务，语句执行却动了连接的超时读数";

    EXPECT_TRUE(transaction.commit()) << transaction.lastError();
}

/**
 * @brief 排队等锁期间越过时限：取到使用权后复查再拒，语句一条都不发
 * @details 「拒的出口」语义是越限之后一条语句都不发，而不只是「在取锁之前查一次」。连接使用权
 *          是排队的（另一条语句可能占着几十秒），排队前的检查通过不等于拿到锁时还在预算内——
 *          少了取锁后的复查，这段缝里发出的语句正好落在「注定不能再往前走」的事务上。
 *          判据不赌调度：主线程先占住使用权，在锁未交还时断言后台查询不可能完成（它正
 *          阻塞在取锁上，而不是以别的路径失败），睡过时限后才交还；后台随即在复查处被拒。
 *          证伪：删掉 `Queryable::acquireConnection()` 里取锁后的那次 `isTimeLimitExceeded()`，
 *          后台的查询照常跑完，返回的消息为空串，第一个断言变红。
 */
TEST_F(TransactionTest, BudgetCheckedAgainAfterWaitingForTheConnectionLock)
{
    Transaction transaction(*m_pool);
    transaction.setTimeLimit(kShortTransactionTimeLimit);

    std::unique_lock<std::mutex> heldLock = transaction.acquireStatementLock();
    ASSERT_TRUE(heldLock.owns_lock());

    std::future<std::string> pendingOutcome = std::async(std::launch::async,
                                                         [&transaction]() -> std::string
                                                         {
                                                             try
                                                             {
                                                                 Queryable<LedgerRow> transactionalQuery(transaction);
                                                                 static_cast<void>(transactionalQuery.toList());
                                                             } catch (const TransactionTimeoutException &failure)
                                                             {
                                                                 return failure.what();
                                                             }
                                                             return std::string{};
                                                         });

    // 锁没交还之前后台到不了终点：既把它「已在排队」这一前提变成可判定的事实，
    // 也保证它走的是「排队前通过、取到锁时已越限」这条缝，而不是「排队前就被拒」那条近路
    EXPECT_EQ(pendingOutcome.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout) << "后台查询没在等使用权：它离开了本用例要考的那条路径";

    // 越限发生在后台阻塞于取锁的期间
    std::this_thread::sleep_for(kSleepPastTheLimit);
    ASSERT_TRUE(transaction.isTimeLimitExceeded()) << "睡眠没睡过时限，本用例的判据无从落地";

    heldLock.unlock();

    const std::string rejectionReason = pendingOutcome.get();
    EXPECT_FALSE(rejectionReason.empty()) << "排队等锁越限之后，语句照样发了出去";
    EXPECT_NE(rejectionReason.find("时限"), std::string::npos) << rejectionReason;

    // 拒绝不改变事务状态：收尾仍要由调用方（或析构）完成
    EXPECT_TRUE(transaction.isActive());
    EXPECT_TRUE(transaction.rollback()) << transaction.lastError();
}

/**
 * @brief 验证 SQLite 没有只读事务形态时构造当场拒绝并点名引擎
 * @details 本引擎的 BEGIN 只有 deferred / immediate / exclusive 三种，都不区分读写。静默退化成
 *          普通事务是最坏的形状：调用方以为钉住了「这一笔不写」，而写语句照旧落库。
 * @note 证伪：把 Transaction 里取只读语句那一步换成普通开启语句，这条红在「不抛」上；
 *       给 SqliteDialect 覆写出一个形态来，同样红（那时引擎就有了，本用例的前提不再成立，要改的是判据归属）。
 */
TEST_F(TransactionTest, ReadOnlyAccessModeIsRejectedOnSqlite)
{
    AsynGyanis::TestSupport::expectFailure<AsynGyanis::Base::InvalidArgumentException>(
            [&] { static_cast<void>(Transaction(*m_pool, TransactionIsolationLevel::Default, AsynGyanis::Database::TransactionAccessMode::ReadOnly)); }, "没有只读事务形态",
            "只读开启");

    // 抛出之后连接要照常归还：构造失败时已借出的那份由 PooledConnection 的析构收尾
    PooledConnection connection = m_pool->acquire();
    ASSERT_TRUE(connection) << "拒绝只读形态时把一条连接漏在池外";
    EXPECT_EQ(countCommittedRows(), 0);
}

/**
 * @brief 验证 withTransaction 在回调正常返回时提交，并把回调的返回值原样交回
 * @note 证伪：摘掉 withTransaction 里那次提交，这条红在「旁观连接读不到那一行」——
 *       事务对象出作用域回滚，那一行根本不存在。
 */
TEST_F(TransactionTest, WithTransactionCommitsWhenCallbackReturns)
{
    const std::int64_t carriedId = m_pool->withTransaction(
            [](Transaction &transaction) -> std::int64_t
            {
                Queryable<LedgerRow> query(transaction);
                static_cast<void>(query.insert(LedgerRow{.id = 1, .name = "作用域助手", .amount = 1.0, .note = std::nullopt}));
                return 41;
            });

    EXPECT_EQ(carriedId, 41) << "回调的返回值没原样交回调用方";
    EXPECT_TRUE(findCommittedRow(1).has_value()) << "回调正常返回却没提交";
}

/// @brief 验证返回 void 的回调走同一条通路：提交照做，助手本身不返回东西
TEST_F(TransactionTest, WithTransactionAcceptsVoidCallbackAndCommits)
{
    m_pool->withTransaction(
            [](Transaction &transaction)
            {
                Queryable<LedgerRow> query(transaction);
                static_cast<void>(query.insert(LedgerRow{.id = 2, .name = "无返回值", .amount = 2.0, .note = std::nullopt}));
            });

    EXPECT_TRUE(findCommittedRow(2).has_value()) << "void 回调那条分支没走到提交";
}

/**
 * @brief 验证回调抛出时事务被回滚、异常原样向外传，连接回到池里
 * @details 「异常照常往外传」是这条助手的承诺之一：把它做成返回 false 就等于让调用方
 *          漏检一个失败分支。回滚靠的是事务对象析构那条既定契约，这里不另加 catch。
 * @note 证伪：把提交改成「不管回调有没有抛出都要提交」，这条红在「表里多出一行」。
 */
TEST_F(TransactionTest, WithTransactionRollsBackWhenCallbackThrows)
{
    AsynGyanis::TestSupport::expectFailure<std::runtime_error>(
            [&]
            {
                m_pool->withTransaction(
                        [](Transaction &transaction) -> int
                        {
                            Queryable<LedgerRow> query(transaction);
                            static_cast<void>(query.insert(LedgerRow{.id = 3, .name = "半途而废", .amount = 3.0, .note = std::nullopt}));
                            throw std::runtime_error("业务在这个点失败");
                        });
            },
            "业务在这个点失败", "回调异常原样外传");

    EXPECT_FALSE(findCommittedRow(3).has_value()) << "回调抛异常而那一行仍落库：事务没回滚";

    // 借出与归还都要平：异常路径上那条连接不能留在池外
    PooledConnection connection = m_pool->acquire();
    ASSERT_TRUE(connection) << "回滚之后连接没归还池";
}

/**
 * @brief COMMIT 失败必须抛出并让池丢掉那条脏连接，而不是把失败静默成成功
 * @details 回调里绕过事务对象执行裸 ROLLBACK，随即让 withTransaction 发 COMMIT：SQLite 会以
 *          「no transaction is active」拒绝。此时若把 commit() 的 false 当成功返回，调用方会
 *          以为数据已落库；若把这条连接还回池，下一位借用者会继承一个状态不明的会话。
 */
TEST_F(TransactionTest, WithTransactionReportsCommitFailureAndKeepsPoolUsable)
{
    const std::size_t discardedBefore = m_pool->discardedCount();

    try
    {
        m_pool->withTransaction(
                [](Transaction &transaction)
                {
                    const auto result = transaction.connection().execute("ROLLBACK");
                    ASSERT_NE(result, nullptr) << "裸 ROLLBACK 没执行成功，构造不出「COMMIT 失败」的前提";
                });
        FAIL() << "COMMIT 失败被当成了成功返回";
    } catch (const QueryExecutionException &failure)
    {
        const std::string message = failure.what();
        EXPECT_NE(message.find("事务提交失败"), std::string::npos) << "原因没有点名提交失败：" << message;
    }

    EXPECT_GE(m_pool->discardedCount(), discardedBefore + 1U) << "提交失败的连接没有被丢弃，仍可能被下一位借用者复用";

    PooledConnection next = m_pool->acquire();
    ASSERT_TRUE(next) << "丢弃脏连接后池没能再借出连接";
    EXPECT_NE(next->execute("SELECT 1"), nullptr) << "新借出的连接不可用";
}
