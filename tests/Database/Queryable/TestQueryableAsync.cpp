// 覆盖场景（读路径与写路径的结果都与同步版逐项相等、不阻塞调用线程、异常按原样穿出）：
// - AsyncQueryResultsMatchSyncVersions
// - AsyncInsertWritesRowReadableBySyncQuery / AsyncUpdateWritesRowReadableBySyncQuery
// - AsyncInsertBatchWritesRowsEqualToSyncInsertBatch
// - AsyncInsertBatchChunkedPathSpansLocalTransaction
// - AsyncInsertBatchWithEmptyCollectionProducesNoStatement
// - AsyncWriteDoesNotBlockCallingThread / AsyncExecutorSubmitDoesNotBlockCallingThread
// - AsyncExecutorShared.SharedInstanceIsSingletonWithWorkers
// - AsyncSqlErrorSurfacesAsOriginalException / AsyncWriteSqlErrorSurfacesAsOriginalException
// - OfflineModeThrowsOnAsyncExecution
// 异步写之后一律用**同步查询**读回，那是「数据真的落库」的权威证据；协程帧由常驻的 TestSupport::EventLoopThread
// 持有，其成员声明顺序保证帧的销毁晚于 m_thread 的 join。

#include "DatabaseTestSupport.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ========================================================================
// 测试用数据结构与 TableSchema 特化
// ========================================================================

namespace
{
    /**
     * @brief 异步查询用例的结构体：覆盖整型主键、文本与可空文本
     */
    struct AsyncAccountRow
    {
        std::int64_t               id;   ///< 主键
        std::string                name; ///< 户名
        std::optional<std::string> note; ///< 备注，可空
    };

