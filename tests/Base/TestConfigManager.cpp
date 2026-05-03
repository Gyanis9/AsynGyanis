/**
 * @file TestConfigManager.cpp
 * @brief ConfigManager 模块单元测试：加载、访问、热加载、验证
 * @copyright Copyright (c) 2026
 */
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "Base/ConfigManager.h"
#include "../TestHelpers.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace Base;
namespace fs = std::filesystem;

namespace
{
    bool doubleNear(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) < eps;
    }
} // namespace

// ============================================================================
// 测试辅助工具
// ============================================================================

class ConfigTestFixture
{
public:
    ConfigTestFixture() : m_dir(fs::temp_directory_path() / ("cfg_test_" + std::to_string(rand())))
    {
        // ConfigManager 是全局单例，并行测试会相互干扰
        // 通过全局互斥锁确保同一时间只有一个 ConfigManager 测试在执行
        configTestMutex().lock();
        fs::create_directories(m_dir);
        ConfigManager::instance().clear();
    }

    ~ConfigTestFixture()
    {
        ConfigManager::instance().disableHotReload();
        ConfigManager::instance().clear();
        std::error_code ec;
        fs::remove_all(m_dir, ec);
        configTestMutex().unlock();
    }

    fs::path path() const
    {
        return m_dir;
    }

    void writeYaml(const std::string &filename, const std::string &content)
    {
        std::ofstream f(m_dir / filename);
        f << content;
    }

private:
    fs::path m_dir;
};

// ============================================================================
// 单例测试
// ============================================================================

TEST_CASE("ConfigManager::instance returns same instance", "[ConfigManager][singleton]")
{
    auto &a = ConfigManager::instance();
    auto &b = ConfigManager::instance();
    REQUIRE(&a == &b);
}

// ============================================================================
// loadFromDirectory 测试
// ============================================================================

TEST_CASE("ConfigManager loadFromDirectory with nonexistent directory fails", "[ConfigManager][load]")
{
    ConfigManager::instance().clear();
    auto result = ConfigManager::instance().loadFromDirectory("/nonexistent/dir/path");
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result);
    REQUIRE(result.errors.size() > 0);
}

TEST_CASE("ConfigManager loadFromDirectory with file instead of directory fails", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    fx.writeYaml("test.yaml", "key: value");
    auto result = ConfigManager::instance().loadFromDirectory(fx.path() / "test.yaml");
    REQUIRE_FALSE(result.success);
}

TEST_CASE("ConfigManager loadFromDirectory with empty directory succeeds", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    auto              result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.empty());
}

TEST_CASE("ConfigManager loadFromDirectory loads YAML files", "[ConfigManager][load]")
{
    ConfigTestFixture fx;
    fx.writeYaml("app.yaml", "server:\n  host: localhost\n  port: 8080\n");

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.size() == 1);

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.has("server.host"));
    REQUIRE(cfg.get<std::string>("server.host", "") == "localhost");
    REQUIRE(cfg.get<int64_t>("server.port", 0) == 8080);
}

TEST_CASE("ConfigManager loadFromDirectory loads multiple files", "[ConfigManager][load]")
{
    ConfigTestFixture fx;
    fx.writeYaml("app.yaml", "server:\n  host: localhost\n");
    fx.writeYaml("db.yaml", "database:\n  host: db.local\n  pool_size: 10\n");

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.size() == 2);

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.has("server.host"));
    REQUIRE(cfg.has("database.host"));
    REQUIRE(cfg.has("database.pool_size"));
    REQUIRE(cfg.get<int64_t>("database.pool_size", 0) == 10);
}

TEST_CASE("ConfigManager loadFromDirectory handles invalid YAML gracefully", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    fx.writeYaml("bad.yaml", "invalid: [::] yaml: {{::}}\n");
    fx.writeYaml("good.yaml", "valid: true\n");

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.loaded_files.size() >= 1);
    bool has_failure = !result.failed_files.empty() || !result.errors.empty();
    REQUIRE(has_failure);
}

