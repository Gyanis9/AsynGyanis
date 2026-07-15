/**
 * @file TestSqliteDatabase.cpp
 * @brief SQLite 数据库连接与结果集全面测试
 * @copyright Copyright (c) 2026
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Database/DatabaseType.h"
#include "Database/SqliteConnection.h"
#include "Database/SqliteResult.h"
#include "Database/DatabaseFactory.h"

#include <filesystem>
#include <cstdio>

using namespace Database;
namespace fs = std::filesystem;

// ============================================================================
// 测试辅助 — 创建内存数据库并建表
// ============================================================================

namespace
{
    /**
     * @brief 创建并连接一个带有测试表的内存 SQLite 数据库
     */
    SqliteConnection createMemoryDatabase()
    {
        ConnectionConfig config;
        config.database = ":memory:";
        SqliteConnection connection(config);
        REQUIRE(connection.connect());

        // 创建测试表
        auto createResult = connection.execute(
            "CREATE TABLE users ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT NOT NULL,"
            "  age INTEGER,"
            "  score REAL,"
            "  active INTEGER DEFAULT 1,"
            "  data BLOB"
            ")");
        REQUIRE(createResult != nullptr);

        // 插入测试数据
        connection.execute("INSERT INTO users (name, age, score) VALUES ('Alice', 30, 95.5)");
        connection.execute("INSERT INTO users (name, age, score, active) VALUES ('Bob', 25, 87.3, 1)");
        connection.execute("INSERT INTO users (name, age, score, active) VALUES ('Charlie', 35, 72.0, 0)");
        connection.execute("INSERT INTO users (name, age) VALUES ('Diana', 28)");
        connection.execute("INSERT INTO users (name, age, score) VALUES ('Eve', 22, 91.2)");

        return connection;
    }
}

// ============================================================================
// SqliteConnection 连接生命周期测试
// ============================================================================

TEST_CASE("SqliteConnection: construction does not auto-connect", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    REQUIRE_FALSE(connection.isConnected());
    REQUIRE(connection.configuration().database == ":memory:");
    REQUIRE(connection.databaseType() == DatabaseType::Sqlite);
}

TEST_CASE("SqliteConnection: connect to memory database", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    REQUIRE(connection.connect());
    REQUIRE(connection.isConnected());
    REQUIRE(connection.lastError().empty());
}

TEST_CASE("SqliteConnection: double connect is safe", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    REQUIRE(connection.connect());
    REQUIRE(connection.isConnected());
    REQUIRE(connection.connect());  // 第二次连接应无副作用
    REQUIRE(connection.isConnected());
}

TEST_CASE("SqliteConnection: disconnect after connect", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    connection.connect();
    REQUIRE(connection.isConnected());

    connection.disconnect();
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("SqliteConnection: disconnect when not connected is safe", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    REQUIRE_NOTHROW(connection.disconnect());
    REQUIRE_FALSE(connection.isConnected());
}

TEST_CASE("SqliteConnection: destructor auto-disconnects", "[SqliteConnection][lifecycle]")
{
    ConnectionConfig config;
    config.database = ":memory:";

    {
        SqliteConnection connection(config);
        connection.connect();
        REQUIRE(connection.isConnected());
    }
    // 离开作用域后析构，不应崩溃
    SUCCEED("Destructor completed without crash");
}

TEST_CASE("SqliteConnection: serverVersion returns non-empty", "[SqliteConnection]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    connection.connect();
    auto version = connection.serverVersion();
    REQUIRE_FALSE(version.empty());
    // SQLite 版本字符串应包含版本号
    REQUIRE(version.find('.') != std::string::npos);
}

TEST_CASE("SqliteConnection: lastInsertRowId after insert", "[SqliteConnection]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value TEXT)");
    connection.execute("INSERT INTO test (value) VALUES ('first')");
    REQUIRE(connection.lastInsertRowId() == 1);

    connection.execute("INSERT INTO test (value) VALUES ('second')");
    REQUIRE(connection.lastInsertRowId() == 2);
}

TEST_CASE("SqliteConnection: lastInsertRowId before any insert", "[SqliteConnection]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    REQUIRE(connection.lastInsertRowId() == 0);
}

TEST_CASE("SqliteConnection: nativeHandle returns non-null after connect", "[SqliteConnection]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    REQUIRE(connection.nativeHandle() != nullptr);
    connection.disconnect();
    REQUIRE(connection.nativeHandle() == nullptr);
}

// ============================================================================
// SqliteConnection execute 测试
// ============================================================================

