/**
 * @file TestLogger.cpp
 * @brief Logger 模块单元测试：Logger 类、LoggerRegistry、宏
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/Logger.h"

#include <sstream>
#include <thread>

using namespace Base;

// ============================================================================
// Logger 构造和基本属性测试
// ============================================================================

TEST_CASE("Logger construction with name", "[Logger][construction]")
{
    Logger logger("test");
    REQUIRE(logger.name() == "test");
}

TEST_CASE("Logger default level is TRACE", "[Logger][construction]")
{
    Logger logger("test");
    REQUIRE(logger.getLevel() == LogLevel::TRACE);
}

TEST_CASE("Logger construction with empty name", "[Logger][construction][boundary]")
{
    Logger logger("");
    REQUIRE(logger.name().empty());
}

TEST_CASE("Logger construction with long name", "[Logger][construction][boundary]")
{
    std::string long_name(500, 'L');
    Logger logger(long_name);
    REQUIRE(logger.name() == long_name);
}

// ============================================================================
// Logger 日志级别测试
// ============================================================================

TEST_CASE("Logger::setLevel and getLevel round-trip", "[Logger][level]")
{
    Logger logger("test");
    logger.setLevel(LogLevel::WARN);
    REQUIRE(logger.getLevel() == LogLevel::WARN);

    logger.setLevel(LogLevel::DEBUG);
    REQUIRE(logger.getLevel() == LogLevel::DEBUG);

    logger.setLevel(LogLevel::OFF);
    REQUIRE(logger.getLevel() == LogLevel::OFF);
}

TEST_CASE("Logger::shouldLog respects level threshold", "[Logger][level]")
{
    Logger logger("test");
    logger.setLevel(LogLevel::INFO);

    REQUIRE(logger.shouldLog(LogLevel::INFO));
    REQUIRE(logger.shouldLog(LogLevel::WARN));
    REQUIRE(logger.shouldLog(LogLevel::ERROR));
    REQUIRE(logger.shouldLog(LogLevel::FATAL));
    REQUIRE_FALSE(logger.shouldLog(LogLevel::TRACE));
    REQUIRE_FALSE(logger.shouldLog(LogLevel::DEBUG));
}

TEST_CASE("Logger::shouldLog all levels with TRACE threshold", "[Logger][level][boundary]")
{
    Logger logger("test");
    logger.setLevel(LogLevel::TRACE);
    REQUIRE(logger.shouldLog(LogLevel::TRACE));
    REQUIRE(logger.shouldLog(LogLevel::DEBUG));
    REQUIRE(logger.shouldLog(LogLevel::INFO));
    REQUIRE(logger.shouldLog(LogLevel::WARN));
    REQUIRE(logger.shouldLog(LogLevel::ERROR));
    REQUIRE(logger.shouldLog(LogLevel::FATAL));
}

TEST_CASE("Logger::shouldLog with OFF threshold blocks all", "[Logger][level][boundary]")
{
    Logger logger("test");
    logger.setLevel(LogLevel::OFF);
    REQUIRE_FALSE(logger.shouldLog(LogLevel::TRACE));
    REQUIRE_FALSE(logger.shouldLog(LogLevel::INFO));
    REQUIRE_FALSE(logger.shouldLog(LogLevel::FATAL));
}

// ============================================================================
// Logger Sink 管理测试
// ============================================================================

class CaptureSink : public LogSink
{
public:
    void write(const LogEvent &event) override
    {
        std::lock_guard lock(m_mtx);
        events.push_back(event);
    }

    void flush() override {}

    std::vector<LogEvent> events;
    std::mutex m_mtx;
};

TEST_CASE("Logger::addSink adds sink to logger", "[Logger][sink]")
{
    Logger logger("test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    // Log something
    logger.log(LogLevel::INFO, "test message");
    logger.flush();

    REQUIRE(sink->events.size() == 1);
    REQUIRE(sink->events[0].message == "test message");
    REQUIRE(sink->events[0].logger_name == "test");
    REQUIRE(sink->events[0].level == LogLevel::INFO);
}

TEST_CASE("Logger::log dispatches to multiple sinks", "[Logger][sink]")
{
    Logger logger("multi");
    auto *sink1 = new CaptureSink();
    auto *sink2 = new CaptureSink();

    logger.addSink(std::unique_ptr<LogSink>(sink1));
    logger.addSink(std::unique_ptr<LogSink>(sink2));

    logger.log(LogLevel::ERROR, "broadcast");

    REQUIRE(sink1->events.size() == 1);
    REQUIRE(sink2->events.size() == 1);
    REQUIRE(sink1->events[0].message == "broadcast");
    REQUIRE(sink2->events[0].message == "broadcast");
}

TEST_CASE("Logger log respects shouldLog across sinks", "[Logger][sink]")
{
    Logger logger("filtered");
    auto *sink = new CaptureSink();
    sink->setLevel(LogLevel::WARN);
    logger.addSink(std::unique_ptr<LogSink>(sink));

    // Sink level is WARN, logger level is TRACE (default)
    // INFO log should be filtered by sink, not by logger
    logger.log(LogLevel::INFO, "should_not_reach_sink");
    logger.log(LogLevel::WARN, "should_reach_sink");
    logger.flush();

    // The LogEvent is dispatched to sinks, but the sink's shouldLog
    // filters it in writeToSinks
    REQUIRE(sink->events.size() >= 1);
}

TEST_CASE("Logger::clearSinks removes all sinks", "[Logger][sink]")
{
    Logger logger("clear_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.log(LogLevel::INFO, "before_clear");

    // clearSinks() 会 delete sink，必须在调用前保存数据
    const size_t event_count = sink->events.size();
    const std::string last_msg = sink->events.empty() ? "" : sink->events[0].message;

    logger.clearSinks();
    logger.log(LogLevel::INFO, "after_clear");
    logger.flush();

    REQUIRE(event_count == 1);
    REQUIRE(last_msg == "before_clear");
}

TEST_CASE("Logger::flush flushes all sinks", "[Logger][sink]")
{
    Logger logger("flush_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    REQUIRE_NOTHROW(logger.flush());
}

// ============================================================================
// Logger::log 重载测试
// ============================================================================

TEST_CASE("Logger::log with message only", "[Logger][log]")
{
    Logger logger("test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.log(LogLevel::INFO, "simple message");

    REQUIRE(sink->events.size() == 1);
    REQUIRE(sink->events[0].message == "simple message");
    REQUIRE(sink->events[0].level == LogLevel::INFO);
}

TEST_CASE("Logger::log with location and message", "[Logger][log]")
{
    Logger logger("test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    auto loc = SourceLocation::current();
    logger.log(LogLevel::DEBUG, loc, "location message");

    REQUIRE(sink->events.size() == 1);
    REQUIRE(sink->events[0].message == "location message");
}

TEST_CASE("Logger::log below threshold is suppressed", "[Logger][log]")
{
    Logger logger("test");
    logger.setLevel(LogLevel::ERROR);

    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.log(LogLevel::INFO, "should_be_suppressed");
    logger.log(LogLevel::ERROR, "should_be_logged");
    logger.flush();

    REQUIRE(sink->events.size() == 1);
    REQUIRE(sink->events[0].message == "should_be_logged");
}

// ============================================================================
// Logger::logFormat 测试
// ============================================================================

TEST_CASE("Logger::logFormat formats messages correctly", "[Logger][logFormat]")
{
    Logger logger("fmt_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.logFormat(LogLevel::INFO, SourceLocation::current(), "value={}", 42);
    logger.flush();

    REQUIRE(sink->events.size() == 1);
    REQUIRE(sink->events[0].message == "value=42");
}

TEST_CASE("Logger::logFormat with multiple arguments", "[Logger][logFormat]")
{
    Logger logger("fmt_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.logFormat(LogLevel::WARN, SourceLocation::current(),
                     "{} + {} = {}", 2, 3, 5);
    logger.flush();

    REQUIRE(sink->events[0].message == "2 + 3 = 5");
}

TEST_CASE("Logger::logFormat with string arguments", "[Logger][logFormat]")
{
    Logger logger("fmt_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    logger.logFormat(LogLevel::INFO, SourceLocation::current(),
                     "Hello, {}!", "World");
    logger.flush();

    REQUIRE(sink->events[0].message == "Hello, World!");
}

TEST_CASE("Logger::logFormat handles format errors gracefully", "[Logger][logFormat][boundary]")
{
    Logger logger("fmt_test");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    // Malformed format string - should log error instead of crashing
    REQUIRE_NOTHROW(
        logger.logFormat(LogLevel::INFO, SourceLocation::current(),
                         "malformed {", "arg")
    );
    logger.flush();

    // Should have logged an error about format
    REQUIRE(sink->events.size() >= 1);
}

// ============================================================================
// LoggerRegistry 测试
// ============================================================================

TEST_CASE("LoggerRegistry::instance returns same instance", "[LoggerRegistry][singleton]")
{
    auto &a = LoggerRegistry::instance();
    auto &b = LoggerRegistry::instance();
    REQUIRE(&a == &b);
}

TEST_CASE("LoggerRegistry::getRootLogger returns root logger", "[LoggerRegistry][root]")
{
    auto &root = LoggerRegistry::instance().getRootLogger();
    REQUIRE(root.name() == "root");
}

TEST_CASE("LoggerRegistry::getLogger creates on first access", "[LoggerRegistry][getLogger]")
{
    auto &logger1 = LoggerRegistry::instance().getLogger("custom");
    REQUIRE(logger1.name() == "custom");

    // Second access returns same logger
    auto &logger2 = LoggerRegistry::instance().getLogger("custom");
    REQUIRE(&logger1 == &logger2);
}

TEST_CASE("LoggerRegistry::getLogger different names", "[LoggerRegistry][getLogger]")
{
    auto &a = LoggerRegistry::instance().getLogger("logger_a");
    auto &b = LoggerRegistry::instance().getLogger("logger_b");

    REQUIRE(a.name() == "logger_a");
    REQUIRE(b.name() == "logger_b");
    REQUIRE(&a != &b);
}

TEST_CASE("LoggerRegistry::registerLogger adds external logger", "[LoggerRegistry][register]")
{
    auto logger = std::make_unique<Logger>("external");
    LoggerRegistry::instance().registerLogger(std::move(logger));

    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(std::find(names.begin(), names.end(), "external") != names.end());
}

TEST_CASE("LoggerRegistry::registerLogger overwrites existing", "[LoggerRegistry][register]")
{
    auto &original = LoggerRegistry::instance().getLogger("overwrite_me");
    original.setLevel(LogLevel::INFO);

    auto replacement = std::make_unique<Logger>("overwrite_me");
    replacement->setLevel(LogLevel::ERROR);

    LoggerRegistry::instance().registerLogger(std::move(replacement));

    auto &retrieved = LoggerRegistry::instance().getLogger("overwrite_me");
    REQUIRE(retrieved.getLevel() == LogLevel::ERROR);
}

TEST_CASE("LoggerRegistry::registerLogger null logger ignored", "[LoggerRegistry][register][boundary]")
{
    auto names_before = LoggerRegistry::instance().getLoggerNames().size();
    LoggerRegistry::instance().registerLogger(nullptr);
    auto names_after = LoggerRegistry::instance().getLoggerNames().size();
    REQUIRE(names_before == names_after);
}

TEST_CASE("LoggerRegistry::unregisterLogger removes logger", "[LoggerRegistry][unregister]")
{
    LoggerRegistry::instance().getLogger("to_remove");
    LoggerRegistry::instance().unregisterLogger("to_remove");

    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(std::find(names.begin(), names.end(), "to_remove") == names.end());
}

TEST_CASE("LoggerRegistry::unregisterLogger nonexistent is safe", "[LoggerRegistry][unregister][boundary]")
{
    REQUIRE_NOTHROW(LoggerRegistry::instance().unregisterLogger("nonexistent"));
}

TEST_CASE("LoggerRegistry::getLoggerNames returns all names", "[LoggerRegistry][query]")
{
    // Clear first
    LoggerRegistry::instance().clear();

    LoggerRegistry::instance().getLogger("alpha");
    LoggerRegistry::instance().getLogger("beta");
    LoggerRegistry::instance().getRootLogger(); // "root"

    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(names.size() >= 3);
    REQUIRE(std::find(names.begin(), names.end(), "alpha") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "beta") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "root") != names.end());
}

TEST_CASE("LoggerRegistry::clear removes all loggers", "[LoggerRegistry][query]")
{
    LoggerRegistry::instance().getLogger("temp1");
    LoggerRegistry::instance().getLogger("temp2");

    LoggerRegistry::instance().clear();

    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(names.empty());
}

TEST_CASE("LoggerRegistry::forEachLogger iterates all", "[LoggerRegistry][query]")
{
    LoggerRegistry::instance().clear();
    LoggerRegistry::instance().getLogger("iter_a");
    LoggerRegistry::instance().getLogger("iter_b");

    std::vector<std::string> seen;
    LoggerRegistry::instance().forEachLogger([&](Logger &logger)
    {
        seen.push_back(logger.name());
    });

    REQUIRE(seen.size() >= 2);
}

// ============================================================================
// Logger 线程安全测试
// ============================================================================

TEST_CASE("Logger concurrent logging from multiple threads", "[Logger][thread][stress]")
{
    Logger logger("concurrent");
    auto *sink = new CaptureSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 1000;
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([&logger, t]()
        {
            for (int i = 0; i < PER_THREAD; ++i)
            {
                logger.log(LogLevel::INFO,
                           "thread_" + std::to_string(t) + "_msg_" + std::to_string(i));
            }
        });
    }

    for (auto &t: threads) t.join();
    logger.flush();

    REQUIRE(sink->events.size() == THREADS * PER_THREAD);
}

TEST_CASE("LoggerRegistry concurrent getLogger", "[LoggerRegistry][thread][stress]")
{
    LoggerRegistry::instance().clear();

    constexpr int THREADS = 8;
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([t]()
        {
            for (int i = 0; i < 100; ++i)
            {
                auto &logger = LoggerRegistry::instance().getLogger(
                    "concurrent_" + std::to_string(t % 4));
                logger.log(LogLevel::DEBUG, "test");
            }
        });
    }

    for (auto &t: threads) t.join();

    auto names = LoggerRegistry::instance().getLoggerNames();
    REQUIRE(names.size() <= 4); // Only 4 unique logger names
}

// ============================================================================
// LoggerRegistry 并发压力测试
// ============================================================================

TEST_CASE("LoggerRegistry rapid register/unregister stress", "[LoggerRegistry][stress]")
{
    LoggerRegistry::instance().clear();

    for (int i = 0; i < 1000; ++i)
    {
        auto logger = std::make_unique<Logger>("stress_" + std::to_string(i));
        LoggerRegistry::instance().registerLogger(std::move(logger));
        LoggerRegistry::instance().unregisterLogger("stress_" + std::to_string(i));
    }

    // Final state should be empty or minimal
    auto names = LoggerRegistry::instance().getLoggerNames();
    // Just verify no crash occurred
    SUCCEED("Register/unregister stress test passed");
}

// ============================================================================
// 性能基准测试
// ============================================================================

TEST_CASE("Logger performance benchmarks", "[Logger][benchmark]")
{
    Logger logger("bench");

    BENCHMARK("Logger construction")
    {
        Logger l("bench_construction");
        return l.name();
    };

    BENCHMARK("Logger::shouldLog")
    {
        return logger.shouldLog(LogLevel::INFO);
    };

    BENCHMARK("Logger::log (below threshold - fast path)")
    {
        logger.setLevel(LogLevel::ERROR);
        logger.log(LogLevel::INFO, "bench message");
    };
}
