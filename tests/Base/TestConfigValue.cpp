/**
 * @file TestConfigValue.cpp
 * @brief ConfigValue 模块单元测试：构造、类型查询、类型转换、嵌套访问
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/ConfigValue.h"

#include <cmath>

using namespace Base;

namespace
{
    // Helper for approximate floating-point comparisons (Catch2 v3 compat)
    bool doubleNear(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) < eps;
    }
}

// ============================================================================
// 构造测试
// ============================================================================

TEST_CASE("ConfigValue default construction creates Null", "[ConfigValue][construction]")
{
    ConfigValue v;
    REQUIRE(v.type() == ConfigValueType::Null);
    REQUIRE(v.isNull());
    REQUIRE(v.is<std::nullptr_t>());
    REQUIRE(v.empty());
}

TEST_CASE("ConfigValue nullptr_t construction", "[ConfigValue][construction]")
{
    ConfigValue v(nullptr);
    REQUIRE(v.type() == ConfigValueType::Null);
    REQUIRE(v.isNull());
}

TEST_CASE("ConfigValue bool construction", "[ConfigValue][construction]")
{
    ConfigValue v_true(true);
    REQUIRE(v_true.type() == ConfigValueType::Bool);
    REQUIRE(v_true.is<bool>());
    REQUIRE_FALSE(v_true.empty());

    ConfigValue v_false(false);
    REQUIRE(v_false.type() == ConfigValueType::Bool);
    REQUIRE(v_false.asBool() == false);
}

TEST_CASE("ConfigValue int construction", "[ConfigValue][construction]")
{
    ConfigValue v(42);
    REQUIRE(v.type() == ConfigValueType::Int);
    REQUIRE(v.asInt() == 42);
}

TEST_CASE("ConfigValue int64_t construction", "[ConfigValue][construction]")
{
    ConfigValue v(INT64_MAX);
    REQUIRE(v.type() == ConfigValueType::Int);
    REQUIRE(v.asInt() == INT64_MAX);

    ConfigValue v_neg(INT64_MIN);
    REQUIRE(v_neg.asInt() == INT64_MIN);

    ConfigValue v_zero(int64_t(0));
    REQUIRE(v_zero.asInt() == 0);
}

TEST_CASE("ConfigValue double construction", "[ConfigValue][construction]")
{
    ConfigValue v(3.14159);
    REQUIRE(v.type() == ConfigValueType::Double);
    REQUIRE(doubleNear(v.asDouble(), 3.14159));

    ConfigValue v_inf(std::numeric_limits<double>::infinity());
    REQUIRE(std::isinf(v_inf.asDouble()));

    ConfigValue v_nan(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::isnan(v_nan.asDouble()));
}

TEST_CASE("ConfigValue const char* construction", "[ConfigValue][construction]")
{
    ConfigValue v("hello");
    REQUIRE(v.type() == ConfigValueType::String);
    REQUIRE(v.asString() == "hello");
}

TEST_CASE("ConfigValue std::string construction", "[ConfigValue][construction]")
{
    ConfigValue v(std::string("world"));
    REQUIRE(v.type() == ConfigValueType::String);
    REQUIRE(v.asString() == "world");

    ConfigValue v_empty(std::string(""));
    REQUIRE(v_empty.type() == ConfigValueType::String);
    REQUIRE(v_empty.asString().empty());
    REQUIRE(v_empty.empty());
}

TEST_CASE("ConfigValue ConfigArray construction", "[ConfigValue][construction]")
{
    ConfigArray arr;
    arr.push_back(ConfigValue(1));
    arr.push_back(ConfigValue("two"));
    arr.push_back(ConfigValue(true));

    ConfigValue v(std::move(arr));
    REQUIRE(v.type() == ConfigValueType::Array);
    REQUIRE(v.asArray().size() == 3);
    REQUIRE(v.size() == 3);
}

TEST_CASE("ConfigValue ConfigObject construction", "[ConfigValue][construction]")
{
    ConfigObject obj;
    obj["key1"] = ConfigValue(100);
    obj["key2"] = ConfigValue("value");

    ConfigValue v(std::move(obj));
    REQUIRE(v.type() == ConfigValueType::Object);
    REQUIRE(v.asObject().size() == 2);
    REQUIRE(v.size() == 2);
}

// ============================================================================
// 拷贝和移动语义测试
// ============================================================================

TEST_CASE("ConfigValue copy construction", "[ConfigValue][copy]")
{
    ConfigValue original(42);
    ConfigValue copy(original);

    REQUIRE(copy.type() == ConfigValueType::Int);
    REQUIRE(copy.asInt() == 42);
    REQUIRE(original.asInt() == 42);
}

TEST_CASE("ConfigValue copy assignment", "[ConfigValue][copy]")
{
    ConfigValue original(42);
    ConfigValue copy("initial");
    copy = original;

    REQUIRE(copy.asInt() == 42);
    REQUIRE(original.asInt() == 42);
}

TEST_CASE("ConfigValue move construction", "[ConfigValue][move]")
{
    ConfigValue original(std::string("movable"));
    ConfigValue moved(std::move(original));

    REQUIRE(moved.type() == ConfigValueType::String);
    REQUIRE(moved.asString() == "movable");
}

TEST_CASE("ConfigValue move assignment", "[ConfigValue][move]")
{
    ConfigValue original(std::string("movable"));
    ConfigValue moved(0);
    moved = std::move(original);

    REQUIRE(moved.type() == ConfigValueType::String);
    REQUIRE(moved.asString() == "movable");
}

// ============================================================================
// 类型查询测试
// ============================================================================

TEST_CASE("ConfigValue::type returns correct types for all variants", "[ConfigValue][type]")
{
    REQUIRE(ConfigValue(nullptr).type() == ConfigValueType::Null);
    REQUIRE(ConfigValue(true).type() == ConfigValueType::Bool);
    REQUIRE(ConfigValue(int64_t(1)).type() == ConfigValueType::Int);
    REQUIRE(ConfigValue(1.0).type() == ConfigValueType::Double);
    REQUIRE(ConfigValue(std::string("s")).type() == ConfigValueType::String);
    REQUIRE(ConfigValue(ConfigArray{}).type() == ConfigValueType::Array);
    REQUIRE(ConfigValue(ConfigObject{}).type() == ConfigValueType::Object);
}

TEST_CASE("ConfigValue::is<T> correctly identifies types", "[ConfigValue][is]")
{
    ConfigValue v(42);
    REQUIRE(v.is<int64_t>());
    REQUIRE_FALSE(v.is<bool>());
    REQUIRE_FALSE(v.is<std::string>());
    REQUIRE_FALSE(v.is<std::nullptr_t>());
}

TEST_CASE("ConfigValue::isNull detects null values", "[ConfigValue][isNull]")
{
    REQUIRE(ConfigValue().isNull());
    REQUIRE(ConfigValue(nullptr).isNull());
    REQUIRE_FALSE(ConfigValue(0).isNull());
    REQUIRE_FALSE(ConfigValue(false).isNull());
    REQUIRE_FALSE(ConfigValue("").isNull());
}

TEST_CASE("ConfigValue::empty semantic checks", "[ConfigValue][empty]")
{
    REQUIRE(ConfigValue().empty());
    REQUIRE(ConfigValue(nullptr).empty());
    REQUIRE(ConfigValue(std::string("")).empty());
    REQUIRE(ConfigValue(ConfigArray{}).empty());
    REQUIRE(ConfigValue(ConfigObject{}).empty());
    REQUIRE_FALSE(ConfigValue(false).empty());
    REQUIRE_FALSE(ConfigValue(0).empty());
    REQUIRE_FALSE(ConfigValue(0.0).empty());
    REQUIRE_FALSE(ConfigValue(std::string("x")).empty());

    ConfigArray arr = {ConfigValue(1)};
    REQUIRE_FALSE(ConfigValue(std::move(arr)).empty());

    ConfigObject obj;
    obj["k"] = ConfigValue(1);
    REQUIRE_FALSE(ConfigValue(std::move(obj)).empty());
}

// ============================================================================
// 强类型访问 as<T>() 测试
// ============================================================================

TEST_CASE("ConfigValue::as<T> returns correct reference", "[ConfigValue][as]")
{
    ConfigValue v(std::string("test_string"));
    REQUIRE(v.as<std::string>() == "test_string");

    // ConfigArray comparison: check size instead of direct equality
    ConfigValue arr_val(ConfigArray{});
    REQUIRE(arr_val.asArray().size() == 0);

    REQUIRE_NOTHROW(v.as<std::string>());
}

TEST_CASE("ConfigValue::as<T> throws on type mismatch", "[ConfigValue][as][exception]")
{
    ConfigValue v(42);
    REQUIRE_THROWS_AS(v.as<std::string>(), ConfigTypeException);
    REQUIRE_THROWS_AS(v.as<bool>(), ConfigTypeException);
    REQUIRE_THROWS_AS(v.as<ConfigArray>(), ConfigTypeException);
    REQUIRE_THROWS_AS(v.as<ConfigObject>(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asBool returns bool", "[ConfigValue][asBool]")
{
    REQUIRE(ConfigValue(true).asBool() == true);
    REQUIRE(ConfigValue(false).asBool() == false);
    REQUIRE_THROWS_AS(ConfigValue(42).asBool(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asInt returns int64_t", "[ConfigValue][asInt]")
{
    REQUIRE(ConfigValue(42).asInt() == 42);
    REQUIRE(ConfigValue(int(0)).asInt() == 0);
    REQUIRE_THROWS_AS(ConfigValue(true).asInt(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asDouble returns double", "[ConfigValue][asDouble]")
{
    REQUIRE(doubleNear(ConfigValue(3.14).asDouble(), 3.14));
    REQUIRE_THROWS_AS(ConfigValue(42).asDouble(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asString returns const string&", "[ConfigValue][asString]")
{
    ConfigValue v(std::string("data"));
    REQUIRE(v.asString() == "data");
    REQUIRE_THROWS_AS(ConfigValue(42).asString(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asArray returns const ConfigArray&", "[ConfigValue][asArray]")
{
    ConfigArray arr = {ConfigValue(1), ConfigValue(2)};
    ConfigValue v(std::move(arr));
    REQUIRE(v.asArray().size() == 2);
    REQUIRE_THROWS_AS(ConfigValue(42).asArray(), ConfigTypeException);
}

TEST_CASE("ConfigValue::asObject returns const ConfigObject&", "[ConfigValue][asObject]")
{
    ConfigObject obj;
    obj["a"] = ConfigValue(1);
    ConfigValue v(std::move(obj));
    REQUIRE(v.asObject().size() == 1);
    REQUIRE_THROWS_AS(ConfigValue(42).asObject(), ConfigTypeException);
}

// ============================================================================
// 安全访问 get<T>() 测试
// ============================================================================

TEST_CASE("ConfigValue::get<T> returns optional on match", "[ConfigValue][get]")
{
    ConfigValue v(42);
    auto result = v.get<int64_t>();
    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
}

TEST_CASE("ConfigValue::get<T> returns nullopt on mismatch", "[ConfigValue][get]")
{
    ConfigValue v(42);
    REQUIRE_FALSE(v.get<std::string>().has_value());
    REQUIRE_FALSE(v.get<bool>().has_value());
    REQUIRE_FALSE(v.get<double>().has_value());
}

TEST_CASE("ConfigValue::getBool returns optional bool", "[ConfigValue][getBool]")
{
    REQUIRE(ConfigValue(true).getBool().has_value());
    REQUIRE(ConfigValue(true).getBool().value() == true);
    REQUIRE_FALSE(ConfigValue(42).getBool().has_value());
}

TEST_CASE("ConfigValue::getInt / getDouble / getString", "[ConfigValue][get]")
{
    auto i = ConfigValue(42).getInt();
    REQUIRE(i.has_value());
    REQUIRE(*i == 42);

    auto d = ConfigValue(3.14).getDouble();
    REQUIRE(d.has_value());
    REQUIRE(doubleNear(*d, 3.14));

    auto s = ConfigValue(std::string("x")).getString();
    REQUIRE(s.has_value());
    REQUIRE(*s == "x");
}

TEST_CASE("ConfigValue::getArray / getObject", "[ConfigValue][get]")
{
    ConfigArray arr = {ConfigValue(1)};
    auto av = ConfigValue(arr).getArray();
    REQUIRE(av.has_value());
    REQUIRE(av->size() == 1);

    ConfigObject obj;
    obj["k"] = ConfigValue(1);
    auto ov = ConfigValue(obj).getObject();
    REQUIRE(ov.has_value());
    REQUIRE(ov->size() == 1);
}

// ============================================================================
// valueOr / *Or 带默认值访问测试
// ============================================================================

TEST_CASE("ConfigValue::valueOr returns value on match", "[ConfigValue][valueOr]")
{
    ConfigValue v(42);
    REQUIRE(v.valueOr(int64_t(99)) == 42);
}

TEST_CASE("ConfigValue::valueOr returns default on mismatch", "[ConfigValue][valueOr]")
{
    ConfigValue v(std::string("text"));
    REQUIRE(v.valueOr(int64_t(99)) == 99);
}

TEST_CASE("ConfigValue::boolOr / intOr / doubleOr / stringOr", "[ConfigValue][valueOr]")
{
    REQUIRE(ConfigValue(true).boolOr(false) == true);
    REQUIRE(ConfigValue(42).boolOr(true) == true);

    REQUIRE(ConfigValue(42).intOr(0) == 42);
    REQUIRE(ConfigValue("text").intOr(100) == 100);

    REQUIRE(doubleNear(ConfigValue(3.14).doubleOr(0.0), 3.14));
    REQUIRE(doubleNear(ConfigValue(42).doubleOr(1.0), 1.0));

    REQUIRE(ConfigValue(std::string("hello")).stringOr("default") == "hello");
    REQUIRE(ConfigValue(42).stringOr("default") == "default");
}

// ============================================================================
// 嵌套对象/数组访问测试
// ============================================================================

TEST_CASE("ConfigValue::contains checks object key existence", "[ConfigValue][contains]")
{
    ConfigObject obj;
    obj["name"] = ConfigValue(std::string("test"));
    obj["port"] = ConfigValue(8080);

    ConfigValue v(std::move(obj));
    REQUIRE(v.contains("name"));
    REQUIRE(v.contains("port"));
    REQUIRE_FALSE(v.contains("nonexistent"));

    REQUIRE_FALSE(ConfigValue(42).contains("key"));
    REQUIRE_FALSE(ConfigValue("str").contains("key"));
}

TEST_CASE("ConfigValue operator[](key) accesses object members", "[ConfigValue][operator_bracket]")
{
    ConfigObject obj;
    obj["host"] = ConfigValue(std::string("localhost"));
    obj["port"] = ConfigValue(8080);

    ConfigValue v(std::move(obj));
    REQUIRE(v["host"].asString() == "localhost");
    REQUIRE(v["port"].asInt() == 8080);

    REQUIRE_THROWS_AS(v["nonexistent"], ConfigKeyNotFoundException);
    REQUIRE_THROWS_AS(ConfigValue(42)["key"], ConfigTypeException);
}

TEST_CASE("ConfigValue::get(key) safe object access", "[ConfigValue][get]")
{
    ConfigObject obj;
    obj["key"] = ConfigValue(std::string("value"));
    ConfigValue v(std::move(obj));

    auto result = v.get("key");
    REQUIRE(result.has_value());
    REQUIRE(result->get().asString() == "value");

    REQUIRE_FALSE(v.get("nonexistent").has_value());
    REQUIRE_FALSE(ConfigValue(42).get("any").has_value());
}

TEST_CASE("ConfigValue templated get(key) returns typed optional", "[ConfigValue][get]")
{
    ConfigObject obj;
    obj["int_key"] = ConfigValue(42);
    obj["str_key"] = ConfigValue(std::string("hello"));
    ConfigValue v(std::move(obj));

    auto int_val = v.get<int64_t>("int_key");
    REQUIRE(int_val.has_value());
    REQUIRE(*int_val == 42);

    auto str_val = v.get<std::string>("str_key");
    REQUIRE(str_val.has_value());
    REQUIRE(*str_val == "hello");

    REQUIRE_FALSE(v.get<bool>("int_key").has_value());
    REQUIRE_FALSE(v.get<int64_t>("missing").has_value());
}

TEST_CASE("ConfigValue operator[](index) accesses array elements", "[ConfigValue][operator_bracket]")
{
    ConfigArray arr;
    arr.push_back(ConfigValue(1));
    arr.push_back(ConfigValue(std::string("two")));
    arr.push_back(ConfigValue(true));

    ConfigValue v(std::move(arr));
    REQUIRE(v[0].asInt() == 1);
    REQUIRE(v[1].asString() == "two");
    REQUIRE(v[2].asBool() == true);

    REQUIRE_THROWS_AS(v[3], ConfigKeyNotFoundException);
    REQUIRE_THROWS_AS(v[100], ConfigKeyNotFoundException);
}

TEST_CASE("ConfigValue::size returns semantic size", "[ConfigValue][size]")
{
    REQUIRE(ConfigValue().size() == 0);
    REQUIRE(ConfigValue(false).size() == 0);
    REQUIRE(ConfigValue(42).size() == 0);
    REQUIRE(ConfigValue(1.0).size() == 0);
    REQUIRE(ConfigValue(std::string("hello")).size() == 5);
    REQUIRE(ConfigValue(std::string("")).size() == 0);

    ConfigArray arr = {ConfigValue(1), ConfigValue(2), ConfigValue(3)};
    REQUIRE(ConfigValue(arr).size() == 3);

    ConfigObject obj;
    obj["a"] = ConfigValue(1);
    obj["b"] = ConfigValue(2);
    REQUIRE(ConfigValue(obj).size() == 2);
}

// ============================================================================
// Variant 底层访问
// ============================================================================

TEST_CASE("ConfigValue::variant returns underlying variant", "[ConfigValue][variant]")
{
    ConfigValue v(42);
    REQUIRE(v.variant().index() == static_cast<size_t>(ConfigValueType::Int));

    ConfigValue v2(std::string("test"));
    REQUIRE(v2.variant().index() == static_cast<size_t>(ConfigValueType::String));
}

TEST_CASE("ConfigValue mutable variant access", "[ConfigValue][variant]")
{
    ConfigValue v(42);
    v.variant() = std::string("changed");
    REQUIRE(v.type() == ConfigValueType::String);
    REQUIRE(v.asString() == "changed");
}

// ============================================================================
// 边界测试
// ============================================================================

TEST_CASE("ConfigValue handles extreme integer values", "[ConfigValue][boundary]")
{
    ConfigValue v_max(INT64_MAX);
    REQUIRE(v_max.asInt() == INT64_MAX);

    ConfigValue v_min(INT64_MIN);
    REQUIRE(v_min.asInt() == INT64_MIN);

    ConfigValue v_zero(int64_t(0));
    REQUIRE(v_zero.asInt() == 0);
}

TEST_CASE("ConfigValue handles extreme double values", "[ConfigValue][boundary]")
{
    ConfigValue v(std::numeric_limits<double>::max());
    REQUIRE(v.asDouble() == std::numeric_limits<double>::max());

    ConfigValue v2(std::numeric_limits<double>::min());
    REQUIRE(v2.asDouble() == std::numeric_limits<double>::min());

    ConfigValue v3(std::numeric_limits<double>::epsilon());
    REQUIRE(v3.asDouble() == std::numeric_limits<double>::epsilon());
}

TEST_CASE("ConfigValue handles very large strings", "[ConfigValue][boundary]")
{
    std::string large(1'000'000, 'X');
    ConfigValue v(large);
    REQUIRE(v.asString().size() == 1'000'000);
    REQUIRE(v.size() == 1'000'000);
}

TEST_CASE("ConfigValue handles deeply nested structures", "[ConfigValue][boundary]")
{
    ConfigObject inner;
    inner["value"] = ConfigValue(42);

    ConfigObject outer;
    outer["inner"] = ConfigValue(inner);

    ConfigValue v(std::move(outer));
    REQUIRE(v.contains("inner"));
    REQUIRE(v["inner"].contains("value"));
    REQUIRE(v["inner"]["value"].asInt() == 42);
}

TEST_CASE("ConfigValue handles large arrays", "[ConfigValue][boundary][stress]")
{
    ConfigArray arr;
    constexpr size_t N = 100000;
    for (size_t i = 0; i < N; ++i)
    {
        arr.push_back(ConfigValue(static_cast<int64_t>(i)));
    }

    ConfigValue v(std::move(arr));
    REQUIRE(v.size() == N);
    REQUIRE(v[0].asInt() == 0);
    REQUIRE(v[N - 1].asInt() == static_cast<int64_t>(N - 1));
}

TEST_CASE("ConfigValue handles NUL characters in strings", "[ConfigValue][boundary]")
{
    std::string with_nul("hello\0world", 11);
    ConfigValue v(with_nul);
    REQUIRE(v.asString().size() == 11);
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("ConfigValue rapid construction/destruction", "[ConfigValue][stress]")
{
    constexpr int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        ConfigValue v(static_cast<int64_t>(i));
        REQUIRE(v.asInt() == i);
    }
    SUCCEED("Rapid construction stress test passed");
}

TEST_CASE("ConfigValue variant switching stress", "[ConfigValue][stress]")
{
    ConfigValue v(nullptr);
    for (int i = 0; i < 10000; ++i)
    {
        v = ConfigValue(static_cast<int64_t>(i));
        REQUIRE(v.asInt() == i);
        v = ConfigValue(std::to_string(i));
        REQUIRE(v.asString() == std::to_string(i));
        v = ConfigValue(i % 2 == 0);
        REQUIRE(v.asBool() == (i % 2 == 0));
    }
    SUCCEED("Variant switching stress test passed");
}

TEST_CASE("ConfigValue performance benchmarks", "[ConfigValue][benchmark]")
{
    BENCHMARK("Construction - int")
    {
        return ConfigValue(42);
    };

    BENCHMARK("Construction - string")
    {
        return ConfigValue(std::string("benchmark_test"));
    };

    BENCHMARK("get<int64_t> - match")
    {
        ConfigValue v(42);
        return v.get<int64_t>();
    };

    BENCHMARK("get<bool> - mismatch")
    {
        ConfigValue v(42);
        return v.get<bool>();
    };
}