    /**
     * @brief 表里不存在的结构体：用于制造「SQL 执行失败」这条异常路径
     */
    struct AsyncMissingTableRow
    {
        std::int64_t id; ///< 唯一一列
    };

} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsyncAccountRow>
{
    // 表名含空格：顺带验证异步路径使用的仍是同一套标识符引用规则
    static constexpr std::string_view kTableName = "async accounts";
    static constexpr auto             kColumns   = std::tuple{
            Column(&AsyncAccountRow::id, "id"),
            Column(&AsyncAccountRow::name, "name"),
            Column(&AsyncAccountRow::note, "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsyncMissingTableRow>
{
    static constexpr std::string_view kTableName = "async missing table";
    static constexpr auto             kColumns   = std::tuple{
            Column(&AsyncMissingTableRow::id, "id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

// ========================================================================
// 共用设施别名与夹具
// ========================================================================

namespace
{
    using AsynGyanis::Core::AsyncExecutor;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::SqliteDialect;
    using AsynGyanis::Database::Queryable::asc;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::Queryable::SchemaMigrator;
    using AsynGyanis::Database::TestSupport::CompletedTask;
    using AsynGyanis::Database::TestSupport::EventLoopThread;
    using AsynGyanis::Database::TestSupport::waitForCondition;

    /**
     * @brief 建表迁移与异步读写共用的测试夹具
     *
     * @details 每个用例一份独立的内存库（池上限 1，保证复用同一条连接），
     *          并由常驻的 EventLoopThread 在后台线程上跑一个事件循环用于恢复协程。
     *          协程帧的销毁纪律（帧必须活到循环线程结束之后）由该运行器承担，
     *          见 DatabaseTestSupport.h 里 EventLoopThread 的类注释。
     */
    class QueryableAsyncTest : public ::testing::Test
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
                        // 连接池的工厂契约要求交出「已经 connect() 完成」的连接
                        connection->connect();
                        return connection;
                    },
                    poolConfiguration);

            // 建表交给 SchemaMigrator：DDL 与 ORM 共用同一份 TableSchema，两边不会脱节
            std::string errorText;
            ASSERT_TRUE(SchemaMigrator::createTable<AsyncAccountRow>(*m_pool, true, &errorText)) << errorText;

            // 事件循环必须真的跑起来：协程的恢复动作由 loop 线程执行，测试线程只负责等待
            ASSERT_TRUE(m_loopRunner.waitUntilRunning()) << "后台事件循环未在时限内进入运行状态";
        }

        // 不需要 TearDown：EventLoopThread 析构时先 stop() 再 join，
        // 且它持有的驱动协程帧晚于 join 销毁（成员声明顺序见其类注释）

        /**
         * @brief 取得后台事件循环（提交协程时作为完成回调的落点）
         * @return AsynGyanis::Core::EventLoop& 后台线程上运行的事件循环
         */
        [[nodiscard]] AsynGyanis::Core::EventLoop &eventLoop() noexcept
        {
            return m_loopRunner.loop();
        }

        /**
         * @brief 通过 ORM 同步插入一行样本数据
         * @param id 主键
         * @param name 户名
         * @param note 备注，可为空
         */
        void insertRow(const std::int64_t id, std::string name, std::optional<std::string> note)
        {
            Queryable<AsyncAccountRow> insertQuery(*m_pool);
            ASSERT_EQ(1, insertQuery.insert(AsyncAccountRow{.id = id, .name = std::move(name), .note = std::move(note)}));
        }

        /**
         * @brief 用同步查询按主键读回一行，便于逐字段核对异步写的效果
         * @param id 主键
         * @return std::optional<AsyncAccountRow> 命中的行；无该行时为空
         */
        [[nodiscard]] std::optional<AsyncAccountRow> readRowBack(const std::int64_t id)
        {
            Queryable<AsyncAccountRow> readQuery(*m_pool);
            return readQuery.where(Column(&AsyncAccountRow::id, "id") == id).first();
        }

        /**
         * @brief 统计表内行数（同步路径）
         * @return std::int64_t 当前行数
         */
        [[nodiscard]] std::int64_t countRows()
        {
            Queryable<AsyncAccountRow> countQuery(*m_pool);
            return countQuery.count();
        }

        std::unique_ptr<ConnectionPool> m_pool;        ///< 用例独占的内存库连接池
        AsyncExecutor                   m_executor{2}; ///< 注入的异步执行器：顺带覆盖 useAsyncExecutor 路径
        EventLoopThread                 m_loopRunner;  ///< 后台事件循环 + 驱动协程帧的持有者（销毁纪律见其类注释）
    };

} // namespace

// ========================================================================
// 结果一致性（读路径）
// ========================================================================

/**
 * @brief 验证四个异步执行器的结果与同步版逐项一致
 */
TEST_F(QueryableAsyncTest, AsyncQueryResultsMatchSyncVersions)
{
    insertRow(1, "张三", std::string("首条"));
    insertRow(2, "Li Si", std::nullopt);
    insertRow(3, "王五", std::string("第三条"));

    // ---- toListAsync 与 toList 相等 ----
    {
        Queryable<AsyncAccountRow>         syncQuery(*m_pool);
        const std::vector<AsyncAccountRow> syncRows = syncQuery.orderBy(asc("id")).toList();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{1}).orderBy(asc("id"));

        const CompletedTask<std::vector<AsyncAccountRow>> completed = m_loopRunner.runToCompletion(asyncQuery.toListAsync(eventLoop()));

        ASSERT_TRUE(completed.finished) << "异步任务未在时限内完成";
        ASSERT_EQ(completed.error, nullptr);
        ASSERT_TRUE(completed.value.has_value());

        // 逐行逐列比对：异步版必须是同一份数据、同一种映射结果
        ASSERT_EQ(completed.value->size(), syncRows.size());
        for (std::size_t index = 0; index < syncRows.size(); ++index)
        {
            EXPECT_EQ((*completed.value)[index].id, syncRows[index].id);
            EXPECT_EQ((*completed.value)[index].name, syncRows[index].name);
            EXPECT_EQ((*completed.value)[index].note, syncRows[index].note);
        }
    }

    // ---- firstAsync 与 first 相等（含可空列） ----
    {
        Queryable<AsyncAccountRow>           syncQuery(*m_pool);
        const std::optional<AsyncAccountRow> syncFirst = syncQuery.where(Column(&AsyncAccountRow::id, "id") == std::int64_t{2}).first();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") == std::int64_t{2});

        const CompletedTask<std::optional<AsyncAccountRow>> completed = m_loopRunner.runToCompletion(asyncQuery.firstAsync(eventLoop()));

        ASSERT_TRUE(completed.finished) << "异步任务未在时限内完成";
        ASSERT_EQ(completed.error, nullptr);
        ASSERT_TRUE(completed.value.has_value());
        ASSERT_TRUE(syncFirst.has_value());
        ASSERT_TRUE(completed.value->has_value());
        EXPECT_EQ(completed.value->value().name, syncFirst->name);
        EXPECT_FALSE(completed.value->value().note.has_value());

        // 命中不到行时同样返回空 optional（与同步版语义一致）
        Queryable<AsyncAccountRow> missingQuery(*m_pool);
        missingQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") == std::int64_t{999});
        const CompletedTask<std::optional<AsyncAccountRow>> missing = m_loopRunner.runToCompletion(missingQuery.firstAsync(eventLoop()));

        ASSERT_TRUE(missing.finished);
        ASSERT_EQ(missing.error, nullptr);
        ASSERT_TRUE(missing.value.has_value());
        EXPECT_FALSE(missing.value->has_value());
    }

    // ---- countAsync 与 count 相等（带条件） ----
    {
        Queryable<AsyncAccountRow> syncQuery(*m_pool);
        const std::int64_t         syncCount = syncQuery.where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{2}).count();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{2});

        const CompletedTask<std::int64_t> completed = m_loopRunner.runToCompletion(asyncQuery.countAsync(eventLoop()));

        ASSERT_TRUE(completed.finished);
        ASSERT_EQ(completed.error, nullptr);
        ASSERT_TRUE(completed.value.has_value());
        EXPECT_EQ(completed.value.value(), syncCount);
        EXPECT_EQ(completed.value.value(), 2);
    }

    // ---- executeNonQueryAsync 与 executeNonQuery 相等（按条件删除） ----
    {
        Queryable<AsyncAccountRow> asyncDeleteQuery(*m_pool);
        asyncDeleteQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{3});

        const CompletedTask<std::int64_t> deleted = m_loopRunner.runToCompletion(asyncDeleteQuery.executeNonQueryAsync(eventLoop()));

        ASSERT_TRUE(deleted.finished);
        ASSERT_EQ(deleted.error, nullptr);
        ASSERT_TRUE(deleted.value.has_value());
        EXPECT_EQ(deleted.value.value(), 1);

        // 同步版看到的是删除后的真实状态：剩下两行
        Queryable<AsyncAccountRow> remainingQuery(*m_pool);
        EXPECT_EQ(remainingQuery.count(), 2);
    }
}

// ========================================================================
// 结果一致性（写路径）
// ========================================================================

/**
 * @brief 验证 insertAsync 的受影响行数与同步 insert 相同，且行真的落库（异步写 → 同步读）
 *
 * @details 钉住两件事：受影响行数与同一张表上的同步 insert 逐值相等；写确实提交到了数据库——判据不是异步接口
 *          自己回报的 1，而是另起一条同步查询按主键读回全部字段（含可空列的两个方向），否则「返回 1 却没写进去」也测不出来。
 *          co_await 的线程归属：insertAsync 的「取连接 → 执行」在 m_executor 的工作线程上跑（语句与参数在提交前就已定型），
 *          完成后协程在 m_loopRunner 的循环线程上被恢复。
 */
