// 覆盖场景（ORM 执行链路「池 → 驱动 → 绑定」所依赖的底层能力：DatabaseConnection 的带参数执行接口）：
// - SqliteBindsScalarParameterTypes（整型/文本/浮点/布尔）
// - SqliteBindsNullParameter（NULL 与空串语义不同）
// - SqliteParameterizedSelectReturnsRows
// - SqliteRejectsParameterCountMismatch（少给 / 多给参数都失败）
// - SqliteRejectsContainerParameter
// - EveryDriverAnswersParameterizedExecuteWithoutDroppingArguments（MySQL 已实现绑定：离线时以「未连接」
//   拒绝；Redis 已接通 argv 通道：离线时走到发送判定，而 SQL 式 "?" 占位符被单独拒）
// - PooledConnectionForwardsParameterizedExecute（经池与基类指针的虚派发）

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Redis/RedisConnection.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using AsynGyanis::Database::ConnectionConfig;
    using AsynGyanis::Database::ConnectionPool;
    using AsynGyanis::Database::DatabaseConnection;
    using AsynGyanis::Database::DatabaseFactory;
    using AsynGyanis::Database::DatabaseValue;
    using AsynGyanis::Database::MySqlConnection;
    using AsynGyanis::Database::PoolConfig;
    using AsynGyanis::Database::PooledConnection;
    using AsynGyanis::Database::RedisConnection;

    /**
     * @brief 打开一份已连接的内存 SQLite 数据库
     * @return std::unique_ptr<DatabaseConnection> 基类指针形式的连接，失败时对象仍非空但未连接
     */
    std::unique_ptr<DatabaseConnection> openMemoryDatabase()
    {
        auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
        // 内存库随连接存活：本用例内所有操作都必须走这一条连接
        static_cast<void>(connection->connect());
        return connection;
    }

    /**
     * @brief 建一张用于参数绑定测试的表
     * @param connection 目标连接
     */
    void createSampleTable(DatabaseConnection &connection)
    {
        const auto createResult = connection.execute("CREATE TABLE samples (id INTEGER PRIMARY KEY, label TEXT, amount REAL, flag INTEGER)");
        ASSERT_TRUE(createResult != nullptr) << connection.lastError();
    }

} // namespace

// ========================================================================
// 参数绑定
// ========================================================================

/**
 * @brief 验证各类标量参数都能按位置正确绑定
 */
TEST(ParameterizedExecution, SqliteBindsScalarParameterTypes)
{
    const auto connection = openMemoryDatabase();
    ASSERT_TRUE(connection->isConnected()) << connection->lastError();
    createSampleTable(*connection);

    const std::vector<DatabaseValue> insertParameters{std::int64_t{7}, std::string("O'Brien -- 中文"), -12.5, true};

    const auto insertResult = connection->execute("INSERT INTO samples (id, label, amount, flag) VALUES (?, ?, ?, ?)", insertParameters);
    ASSERT_TRUE(insertResult != nullptr) << connection->lastError();

    const std::vector<DatabaseValue> selectParameters{std::int64_t{7}};
    const auto                       selectResult = connection->execute("SELECT id, label, amount, flag FROM samples WHERE id = ?", selectParameters);
    ASSERT_TRUE(selectResult != nullptr) << connection->lastError();
    ASSERT_TRUE(selectResult->next());

    EXPECT_EQ(std::get<std::int64_t>(selectResult->getValue("id")), 7);
    // 含单引号与 "--" 的文本原样存回：绑定不会把引号当作文本边界，也不会把 "--" 当注释
    EXPECT_EQ(std::get<std::string>(selectResult->getValue("label")), "O'Brien -- 中文");
    EXPECT_DOUBLE_EQ(std::get<double>(selectResult->getValue("amount")), -12.5);
    // 布尔在 SQLite 里以 0/1 的 INTEGER 存储
    EXPECT_EQ(std::get<std::int64_t>(selectResult->getValue("flag")), 1);
    EXPECT_FALSE(selectResult->next());
}

/**
 * @brief 验证 monostate 参数绑定为真正的 SQL NULL，而不是空串
 */