TEST_CASE("SqliteConnection: execute CREATE TABLE", "[SqliteConnection][execute]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    auto result = connection.execute(
        "CREATE TABLE test (id INTEGER, name TEXT)");
    REQUIRE(result != nullptr);
    REQUIRE(result->isEmpty());
}

TEST_CASE("SqliteConnection: execute INSERT", "[SqliteConnection][execute]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE test (id INTEGER, name TEXT)");
    auto result = connection.execute("INSERT INTO test VALUES (1, 'hello')");
    REQUIRE(result != nullptr);
}

TEST_CASE("SqliteConnection: execute SELECT returns data", "[SqliteConnection][execute]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE test (id INTEGER, name TEXT)");
    connection.execute("INSERT INTO test VALUES (1, 'Alice')");
    connection.execute("INSERT INTO test VALUES (2, 'Bob')");

    auto result = connection.execute("SELECT * FROM test ORDER BY id");
    REQUIRE(result != nullptr);
    REQUIRE_FALSE(result->isEmpty());
    REQUIRE(result->rowCount() == 2);
}

TEST_CASE("SqliteConnection: execute invalid SQL returns error", "[SqliteConnection][execute][error]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    auto result = connection.execute("INVALID SQL SYNTAX!!!");
    REQUIRE(result == nullptr);
    REQUIRE_FALSE(connection.lastError().empty());
}

TEST_CASE("SqliteConnection: execute empty command returns error", "[SqliteConnection][execute][error]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    auto result = connection.execute("");
    REQUIRE(result == nullptr);
}

TEST_CASE("SqliteConnection: execute when not connected returns error", "[SqliteConnection][execute][error]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);

    auto result = connection.execute("SELECT 1");
    REQUIRE(result == nullptr);
    REQUIRE_FALSE(connection.lastError().empty());
}

TEST_CASE("SqliteConnection: execute UPDATE", "[SqliteConnection][execute]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE test (id INTEGER, value TEXT)");
    connection.execute("INSERT INTO test VALUES (1, 'old')");
    auto result = connection.execute("UPDATE test SET value = 'new' WHERE id = 1");
    REQUIRE(result != nullptr);

    auto select = connection.execute("SELECT value FROM test WHERE id = 1");
    REQUIRE(select != nullptr);
    REQUIRE(select->next());
    REQUIRE(std::get<std::string>(select->getValue(0)) == "new");
}

TEST_CASE("SqliteConnection: execute DELETE", "[SqliteConnection][execute]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE test (id INTEGER)");
    connection.execute("INSERT INTO test VALUES (1)");
    connection.execute("INSERT INTO test VALUES (2)");

    auto result = connection.execute("DELETE FROM test WHERE id = 1");
    REQUIRE(result != nullptr);

    auto select = connection.execute("SELECT COUNT(*) FROM test");
    REQUIRE(select != nullptr);
    REQUIRE(select->next());
    REQUIRE(std::get<int64_t>(select->getValue(0)) == 1);
}

// ============================================================================
// SqliteConnection 事务测试
// ============================================================================

TEST_CASE("SqliteConnection: beginTransaction returns true", "[SqliteConnection][transaction]")
{
    auto connection = createMemoryDatabase();
    REQUIRE(connection.beginTransaction());
}

TEST_CASE("SqliteConnection: commit after transaction", "[SqliteConnection][transaction]")
{
    auto connection = createMemoryDatabase();

    connection.beginTransaction();
    connection.execute("INSERT INTO users (name, age) VALUES ('Frank', 40)");
    REQUIRE(connection.commit());

    auto result = connection.execute("SELECT COUNT(*) FROM users WHERE name = 'Frank'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue(0)) == 1);
}

TEST_CASE("SqliteConnection: rollback undoes changes", "[SqliteConnection][transaction]")
{
    auto connection = createMemoryDatabase();

    connection.beginTransaction();
    connection.execute("INSERT INTO users (name, age) VALUES ('Ghost', 99)");
    REQUIRE(connection.rollback());

    auto result = connection.execute("SELECT COUNT(*) FROM users WHERE name = 'Ghost'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue(0)) == 0);
}

TEST_CASE("SqliteConnection: rollback when no transaction is safe", "[SqliteConnection][transaction]")
{
    auto connection = createMemoryDatabase();
    // SQLite 允许在没有活动事务时执行 ROLLBACK
    REQUIRE_NOTHROW(connection.rollback());
}

// ============================================================================
// SqliteResult 基本测试
// ============================================================================

TEST_CASE("SqliteResult: empty result has isEmpty true", "[SqliteResult]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();
    connection.execute("CREATE TABLE empty_table (id INTEGER)");

    auto result = connection.execute("SELECT * FROM empty_table");
    REQUIRE(result != nullptr);
    REQUIRE(result->isEmpty());
    REQUIRE(result->rowCount() == 0);
}

TEST_CASE("SqliteResult: rowCount returns correct count", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT * FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->rowCount() == 5);
}

TEST_CASE("SqliteResult: columnCount returns correct number", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name, age FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->columnCount() == 3);
}