TEST_CASE("ConfigManager loadFromDirectory with non-map root node", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    fx.writeYaml("list.yaml", "- item1\n- item2\n");

    auto result              = ConfigManager::instance().loadFromDirectory(fx.path());
    bool has_loaded_or_error = !result.loaded_files.empty() || !result.errors.empty();
    REQUIRE(has_loaded_or_error);
}

TEST_CASE("ConfigManager loadFromDirectory with empty YAML file", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    fx.writeYaml("empty.yaml", "");
    fx.writeYaml("valid.yaml", "key: value\n");

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);
}

TEST_CASE("ConfigManager loadFromDirectory non-recursive", "[ConfigManager][load][boundary]")
{
    ConfigTestFixture fx;
    auto              subdir = fx.path() / "subdir";
    fs::create_directories(subdir);
    fx.writeYaml("root.yaml", "root: true\n");
    std::ofstream(subdir / "sub.yaml") << "sub: true\n";

    auto result = ConfigManager::instance().loadFromDirectory(fx.path(), false);
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.size() == 1);
}

TEST_CASE("ConfigManager loadFromDirectory recursive loads subdirectories", "[ConfigManager][load]")
{
    ConfigTestFixture fx;
    auto              subdir = fx.path() / "subdir";
    fs::create_directories(subdir);
    fx.writeYaml("root.yaml", "root: true\n");
    std::ofstream(subdir / "sub.yaml") << "sub: true\n";

    auto result = ConfigManager::instance().loadFromDirectory(fx.path(), true);
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.size() == 2);
}

// ============================================================================
// loadFiles 测试
// ============================================================================

TEST_CASE("ConfigManager loadFiles with empty list succeeds", "[ConfigManager][loadFiles][boundary]")
{
    ConfigManager::instance().clear();
    auto result = ConfigManager::instance().loadFiles({});
    REQUIRE(result.success);
}

TEST_CASE("ConfigManager loadFiles loads specified files", "[ConfigManager][loadFiles]")
{
    ConfigTestFixture fx;
    fx.writeYaml("a.yaml", "a: 1\n");
    fx.writeYaml("b.yaml", "b: 2\n");

    auto result = ConfigManager::instance().loadFiles({fx.path() / "a.yaml", fx.path() / "b.yaml"});
    REQUIRE(result.success);
    REQUIRE(result.loaded_files.size() == 2);

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.get<int64_t>("a", 0) == 1);
    REQUIRE(cfg.get<int64_t>("b", 0) == 2);
}

TEST_CASE("ConfigManager loadFiles with nonexistent file", "[ConfigManager][loadFiles][boundary]")
{
    ConfigManager::instance().clear();
    auto result = ConfigManager::instance().loadFiles({fs::path("/nonexistent/file.yaml")});
    REQUIRE(result.failed_files.size() > 0);
    REQUIRE_FALSE(result.success);
}

TEST_CASE("ConfigManager loadFiles with non-YAML file", "[ConfigManager][loadFiles][boundary]")
{
    ConfigTestFixture fx;
    {
        std::ofstream f(fx.path() / "test.txt");
        f << "not yaml";
    }

    auto result = ConfigManager::instance().loadFiles({fx.path() / "test.txt"});
    REQUIRE(result.failed_files.size() > 0);
}

// ============================================================================
// reload 测试
// ============================================================================

TEST_CASE("ConfigManager reload without prior loadFromDirectory fails", "[ConfigManager][reload][boundary]")
{
    ConfigManager::instance().clear();
    auto result = ConfigManager::instance().reload();
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.errors.empty());
}

TEST_CASE("ConfigManager reload after load picks up new files", "[ConfigManager][reload]")
{
    ConfigTestFixture fx;
    fx.writeYaml("app.yaml", "key: original\n");

    ConfigManager::instance().loadFromDirectory(fx.path());

    fx.writeYaml("extra.yaml", "extra: value\n");

    auto result = ConfigManager::instance().reload();
    REQUIRE(result.success);
    REQUIRE(ConfigManager::instance().has("extra"));
}

