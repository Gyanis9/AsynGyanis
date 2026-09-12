/**
 * @file TestQueryableAsync.cpp
 * @brief 异步执行测试 —— 阻塞调用挪出事件循环、结果与同步版一致、异常按原样穿出
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 三个层次：
 * - 结果一致性：同一份内存 SQLite 上，toListAsync / firstAsync / countAsync /
 *   executeNonQueryAsync 的结果与同步版逐项相等；
 * - 不阻塞调用线程：用一个被闩锁（std::latch）卡住的工作任务做确定性验证——
 *   提交后控制权立刻回到调用线程（耗时远小于任务被卡住的时长），
 *   且「任务是否完成」与「阻塞调用是否在别的线程上执行」都能被直接观测；
 * - 异常路径：离线模式（std::logic_error）与 SQL 错误（std::runtime_error，含中文原因）
 *   都在协程被恢复处重新抛出，类型与消息与同步版一致。
 *
 * ## 协程帧的销毁时机（本文件最容易写错的地方）
 * 完成标记由驱动协程在事件循环线程上置位，而测试线程看到标记后就会继续断言并离开作用域。
 * 此时事件循环线程可能正处在 resume() 的收尾阶段（协程已执行完、尚未从 final_suspend 返回），
 * 若测试线程当场销毁协程帧就会与之竞态。因此本文件把驱动协程对象保存在夹具成员里，
 * 让它的销毁发生在「事件循环线程 join 之后」（见夹具的成员声明顺序）。
 *
 * 覆盖场景：
 * - AsyncQueryResultsMatchSyncVersions
 * - AsyncExecutorSubmitDoesNotBlockCallingThread
 * - AsyncExecutorShared.SharedInstanceIsSingletonWithWorkers
 * - AsyncSqlErrorSurfacesAsOriginalException / OfflineModeThrowsOnAsyncExecution
 */