TEST_F(QueryableAsyncTest, AsyncInsertWritesRowReadableBySyncQuery)
{
    // ---- 异步插入一行（备注有值） ----
    Queryable<AsyncAccountRow> asyncInsertQuery(*m_pool);
    asyncInsertQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> inserted =
            m_loopRunner.runToCompletion(asyncInsertQuery.insertAsync(AsyncAccountRow{.id = 1, .name = "异步写入", .note = std::string("首条")}, eventLoop()));

    ASSERT_TRUE(inserted.finished) << "异步写入未在时限内完成";
    ASSERT_EQ(inserted.error, nullptr);
    ASSERT_TRUE(inserted.value.has_value());
    EXPECT_EQ(inserted.value.value(), 1);

    // ---- 同步插入一行作对照：受影响行数必须与异步版相同 ----
    Queryable<AsyncAccountRow> syncInsertQuery(*m_pool);
    const std::int64_t         syncAffectedRows = syncInsertQuery.insert(AsyncAccountRow{.id = 2, .name = "同步写入", .note = std::nullopt});
    EXPECT_EQ(inserted.value.value(), syncAffectedRows);

    // ---- 异步写入一行备注为 NULL：可空列的另一个方向也要能读回空 optional ----
    Queryable<AsyncAccountRow> asyncNullNoteQuery(*m_pool);
    asyncNullNoteQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> insertedNullNote =
            m_loopRunner.runToCompletion(asyncNullNoteQuery.insertAsync(AsyncAccountRow{.id = 3, .name = "无备注", .note = std::nullopt}, eventLoop()));

    ASSERT_TRUE(insertedNullNote.finished);
    ASSERT_EQ(insertedNullNote.error, nullptr);
    ASSERT_TRUE(insertedNullNote.value.has_value());
    EXPECT_EQ(insertedNullNote.value.value(), 1);

    // ---- 同步读回：行数、主键与逐字段取值都要与写入时一致 ----
    EXPECT_EQ(countRows(), 3);

    const std::optional<AsyncAccountRow> readBackFirst = readRowBack(1);
    ASSERT_TRUE(readBackFirst.has_value());
    EXPECT_EQ(readBackFirst->id, 1);
    EXPECT_EQ(readBackFirst->name, "异步写入");
    ASSERT_TRUE(readBackFirst->note.has_value());
    EXPECT_EQ(readBackFirst->note.value(), "首条");

    const std::optional<AsyncAccountRow> readBackThird = readRowBack(3);
    ASSERT_TRUE(readBackThird.has_value());
    EXPECT_EQ(readBackThird->id, 3);
    EXPECT_EQ(readBackThird->name, "无备注");
    // 写入时是 nullopt，读回必须仍是空 optional（NULL 不会被折成空串）
    EXPECT_FALSE(readBackThird->note.has_value());
}

/**
 * @brief 验证 updateAsync 的受影响行数与同步 update 相同，改后的值可被同步读回，主键不存在时为 0
 *
 * @details 钉住三件事：updateAsync 改的确实是已存在的那一行且只改它（另一行逐字段不受影响）；
 *          受影响行数与同步 update 逐值相等；主键不命中时返回 0 且表里没有任何一行被改动——
 *          「0」不能与「改了别的行」混淆。可空备注被从「有值」置为 NULL，是同步读回时最容易被
 *          悄悄写成空串的形态。co_await 的线程归属与 insertAsync 相同。
 */
TEST_F(QueryableAsyncTest, AsyncUpdateWritesRowReadableBySyncQuery)
{
    // 先用同步插入铺两行基线：证明被改的是真实存在的行，且另一行应当纹丝不动
    insertRow(1, "改前", std::string("改前备注"));
    insertRow(2, "旁观者", std::nullopt);

    // ---- 异步更新主键 1：备注置为 NULL ----
    Queryable<AsyncAccountRow> asyncUpdateQuery(*m_pool);
    asyncUpdateQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> updated =
            m_loopRunner.runToCompletion(asyncUpdateQuery.updateAsync(AsyncAccountRow{.id = 1, .name = "改后", .note = std::nullopt}, eventLoop()));

    ASSERT_TRUE(updated.finished) << "异步更新未在时限内完成";
    ASSERT_EQ(updated.error, nullptr);
    ASSERT_TRUE(updated.value.has_value());
    EXPECT_EQ(updated.value.value(), 1);

    // ---- 同步更新另一行作对照：受影响行数必须与异步版相同 ----
    Queryable<AsyncAccountRow> syncUpdateQuery(*m_pool);
    const std::int64_t         syncAffectedRows = syncUpdateQuery.update(AsyncAccountRow{.id = 2, .name = "旁观者改后", .note = std::string("同步备注")});
    EXPECT_EQ(updated.value.value(), syncAffectedRows);

    // ---- 同步读回：逐字段核对，且未命中条件的那一行不受影响 ----
    EXPECT_EQ(countRows(), 2);

    const std::optional<AsyncAccountRow> readBackUpdated = readRowBack(1);
    ASSERT_TRUE(readBackUpdated.has_value());
    EXPECT_EQ(readBackUpdated->id, 1);
    EXPECT_EQ(readBackUpdated->name, "改后");
    EXPECT_FALSE(readBackUpdated->note.has_value());

    const std::optional<AsyncAccountRow> readBackBystander = readRowBack(2);
    ASSERT_TRUE(readBackBystander.has_value());
    EXPECT_EQ(readBackBystander->name, "旁观者改后");
    ASSERT_TRUE(readBackBystander->note.has_value());
    EXPECT_EQ(readBackBystander->note.value(), "同步备注");

    // ---- 主键不存在：返回 0，且没有任何行被改动 ----
    Queryable<AsyncAccountRow> missingUpdateQuery(*m_pool);
    missingUpdateQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> missing =
            m_loopRunner.runToCompletion(missingUpdateQuery.updateAsync(AsyncAccountRow{.id = 999, .name = "不存在的行", .note = std::nullopt}, eventLoop()));

    ASSERT_TRUE(missing.finished);
    ASSERT_EQ(missing.error, nullptr);
    ASSERT_TRUE(missing.value.has_value());
    EXPECT_EQ(missing.value.value(), 0);
    EXPECT_EQ(countRows(), 2);
}