// ============================================================================
// 配置访问测试
// ============================================================================

TEST_CASE("ConfigManager::get with existing key returns value", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\nvalue: 42\nflag: true\nratio: 3.14\n");

    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.get<std::string>("name", "") == "test");
    REQUIRE(cfg.get<int64_t>("value", 0) == 42);
    REQUIRE(cfg.get<bool>("flag", false) == true);
    REQUIRE(doubleNear(cfg.get<double>("ratio", 0.0), 3.14));
}

TEST_CASE("ConfigManager::get returns default for missing key", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.get<std::string>("missing", "default") == "default");
    REQUIRE(cfg.get<int64_t>("missing", 99) == 99);
    REQUIRE(cfg.get<bool>("missing", true) == true);
}

TEST_CASE("ConfigManager::get throws on existing key", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    auto  val = cfg.get("name");
    REQUIRE(val.asString() == "test");
    REQUIRE_THROWS_AS(cfg.get("nonexistent"), ConfigKeyNotFoundException);
}

TEST_CASE("ConfigManager::getOptional returns nullopt for missing", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    auto  opt = cfg.getOptional("name");
    REQUIRE(opt.has_value());

    auto missing = cfg.getOptional("nonexistent");
    REQUIRE_FALSE(missing.has_value());
}

TEST_CASE("ConfigManager::getRequired throws on missing", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.getRequired<std::string>("name") == "test");
    REQUIRE_THROWS_AS(cfg.getRequired<std::string>("missing"), ConfigKeyNotFoundException);
}

TEST_CASE("ConfigManager convenience accessors", "[ConfigManager][access]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "debug: true\ncount: 42\npi: 3.14\ntitle: Hello World\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.getBool("debug", false) == true);
    REQUIRE(cfg.getInt("count", 0) == 42);
    REQUIRE(doubleNear(cfg.getDouble("pi", 0.0), 3.14));
    REQUIRE(cfg.getString("title", "") == "Hello World");

    REQUIRE(cfg.getBool("missing", true) == true);
    REQUIRE(cfg.getInt("missing", 100) == 100);
    REQUIRE(doubleNear(cfg.getDouble("missing", 1.5), 1.5));
    REQUIRE(cfg.getString("missing", "default") == "default");
}

// ============================================================================
// 配置查询测试
// ============================================================================

TEST_CASE("ConfigManager::has checks key existence", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "server:\n  host: localhost\n  port: 8080\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.has("server.host"));
    REQUIRE(cfg.has("server.port"));
    REQUIRE_FALSE(cfg.has("server.nonexistent"));
    REQUIRE_FALSE(cfg.has(""));
}

TEST_CASE("ConfigManager::keys returns sorted list", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "z: 3\na: 1\nm: 2\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto keys = ConfigManager::instance().keys();
    REQUIRE(keys.size() == 3);
    REQUIRE(std::is_sorted(keys.begin(), keys.end()));
}

TEST_CASE("ConfigManager::dump returns snapshot", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: value\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto dump = ConfigManager::instance().dump();
    REQUIRE(dump.size() == 1);
    REQUIRE(dump["key"].asString() == "value");
}

TEST_CASE("ConfigManager::loadedFiles returns file list", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    fx.writeYaml("a.yaml", "a: 1\n");
    fx.writeYaml("b.yaml", "b: 2\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto files = ConfigManager::instance().loadedFiles();
    REQUIRE(files.size() == 2);
}

TEST_CASE("ConfigManager::configDirectory returns path", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(ConfigManager::instance().configDirectory() == fx.path());
}

TEST_CASE("ConfigManager::clear removes all data", "[ConfigManager][query]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: value\n");
    ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(ConfigManager::instance().has("key"));

    ConfigManager::instance().clear();
    REQUIRE_FALSE(ConfigManager::instance().has("key"));
    REQUIRE(ConfigManager::instance().keys().empty());
}

// ============================================================================
// validateRequired 测试
// ============================================================================