#include "Core/EventLoop/EventLoop.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Pool/AsyncExecutor.h"
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
    static constexpr auto kColumns = std::tuple{
        Column(&AsyncAccountRow::id,   "id"),
        Column(&AsyncAccountRow::name, "name"),
        Column(&AsyncAccountRow::note, "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsyncMissingTableRow>
{
    static constexpr std::string_view kTableName = "async missing table";
    static constexpr auto kColumns = std::tuple{
        Column(&AsyncMissingTableRow::id, "id"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

// ========================================================================
// 协程驱动辅助
// ========================================================================

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Database::AsyncExecutor;
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::Queryable::asc;
    using AsynGyanis::Database::Queryable::Column;
    using AsynGyanis::Database::Queryable::Queryable;
    using AsynGyanis::Database::Queryable::SchemaMigrator;

    /// 轮询等待的上限：所有用例的正常耗时都在毫秒级，5 秒足够暴露「协程没被恢复」这类问题
    constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /**
     * @brief 轮询等待条件成立（避免固定 sleep 造成的偶发失败）
     * @tparam Predicate 判定可调用对象
     * @param predicate 判定函数
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    [[nodiscard]] bool waitForCondition(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /**
     * @brief 一次异步任务的观测结果
     * @tparam ResultType 任务结果类型
     */
    template<typename ResultType>
    struct CompletedTask
    {
        std::optional<ResultType> value;         ///< 任务返回值（成功时才有值）
        std::exception_ptr        error;         ///< 任务抛出的异常（失败时非空）
        bool                      finished = false; ///< 是否在时限内收到完成通知
    };

    /**
     * @brief 驱动协程：co_await 目标任务，把结果或异常搬进调用方提供的变量
     *
     * @details 之所以需要这层驱动，是因为 Task<T> 只能被协程 co_await：
     *          本协程先挂起在内层任务上，内层完成后（在事件循环线程上）恢复，
     *          这里把结果写出去并最后置完成标记——标记由事件循环线程写入，
     *          调用线程读取前必须做 acquire 语义的同步（见 std::atomic 内存序）。
     *
     * @tparam ResultType 内层任务的结果类型
     * @param inner 待等待的任务（按值接收，帧内持有它的生命周期）
     * @param value 出参：任务返回值
     * @param error 出参：任务抛出的异常
     * @param finished 出参：完成标记，写于所有其它出参之后
     * @return Task<void> 驱动协程
     */
    template<typename ResultType>
    Task<void> collectTask(Task<ResultType> inner,
                           std::optional<ResultType> &value,
                           std::exception_ptr &error,
                           std::atomic<bool> &finished)
    {
        try
        {
            value.emplace(co_await std::move(inner));
        }
        catch (...)
        {
            // 任务异常在此收敛：驱动协程本身不向上抛，调用线程只需看 error 是否被写入
            error = std::current_exception();
        }

        // release 语义：保证上面的写入对读取到本标记的线程可见
        finished.store(true, std::memory_order_release);
    }

    /**
     * @brief 建表迁移与异步查询共用的测试夹具
     *
     * @details 每个用例一份独立的内存库（池上限 1，保证复用同一条连接），
     *          并在后台线程上跑一个事件循环用于投递协程恢复。
     *          成员声明顺序即销毁顺序的逆序，末尾的驱动协程容器会晚于事件循环线程销毁，
     *          因此协程帧的析构不会与正在收尾的 resume() 竞态。
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

            // 事件循环跑在后台线程：协程的恢复动作必须由 loop 线程执行，本线程只负责等待
            m_loopThread = std::jthread(
                [this]()
                {
                    m_loop.run();
                });
            ASSERT_TRUE(waitForCondition([this]() { return m_loop.isRunning(); }));
        }

        void TearDown() override
        {
            // 先停事件循环：run() 返回后线程结束，jthread 析构时自动 join。
            // 驱动协程的帧在 join 之后才销毁（成员声明顺序保证），不会与 resume() 收尾竞态
            m_loop.stop();
        }

        /**
         * @brief 在测试线程上启动一个异步任务并等到它完成
         *
         * @details 启动方式是 resume 驱动协程：内层任务会内联执行到「把阻塞任务交给执行器」
         *          这一步就挂起，因此本方法在任务真正完成之前不会占用调用线程等待数据库，
         *          而是让执行器与事件循环线程协作推进。
         *
         * @tparam ResultType 任务结果类型
         * @param task 待执行的异步任务
         * @return CompletedTask<ResultType> 结果、异常与完成情况
         */
        template<typename ResultType>
        [[nodiscard]] CompletedTask<ResultType> runToCompletion(Task<ResultType> task)
        {
            CompletedTask<ResultType> completed;
            std::atomic<bool>         finishedFlag{false};

            Task<void> driver = collectTask<ResultType>(std::move(task), completed.value, completed.error, finishedFlag);
            // 内联启动：阻塞任务只做入队，控制权在这里立刻回到测试线程
            driver.handle().resume();

            completed.finished = waitForCondition([&finishedFlag]()
            {
                return finishedFlag.load(std::memory_order_acquire);
            });

            // 帧的销毁推迟到事件循环线程 join 之后（见夹具的成员声明顺序）
            m_driverTasks.push_back(std::move(driver));
            return completed;
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
            ASSERT_EQ(1, insertQuery.insert(AsyncAccountRow{
                              .id = id, .name = std::move(name), .note = std::move(note)
                          }));
        }

        std::unique_ptr<ConnectionPool>  m_pool;        ///< 用例独占的内存库连接池
        AsyncExecutor                    m_executor{2}; ///< 注入的异步执行器：顺带覆盖 useAsyncExecutor 路径
        EventLoop                        m_loop;        ///< 后台事件循环，负责恢复协程
        std::vector<Task<void>>          m_driverTasks; ///< 驱动协程：必须活过事件循环线程（销毁顺序见上）
        std::jthread                     m_loopThread;  ///< 跑 m_loop 的后台线程，析构自动 join
    };

} // namespace

// ========================================================================
// 结果一致性
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
        Queryable<AsyncAccountRow> syncQuery(*m_pool);
        const std::vector<AsyncAccountRow> syncRows = syncQuery.orderBy(asc("id")).toList();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor)
                  .where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{1})
                  .orderBy(asc("id"));

        const CompletedTask<std::vector<AsyncAccountRow>> completed =
            runToCompletion(asyncQuery.toListAsync(m_loop));

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
        Queryable<AsyncAccountRow> syncQuery(*m_pool);
        const std::optional<AsyncAccountRow> syncFirst =
            syncQuery.where(Column(&AsyncAccountRow::id, "id") == std::int64_t{2}).first();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") == std::int64_t{2});

        const CompletedTask<std::optional<AsyncAccountRow>> completed =
            runToCompletion(asyncQuery.firstAsync(m_loop));

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
        const CompletedTask<std::optional<AsyncAccountRow>> missing =
            runToCompletion(missingQuery.firstAsync(m_loop));

        ASSERT_TRUE(missing.finished);
        ASSERT_EQ(missing.error, nullptr);
        ASSERT_TRUE(missing.value.has_value());
        EXPECT_FALSE(missing.value->has_value());
    }

    // ---- countAsync 与 count 相等（带条件） ----
    {
        Queryable<AsyncAccountRow> syncQuery(*m_pool);
        const std::int64_t syncCount = syncQuery.where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{2}).count();

        Queryable<AsyncAccountRow> asyncQuery(*m_pool);
        asyncQuery.useAsyncExecutor(m_executor).where(Column(&AsyncAccountRow::id, "id") >= std::int64_t{2});

        const CompletedTask<std::int64_t> completed = runToCompletion(asyncQuery.countAsync(m_loop));

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

        const CompletedTask<std::int64_t> deleted = runToCompletion(asyncDeleteQuery.executeNonQueryAsync(m_loop));

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
// 不阻塞调用线程
// ========================================================================