// ========================================================================
// 批量插入
// ========================================================================

/**
 * @brief 验证 insertBatchAsync 与同步 insertBatch 结果一致，且每一行的各列都能原样读回
 *
 * @details 行数刻意小于「参数上限 ÷ 列数」，走的是一次生成多行 VALUES 的单语句分支
 *          （不分块、不开本地事务），把批量路径与上一条用例的单行路径区分开。
 *          对照方式：异步批量写入后先读回全部行，清空表再交给同步 insertBatch 写同一份数据，
 *          两次读回的行必须逐字段相等——这比只比影响行数更能证明两条路径的一致性。
 */
TEST_F(QueryableAsyncTest, AsyncInsertBatchWritesRowsEqualToSyncInsertBatch)
{
    const std::vector<AsyncAccountRow> sampleRows{AsyncAccountRow{.id = 1, .name = "批量甲", .note = std::string("有备注")},
                                                  AsyncAccountRow{.id = 2, .name = "批量乙", .note = std::nullopt},
                                                  AsyncAccountRow{.id = 3, .name = "批量丙", .note = std::string("")}};

    // ---- 异步批量写：只 co_await 一次，语句在工作线程上执行 ----
    Queryable<AsyncAccountRow> asyncBatchQuery(*m_pool);
    asyncBatchQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> asyncInserted = m_loopRunner.runToCompletion(asyncBatchQuery.insertBatchAsync(sampleRows, eventLoop()));

    ASSERT_TRUE(asyncInserted.finished) << "异步批量插入未在时限内完成";
    ASSERT_EQ(asyncInserted.error, nullptr);
    ASSERT_TRUE(asyncInserted.value.has_value());
    EXPECT_EQ(asyncInserted.value.value(), 3);

    // 同步读回全部行（顺序显式指定，避免依赖引擎的返回顺序）
    auto readAllRows = [this]()
    {
        Queryable<AsyncAccountRow> readQuery(*m_pool);
        return readQuery.orderBy(asc("id")).toList();
    };
    const std::vector<AsyncAccountRow> asyncRows = readAllRows();
    ASSERT_EQ(asyncRows.size(), sampleRows.size());
    for (std::size_t index = 0; index < sampleRows.size(); ++index)
    {
        EXPECT_EQ(asyncRows[index].id, sampleRows[index].id);
        EXPECT_EQ(asyncRows[index].name, sampleRows[index].name);
        EXPECT_EQ(asyncRows[index].note, sampleRows[index].note);
    }

    // ---- 清空后交给同步 insertBatch 写同一份数据：结果必须一致 ----
    Queryable<AsyncAccountRow> clearQuery(*m_pool);
    // 查询树不带条件时生成的就是 "DELETE FROM 表"：本用例的表里只有刚写入的这一批，用于清空
    EXPECT_EQ(clearQuery.executeNonQuery(), 3);

    Queryable<AsyncAccountRow> syncBatchQuery(*m_pool);
    EXPECT_EQ(syncBatchQuery.insertBatch(sampleRows), asyncInserted.value.value());

    const std::vector<AsyncAccountRow> syncRows = readAllRows();
    ASSERT_EQ(syncRows.size(), asyncRows.size());
    for (std::size_t index = 0; index < syncRows.size(); ++index)
    {
        EXPECT_EQ(syncRows[index].id, asyncRows[index].id);
        EXPECT_EQ(syncRows[index].name, asyncRows[index].name);
        EXPECT_EQ(syncRows[index].note, asyncRows[index].note);
    }
}

/**
 * @brief 验证 insertBatchAsync 在行数 × 列数超过方言参数上限时自动分块，且全部块都在一个事务里
 *
 * @details SQLite 单条语句的参数个数上限是 999，本结构体恰好 3 列，因此每批最多 333 行。插入 400 行（400 × 3 = 1200 > 999）
 *          必然切成两块，于是走到 Queryable::insertBatchOn() 里「工作线程上临时开一个本地事务覆盖全部块，最后提交」这条分支
 *          ——它只有越过分块阈值才被执行得到，是本用例存在的首要理由。分块最常见的缺陷是最后一块的上界算错（丢行或重复写），
 *          因此边界两侧的两行都要读回核对；分块、本地事务与每一块的执行都在 m_executor 的工作线程上完成。
 */