TEST_CASE("SqliteResult: columnCount for SELECT all", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT * FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->columnCount() == 6);  // id, name, age, score, active, data
}

TEST_CASE("SqliteResult: columnNames returns correct names", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name, age FROM users");
    REQUIRE(result != nullptr);

    auto names = result->columnNames();
    REQUIRE(names.size() == 3);
    REQUIRE(names[0] == "id");
    REQUIRE(names[1] == "name");
    REQUIRE(names[2] == "age");
}

TEST_CASE("SqliteResult: columnName by index", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name FROM users");
    REQUIRE(result != nullptr);

    auto col0 = result->columnName(0);
    REQUIRE(col0.has_value());
    REQUIRE(col0.value() == "id");

    auto col1 = result->columnName(1);
    REQUIRE(col1.has_value());
    REQUIRE(col1.value() == "name");

    // 超出范围的索引
    auto invalid = result->columnName(999);
    REQUIRE_FALSE(invalid.has_value());
}

TEST_CASE("SqliteResult: columnIndex by name", "[SqliteResult]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name, age FROM users");
    REQUIRE(result != nullptr);

    auto idx = result->columnIndex("name");
    REQUIRE(idx.has_value());
    REQUIRE(idx.value() == 1);

    idx = result->columnIndex("id");
    REQUIRE(idx.has_value());
    REQUIRE(idx.value() == 0);

    idx = result->columnIndex("age");
    REQUIRE(idx.has_value());
    REQUIRE(idx.value() == 2);

    // 不存在的列名
    auto invalid = result->columnIndex("nonexistent");
    REQUIRE_FALSE(invalid.has_value());
}

// ============================================================================
// SqliteResult 行遍历测试
// ============================================================================

TEST_CASE("SqliteResult: next iterates all rows", "[SqliteResult][iteration]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT * FROM users ORDER BY id");
    REQUIRE(result != nullptr);

    int count = 0;
    while (result->next())
    {
        ++count;
    }
    REQUIRE(count == 5);
}

TEST_CASE("SqliteResult: first row data is correct", "[SqliteResult][iteration]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT name, age FROM users ORDER BY id LIMIT 1");
    REQUIRE(result != nullptr);

    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue("name")) == "Alice");
    REQUIRE(std::get<int64_t>(result->getValue("age")) == 30);
}

TEST_CASE("SqliteResult: can iterate multiple times after reset", "[SqliteResult][iteration]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT * FROM users ORDER BY id");
    REQUIRE(result != nullptr);

    // 第一次遍历
    int count1 = 0;
    while (result->next()) { ++count1; }
    REQUIRE(count1 == 5);

    // 重置后再次遍历
    result->reset();
    int count2 = 0;
    while (result->next()) { ++count2; }
    REQUIRE(count2 == 5);
}

TEST_CASE("SqliteResult: next returns false after last row", "[SqliteResult][iteration]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT * FROM users WHERE id = 1");
    REQUIRE(result != nullptr);

    // 只有一行匹配
    REQUIRE(result->rowCount() == 1);
    REQUIRE(result->next());
    // 所有行已消费完毕，再次调用应返回 false
    REQUIRE_FALSE(result->next());
}

// ============================================================================
// SqliteResult getValue 数据类型测试
// ============================================================================

TEST_CASE("SqliteResult: getValue INTEGER column returns int64_t", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT age FROM users WHERE name = 'Alice'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(0);
    REQUIRE(std::holds_alternative<int64_t>(value));
    REQUIRE(std::get<int64_t>(value) == 30);
}

TEST_CASE("SqliteResult: getValue REAL column returns double", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT score FROM users WHERE name = 'Alice'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(0);
    REQUIRE(std::holds_alternative<double>(value));
    auto score = std::get<double>(value);
    REQUIRE(score > 95.0);
    REQUIRE(score < 96.0);
}