/**
 * @brief 验证提交阻塞任务后调用线程立刻拿回控制权，且阻塞代码在别的线程上执行
 *
 * @details 用两个闩锁把工作线程卡住：workStarted 由工作任务在开始执行时打开，
 *          releaseWork 只在测试断言「控制权已返回」之后才打开。因此三点可被确定性观测：
 *          - 提交耗时远小于任务被卡住的时长（调用线程没有被阻塞）；
 *          - 任务确实已经在某个线程上开始执行（否则 try_wait_for 会超时）；
 *          - 该线程不是调用线程（阻塞代码没有跑在调用线程上）。
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

    Task<void> driver = collectTask<int>(
        blockingExecutor.submit<int>(m_loop,
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
    const std::chrono::milliseconds submitDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - submitBegin);

    // 提交耗时应当远小于「任务被卡住的时长」；100ms 对一次入队来说是极宽的上限
    EXPECT_LT(submitDuration, std::chrono::milliseconds(100)) << "提交动作耗时 " << submitDuration.count() << "ms";
    // 工作任务已经在某个线程上开始执行，而调用线程没有等它完成
    EXPECT_TRUE(waitForCondition([&workStarted]()
    {
        return workStarted.load(std::memory_order_acquire);
    })) << "工作任务迟迟没有开始";
    EXPECT_FALSE(finishedFlag.load(std::memory_order_acquire));
    EXPECT_NE(workThreadIdentifier.load(), std::this_thread::get_id());

    // 放行后任务完成，协程在事件循环线程上被恢复
    releaseWork.count_down();
    ASSERT_TRUE(waitForCondition([&finishedFlag]()
    {
        return finishedFlag.load(std::memory_order_acquire);
    })) << "任务放行后仍未在时限内恢复协程";
    ASSERT_EQ(error, nullptr);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), 42);

    m_driverTasks.push_back(std::move(driver));
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
    }
    catch (const std::runtime_error &exception)
    {
        syncMessage = exception.what();
    }

    Queryable<AsyncMissingTableRow> asyncQuery(*m_pool);
    asyncQuery.useAsyncExecutor(m_executor);
    const CompletedTask<std::vector<AsyncMissingTableRow>> completed =
        runToCompletion(asyncQuery.toListAsync(m_loop));

    ASSERT_TRUE(completed.finished) << "异常路径也必须完成（否则协程会被永久挂起）";
    ASSERT_NE(completed.error, nullptr);
    EXPECT_FALSE(completed.value.has_value());

    try
    {
        std::rethrow_exception(completed.error);
        FAIL() << "异步任务的异常应当在 co_await 处重新抛出";
    }
    catch (const std::runtime_error &exception)
    {
        // 类型与消息都与同步版一致：异常原样穿过工作线程与调度投递，没有被包装或降级
        EXPECT_EQ(std::string(exception.what()), syncMessage);
        EXPECT_NE(std::string(exception.what()).find("查询执行失败"), std::string::npos);
    }
}

/**
 * @brief 验证离线模式下异步执行器方法抛出逻辑错误（与同步版同一道前置校验）
 */
TEST_F(QueryableAsyncTest, OfflineModeThrowsOnAsyncExecution)
{
    Queryable<AsyncAccountRow> offlineQuery;

    // Task 是惰性启动的：构造它不会执行任何代码，异常在协程真正被恢复时抛出
    const CompletedTask<std::vector<AsyncAccountRow>> completed = runToCompletion(offlineQuery.toListAsync(m_loop));

    ASSERT_TRUE(completed.finished);
    ASSERT_NE(completed.error, nullptr);

    try
    {
        std::rethrow_exception(completed.error);
        FAIL() << "离线模式下的异步查询应当抛出 std::logic_error";
    }
    catch (const std::logic_error &exception)
    {
        EXPECT_NE(std::string(exception.what()).find("toListAsync()"), std::string::npos);
        EXPECT_NE(std::string(exception.what()).find("离线模式"), std::string::npos);
    }
}