TEST_F(QueryableAsyncTest, AsyncInsertBatchChunkedPathSpansLocalTransaction)
{
    // 参数上限与列数都取自生产实现：不把 999 与 3 硬编码成两个可能过期的数字
    constexpr std::size_t kColumnCount     = 3; ///< AsyncAccountRow 的列数（id / name / note）
    const std::size_t     parameterLimit   = SqliteDialect::kMaximumStatementParameters;
    const std::size_t     rowsPerStatement = parameterLimit / kColumnCount;

    const std::size_t totalRowCount = rowsPerStatement + 7U;
    // 自检：本用例必须真的越过参数上限，否则它退化成一个普通批量用例，覆盖不到分块分支
    ASSERT_GT(totalRowCount * kColumnCount, parameterLimit) << "本用例要求行数 × 列数超过 SQLite 的参数上限，否则覆盖不到分块分支";

    std::vector<AsyncAccountRow> chunkedRows;
    chunkedRows.reserve(totalRowCount);
    for (std::size_t rowIndex = 0; rowIndex < totalRowCount; ++rowIndex)
    {
        const std::int64_t identifier = static_cast<std::int64_t>(rowIndex) + 1;
        chunkedRows.push_back(AsyncAccountRow{.id   = identifier,
                                              .name = "分块行" + std::to_string(identifier),
                                              // 奇数行给备注、偶数行不给：让分块边界两侧行的取值形态也不同
                                              .note = (identifier % 2 == 0) ? std::optional<std::string>{} : std::optional<std::string>{"奇数行备注"}});
    }

    Queryable<AsyncAccountRow> asyncBatchQuery(*m_pool);
    asyncBatchQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> asyncInserted = m_loopRunner.runToCompletion(asyncBatchQuery.insertBatchAsync(chunkedRows, eventLoop()));

    ASSERT_TRUE(asyncInserted.finished) << "异步分块批量插入未在时限内完成";
    ASSERT_EQ(asyncInserted.error, nullptr);
    ASSERT_TRUE(asyncInserted.value.has_value());
    // 两个块的影响行数之和等于总行数：多一块少一块都能在这里暴露
    EXPECT_EQ(asyncInserted.value.value(), static_cast<std::int64_t>(totalRowCount));

    // 同步 count() 核对行数：本地事务提交后数据必须全部可见
    EXPECT_EQ(countRows(), static_cast<std::int64_t>(totalRowCount));

    // 分块边界：第一块的最后一行与第二块的第一行，两侧都要在表里
    const std::int64_t                   boundaryIdentifier = static_cast<std::int64_t>(rowsPerStatement);
    const std::optional<AsyncAccountRow> lastOfFirstChunk   = readRowBack(boundaryIdentifier);
    ASSERT_TRUE(lastOfFirstChunk.has_value()) << "第一块的最后一行丢失，分块上界算错了";
    EXPECT_EQ(lastOfFirstChunk->name, "分块行" + std::to_string(rowsPerStatement));

    const std::optional<AsyncAccountRow> firstOfSecondChunk = readRowBack(boundaryIdentifier + 1);
    ASSERT_TRUE(firstOfSecondChunk.has_value()) << "第二块的第一行丢失";
    EXPECT_EQ(firstOfSecondChunk->name, "分块行" + std::to_string(rowsPerStatement + 1U));

    // ---- 与同步路径对照：清空后把同一份数据交给同步 insertBatch，边界行取值必须一致 ----
    Queryable<AsyncAccountRow> clearQuery(*m_pool);
    EXPECT_EQ(clearQuery.executeNonQuery(), static_cast<std::int64_t>(totalRowCount));

    Queryable<AsyncAccountRow> syncBatchQuery(*m_pool);
    EXPECT_EQ(syncBatchQuery.insertBatch(chunkedRows), asyncInserted.value.value());
    EXPECT_EQ(countRows(), static_cast<std::int64_t>(totalRowCount));

    const std::optional<AsyncAccountRow> syncBoundaryRow = readRowBack(boundaryIdentifier);
    ASSERT_TRUE(syncBoundaryRow.has_value());
    EXPECT_EQ(syncBoundaryRow->name, lastOfFirstChunk->name);
    EXPECT_EQ(syncBoundaryRow->note, lastOfFirstChunk->note);
}

/**
 * @brief 验证 insertBatchAsync 收到空集合时返回 0，且不产生任何语句
 *
 * @details 「空集合直接返回 0」是 ORM 的既定契约：VALUES 后面必须至少有一组括号，
 *          没有可写内容就没有语句。判据有两条：返回 0，以及调用后表里仍然是 0 行
 *          （若实现把空集合拼成一条残缺的 INSERT，SQLite 会在执行时报语法错误，
 *          这里也就不会得到 0）。
 */
TEST_F(QueryableAsyncTest, AsyncInsertBatchWithEmptyCollectionProducesNoStatement)
{
    const std::vector<AsyncAccountRow> noRows;

    Queryable<AsyncAccountRow> asyncBatchQuery(*m_pool);
    asyncBatchQuery.useAsyncExecutor(m_executor);

    const CompletedTask<std::int64_t> asyncInserted = m_loopRunner.runToCompletion(asyncBatchQuery.insertBatchAsync(noRows, eventLoop()));

    ASSERT_TRUE(asyncInserted.finished) << "空集合的异步批量插入也必须完成（不能挂起）";
    ASSERT_EQ(asyncInserted.error, nullptr);
    ASSERT_TRUE(asyncInserted.value.has_value());
    EXPECT_EQ(asyncInserted.value.value(), 0);
    // 同步读回仍然是一张空表：确实没有产生任何语句
    EXPECT_EQ(countRows(), 0);
}

/**
 * @brief 验证空集合的异步批量插入不需要连接：池被占满时也当场给出 0
 *
 * @details 方言探测会为了「问数据库类型」借一条连接。把它排在空集合判断之前，池饱和的调用方
 *          拿到的就不再是 0，而是先等满 acquireTimeout 再抛 ConnectionUnavailableException——
 *          而契约写的是「空集合直接得到 0，不产生任何语句」，同步版也是这个形状。
 */
