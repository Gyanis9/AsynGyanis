/**
 * @file TestDatabaseCore.cpp
 * @brief Database 核心类型、工厂、MySQL/Redis 桩实现测试
 * @copyright Copyright (c) 2026
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Database/DatabaseType.h"
#include "Database/DatabaseResult.h"
#include "Database/DatabaseConnection.h"
#include "Database/DatabaseFactory.h"
#include "Database/MySqlConnection.h"
#include "Database/MySqlResult.h"
#include "Database/RedisConnection.h"
#include "Database/RedisResult.h"

#include <unordered_map>
#include <vector>

using namespace Database;

// ============================================================================
// DatabaseType 枚举测试
// ============================================================================

TEST_CASE("DatabaseType: databaseTypeName returns correct strings", "[DatabaseType]")
{
    REQUIRE(std::string(databaseTypeName(DatabaseType::MySql)) == "MySql");
    REQUIRE(std::string(databaseTypeName(DatabaseType::Redis)) == "Redis");
    REQUIRE(std::string(databaseTypeName(DatabaseType::Sqlite)) == "Sqlite");
    REQUIRE(std::string(databaseTypeName(DatabaseType::PostgreSql)) == "PostgreSql");
}

TEST_CASE("DatabaseType: databaseTypeName handles edge values", "[DatabaseType][boundary]")
{
    // 使用 static_cast 测试边界外的值
    const auto unknown = static_cast<DatabaseType>(99);
    REQUIRE(std::string(databaseTypeName(unknown)) == "Unknown");
}

// ============================================================================
// DatabaseValue 变体类型测试
// ============================================================================

TEST_CASE("DatabaseValue: monostate default represents NULL", "[DatabaseValue]")
{
    DatabaseValue value = std::monostate{};
    REQUIRE(std::holds_alternative<std::monostate>(value));
    REQUIRE(value.index() == 0);
}

TEST_CASE("DatabaseValue: holds bool", "[DatabaseValue]")
{
    DatabaseValue value = true;
    REQUIRE(std::holds_alternative<bool>(value));
    REQUIRE(std::get<bool>(value) == true);

    value = false;
    REQUIRE(std::get<bool>(value) == false);
}

TEST_CASE("DatabaseValue: holds int64_t", "[DatabaseValue]")
{
    DatabaseValue value = static_cast<int64_t>(42);
    REQUIRE(std::holds_alternative<int64_t>(value));
    REQUIRE(std::get<int64_t>(value) == 42);

    value = static_cast<int64_t>(-100);
    REQUIRE(std::get<int64_t>(value) == -100);

    value = static_cast<int64_t>(0);
    REQUIRE(std::get<int64_t>(value) == 0);
}

TEST_CASE("DatabaseValue: holds double", "[DatabaseValue]")
{
    DatabaseValue value = 3.14;
    REQUIRE(std::holds_alternative<double>(value));
    REQUIRE(std::get<double>(value) > 3.13);
    REQUIRE(std::get<double>(value) < 3.15);

    value = -0.001;
    REQUIRE(std::get<double>(value) < 0.0);
    REQUIRE(std::get<double>(value) > -0.002);
}

TEST_CASE("DatabaseValue: holds string", "[DatabaseValue]")
{
    DatabaseValue value = std::string("hello world");
    REQUIRE(std::holds_alternative<std::string>(value));
    REQUIRE(std::get<std::string>(value) == "hello world");

    value = std::string{};
    REQUIRE(std::get<std::string>(value).empty());
}

TEST_CASE("DatabaseValue: holds list of strings", "[DatabaseValue]")
{
    DatabaseValue value = std::vector<std::string>{"a", "b", "c"};
    REQUIRE(std::holds_alternative<std::vector<std::string>>(value));
    auto &list = std::get<std::vector<std::string>>(value);
    REQUIRE(list.size() == 3);
    REQUIRE(list[0] == "a");
    REQUIRE(list[1] == "b");
    REQUIRE(list[2] == "c");

    value = std::vector<std::string>{};
    REQUIRE(std::get<std::vector<std::string>>(value).empty());
}

TEST_CASE("DatabaseValue: holds hash map", "[DatabaseValue]")
{
    DatabaseValue value = std::unordered_map<std::string, std::string>{
        {"key1", "value1"}, {"key2", "value2"}};
    REQUIRE(std::holds_alternative<
            std::unordered_map<std::string, std::string>>(value));
    auto &map = std::get<std::unordered_map<std::string, std::string>>(value);
    REQUIRE(map.size() == 2);
    REQUIRE(map["key1"] == "value1");
    REQUIRE(map["key2"] == "value2");
}

TEST_CASE("DatabaseValue: databaseValueTypeName returns correct names", "[DatabaseValue]")
{
    REQUIRE(std::string(databaseValueTypeName(std::monostate{})) == "Null");
    REQUIRE(std::string(databaseValueTypeName(true)) == "Bool");
    REQUIRE(std::string(databaseValueTypeName(static_cast<int64_t>(1))) == "Int64");
    REQUIRE(std::string(databaseValueTypeName(1.0)) == "Double");
    REQUIRE(std::string(databaseValueTypeName(std::string("x"))) == "String");
    REQUIRE(std::string(databaseValueTypeName(
        std::vector<std::string>{})) == "List");
    REQUIRE(std::string(databaseValueTypeName(
        std::unordered_map<std::string, std::string>{})) == "Hash");
}

// ============================================================================
// ConnectionConfig 测试
// ============================================================================

TEST_CASE("ConnectionConfig: default values are initialized", "[ConnectionConfig]")
{
    ConnectionConfig config;
    REQUIRE(config.host.empty());
    REQUIRE(config.port == 0);
    REQUIRE(config.userName.empty());
    REQUIRE(config.password.empty());
    REQUIRE(config.database.empty());
    REQUIRE(config.poolSize == 1);
}

TEST_CASE("ConnectionConfig: mySqlDefault sets correct defaults", "[ConnectionConfig]")
{
    auto config = ConnectionConfig::mySqlDefault();
    REQUIRE(config.host == "127.0.0.1");
    REQUIRE(config.port == 3306);
    REQUIRE(config.userName == "root");
    REQUIRE(config.database == "test");
    REQUIRE(config.poolSize == 1);
}

TEST_CASE("ConnectionConfig: redisDefault sets correct defaults", "[ConnectionConfig]")
{
    auto config = ConnectionConfig::redisDefault();
    REQUIRE(config.host == "127.0.0.1");
    REQUIRE(config.port == 6379);
    REQUIRE(config.poolSize == 1);
}

TEST_CASE("ConnectionConfig: custom assignment works", "[ConnectionConfig]")
{
    ConnectionConfig config;
    config.host     = "db.example.com";
    config.port     = 5432;
    config.userName = "admin";
    config.password = "secret";
    config.database = "mydb";
    config.poolSize = 5;

    REQUIRE(config.host == "db.example.com");
    REQUIRE(config.port == 5432);
    REQUIRE(config.userName == "admin");
    REQUIRE(config.password == "secret");
    REQUIRE(config.database == "mydb");
    REQUIRE(config.poolSize == 5);
}

// ============================================================================
// DatabaseResult 抽象基类测试
// ============================================================================

namespace
{
    // 最小化的 DatabaseResult 桩实现，用于测试抽象接口
    class StubResult : public DatabaseResult
    {
    public:
        bool next() override { return false; }
        [[nodiscard]] size_t rowCount() const override { return 0; }
        [[nodiscard]] size_t columnCount() const override { return 0; }
        [[nodiscard]] std::optional<std::string> columnName(size_t) const override
        { return std::nullopt; }
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view) const override
        { return std::nullopt; }
        [[nodiscard]] DatabaseValue getValue(size_t) const override
        { return std::monostate{}; }
        [[nodiscard]] DatabaseValue getValue(std::string_view) const override
        { return std::monostate{}; }
        [[nodiscard]] std::vector<std::string> columnNames() const override
        { return {}; }
        void reset() override {}
        [[nodiscard]] bool isEmpty() const override { return true; }
    };
}

TEST_CASE("DatabaseResult: stub implementation returns defaults", "[DatabaseResult]")
{
    StubResult result;
    REQUIRE_FALSE(result.next());
    REQUIRE(result.rowCount() == 0);
    REQUIRE(result.columnCount() == 0);
    REQUIRE_FALSE(result.columnName(0).has_value());
    REQUIRE_FALSE(result.columnIndex("test").has_value());
    REQUIRE(std::holds_alternative<std::monostate>(result.getValue(0)));
    REQUIRE(std::holds_alternative<std::monostate>(result.getValue("test")));
    REQUIRE(result.columnNames().empty());
    REQUIRE(result.isEmpty());
    REQUIRE(result.lastError().empty());
    REQUIRE_NOTHROW(result.reset());
}

TEST_CASE("DatabaseResult: lastError can be read", "[DatabaseResult]")
{
    StubResult result;
    REQUIRE(result.lastError().empty());
}

// ============================================================================
// DatabaseFactory 测试
// ============================================================================

TEST_CASE("DatabaseFactory: guessType returns correct types by port", "[DatabaseFactory]")
{
    REQUIRE(DatabaseFactory::guessType(3306) == DatabaseType::MySql);
    REQUIRE(DatabaseFactory::guessType(6379) == DatabaseType::Redis);
    REQUIRE(DatabaseFactory::guessType(5432) == DatabaseType::PostgreSql);
}

TEST_CASE("DatabaseFactory: guessType defaults to MySql for unknown ports", "[DatabaseFactory]")
{
    REQUIRE(DatabaseFactory::guessType(8080) == DatabaseType::MySql);
    REQUIRE(DatabaseFactory::guessType(0) == DatabaseType::MySql);
    REQUIRE(DatabaseFactory::guessType(9999) == DatabaseType::MySql);
}

TEST_CASE("DatabaseFactory: createMySql returns non-null", "[DatabaseFactory]")
{
    auto conn = DatabaseFactory::createMySql(ConnectionConfig::mySqlDefault());
    REQUIRE(conn != nullptr);
    REQUIRE(conn->databaseType() == DatabaseType::MySql);
    REQUIRE_FALSE(conn->isConnected());
}

TEST_CASE("DatabaseFactory: createRedis returns non-null", "[DatabaseFactory]")
{
    auto conn = DatabaseFactory::createRedis(ConnectionConfig::redisDefault());
    REQUIRE(conn != nullptr);
    REQUIRE(conn->databaseType() == DatabaseType::Redis);
    REQUIRE_FALSE(conn->isConnected());
}

TEST_CASE("DatabaseFactory: createSqlite returns non-null", "[DatabaseFactory]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    auto conn = DatabaseFactory::createSqlite(config);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->databaseType() == DatabaseType::Sqlite);
}

TEST_CASE("DatabaseFactory: create by type returns correct instance", "[DatabaseFactory]")
{
    {
        auto conn = DatabaseFactory::create(DatabaseType::MySql,
            ConnectionConfig::mySqlDefault());
        REQUIRE(conn != nullptr);
        REQUIRE(conn->databaseType() == DatabaseType::MySql);
    }
    {
        auto conn = DatabaseFactory::create(DatabaseType::Redis,
            ConnectionConfig::redisDefault());
        REQUIRE(conn != nullptr);
        REQUIRE(conn->databaseType() == DatabaseType::Redis);
    }
    {
        ConnectionConfig config;
        config.database = ":memory:";
        auto conn = DatabaseFactory::create(DatabaseType::Sqlite, config);
        REQUIRE(conn != nullptr);
        REQUIRE(conn->databaseType() == DatabaseType::Sqlite);
    }
}

TEST_CASE("DatabaseFactory: create by config guesses type", "[DatabaseFactory]")
{
    auto mySqlConfig = ConnectionConfig::mySqlDefault();
    auto conn = DatabaseFactory::create(mySqlConfig);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->databaseType() == DatabaseType::MySql);
}

// ============================================================================
// MySqlConnection 桩实现测试
// ============================================================================

TEST_CASE("MySqlConnection: construction with config", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    config.database = "testdb";
    MySqlConnection connection(config);
    REQUIRE(connection.configuration().database == "testdb");
    REQUIRE(connection.databaseType() == DatabaseType::MySql);
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("MySqlConnection: connect returns false when library not available", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE_FALSE(connection.connect());
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("MySqlConnection: execute returns nullptr when not connected", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    auto result = connection.execute("SELECT 1");
    REQUIRE(result == nullptr);
    REQUIRE_FALSE(connection.lastError().empty());
}

TEST_CASE("MySqlConnection: disconnect when not connected is safe", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE_NOTHROW(connection.disconnect());
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("MySqlConnection: transaction methods return false", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE_FALSE(connection.beginTransaction());
    REQUIRE_FALSE(connection.commit());
    REQUIRE_FALSE(connection.rollback());
}

TEST_CASE("MySqlConnection: serverVersion returns empty", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE(connection.serverVersion().empty());
}

TEST_CASE("MySqlConnection: nativeHandle returns nullptr", "[MySqlConnection][stub]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE(connection.nativeHandle() == nullptr);
}

TEST_CASE("MySqlConnection: setTimeout functions", "[MySqlConnection]")
{
    auto config = ConnectionConfig::mySqlDefault();
    MySqlConnection connection(config);
    REQUIRE_NOTHROW(connection.setConnectTimeout(3000));
    REQUIRE_NOTHROW(connection.setQueryTimeout(10000));
}

// ============================================================================
// RedisConnection 桩实现测试
// ============================================================================

TEST_CASE("RedisConnection: construction with config", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    config.password = "secret";
    RedisConnection connection(config);
    REQUIRE(connection.configuration().host == "127.0.0.1");
    REQUIRE(connection.configuration().port == 6379);
    REQUIRE(connection.databaseType() == DatabaseType::Redis);
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("RedisConnection: connect returns false when library not available", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    REQUIRE_FALSE(connection.connect());
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("RedisConnection: execute returns nullptr when not connected", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    auto result = connection.execute("PING");
    REQUIRE(result == nullptr);
    REQUIRE_FALSE(connection.lastError().empty());
}

TEST_CASE("RedisConnection: disconnect when not connected is safe", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    REQUIRE_NOTHROW(connection.disconnect());
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("RedisConnection: executeCommand with empty arguments", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    auto result = connection.executeCommand({});
    REQUIRE(result == nullptr);
}

TEST_CASE("RedisConnection: pipeline commands", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    REQUIRE_NOTHROW(connection.pipelineCommand("SET key value"));
    REQUIRE_NOTHROW(connection.pipelineCommand("GET key"));
    auto results = connection.flushPipeline();
    // 未连接时返回空管道结果
    REQUIRE(results.empty());
}

TEST_CASE("RedisConnection: selectDatabase returns false", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    REQUIRE_FALSE(connection.selectDatabase(0));
}

TEST_CASE("RedisConnection: nativeHandle returns nullptr", "[RedisConnection][stub]")
{
    auto config = ConnectionConfig::redisDefault();
    RedisConnection connection(config);
    REQUIRE(connection.nativeHandle() == nullptr);
}
