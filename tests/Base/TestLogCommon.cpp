/**
 * @file TestLogCommon.cpp
 * @brief LogCommon 模块单元测试：日志等级、时间戳、线程ID、
 *        SourceLocation、LogEvent、颜色工具
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/LogCommon.h"

using namespace Base;

// ============================================================================
// LogLevel 枚举测试
// ============================================================================

TEST_CASE("LogLevel enum values are sequential", "[LogCommon][LogLevel]")
{
    REQUIRE(static_cast<uint8_t>(LogLevel::TRACE) == 0);
    REQUIRE(static_cast<uint8_t>(LogLevel::DEBUG) == 1);
    REQUIRE(static_cast<uint8_t>(LogLevel::INFO) == 2);
    REQUIRE(static_cast<uint8_t>(LogLevel::WARN) == 3);
    REQUIRE(static_cast<uint8_t>(LogLevel::ERROR) == 4);
    REQUIRE(static_cast<uint8_t>(LogLevel::FATAL) == 5);
    REQUIRE(static_cast<uint8_t>(LogLevel::OFF) == 6);
}

TEST_CASE("LogLevel ordering: higher = more severe", "[LogCommon][LogLevel]")
{
    REQUIRE(LogLevel::TRACE < LogLevel::DEBUG);
    REQUIRE(LogLevel::DEBUG < LogLevel::INFO);
    REQUIRE(LogLevel::INFO < LogLevel::WARN);
    REQUIRE(LogLevel::WARN < LogLevel::ERROR);
    REQUIRE(LogLevel::ERROR < LogLevel::FATAL);
    REQUIRE(LogLevel::FATAL < LogLevel::OFF);
}

// ============================================================================
// logLevelToString 测试
// ============================================================================

TEST_CASE("logLevelToString returns correct string for each level", "[LogCommon][logLevelToString]")
{
    REQUIRE(std::string(logLevelToString(LogLevel::TRACE)) == "TRACE");
    REQUIRE(std::string(logLevelToString(LogLevel::DEBUG)) == "DEBUG");
    REQUIRE(std::string(logLevelToString(LogLevel::INFO)) == "INFO ");
    REQUIRE(std::string(logLevelToString(LogLevel::WARN)) == "WARN ");
    REQUIRE(std::string(logLevelToString(LogLevel::ERROR)) == "ERROR");
    REQUIRE(std::string(logLevelToString(LogLevel::FATAL)) == "FATAL");
}

TEST_CASE("logLevelToString returns ??? for invalid levels", "[LogCommon][logLevelToString][boundary]")
{
    REQUIRE(std::string(logLevelToString(LogLevel::OFF)) == "?????");
    REQUIRE(std::string(logLevelToString(static_cast<LogLevel>(255))) == "?????");
    REQUIRE(std::string(logLevelToString(static_cast<LogLevel>(100))) == "?????");
}

// ============================================================================
// logLevelFromString 测试
// ============================================================================

TEST_CASE("logLevelFromString returns correct level for valid strings", "[LogCommon][logLevelFromString]")
{
    REQUIRE(logLevelFromString("TRACE") == LogLevel::TRACE);
    REQUIRE(logLevelFromString("DEBUG") == LogLevel::DEBUG);
    REQUIRE(logLevelFromString("INFO") == LogLevel::INFO);
    REQUIRE(logLevelFromString("WARN") == LogLevel::WARN);
    REQUIRE(logLevelFromString("ERROR") == LogLevel::ERROR);
    REQUIRE(logLevelFromString("FATAL") == LogLevel::FATAL);
    REQUIRE(logLevelFromString("OFF") == LogLevel::OFF);
}

TEST_CASE("logLevelFromString returns INFO for unknown strings", "[LogCommon][logLevelFromString][boundary]")
{
    REQUIRE(logLevelFromString("") == LogLevel::INFO);
    REQUIRE(logLevelFromString("UNKNOWN") == LogLevel::INFO);
    REQUIRE(logLevelFromString("info") == LogLevel::INFO);  // case sensitive
    REQUIRE(logLevelFromString("debug") == LogLevel::INFO);
    REQUIRE(logLevelFromString("trace") == LogLevel::INFO);
}

TEST_CASE("logLevelFromString handles edge cases", "[LogCommon][logLevelFromString][boundary]")
{
    // Very long string
    std::string long_str(10000, 'X');
    REQUIRE(logLevelFromString(long_str) == LogLevel::INFO);

    // String with null characters
    std::string with_nul("INFO\0extra", 9);
    REQUIRE(logLevelFromString(std::string_view(with_nul.data(), 4)) == LogLevel::INFO);
}

// ============================================================================
// currentTimestamp 测试
// ============================================================================

TEST_CASE("currentTimestamp returns non-empty string", "[LogCommon][currentTimestamp]")
{
    auto ts = currentTimestamp();
    REQUIRE_FALSE(ts.empty());
}

TEST_CASE("currentTimestamp format is YYYY-MM-DD HH:MM:SS.mmm", "[LogCommon][currentTimestamp]")
{
    auto ts = currentTimestamp();
    // Expected format: "2026-04-28 20:30:45.123"
    REQUIRE(ts.size() == 23);
    REQUIRE(ts[4] == '-');
    REQUIRE(ts[7] == '-');
    REQUIRE(ts[10] == ' ');
    REQUIRE(ts[13] == ':');
    REQUIRE(ts[16] == ':');
    REQUIRE(ts[19] == '.');

    // Verify all other chars are digits
    for (size_t i = 0; i < ts.size(); ++i)
    {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19)
            continue;
        REQUIRE(isdigit(ts[i]));
    }
}

TEST_CASE("currentTimestamp is monotonic within reasonable bounds", "[LogCommon][currentTimestamp]")
{
    auto ts1 = currentTimestamp();
    auto ts2 = currentTimestamp();
    // Two consecutive calls should produce timestamps
    REQUIRE_FALSE(ts1.empty());
    REQUIRE_FALSE(ts2.empty());
    REQUIRE(ts1 <= ts2);
}

// ============================================================================
// threadIdString 测试
// ============================================================================

TEST_CASE("threadIdString returns non-empty string", "[LogCommon][threadIdString]")
{
    auto tid = threadIdString();
    REQUIRE_FALSE(tid.empty());
}

TEST_CASE("threadIdString returns unique values from different threads", "[LogCommon][threadIdString][thread]")
{
    std::string main_tid = threadIdString();

    std::string other_tid;
    std::thread t([&]()
    {
        other_tid = threadIdString();
    });
    t.join();

    REQUIRE_FALSE(other_tid.empty());
    REQUIRE(main_tid != other_tid);
}

// ============================================================================
// SourceLocation 测试
// ============================================================================

TEST_CASE("SourceLocation default construction", "[LogCommon][SourceLocation]")
{
    SourceLocation loc;
    REQUIRE(loc.file_name == nullptr);
    REQUIRE(loc.line == 0);
    REQUIRE(loc.function_name == nullptr);
}

TEST_CASE("SourceLocation explicit construction", "[LogCommon][SourceLocation]")
{
    SourceLocation loc("test.cpp", 42, "test_func");
    REQUIRE(std::string(loc.file_name) == "test.cpp");
    REQUIRE(loc.line == 42);
    REQUIRE(std::string(loc.function_name) == "test_func");
}

TEST_CASE("SourceLocation::current captures source info", "[LogCommon][SourceLocation]")
{
    auto loc = SourceLocation::current();
    REQUIRE(loc.file_name != nullptr);
    REQUIRE(std::string(loc.file_name).length() > 0);
    REQUIRE(loc.line > 0);
    REQUIRE(loc.function_name != nullptr);
}

TEST_CASE("SourceLocation::shortFileName extracts filename from path", "[LogCommon][SourceLocation]")
{
    SourceLocation loc1("/home/user/project/src/main.cpp", 100, "main");
    REQUIRE(std::string(loc1.shortFileName()) == "main.cpp");

    SourceLocation loc2("src/main.cpp", 50, "func");
    REQUIRE(std::string(loc2.shortFileName()) == "main.cpp");

    SourceLocation loc3("main.cpp", 1, "f");
    REQUIRE(std::string(loc3.shortFileName()) == "main.cpp");

    // Windows path
    SourceLocation loc4("C:\\Users\\test\\file.cpp", 10, "f");
    REQUIRE(std::string(loc4.shortFileName()) == "file.cpp");

    // No slashes
    SourceLocation loc5("file.h", 5, "f");
    REQUIRE(std::string(loc5.shortFileName()) == "file.h");

    // nullptr file_name
    SourceLocation loc6;
    REQUIRE(std::string(loc6.shortFileName()) == "");
}

// ============================================================================
// LOG_SOURCE_LOCATION 测试
// ============================================================================

TEST_CASE("LOG_SOURCE_LOCATION macro captures current location", "[LogCommon][macros]")
{
    auto loc = LOG_SOURCE_LOCATION();
    REQUIRE(loc.file_name != nullptr);
    REQUIRE(loc.line > 0);
}

// ============================================================================
// LogEvent 测试
// ============================================================================

TEST_CASE("LogEvent default construction initializes string fields empty", "[LogCommon][LogEvent]")
{
    LogEvent event;
    // Note: level is uninitialized with =default constructor (trivial type)
    // Only test std::string fields which are properly default-initialized
    REQUIRE(event.timestamp.empty());
    REQUIRE(event.thread_id.empty());
    REQUIRE(event.logger_name.empty());
    REQUIRE(event.message.empty());
}

TEST_CASE("LogEvent explicit construction", "[LogCommon][LogEvent]")
{
    SourceLocation loc("file.cpp", 100, "func");
    LogEvent event(LogLevel::WARN, "2026-01-01 00:00:00.000", "12345",
                   loc, "mylogger", "test message");

    REQUIRE(event.level == LogLevel::WARN);
    REQUIRE(event.timestamp == "2026-01-01 00:00:00.000");
    REQUIRE(event.thread_id == "12345");
    REQUIRE(event.logger_name == "mylogger");
    REQUIRE(event.message == "test message");
    REQUIRE(event.location.line == 100);
}

TEST_CASE("LogEvent copy semantics", "[LogCommon][LogEvent]")
{
    LogEvent original(LogLevel::ERROR, "ts", "tid",
                      SourceLocation("f.cpp", 1, "f"), "log", "msg");

    LogEvent copy(original);
    REQUIRE(copy.level == original.level);
    REQUIRE(copy.timestamp == original.timestamp);
    REQUIRE(copy.thread_id == original.thread_id);
    REQUIRE(copy.message == original.message);
}

TEST_CASE("LogEvent move semantics", "[LogCommon][LogEvent]")
{
    LogEvent original(LogLevel::ERROR, "ts", "tid",
                      SourceLocation("f.cpp", 1, "f"), "log", "original_message");

    LogEvent moved(std::move(original));
    REQUIRE(moved.message == "original_message");
    REQUIRE(moved.logger_name == "log");
    // original is in valid but unspecified state
}

// ============================================================================
// color 命名空间测试
// ============================================================================

TEST_CASE("color::getColorForLevel returns different colors", "[LogCommon][color]")
{
    auto trace_color = color::getColorForLevel(LogLevel::TRACE);
    auto debug_color = color::getColorForLevel(LogLevel::DEBUG);
    auto info_color  = color::getColorForLevel(LogLevel::INFO);
    auto warn_color  = color::getColorForLevel(LogLevel::WARN);
    auto error_color = color::getColorForLevel(LogLevel::ERROR);
    auto fatal_color = color::getColorForLevel(LogLevel::FATAL);

    REQUIRE(trace_color != nullptr);
    REQUIRE(debug_color != nullptr);
    REQUIRE(info_color != nullptr);
    REQUIRE(warn_color != nullptr);
    REQUIRE(error_color != nullptr);
    REQUIRE(fatal_color != nullptr);

    // FATAL and ERROR should differ
    REQUIRE(std::string(error_color) != std::string(fatal_color));
}

TEST_CASE("color::getColorForLevel default returns RESET", "[LogCommon][color]")
{
    auto default_color = color::getColorForLevel(LogLevel::OFF);
    REQUIRE(std::string(default_color) == color::RESET);

    auto invalid_color = color::getColorForLevel(static_cast<LogLevel>(99));
    REQUIRE(std::string(invalid_color) == color::RESET);
}

TEST_CASE("color constants are valid ANSI escape sequences", "[LogCommon][color]")
{
    // All color constants should start with \033[
    auto check_ansi = [](const char *seq)
    {
        REQUIRE(seq[0] == '\033');
        REQUIRE(seq[1] == '[');
    };

    check_ansi(color::RESET);
    check_ansi(color::RED);
    check_ansi(color::GREEN);
    check_ansi(color::YELLOW);
    check_ansi(color::BLUE);
    check_ansi(color::MAGENTA);
    check_ansi(color::CYAN);
    check_ansi(color::WHITE);
}

TEST_CASE("color::terminalSupportsColor returns bool", "[LogCommon][color]")
{
    // Should not crash or throw
    auto supports = color::terminalSupportsColor();
    REQUIRE((supports == true || supports == false));
}

// ============================================================================
// Round-trip: logLevelToString + logLevelFromString
// ============================================================================

TEST_CASE("LogLevel round-trip through string conversion", "[LogCommon][roundtrip]")
{
    for (uint8_t i = 0; i <= 5; ++i)
    {
        auto level = static_cast<LogLevel>(i);
        auto str   = logLevelToString(level);
        // Remove padding spaces for comparison
        std::string trimmed(str);
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();

        auto recovered = logLevelFromString(trimmed);
        REQUIRE(recovered == level);
    }
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("LogCommon timestamp stress test", "[LogCommon][stress]")
{
    constexpr int ITERATIONS = 10000;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        auto ts = currentTimestamp();
        REQUIRE_FALSE(ts.empty());
        REQUIRE(ts.size() == 23);
    }
    SUCCEED("Timestamp stress test passed");
}

TEST_CASE("LogCommon threadIdString stress test", "[LogCommon][stress]")
{
    constexpr int ITERATIONS = 10000;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        auto tid = threadIdString();
        REQUIRE_FALSE(tid.empty());
    }
    SUCCEED("Thread ID stress test passed");
}

TEST_CASE("LogCommon color functions stress test", "[LogCommon][stress]")
{
    constexpr int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        auto c = color::getColorForLevel(static_cast<LogLevel>(i % 7));
        REQUIRE(c != nullptr);
    }
    SUCCEED("Color stress test passed");
}

// ============================================================================
// 性能基准测试
// ============================================================================

TEST_CASE("LogCommon performance benchmarks", "[LogCommon][benchmark]")
{
    BENCHMARK("logLevelToString")
    {
        return logLevelToString(LogLevel::INFO);
    };

    BENCHMARK("logLevelFromString")
    {
        return logLevelFromString("INFO");
    };

    BENCHMARK("currentTimestamp")
    {
        return currentTimestamp();
    };

    BENCHMARK("threadIdString")
    {
        return threadIdString();
    };

    BENCHMARK("SourceLocation::current")
    {
        return SourceLocation::current();
    };

    BENCHMARK("getColorForLevel")
    {
        return color::getColorForLevel(LogLevel::ERROR);
    };
}