TEST(ParameterizedExecution, SqliteBindsNullParameter)
{
    const auto connection = openMemoryDatabase();
    ASSERT_TRUE(connection->isConnected()) << connection->lastError();
    createSampleTable(*connection);

    const std::vector<DatabaseValue> insertParameters{std::int64_t{1}, std::monostate{}, std::monostate{}, std::int64_t{0}};
    const auto                       insertResult = connection->execute("INSERT INTO samples (id, label, amount, flag) VALUES (?, ?, ?, ?)", insertParameters);
    ASSERT_TRUE(insertResult != nullptr) << connection->lastError();

    const std::vector<DatabaseValue> selectParameters{std::int64_t{1}};
    // label 是 TEXT 且绑了 NULL：只有真正绑成 NULL，"IS NULL" 才成立
    const auto selectResult = connection->execute("SELECT label IS NULL, amount IS NULL FROM samples WHERE id = ?", selectParameters);
    ASSERT_TRUE(selectResult != nullptr) << connection->lastError();
    ASSERT_TRUE(selectResult->next());

    EXPECT_EQ(std::get<std::int64_t>(selectResult->getValue(0)), 1);
    EXPECT_EQ(std::get<std::int64_t>(selectResult->getValue(1)), 1);

    // 对照：绑空串时 IS NULL 不成立，证明二者没有被混为一谈
    const std::vector<DatabaseValue> emptyTextParameters{std::int64_t{2}, std::string(""), 0.0, std::int64_t{0}};
    ASSERT_TRUE(connection->execute("INSERT INTO samples (id, label, amount, flag) VALUES (?, ?, ?, ?)", emptyTextParameters) != nullptr) << connection->lastError();

    const std::vector<DatabaseValue> emptyTextSelectParameters{std::int64_t{2}};
    const auto                       emptyTextResult = connection->execute("SELECT label IS NULL FROM samples WHERE id = ?", emptyTextSelectParameters);
    ASSERT_TRUE(emptyTextResult != nullptr) << connection->lastError();
    ASSERT_TRUE(emptyTextResult->next());
    EXPECT_EQ(std::get<std::int64_t>(emptyTextResult->getValue(0)), 0);
}

/**
 * @brief 验证带参数的 SELECT 能正常返回结果集
 */
TEST(ParameterizedExecution, SqliteParameterizedSelectReturnsRows)
{
    const auto connection = openMemoryDatabase();
    ASSERT_TRUE(connection->isConnected()) << connection->lastError();

    const std::vector<DatabaseValue> parameters{std::int64_t{41}, std::string("七")};
    const auto                       result = connection->execute("SELECT ? AS doubled, ? AS text", parameters);
    ASSERT_TRUE(result != nullptr) << connection->lastError();
    ASSERT_TRUE(result->next());

    EXPECT_EQ(std::get<std::int64_t>(result->getValue("doubled")), 41);
    EXPECT_EQ(std::get<std::string>(result->getValue("text")), "七");
}

// ========================================================================
// 错误路径
// ========================================================================

/**
 * @brief 验证参数个数与占位符个数不一致时直接失败，而不是按 NULL 静默执行
 */
TEST(ParameterizedExecution, SqliteRejectsParameterCountMismatch)
{
    const auto connection = openMemoryDatabase();
    ASSERT_TRUE(connection->isConnected()) << connection->lastError();
    createSampleTable(*connection);

    // 少给参数：若按缺省 NULL 执行，WHERE id = NULL 会静默变成永假
    const auto tooFewParameters = connection->execute("SELECT id FROM samples WHERE id = ?", std::vector<DatabaseValue>{});
    EXPECT_TRUE(tooFewParameters == nullptr);
    EXPECT_NE(connection->lastError().find("参数数量不匹配"), std::string::npos);

    // 多给参数：多出来的取值没有任何位置可绑定，属于调用方写错了 SQL 或参数列表
    const auto tooManyParameters = connection->execute("SELECT id FROM samples", std::vector<DatabaseValue>{std::int64_t{1}});
    EXPECT_TRUE(tooManyParameters == nullptr);
    EXPECT_NE(connection->lastError().find("参数数量不匹配"), std::string::npos);
}

/**
 * @brief 验证容器类型参数被明确拒绝并给出中文原因
 */
TEST(ParameterizedExecution, SqliteRejectsContainerParameter)
{
    const auto connection = openMemoryDatabase();
    ASSERT_TRUE(connection->isConnected()) << connection->lastError();

    // List / Hash 这类容器在 SQL 里无法映射成单个标量参数，正确做法是展开成多个占位符
    const std::vector<DatabaseValue> parameters{std::vector<std::string>{"甲", "乙"}};
    const auto                       result = connection->execute("SELECT ?", parameters);

    EXPECT_TRUE(result == nullptr);
    EXPECT_NE(connection->lastError().find("容器类型"), std::string::npos);
    EXPECT_NE(connection->lastError().find("List"), std::string::npos);
}