TEST_F(QueryableAsyncTest, AsyncInsertBatchWithEmptyCollectionNeedsNoConnection)
{
    // 夹具的池上限是 1：占住唯一额度之后，任何借用都会撞上限
    PooledConnection occupancy = m_pool->acquire();
    ASSERT_TRUE(static_cast<bool>(occupancy)) << "前提不成立：占位连接没拿到";

    const std::vector<AsyncAccountRow> noRows;

    Queryable<AsyncAccountRow> asyncBatchQuery(*m_pool);
    asyncBatchQuery.useAsyncExecutor(m_executor);

    const auto                        startedAt           = std::chrono::steady_clock::now();
    const CompletedTask<std::int64_t> asyncInserted       = m_loopRunner.runToCompletion(asyncBatchQuery.insertBatchAsync(noRows, eventLoop()));
    const auto                        elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();

    ASSERT_TRUE(asyncInserted.finished) << "池被占满时空集合也该当场完成，而不是先等一次借连接";
    ASSERT_EQ(asyncInserted.error, nullptr) << "空集合被「借不到连接」顶掉了（契约是给出 0）";
    ASSERT_TRUE(asyncInserted.value.has_value());
    EXPECT_EQ(asyncInserted.value.value(), 0);
    EXPECT_LT(elapsedMilliseconds, 1000) << "耗时 " << elapsedMilliseconds << " 毫秒，像是先等满了 acquireTimeout";
}

// ========================================================================
// 不阻塞调用线程
// ========================================================================

/**
 * @brief 验证提交阻塞任务后调用线程立刻拿回控制权，且阻塞代码在别的线程上执行
 *
 * @details 用两个闩锁把工作线程卡住：workStarted 由工作任务在开始执行时打开，releaseWork 只在测试断言「控制权已返回」
 *          之后才打开。因此三点可被确定性观测：提交耗时远小于任务被卡住的时长（调用线程没有被阻塞）；任务确实已在某个线程上
 *          开始执行（否则 try_wait_for 会超时）；该线程不是调用线程（阻塞代码没有跑在调用线程上）。
 */
TEST_F(QueryableAsyncTest, AsyncExecutorSubmitDoesNotBlockCallingThread)
{
    AsyncExecutor blockingExecutor(1);
    ASSERT_EQ(blockingExecutor.workerCount(), 1U);

    // 开工标记用原子变量 + 轮询等待：本测试只用到 std::latch 的 wait/count_down，
    // 开工侧改用原子变量可以避免依赖各标准库实现补齐 latch 的全部接口
    std::atomic<bool>            workStarted{false};
    std::latch                   releaseWork{1};
    std::atomic<std::thread::id> workThreadIdentifier{};
    std::optional<int>           value;
    std::exception_ptr           error;
    std::atomic<bool>            finishedFlag{false};

    Task<void> driver = AsynGyanis::Database::TestSupport::collectTask<int>(blockingExecutor.submit<int>(eventLoop(),
                                                                                                         [&workStarted, &releaseWork, &workThreadIdentifier]() -> int
                                                                                                         {
                                                                                                             // 记录执行线程并宣告「已经开工」，然后卡在这里等测试放行
                                                                                                             workThreadIdentifier.store(std::this_thread::get_id());
                                                                                                             workStarted.store(true, std::memory_order_release);
                                                                                                             releaseWork.wait();
                                                                                                             return 42;
                                                                                                         }),
                                                                            value, error, finishedFlag);

    const auto submitBegin = std::chrono::steady_clock::now();
    // 提交动作只做入队：这里的 resume 不做任何阻塞调用，必须立刻返回
    driver.handle().resume();
    const std::chrono::milliseconds submitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - submitBegin);

    // 提交耗时应当远小于「任务被卡住的时长」；100ms 对一次入队来说是极宽的上限
    EXPECT_LT(submitDuration, std::chrono::milliseconds(100)) << "提交动作耗时 " << submitDuration.count() << "ms";
    // 工作任务已经在某个线程上开始执行，而调用线程没有等它完成
    EXPECT_TRUE(waitForCondition([&workStarted]() { return workStarted.load(std::memory_order_acquire); })) << "工作任务迟迟没有开始";
    EXPECT_FALSE(finishedFlag.load(std::memory_order_acquire));
    EXPECT_NE(workThreadIdentifier.load(), std::this_thread::get_id());

    // 放行后任务完成，协程在事件循环线程上被恢复
    releaseWork.count_down();
    ASSERT_TRUE(waitForCondition([&finishedFlag]() { return finishedFlag.load(std::memory_order_acquire); })) << "任务放行后仍未在时限内恢复协程";
    ASSERT_EQ(error, nullptr);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), 42);

    m_loopRunner.parkDriver(std::move(driver));
}

/**
 * @brief 验证 insertAsync 提交后调用线程立刻拿回控制权，写语句只可能在别的工作线程上执行
 *
 * @details 不测量耗时，判据全是确定性的：先把专用执行器唯一的工作线程用闩锁卡住，此时任何投给它的任务都不可能开始执行；
 *          再提交 insertAsync，于是三点可观测：完成标记没有置位、结果还没有值（若实现把写操作内联在调用线程上执行，这里必然已置位）；
 *          同步查询读回 0 行（数据确实还没落库）；放行工作线程后异步写才完成，同步查询随后读到那一行。
 *          闩锁一定会在断言之间被放行：本用例在放行之前不使用 ASSERT，只有 EXPECT，不存在「断言失败提前返回、线程永远卡住」的悬挂路径。
 */
