/**
 * @file TestTransaction.cpp
 * @brief 事务端到端测试 —— 内存/文件 SQLite + 连接池 + ORM 的事务语义
 * @author Gyanis
 * @date 2026-09-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 用真实 SQLite 钉住事务的三条核心语义：提交前别的连接看不到本次写入、提交后才可见；回滚后写入不可见
 *          （含析构自动回滚与异常穿越两条路径）；重复 commit / rollback 幂等且不会误撤销已提交的工作。另覆盖「事务持有连接」
 *          这一前提（池满时第二个事务拿不到连接）与批量插入的分块执行。
 *          用文件库而不是 ":memory:"：内存库不跨连接共享，只有一个连接时根本观察不到「提交前不可见」，事务语义无从验证。
 *
 * 覆盖场景：
 * - CommitMakesChangesVisibleToOtherConnections
 * - RollbackDiscardsChanges / DestructorRollsBackUncommittedWork
 * - ExceptionPathRollsBackAndKeepsDatabaseClean
 * - RepeatedCommitAndRollbackAreIdempotent
 * - CommitThenRollbackKeepsCommittedData
 * - TransactionHoldsItsConnectionUntilItEnds
 * - BatchInsertChunksAutomatically / BatchInsertOnTransactionRollsBackWithIt
 * - BatchInsertOfEmptyRangeWritesNothing / BatchInsertRejectsDuplicatePrimaryKey
 */
#include "Database/Common/ConnectionConfig.h"
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

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstdint>
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
    [[nodiscard]] LedgerRow makeLedgerRow(const std::int64_t id,
                                          std::string name,
                                          const double amount,
                                          std::optional<std::string> note)
    {
        return LedgerRow{
            .id     = id,
            .name   = std::move(name),
            .amount = amount,
            .note   = std::move(note)
        };
    }

} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<LedgerRow>
{
    static constexpr std::string_view kTableName = "ledger";
    static constexpr auto kColumns = std::tuple{
        Column(&LedgerRow::id,     "id"),
        Column(&LedgerRow::name,   "name"),
        Column(&LedgerRow::amount, "amount"),
        Column(&LedgerRow::note,   "note"),
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
    using AsynGyanis::Database::SqliteDialect;
    using AsynGyanis::Database::Transaction;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::TestSupport::TemporaryDatabaseFile;

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
                    auto connection = DatabaseFactory::createSqlite(
                        ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
                    // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                    connection->connect();
                    return connection;
                },
                makePoolConfiguration(2));

            PooledConnection connection = m_pool->acquire();
            ASSERT_TRUE(connection);
            // 建表走原生 SQL：DDL 不由 ORM 生成
            const auto createResult = connection->execute(
                "CREATE TABLE ledger ("
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

        TemporaryDatabaseFile          m_databaseFile{"Transaction"}; ///< 临时数据库文件，必须先于池析构
        std::unique_ptr<ConnectionPool> m_pool;                       ///< 用例独占的连接池
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
        Transaction transaction(*m_pool);
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
        Transaction transaction(*m_pool);
        Queryable<LedgerRow> transactionalQuery(transaction);
        EXPECT_EQ(transactionalQuery.insert(makeLedgerRow(1, "未提交", 1.0, std::nullopt)), 1);

        // 故意不提交：离开作用域时由析构补一次 ROLLBACK
    }

    EXPECT_EQ(countCommittedRows(), 0);
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
        Transaction transaction(*m_pool);
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
        Transaction transaction(*m_pool);
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
        Transaction transaction(*m_pool);
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
            auto connection = DatabaseFactory::createSqlite(
                ConnectionConfig::sqliteDefault(m_databaseFile.utf8Path()));
            connection->connect();
            return connection;
        },
        makePoolConfiguration(1));

    Transaction transaction(singleConnectionPool);
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
        Transaction transaction(*m_pool);
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
