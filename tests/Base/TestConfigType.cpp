/**
 * @file TestConfigType.cpp
 * @brief ConfigType 模块单元测试：typeName, isYamlFile, splitKey
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/ConfigType.h"

using namespace Base;

// ============================================================================
// typeName 测试
// ============================================================================

TEST_CASE("typeName returns correct string for each ConfigValueType", "[ConfigType][typeName]")
{
    REQUIRE(std::string(typeName(ConfigValueType::Null)) == "null");
    REQUIRE(std::string(typeName(ConfigValueType::Bool)) == "bool");
    REQUIRE(std::string(typeName(ConfigValueType::Int)) == "int");
    REQUIRE(std::string(typeName(ConfigValueType::Double)) == "double");
    REQUIRE(std::string(typeName(ConfigValueType::String)) == "string");
    REQUIRE(std::string(typeName(ConfigValueType::Array)) == "array");
    REQUIRE(std::string(typeName(ConfigValueType::Object)) == "object");
}

TEST_CASE("typeName handles all valid enum values", "[ConfigType][typeName][boundary]")
{
    // Test every valid enum value explicitly
    for (uint8_t i = 0; i <= 6; ++i)
    {
        auto type = static_cast<ConfigValueType>(i);
        auto name = typeName(type);
        REQUIRE(name != nullptr);
        REQUIRE(std::string(name).length() > 0);
    }
}

TEST_CASE("typeName returns unknown for out-of-range values", "[ConfigType][typeName][boundary]")
{
    // Test with out-of-range enum values
    auto unknown = static_cast<ConfigValueType>(255);
    REQUIRE(std::string(typeName(unknown)) == "unknown");

    auto also_unknown = static_cast<ConfigValueType>(7);
    REQUIRE(std::string(typeName(also_unknown)) == "unknown");

    auto extreme = static_cast<ConfigValueType>(128);
    REQUIRE(std::string(typeName(extreme)) == "unknown");
}

// ============================================================================
// isYamlFile 测试
// ============================================================================

TEST_CASE("isYamlFile recognizes .yaml and .yml files", "[ConfigType][isYamlFile]")
{
    REQUIRE(isYamlFile("config.yaml"));
    REQUIRE(isYamlFile("config.yml"));
    REQUIRE(isYamlFile("/path/to/config.yaml"));
    REQUIRE(isYamlFile("/path/to/config.yml"));
    REQUIRE(isYamlFile("./relative/path/app.yaml"));
    REQUIRE(isYamlFile("./relative/path/app.yml"));
}

TEST_CASE("isYamlFile rejects non-YAML files", "[ConfigType][isYamlFile]")
{
    REQUIRE_FALSE(isYamlFile("config.json"));
    REQUIRE_FALSE(isYamlFile("config.xml"));
    REQUIRE_FALSE(isYamlFile("config.toml"));
    REQUIRE_FALSE(isYamlFile("config.ini"));
    REQUIRE_FALSE(isYamlFile("config"));
    REQUIRE_FALSE(isYamlFile("yaml"));
    REQUIRE_FALSE(isYamlFile("yml"));
}

TEST_CASE("isYamlFile handles edge cases", "[ConfigType][isYamlFile][boundary]")
{
    // Empty string
    REQUIRE_FALSE(isYamlFile(""));

    // Very short strings
    REQUIRE_FALSE(isYamlFile("a"));
    REQUIRE_FALSE(isYamlFile(".y"));
    REQUIRE_FALSE(isYamlFile(".ym"));
    REQUIRE(isYamlFile("a.yml"));

    // Strings that are exactly the extension
    REQUIRE(isYamlFile(".yaml"));
    REQUIRE(isYamlFile(".yml"));

    // Strings with YAML substring but not as extension
    REQUIRE_FALSE(isYamlFile("yaml.txt"));
    REQUIRE_FALSE(isYamlFile("yml.txt"));

    // Hidden files with YAML extension
    REQUIRE(isYamlFile(".hidden.yaml"));
    REQUIRE(isYamlFile(".hidden.yml"));

    // Long paths
    std::string long_path(1000, 'a');
    long_path += ".yaml";
    REQUIRE(isYamlFile(long_path));

    // Case-sensitive: only lowercase .yaml/.yml recognized
    REQUIRE_FALSE(isYamlFile("Config.YaMl"));
    REQUIRE_FALSE(isYamlFile("Config.YmL"));
}

TEST_CASE("isYamlFile stress test", "[ConfigType][isYamlFile][stress]")
{
    constexpr size_t ITERATIONS = 10000;

    for (size_t i = 0; i < ITERATIONS; ++i)
    {
        isYamlFile("config.yaml");
        isYamlFile("config.yml");
        isYamlFile("config.json");
        isYamlFile("config");
    }
    SUCCEED("Stress test completed without crash");
}

// ============================================================================
// splitKey 测试
// ============================================================================

TEST_CASE("splitKey splits dotted configuration keys", "[ConfigType][splitKey]")
{
    auto parts = splitKey("server.host");
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0] == "server");
    REQUIRE(parts[1] == "host");

    parts = splitKey("a.b.c.d.e");
    REQUIRE(parts.size() == 5);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[4] == "e");
}

TEST_CASE("splitKey handles single level keys", "[ConfigType][splitKey]")
{
    auto parts = splitKey("server");
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "server");
}

TEST_CASE("splitKey handles empty key", "[ConfigType][splitKey][boundary]")
{
    auto parts = splitKey("");
    REQUIRE(parts.empty());

    parts = splitKey("", '.');
    REQUIRE(parts.empty());

    parts = splitKey("", '/');
    REQUIRE(parts.empty());
}

TEST_CASE("splitKey handles consecutive dots", "[ConfigType][splitKey][boundary]")
{
    auto parts = splitKey("a..b");
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[1] == "b");

    parts = splitKey("...");
    REQUIRE(parts.empty());

    parts = splitKey(".leading");
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "leading");

    parts = splitKey("trailing.");
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "trailing");

    parts = splitKey("a....b");
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[1] == "b");
}

TEST_CASE("splitKey handles custom delimiter", "[ConfigType][splitKey]")
{
    auto parts = splitKey("a/b/c", '/');
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[1] == "b");
    REQUIRE(parts[2] == "c");

    parts = splitKey("path/to/file", '/');
    REQUIRE(parts.size() == 3);

    parts = splitKey("single", '/');
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "single");
}

TEST_CASE("splitKey handles unicode and special characters", "[ConfigType][splitKey][boundary]")
{
    // Non-ASCII characters (UTF-8)
    auto parts = splitKey("数据库.mysql.连接池");
    REQUIRE(parts.size() == 3);

    // Special characters within segments
    parts = splitKey("hello_world.foo-bar.baz123");
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0] == "hello_world");
    REQUIRE(parts[1] == "foo-bar");
    REQUIRE(parts[2] == "baz123");
}

TEST_CASE("splitKey stress test - many segments", "[ConfigType][splitKey][stress]")
{
    // Build a key with many segments
    std::string key;
    constexpr int SEGMENTS = 1000;
    for (int i = 0; i < SEGMENTS; ++i)
    {
        if (i > 0) key += '.';
        key += "segment_" + std::to_string(i);
    }

    auto parts = splitKey(key);
    REQUIRE(parts.size() == static_cast<size_t>(SEGMENTS));

    BENCHMARK("splitKey - 1000 segments")
    {
        return splitKey(key);
    };
}

TEST_CASE("splitKey stress test - long segments", "[ConfigType][splitKey][stress]")
{
    std::string long_segment(10000, 'x');
    auto parts = splitKey(long_segment);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0].size() == 10000);
}

TEST_CASE("splitKey benchmark - typical usage", "[ConfigType][splitKey][benchmark]")
{
    BENCHMARK("splitKey - typical 3-level key")
    {
        return splitKey("database.connection.pool_size");
    };
}