TEST_F(QueryableAsyncTest, AsyncWriteDoesNotBlockCallingThread)
{
    AsyncExecutor blockingExecutor(1);
    ASSERT_EQ(blockingExecutor.workerCount(), 1U);

    std::atomic<bool> workStarted{false};
    std::latch        releaseWork{1};

    // 占位任务：只为把唯一的工作线程卡住，本身不碰数据库
    std::optional<int> blockerValue;
    std::exception_ptr blockerError;
    std::atomic<bool>  blockerFinished{false};

    Task<void> blockerDriver = AsynGyanis::Database::TestSupport::collectTask<int>(blockingExecutor.submit<int>(eventLoop(),
                                                                                                                [&workStarted, &releaseWork]() -> int
                                                                                                                {
                                                                                                                    workStarted.store(true, std::memory_order_release);
                                                                                                                    releaseWork.wait();
                                                                                                                    return 0;
                                                                                                                }),
                                                                                   blockerValue, blockerError, blockerFinished);

    blockerDriver.handle().resume();
    EXPECT_TRUE(waitForCondition([&workStarted]() { return workStarted.load(std::memory_order_acquire); })) << "占位任务迟迟没有开始，后续判据失去意义";

    // 工作线程此刻被卡住：任何投给 blockingExecutor 的任务都还没有机会执行
    Queryable<AsyncAccountRow> asyncInsertQuery(*m_pool);
    asyncInsertQuery.useAsyncExecutor(blockingExecutor);

    // 待写入的行必须是具名局部变量：insertAsync 的入参是引用，而 Task 惰性启动——
    // 取值直到协程被 resume（= 提交点）那一刻才被读走，因此一个作为实参传入的临时对象
    // 会在本语句末尾销毁，等到下一句 resume 时就已经是悬垂引用了
    const AsyncAccountRow pendingRow{.id = 1, .name = "不阻塞", .note = std::nullopt};

    std::optional<std::int64_t> insertedRows;
    std::exception_ptr          insertError;
    std::atomic<bool>           finishedFlag{false};

    Task<void> insertDriver =
            AsynGyanis::Database::TestSupport::collectTask<std::int64_t>(asyncInsertQuery.insertAsync(pendingRow, eventLoop()), insertedRows, insertError, finishedFlag);

    // 内联启动：提交动作只入队，控制权立刻回到测试线程
    insertDriver.handle().resume();

    // 确定性判据一：写还没完成（若阻塞链路跑在调用线程上，这里已经完成）
    EXPECT_FALSE(finishedFlag.load(std::memory_order_acquire));
    // 确定性判据二：数据还没落库。同步查询在测试线程上直接执行，读到的就是物理现状；
    // 此时异步任务尚未开始，因此这里不可能读到那一行
    EXPECT_EQ(countRows(), 0);

    // 放行工作线程：占位任务返回后，写语句才会在工作线程上真正执行
    releaseWork.count_down();
    // 帧交给运行器保管：即使下面的断言失败提前返回，帧的销毁也仍在循环线程 join 之后
    m_loopRunner.parkDriver(std::move(blockerDriver));
    m_loopRunner.parkDriver(std::move(insertDriver));

    ASSERT_TRUE(waitForCondition([&finishedFlag]() { return finishedFlag.load(std::memory_order_acquire); })) << "放行工作线程后异步写仍未在时限内完成";
    ASSERT_EQ(insertError, nullptr);
    ASSERT_TRUE(insertedRows.has_value());
    EXPECT_EQ(insertedRows.value(), 1);

    // 同步读回：异步写确实在别的工作线程上完成并落库
    const std::optional<AsyncAccountRow> readBack = readRowBack(1);
    ASSERT_TRUE(readBack.has_value());
    EXPECT_EQ(readBack->name, "不阻塞");
}

// ========================================================================
// 进程级共享执行器
// ========================================================================

/**
 * @brief 验证进程级共享执行器是单例且至少有一个工作线程
 *
 * @details 未调用 Queryable::useAsyncExecutor() 时异步 API 走的就是这个实例，
 *          因此它必须满足两点：多次取得是同一对象（否则每个查询都会新建一组线程），
 *          且线程数大于 0（否则提交的任务永远不会被执行）。
 */
TEST(AsyncExecutorShared, SharedInstanceIsSingletonWithWorkers)
{
    AsyncExecutor &firstAccess  = AsyncExecutor::shared();
    AsyncExecutor &secondAccess = AsyncExecutor::shared();

    EXPECT_EQ(&firstAccess, &secondAccess);
    EXPECT_GE(firstAccess.workerCount(), 1U);
    // 没有任务时队列为空
    EXPECT_EQ(firstAccess.pendingTaskCount(), 0U);
}

// ========================================================================
// 异常路径
// ========================================================================

/**
 * @brief 验证 SQL 错误以原始异常类型与中文原因从协程里穿出
 */
