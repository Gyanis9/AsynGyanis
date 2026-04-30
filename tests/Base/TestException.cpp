/**
 * @file TestException.cpp
 * @brief Expection 模块单元测试：所有异常类的构造、属性和继承关系
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/Exception.h"

using namespace Base;

// ============================================================================
// ConfigException 基础测试
// ============================================================================

TEST_CASE("ConfigException inherits from std::runtime_error", "[Exception][ConfigException]")
{
    ConfigException ex("test error");
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
    REQUIRE_THROWS_AS(throw ex, std::exception);
    REQUIRE_THROWS_AS(throw ex, ConfigException);
}

TEST_CASE("ConfigException stores message and location", "[Exception][ConfigException]")
{
    auto loc = std::source_location::current();
    ConfigException ex("test message", loc);

    std::string what_str(ex.what());
    REQUIRE(what_str.find("[Exception]") != std::string::npos);
    REQUIRE(what_str.find("test message") != std::string::npos);
    REQUIRE(what_str.find(loc.file_name()) != std::string::npos);
    REQUIRE(what_str.find(std::to_string(loc.line())) != std::string::npos);

    const auto &stored_loc = ex.location();
    // Verify location is stored correctly
    REQUIRE(stored_loc.line() == loc.line());
}

TEST_CASE("ConfigException can be caught by what()", "[Exception][ConfigException][boundary]")
{
    ConfigException ex("boundary test");
    REQUIRE(std::string(ex.what()).length() > 0);
}

TEST_CASE("ConfigException with empty message", "[Exception][ConfigException][boundary]")
{
    ConfigException ex("");
    std::string what_str(ex.what());
    REQUIRE(what_str.find("[Exception]") != std::string::npos);
}

TEST_CASE("ConfigException with long message", "[Exception][ConfigException][boundary]")
{
    std::string long_msg(10000, 'X');
    ConfigException ex(long_msg);
    std::string what_str(ex.what());
    REQUIRE(what_str.find(long_msg) != std::string::npos);
}

// ============================================================================
// ConfigFileException 测试
// ============================================================================

TEST_CASE("ConfigFileException inherits from ConfigException", "[Exception][ConfigFileException]")
{
    ConfigFileException ex("/path/to/file", "permission denied");
    REQUIRE_THROWS_AS(throw ex, ConfigException);
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
}

TEST_CASE("ConfigFileException stores file path and reason", "[Exception][ConfigFileException]")
{
    ConfigFileException ex("/etc/config.yaml", "permission denied");
    REQUIRE(ex.filePath() == "/etc/config.yaml");
    REQUIRE(std::string(ex.what()).find("/etc/config.yaml") != std::string::npos);
    REQUIRE(std::string(ex.what()).find("permission denied") != std::string::npos);
}

TEST_CASE("ConfigFileException with empty path", "[Exception][ConfigFileException][boundary]")
{
    ConfigFileException ex("", "reason");
    REQUIRE(ex.filePath().empty());
}

TEST_CASE("ConfigFileException with empty reason", "[Exception][ConfigFileException][boundary]")
{
    ConfigFileException ex("file.yaml", "");
    REQUIRE(ex.filePath() == "file.yaml");
}

TEST_CASE("ConfigFileException with unicode path", "[Exception][ConfigFileException][boundary]")
{
    ConfigFileException ex("/配置/文件.yaml", "错误");
    REQUIRE(ex.filePath() == "/配置/文件.yaml");
}

// ============================================================================
// ConfigParseException 测试
// ============================================================================

TEST_CASE("ConfigParseException inherits from ConfigException", "[Exception][ConfigParseException]")
{
    ConfigParseException ex("config.yaml", "invalid syntax");
    REQUIRE_THROWS_AS(throw ex, ConfigException);
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
}

TEST_CASE("ConfigParseException stores file path and reason", "[Exception][ConfigParseException]")
{
    ConfigParseException ex("config.yaml", "unexpected token at line 42");
    REQUIRE(ex.filePath() == "config.yaml");
    REQUIRE(std::string(ex.what()).find("config.yaml") != std::string::npos);
    REQUIRE(std::string(ex.what()).find("unexpected token") != std::string::npos);
}

TEST_CASE("ConfigParseException boundary cases", "[Exception][ConfigParseException][boundary]")
{
    // Very long parse error
    std::string long_reason(5000, 'e');
    ConfigParseException ex("f.yaml", long_reason);
    REQUIRE(ex.filePath() == "f.yaml");

    // Empty strings
    ConfigParseException ex2("", "");
    REQUIRE(ex2.filePath().empty());
}

// ============================================================================
// ConfigKeyNotFoundException 测试
// ============================================================================

TEST_CASE("ConfigKeyNotFoundException inherits from ConfigException", "[Exception][ConfigKeyNotFoundException]")
{
    ConfigKeyNotFoundException ex("server.port");
    REQUIRE_THROWS_AS(throw ex, ConfigException);
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
}

TEST_CASE("ConfigKeyNotFoundException stores missing key", "[Exception][ConfigKeyNotFoundException]")
{
    ConfigKeyNotFoundException ex("database.connection.url");
    REQUIRE(ex.key() == "database.connection.url");
    REQUIRE(std::string(ex.what()).find("database.connection.url") != std::string::npos);
    REQUIRE(std::string(ex.what()).find("not found") != std::string::npos);
}

TEST_CASE("ConfigKeyNotFoundException with empty key", "[Exception][ConfigKeyNotFoundException][boundary]")
{
    ConfigKeyNotFoundException ex("");
    REQUIRE(ex.key().empty());
}

TEST_CASE("ConfigKeyNotFoundException with various key formats", "[Exception][ConfigKeyNotFoundException][boundary]")
{
    auto keys = GENERATE(
        "a",
        "a.b.c.d.e.f.g.h.i.j",
        "very_long_key_name_that_exceeds_typical_lengths_for_configuration_keys",
        "key.with.numbers.123",
        "key-with-special_chars@test"
    );

    ConfigKeyNotFoundException ex(keys);
    REQUIRE(ex.key() == keys);
}

// ============================================================================
// ConfigTypeException 测试
// ============================================================================

TEST_CASE("ConfigTypeException inherits from ConfigException", "[Exception][ConfigTypeException]")
{
    ConfigTypeException ex("server.port", "int64_t", "std::string");
    REQUIRE_THROWS_AS(throw ex, ConfigException);
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
}

TEST_CASE("ConfigTypeException stores type mismatch info", "[Exception][ConfigTypeException]")
{
    ConfigTypeException ex("server.port", "int64_t", "std::string");
    REQUIRE(ex.key() == "server.port");
    REQUIRE(ex.expectedType() == "int64_t");
    REQUIRE(ex.actualType() == "std::string");

    std::string what_str(ex.what());
    REQUIRE(what_str.find("server.port") != std::string::npos);
    REQUIRE(what_str.find("expected") != std::string::npos);
    REQUIRE(what_str.find("int64_t") != std::string::npos);
    REQUIRE(what_str.find("std::string") != std::string::npos);
}

TEST_CASE("ConfigTypeException with empty fields", "[Exception][ConfigTypeException][boundary]")
{
    ConfigTypeException ex("", "", "");
    REQUIRE(ex.key().empty());
    REQUIRE(ex.expectedType().empty());
    REQUIRE(ex.actualType().empty());
}

// ============================================================================
// ConfigValidationException 测试
// ============================================================================

TEST_CASE("ConfigValidationException inherits from ConfigException", "[Exception][ConfigValidationException]")
{
    ConfigValidationException ex("server.port", "must be between 1 and 65535");
    REQUIRE_THROWS_AS(throw ex, ConfigException);
    REQUIRE_THROWS_AS(throw ex, std::runtime_error);
}

TEST_CASE("ConfigValidationException stores validation failure info", "[Exception][ConfigValidationException]")
{
    ConfigValidationException ex("server.port", "must be between 1 and 65535");
    REQUIRE(ex.key() == "server.port");

    std::string what_str(ex.what());
    REQUIRE(what_str.find("server.port") != std::string::npos);
    REQUIRE(what_str.find("must be between 1 and 65535") != std::string::npos);
    REQUIRE(what_str.find("Validation failed") != std::string::npos);
}

TEST_CASE("ConfigValidationException empty reason", "[Exception][ConfigValidationException][boundary]")
{
    ConfigValidationException ex("key", "");
    REQUIRE(ex.key() == "key");
}

// ============================================================================
// 异常层次结构完整性测试
// ============================================================================

TEST_CASE("Exception hierarchy is properly catchable by base types", "[Exception][hierarchy]")
{
    // All exceptions should be catchable by std::exception
    auto testCatch = [](const std::exception &e) -> bool
    {
        return std::string(e.what()).length() > 0;
    };

    REQUIRE(testCatch(ConfigException("test")));
    REQUIRE(testCatch(ConfigFileException("f", "r")));
    REQUIRE(testCatch(ConfigParseException("f", "r")));
    REQUIRE(testCatch(ConfigKeyNotFoundException("k")));
    REQUIRE(testCatch(ConfigTypeException("k", "e", "a")));
    REQUIRE(testCatch(ConfigValidationException("k", "r")));
}

TEST_CASE("Exception copy preserves information", "[Exception][boundary]")
{
    ConfigFileException original("/path/file.yaml", "test reason");
    ConfigFileException copy(original);

    REQUIRE(copy.filePath() == original.filePath());
    REQUIRE(std::string(copy.what()) == std::string(original.what()));
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("Exception stress test - rapid throw/catch", "[Exception][stress]")
{
    constexpr int ITERATIONS = 10000;

    for (int i = 0; i < ITERATIONS; ++i)
    {
        try
        {
            throw ConfigKeyNotFoundException("key_" + std::to_string(i));
        } catch (const ConfigException &e)
        {
            REQUIRE(std::string(e.what()).length() > 0);
        }
    }
    SUCCEED("Stress test completed");
}

TEST_CASE("Exception construction performance", "[Exception][benchmark]")
{
    BENCHMARK("Create ConfigException")
    {
        return ConfigException("test message for benchmarking");
    };

    BENCHMARK("Create ConfigTypeException")
    {
        return ConfigTypeException("server.port", "int64_t", "std::string");
    };
}