TEST_CASE("SqliteResult: getValue TEXT column returns string", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT name FROM users WHERE id = 1");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(0);
    REQUIRE(std::holds_alternative<std::string>(value));
    REQUIRE(std::get<std::string>(value) == "Alice");
}

TEST_CASE("SqliteResult: getValue NULL column returns monostate", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT score FROM users WHERE name = 'Diana'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(0);
    // Diana 的 score 是 NULL
    REQUIRE(std::holds_alternative<std::monostate>(value));
}

TEST_CASE("SqliteResult: getValue by name works for all types", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name, age, score FROM users WHERE name = 'Alice'");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    REQUIRE(std::get<int64_t>(result->getValue("id")) == 1);
    REQUIRE(std::get<std::string>(result->getValue("name")) == "Alice");
    REQUIRE(std::get<int64_t>(result->getValue("age")) == 30);
    auto score = std::get<double>(result->getValue("score"));
    REQUIRE(score > 95.0);
    REQUIRE(score < 96.0);
}

TEST_CASE("SqliteResult: getValue with out-of-range index returns monostate", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id FROM users WHERE id = 1");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(999);
    REQUIRE(std::holds_alternative<std::monostate>(value));
}

TEST_CASE("SqliteResult: getValue before calling next returns monostate", "[SqliteResult][types]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id FROM users WHERE id = 1");
    REQUIRE(result != nullptr);

    // 未调用 next() 时，getValue 应返回 monostate（因为尚未读取任何行）
    auto value = result->getValue(0);
    REQUIRE(std::holds_alternative<std::monostate>(value));
}

// ============================================================================
// SqliteResult 高级查询测试
// ============================================================================

TEST_CASE("SqliteResult: SELECT with WHERE clause", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT name FROM users WHERE age > 25 ORDER BY age");
    REQUIRE(result != nullptr);

    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == "Diana");  // 28
    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == "Alice");  // 30
    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == "Charlie"); // 35
    REQUIRE_FALSE(result->next());
}

TEST_CASE("SqliteResult: SELECT COUNT works", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT COUNT(*) FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue(0)) == 5);
}

TEST_CASE("SqliteResult: SELECT MAX works", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT MAX(age) FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue(0)) == 35);
}

TEST_CASE("SqliteResult: SELECT AVG returns double", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT AVG(age) FROM users");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    auto avg = std::get<double>(result->getValue(0));
    REQUIRE(avg > 27.0);
    REQUIRE(avg < 29.0);
}

TEST_CASE("SqliteResult: SELECT with ORDER BY DESC", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT name, age FROM users ORDER BY age DESC");
    REQUIRE(result != nullptr);

    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue("age")) == 35);

    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue("age")) == 30);

    REQUIRE(result->next());
    REQUIRE(std::get<int64_t>(result->getValue("age")) == 28);
}

TEST_CASE("SqliteResult: SELECT with LIMIT", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT name FROM users ORDER BY id LIMIT 3");
    REQUIRE(result != nullptr);
    REQUIRE(result->rowCount() == 3);
}

TEST_CASE("SqliteResult: SELECT with LIKE", "[SqliteResult][queries]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute(
        "SELECT name FROM users WHERE name LIKE 'A%' OR name LIKE 'E%'");
    REQUIRE(result != nullptr);

    int count = 0;
    while (result->next()) { ++count; }
    REQUIRE(count == 2);  // Alice, Eve
}

// ============================================================================
// SqliteResult BLOB 类型测试
// ============================================================================

TEST_CASE("SqliteResult: BLOB data can be stored and retrieved", "[SqliteResult][blob]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE blobs (id INTEGER, content BLOB)");

    // 插入二进制数据
    const unsigned char binaryData[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};
    // 使用 SQLite 的 X'...' 语法插入十六进制 BLOB
    connection.execute(
        "INSERT INTO blobs VALUES (1, X'000102FFFEFD')");

    auto result = connection.execute("SELECT content FROM blobs WHERE id = 1");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());

    auto value = result->getValue(0);
    // BLOB 会被转换为字符串
    REQUIRE(std::holds_alternative<std::string>(value));
    auto &blobStr = std::get<std::string>(value);
    REQUIRE(blobStr.size() == 6);
    REQUIRE(static_cast<unsigned char>(blobStr[0]) == 0x00);
    REQUIRE(static_cast<unsigned char>(blobStr[3]) == 0xFF);
}

// ============================================================================
// SqliteResult 边界测试
// ============================================================================