TEST_CASE("ConfigManager::validateRequired returns missing keys", "[ConfigManager][validate]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "name: test\nport: 8080\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto missing = ConfigManager::instance().validateRequired({"name", "port", "host", "database"});
    REQUIRE(missing.size() == 2);
    REQUIRE(std::find(missing.begin(), missing.end(), "host") != missing.end());
    REQUIRE(std::find(missing.begin(), missing.end(), "database") != missing.end());
}

TEST_CASE("ConfigManager::validateRequired all present returns empty", "[ConfigManager][validate]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "a: 1\nb: 2\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto missing = ConfigManager::instance().validateRequired({"a", "b"});
    REQUIRE(missing.empty());
}

TEST_CASE("ConfigManager::validateRequired empty list returns empty", "[ConfigManager][validate][boundary]")
{
    auto missing = ConfigManager::instance().validateRequired({});
    REQUIRE(missing.empty());
}

// ============================================================================
// 复杂配置类型测试
// ============================================================================

TEST_CASE("ConfigManager handles nested YAML objects", "[ConfigManager][nested]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "database:\n"
                             "  mysql:\n"
                             "    host: db.local\n"
                             "    port: 3306\n"
                             "  redis:\n"
                             "    host: cache.local\n"
                             "    port: 6379\n");

    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.get<std::string>("database.mysql.host", "") == "db.local");
    REQUIRE(cfg.get<int64_t>("database.mysql.port", 0) == 3306);
    REQUIRE(cfg.get<std::string>("database.redis.host", "") == "cache.local");
    REQUIRE(cfg.get<int64_t>("database.redis.port", 0) == 6379);
}

TEST_CASE("ConfigManager handles various data types", "[ConfigManager][types]")
{
    ConfigTestFixture fx;
    fx.writeYaml("types.yaml", "bool_true: true\n"
                               "bool_false: false\n"
                               "int_positive: 12345\n"
                               "int_negative: -9876\n"
                               "int_zero: 0\n"
                               "float_val: 3.141592653589793\n"
                               "string_val: hello_world\n"
                               "empty_string: \"\"\n");

    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();
    REQUIRE(cfg.get<bool>("bool_true", false) == true);
    REQUIRE(cfg.get<bool>("bool_false", true) == false);
    REQUIRE(cfg.get<int64_t>("int_positive", 0) == 12345);
    REQUIRE(cfg.get<int64_t>("int_negative", 0) == -9876);
    REQUIRE(cfg.get<int64_t>("int_zero", 99) == 0);
    REQUIRE(doubleNear(cfg.get<double>("float_val", 0.0), 3.141592653589793));
    REQUIRE(cfg.get<std::string>("string_val", "") == "hello_world");
}

// ============================================================================
// 热加载测试
// ============================================================================

TEST_CASE("ConfigManager hot reload enable/disable", "[ConfigManager][hotreload]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: value\n");

    auto &cfg = ConfigManager::instance();
    cfg.loadFromDirectory(fx.path());

    REQUIRE_FALSE(cfg.isHotReloadEnabled());

    bool result = cfg.enableHotReload();
    if (result)
    {
        REQUIRE(cfg.isHotReloadEnabled());
        cfg.disableHotReload();
        REQUIRE_FALSE(cfg.isHotReloadEnabled());
    }
}

TEST_CASE("ConfigManager hot reload double enable is idempotent", "[ConfigManager][hotreload][boundary]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: value\n");

    auto &cfg = ConfigManager::instance();
    cfg.disableHotReload();
    cfg.loadFromDirectory(fx.path());

    REQUIRE(cfg.enableHotReload());
    REQUIRE(cfg.enableHotReload());
    REQUIRE(cfg.isHotReloadEnabled());

    cfg.disableHotReload();
}

TEST_CASE("ConfigManager hot reload without load fails", "[ConfigManager][hotreload][boundary]")
{
    ConfigManager::instance().clear();
    ConfigManager::instance().disableHotReload();

    REQUIRE_FALSE(ConfigManager::instance().isHotReloadEnabled());
    auto result = ConfigManager::instance().enableHotReload();
    REQUIRE_FALSE(result);
}