TEST_F(QueryableAsyncTest, AsyncSqlErrorSurfacesAsOriginalException)
{
    Queryable<AsyncMissingTableRow> missingQuery(*m_pool);
    missingQuery.useAsyncExecutor(m_executor);

    // 同步版先把失败原因固定下来，异步版必须给出同样的类型与消息
    std::string syncMessage;
    try
    {
        static_cast<void>(missingQuery.toList());
        FAIL() << "表不存在时同步查询应当抛出 std::runtime_error";
    } catch (const std::runtime_error &exception)
    {
        syncMessage = exception.what();
    }

    Queryable<AsyncMissingTableRow> asyncQuery(*m_pool);
    asyncQuery.useAsyncExecutor(m_executor);
    const CompletedTask<std::vector<AsyncMissingTableRow>> completed = m_loopRunner.runToCompletion(asyncQuery.toListAsync(eventLoop()));

    ASSERT_TRUE(completed.finished) << "异常路径也必须完成（否则协程会被永久挂起）";
    ASSERT_NE(completed.error, nullptr);
    EXPECT_FALSE(completed.value.has_value());

    try
    {
        // 说明同上一处：rethrow_exception 必然抛出，后面不可能有可达语句
        std::rethrow_exception(completed.error);
    } catch (const std::runtime_error &exception)
    {
        // 类型与消息都与同步版一致：异常原样穿过工作线程与调度投递，没有被包装或降级
        EXPECT_EQ(std::string(exception.what()), syncMessage);
        EXPECT_NE(std::string(exception.what()).find("查询执行失败"), std::string::npos);
    }
}

/**
 * @brief 验证异步写落在不存在的表上时，异常在 co_await 处按原类型与原消息重新抛出
 *
 * @details 写路径的异常比读路径更值得单独钉一次：语句在调用线程上预先定型，真正的失败发生在工作线程上，中间要经过
 *          「工作线程 → 调度投递 → 协程恢复」三跳。因此这里既比对异常类型（std::runtime_error），也逐字比对消息与同步版完全一致，
 *          并覆盖单行写入与批量写入两个入口；消息里必须出现「语句执行失败」这个中文前缀，证明抛出的是 ORM 的写失败原因。
 */
TEST_F(QueryableAsyncTest, AsyncWriteSqlErrorSurfacesAsOriginalException)
{
    // ---- 同步版先把单行写入的失败原因固定下来 ----
    Queryable<AsyncMissingTableRow> syncQuery(*m_pool);
    std::string                     syncInsertMessage;
    try
    {
        static_cast<void>(syncQuery.insert(AsyncMissingTableRow{.id = 1}));
        FAIL() << "表不存在时同步插入应当抛出 std::runtime_error";
    } catch (const std::runtime_error &exception)
    {
        syncInsertMessage = exception.what();
    }
    EXPECT_NE(syncInsertMessage.find("语句执行失败"), std::string::npos) << syncInsertMessage;

    Queryable<AsyncMissingTableRow> asyncQuery(*m_pool);
    asyncQuery.useAsyncExecutor(m_executor);

    // ---- 异步单行写入：异常在恢复处重新抛出，类型与消息与同步版一致 ----
    // co_await 的线程归属：语句在测试线程上生成并绑定，执行失败发生在 m_executor 的工作线程上，
    // 异常随协程恢复在 m_loopRunner 的循环线程上被重新抛出，再被驱动协程收进 completed.error
    const CompletedTask<std::int64_t> singleInsert = m_loopRunner.runToCompletion(asyncQuery.insertAsync(AsyncMissingTableRow{.id = 1}, eventLoop()));

    ASSERT_TRUE(singleInsert.finished) << "写失败也必须完成（否则协程会被永久挂起）";
    ASSERT_NE(singleInsert.error, nullptr);
    EXPECT_FALSE(singleInsert.value.has_value());

    try
    {
        // rethrow_exception 是 [[noreturn]]：它必然抛出，后面写 FAIL() 只会被判成不可达代码。
        // 若抛出的类型与下面的 catch 不符，异常会继续外传，gtest 同样把这条测试判失败
        std::rethrow_exception(singleInsert.error);
    } catch (const std::runtime_error &exception)
    {
        EXPECT_EQ(std::string(exception.what()), syncInsertMessage);
    }

    // ---- 异步批量写入：同一条异常链路，入口不同（走 insertBatchOn 的执行分支）----
    const std::vector<AsyncMissingTableRow> oneMissingRow{AsyncMissingTableRow{.id = 2}};
    const CompletedTask<std::int64_t>       batchInsert = m_loopRunner.runToCompletion(asyncQuery.insertBatchAsync(oneMissingRow, eventLoop()));

    ASSERT_TRUE(batchInsert.finished);
    ASSERT_NE(batchInsert.error, nullptr);
    EXPECT_FALSE(batchInsert.value.has_value());

    try
    {
        // 说明同上一处：rethrow_exception 必然抛出，后面不可能有可达语句
        std::rethrow_exception(batchInsert.error);
    } catch (const std::runtime_error &exception)
    {
        // 同步批量写入给出的一定是同一句话：失败发生在同一条语句上
        EXPECT_EQ(std::string(exception.what()), syncInsertMessage);
    }
}

/**
 * @brief 验证离线模式下异步执行器方法抛出逻辑错误（与同步版同一道前置校验）
 */
TEST_F(QueryableAsyncTest, OfflineModeThrowsOnAsyncExecution)
{
    Queryable<AsyncAccountRow> offlineQuery;

    // Task 是惰性启动的：构造它不会执行任何代码，异常在协程真正被恢复时抛出
    const CompletedTask<std::vector<AsyncAccountRow>> completed = m_loopRunner.runToCompletion(offlineQuery.toListAsync(eventLoop()));

    ASSERT_TRUE(completed.finished);
    ASSERT_NE(completed.error, nullptr);

    try
    {
        // 说明同上一处：rethrow_exception 必然抛出，后面不可能有可达语句
        std::rethrow_exception(completed.error);
    } catch (const std::logic_error &exception)
    {
        EXPECT_NE(std::string(exception.what()).find("toListAsync()"), std::string::npos);
        EXPECT_NE(std::string(exception.what()).find("离线模式"), std::string::npos);
    }
}