TEST_CASE("SqliteResult: empty table select returns isEmpty", "[SqliteResult][boundary]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE temp (id INTEGER)");
    auto result = connection.execute("SELECT * FROM temp");
    REQUIRE(result != nullptr);
    REQUIRE(result->isEmpty());
    REQUIRE(result->rowCount() == 0);
    REQUIRE(result->columnCount() == 1);
}

TEST_CASE("SqliteResult: nonexistent column name returns nullopt", "[SqliteResult][boundary]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id, name FROM users LIMIT 1");
    REQUIRE(result != nullptr);

    REQUIRE_FALSE(result->columnIndex("nonexistent_column").has_value());
}

TEST_CASE("SqliteResult: columnName with out-of-range index", "[SqliteResult][boundary]")
{
    auto connection = createMemoryDatabase();
    auto result = connection.execute("SELECT id FROM users");
    REQUIRE(result != nullptr);

    REQUIRE_FALSE(result->columnName(999).has_value());
    REQUIRE_FALSE(result->columnName(static_cast<size_t>(-1)).has_value());
}

TEST_CASE("SqliteResult: very long text value", "[SqliteResult][boundary]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE long_text (content TEXT)");

    std::string longString(10000, 'X');
    connection.execute(
        "INSERT INTO long_text VALUES ('" + longString + "')");

    auto result = connection.execute("SELECT content FROM long_text");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == longString);
}

TEST_CASE("SqliteResult: special characters in text", "[SqliteResult][boundary]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    SqliteConnection connection(config);
    connection.connect();

    connection.execute("CREATE TABLE special (text TEXT)");

    // 插入包含特殊字符的数据（单引号用两个单引号转义）
    connection.execute(
        "INSERT INTO special VALUES ('Hello''World')");

    auto result = connection.execute("SELECT text FROM special");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == "Hello'World");
}

// ============================================================================
// SQLite 文件数据库测试
// ============================================================================

TEST_CASE("SqliteConnection: file database persists data", "[SqliteConnection][file]")
{
    // 使用临时目录
    const auto tempPath = fs::temp_directory_path() / "test_sqlite_persist.db";

    // 清理可能残留的文件
    std::remove(tempPath.string().c_str());

    {
        ConnectionConfig config;
        config.database = tempPath.string();
        SqliteConnection connection(config);
        REQUIRE(connection.connect());

        connection.execute("CREATE TABLE persist (key TEXT, value TEXT)");
        connection.execute("INSERT INTO persist VALUES ('hello', 'world')");
    }
    // 连接关闭，数据应持久化

    {
        ConnectionConfig config;
        config.database = tempPath.string();
        SqliteConnection connection(config);
        REQUIRE(connection.connect());

        auto result = connection.execute("SELECT value FROM persist WHERE key = 'hello'");
        REQUIRE(result != nullptr);
        REQUIRE(result->next());
        REQUIRE(std::get<std::string>(result->getValue(0)) == "world");
    }

    // 清理
    std::remove(tempPath.string().c_str());
}

TEST_CASE("SqliteConnection: file database create new file", "[SqliteConnection][file]")
{
    const auto tempPath = fs::temp_directory_path() / "test_sqlite_new.db";
    std::remove(tempPath.string().c_str());
    REQUIRE_FALSE(fs::exists(tempPath));

    {
        ConnectionConfig config;
        config.database = tempPath.string();
        SqliteConnection connection(config);
        REQUIRE(connection.connect());
        connection.execute("CREATE TABLE t (x INTEGER)");
    }

    REQUIRE(fs::exists(tempPath));
    std::remove(tempPath.string().c_str());
}

// ============================================================================
// 通过 DatabaseFactory 使用 SQLite 的集成测试
// ============================================================================

TEST_CASE("DatabaseFactory: SQLite through factory works", "[DatabaseFactory][integration]")
{
    ConnectionConfig config;
    config.database = ":memory:";
    auto connection = DatabaseFactory::create(DatabaseType::Sqlite, config);

    REQUIRE(connection != nullptr);
    REQUIRE(connection->databaseType() == DatabaseType::Sqlite);
    REQUIRE(connection->connect());

    connection->execute("CREATE TABLE factory_test (id INTEGER, name TEXT)");
    connection->execute("INSERT INTO factory_test VALUES (1, 'factory')");

    auto result = connection->execute("SELECT name FROM factory_test WHERE id = 1");
    REQUIRE(result != nullptr);
    REQUIRE(result->next());
    REQUIRE(std::get<std::string>(result->getValue(0)) == "factory");

    connection->disconnect();
    REQUIRE_FALSE(connection->isConnected());
}