// ============================================================================
// 边界测试
// ============================================================================

TEST_CASE("ConfigManager handles very large config values", "[ConfigManager][boundary][stress]")
{
    ConfigTestFixture fx;
    std::string       large_value(50000, 'X');
    fx.writeYaml("large.yaml", "data: \"" + large_value + "\"\n");

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);

    auto &cfg = ConfigManager::instance();
    auto  val = cfg.get<std::string>("data", "");
    REQUIRE_FALSE(val.empty());
}

TEST_CASE("ConfigManager handles many keys", "[ConfigManager][boundary][stress]")
{
    ConfigTestFixture fx;
    std::string       yaml_content;
    for (int i = 0; i < 1000; ++i)
    {
        yaml_content += "key_" + std::to_string(i) + ": " + std::to_string(i) + "\n";
    }
    fx.writeYaml("many.yaml", yaml_content);

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);

    auto keys = ConfigManager::instance().keys();
    REQUIRE(keys.size() == 1000);
}

TEST_CASE("ConfigManager handles deeply nested keys", "[ConfigManager][boundary]")
{
    ConfigTestFixture fx;
    // Create YAML with proper nesting via file
    {
        std::ofstream f(fx.path() / "deep.yaml");
        f << "level0:\n";
        for (int i = 0; i < 19; ++i)
        {
            f << std::string((i + 1) * 2, ' ') << "level" << (i + 1) << ":\n";
        }
        f << std::string(40, ' ') << "value: leaf\n";
    }

    auto result = ConfigManager::instance().loadFromDirectory(fx.path());
    REQUIRE(result.success);

    auto       &cfg = ConfigManager::instance();
    std::string deep_key = "level0";
    for (int i = 1; i < 20; ++i)
    {
        deep_key += ".level" + std::to_string(i);
    }
    deep_key += ".value";

    REQUIRE(cfg.has(deep_key));
    REQUIRE(cfg.get<std::string>(deep_key, "") == "leaf");
}

TEST_CASE("ConfigManager thread safety - concurrent reads", "[ConfigManager][thread][stress]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: 42\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    std::atomic<bool>        running{true};
    std::atomic<int>         errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back(
                [&]()
                {
                    while (running.load())
                    {
                        try
                        {
                            auto &cfg = ConfigManager::instance();
                            cfg.has("key");
                            cfg.get<int64_t>("key", -1);
                            cfg.keys();
                            cfg.dump();
                        } catch (...)
                        {
                            errors.fetch_add(1);
                        }
                    }
                });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running.store(false);

    for (auto &t: threads)
    {
        t.join();
    }

    REQUIRE(errors.load() == 0);
}

// ============================================================================
// ConfigLoadResult 测试
// ============================================================================

TEST_CASE("ConfigLoadResult implicit bool conversion", "[ConfigManager][ConfigLoadResult]")
{
    ConfigLoadResult r1;
    r1.success = true;
    REQUIRE(static_cast<bool>(r1));

    ConfigLoadResult r2;
    r2.success = false;
    REQUIRE_FALSE(static_cast<bool>(r2));
}

// ============================================================================
// 性能测试
// ============================================================================

TEST_CASE("ConfigManager performance benchmarks", "[ConfigManager][benchmark]")
{
    ConfigTestFixture fx;
    fx.writeYaml("cfg.yaml", "key: value\ncount: 42\n");
    ConfigManager::instance().loadFromDirectory(fx.path());

    auto &cfg = ConfigManager::instance();

    BENCHMARK("get - existing key")
    {
        return cfg.get<std::string>("key", "");
    };

    BENCHMARK("get - missing key")
    {
        return cfg.get<std::string>("missing", "default");
    };

    BENCHMARK("has - key lookup")
    {
        return cfg.has("key");
    };

    BENCHMARK("keys - enumerate")
    {
        return cfg.keys();
    };
}
