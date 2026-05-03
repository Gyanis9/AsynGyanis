/**
 * @file TestLoggerConfig.cpp
 * @brief LoggerConfig 模块单元测试：LoggerConfigLoader 配置加载
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "Base/LoggerConfig.h"
#include "Base/ConfigManager.h"
#include "../TestHelpers.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

using namespace Base;
namespace fs = std::filesystem;

// ============================================================================
// 测试辅助
// ============================================================================

class LoggerConfigFixture
{
public:
    LoggerConfigFixture()
        : m_dir(fs::temp_directory_path() / ("logcfg_test_" + std::to_string(std::random_device{}())))
    {
        configTestMutex().lock();
        fs::create_directories(m_dir);
        ConfigManager::instance().clear();
        LoggerRegistry::instance().clear();
    }

    ~LoggerConfigFixture()
    {
        ConfigManager::instance().disableHotReload();
        ConfigManager::instance().clear();
        LoggerRegistry::instance().clear();
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

    void loadConfig(const std::string &yaml_content)
    {
        writeYaml("logging.yaml", yaml_content);
        ConfigManager::instance().loadFromDirectory(m_dir);
    }

private:
    fs::path m_dir;
};

// ============================================================================
// loadFromConfig 基本测试
// ============================================================================

TEST_CASE("LoggerConfigLoader::loadFromConfig with empty config creates root logger", "[LoggerConfig][load]")
{
    LoggerConfigFixture fx;
    ConfigManager::instance().clear();
    LoggerRegistry::instance().clear();

    // No config loaded — should still not crash
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

TEST_CASE("LoggerConfigLoader::loadFromConfig with basic logging config", "[LoggerConfig][load]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: DEBUG
  loggers:
    root:
      level: INFO
      sinks:
        - type: console
          color: false
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    auto &root = LoggerRegistry::instance().getRootLogger();
    REQUIRE(root.getLevel() == LogLevel::INFO);
}

TEST_CASE("LoggerConfigLoader::loadFromConfig with custom loggers", "[LoggerConfig][load]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: WARN
  loggers:
    root:
      level: INFO
      sinks:
        - type: console
          color: false
    network:
      level: DEBUG
      sinks:
        - type: console
          color: false
    database:
      level: ERROR
      sinks:
        - type: console
          color: false
)");

    LoggerRegistry::instance().clear();
    LoggerConfigLoader::loadFromConfig();

    // Verify loggers exist
    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(std::find(names.begin(), names.end(), "root") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "network") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "database") != names.end());

    // Verify levels
    auto &net = LoggerRegistry::instance().getLogger("network");
    REQUIRE(net.getLevel() == LogLevel::DEBUG);

    auto &db = LoggerRegistry::instance().getLogger("database");
    REQUIRE(db.getLevel() == LogLevel::ERROR);
}

TEST_CASE("LoggerConfigLoader::loadFromConfig with custom config_prefix", "[LoggerConfig][load]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
myapp_logging:
  global_level: ERROR
  loggers:
    root:
      level: WARN
      sinks:
        - type: console
          color: false
)");

    LoggerRegistry::instance().clear();
    LoggerConfigLoader::loadFromConfig("myapp_logging");

    auto &root = LoggerRegistry::instance().getRootLogger();
    REQUIRE(root.getLevel() == LogLevel::WARN);
}

// ============================================================================
// Sink 类型测试
// ============================================================================

TEST_CASE("LoggerConfigLoader creates console sink", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: console
          color: true
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

TEST_CASE("LoggerConfigLoader creates file sink", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_file = (fx.path() / "app.log").string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: file
          path: ")" + log_file + R"("
          truncate: false
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    // Write to the logger
    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "file_sink_test");
    root.flush();
}

TEST_CASE("LoggerConfigLoader creates rolling_file sink", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_dir = fx.path().string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: rolling.log
          directory: ")" + log_dir + R"("
          policy: size
          max_size_mb: 10
          max_backup: 5
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "rolling_test");
    root.flush();

    // Verify the log file was created
    REQUIRE(fs::exists(fs::path(log_dir) / "rolling.log"));
}

TEST_CASE("LoggerConfigLoader creates rolling_file with daily policy", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_dir = fx.path().string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: daily.log
          directory: ")" + log_dir + R"("
          policy: daily
          max_backup: 7
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "daily_test");
    root.flush();
}

TEST_CASE("LoggerConfigLoader creates rolling_file with hourly policy", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_dir = fx.path().string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: rolling_file
          base_filename: hourly.log
          directory: ")" + log_dir + R"("
          policy: hourly
          max_backup: 24
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "hourly_test");
    root.flush();
}

TEST_CASE("LoggerConfigLoader creates async sink wrapping file", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_file = (fx.path() / "async_cfg.log").string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 512
          overflow_policy: block
          wrapped:
            type: file
            path: ")" + log_file + R"("
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "async_cfg_test");
    root.flush();
}

TEST_CASE("LoggerConfigLoader creates async sink with drop policy", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_file = (fx.path() / "async_drop.log").string();

    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 256
          overflow_policy: drop
          wrapped:
            type: console
            color: false
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

// ============================================================================
// Sink 级别过滤测试
// ============================================================================

TEST_CASE("LoggerConfigLoader sink level filtering", "[LoggerConfig][sink]")
{
    LoggerConfigFixture fx;
    auto log_file = (fx.path() / "filtered_cfg.log").string();

    fx.loadConfig(R"(
logging:
  global_level: TRACE
  loggers:
    root:
      level: TRACE
      sinks:
        - type: file
          path: ")" + log_file + R"("
          level: WARN
)");

    LoggerRegistry::instance().clear();
    LoggerConfigLoader::loadFromConfig();

    auto &root = LoggerRegistry::instance().getRootLogger();
    root.log(LogLevel::INFO, "filtered_out_by_sink");
    root.log(LogLevel::ERROR, "should_appear");
    root.flush();
}

// ============================================================================
// 边界测试
// ============================================================================

TEST_CASE("LoggerConfigLoader handles unknown sink type gracefully", "[LoggerConfig][boundary]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: unknown_type
          some_param: value
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

TEST_CASE("LoggerConfigLoader handles missing sinks array", "[LoggerConfig][boundary]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

TEST_CASE("LoggerConfigLoader handles missing loggers section", "[LoggerConfig][boundary]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: TRACE
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    // Should have created root logger with default settings
    auto &root = LoggerRegistry::instance().getRootLogger();
    REQUIRE(root.getLevel() == LogLevel::TRACE);
}

TEST_CASE("LoggerConfigLoader handles invalid level string", "[LoggerConfig][boundary]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: INVALID_LEVEL
  loggers:
    root:
      level: NOT_A_LEVEL
      sinks:
        - type: console
          color: false
)");

    LoggerRegistry::instance().clear();
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());

    // Invalid levels should default to INFO
    auto &root = LoggerRegistry::instance().getRootLogger();
    REQUIRE(root.getLevel() == LogLevel::INFO);
}

TEST_CASE("LoggerConfigLoader handles empty config gracefully", "[LoggerConfig][boundary]")
{
    LoggerRegistry::instance().clear();
    ConfigManager::instance().clear();

    // No config at all
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}

TEST_CASE("LoggerConfigLoader handles async sink with missing wrapped config", "[LoggerConfig][boundary]")
{
    LoggerConfigFixture fx;
    fx.loadConfig(R"(
logging:
  global_level: INFO
  loggers:
    root:
      level: INFO
      sinks:
        - type: async
          queue_size: 128
)");

    LoggerRegistry::instance().clear();
    // LoggerConfigLoader 应妥善处理缺失必填字段，跳过该 sink 而非抛出异常
    REQUIRE_NOTHROW(LoggerConfigLoader::loadFromConfig());
}