/**
 * @brief 验证各驱动在无可用连接/无绑定实现时给出中文错误，而不是静默忽略参数
 */
/**
 * @brief 验证三个驱动都不把参数化调用当成死路，也绝不静默丢掉参数
 * @details Redis 那一档旧断言是「走基类默认实现，报『暂不支持参数化查询（Redis）』」——本轮把它接通了
 *          （Redis 的命令行本来就是参数数组），旧语义作废：现在未连接时看到的是发送路径的「未连接到 Redis」，
 *          而 SQL 习惯写下的 `?` 占位符被单独拒（Redis 没有这个语法，静默发出去会写成一个名叫 "?" 的键）。
 *          MySQL 那条判据不变：已实现绑定，未连接时以「未连接」拒绝。
 */
TEST(ParameterizedExecution, EveryDriverAnswersParameterizedExecuteWithoutDroppingArguments)
{
    const std::vector<DatabaseValue> parameters{std::int64_t{1}};

    // MySQL 已实现参数绑定（预处理语句）：未连接时以「未连接」这条判定拒绝，绝不把参数丢掉。
    // 参数个数与占位符个数是否匹配要在 prepare 之后才知道，需要可用的服务端才能验证
    MySqlConnection mySqlConnection(ConnectionConfig::mySqlDefault());
    const auto      mySqlResult = mySqlConnection.execute("SELECT ?", parameters);
    EXPECT_TRUE(mySqlResult == nullptr);
    EXPECT_NE(mySqlConnection.lastError().find("MySQL"), std::string::npos) << mySqlConnection.lastError();

    // Redis：正常形状走到发送那一步（本用例没连接，因此报的是连接状态而不是「不支持」）
    RedisConnection redisConnection(ConnectionConfig::redisDefault());
    const auto      redisResult = redisConnection.execute("GET", parameters);
    EXPECT_TRUE(redisResult == nullptr);
    EXPECT_EQ(redisConnection.lastError(), "未连接到 Redis，命令未执行") << "参数化调用没有到达发送那一步：「" << redisConnection.lastError() << "」";

    // 占位符形状仍然被拒，且给的是本层的新判据而不是基类那句通用拒绝
    const auto redisPlaceholderResult = redisConnection.execute("GET ?", parameters);
    EXPECT_TRUE(redisPlaceholderResult == nullptr);
    EXPECT_NE(redisConnection.lastError().find("Redis 没有占位符语法"), std::string::npos) << redisConnection.lastError();
    EXPECT_EQ(redisConnection.lastError().find("暂不支持"), std::string::npos) << redisConnection.lastError();
}

// ========================================================================
// 连接池转发
// ========================================================================

/**
 * @brief 验证经 PooledConnection 与基类指针调用参数化接口能正确虚派发到 SQLite 驱动
 */
TEST(ParameterizedExecution, PooledConnectionForwardsParameterizedExecute)
{
    PoolConfig poolConfiguration;
    // 内存库不跨连接共享，上限压到 1 让三次 acquire 拿到同一条连接
    poolConfiguration.maximumPoolSize = 1;

    ConnectionPool pool(
            []()
            {
                auto connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                connection->connect();
                return connection;
            },
            poolConfiguration);

    {
        PooledConnection connection = pool.acquire();
        ASSERT_TRUE(connection);
        ASSERT_TRUE(connection->execute("CREATE TABLE items (id INTEGER, name TEXT)") != nullptr) << connection->lastError();
    }

    {
        PooledConnection connection = pool.acquire();
        ASSERT_TRUE(connection);
        const std::vector<DatabaseValue> parameters{std::int64_t{5}, std::string("需绑定的值")};
        ASSERT_TRUE(connection->execute("INSERT INTO items (id, name) VALUES (?, ?)", parameters) != nullptr) << connection->lastError();
    }

    {
        PooledConnection connection = pool.acquire();
        ASSERT_TRUE(connection);
        const std::vector<DatabaseValue> parameters{std::int64_t{5}};
        const auto                       result = connection->execute("SELECT name FROM items WHERE id = ?", parameters);
        ASSERT_TRUE(result != nullptr) << connection->lastError();
        ASSERT_TRUE(result->next());
        EXPECT_EQ(std::get<std::string>(result->getValue(0)), "需绑定的值");
    }
}
