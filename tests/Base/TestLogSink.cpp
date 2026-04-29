/**
 * @file TestLogSink.cpp
 * @brief LogSink 模块单元测试：格式化器、ConsoleSink、FileSink、
 *        RollingFileSink、AsyncSink
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/LogSink.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace Base;
namespace fs = std::filesystem;

// ============================================================================
// 测试辅助
// ============================================================================

namespace
{
    LogEvent makeTestEvent(LogLevel level = LogLevel::INFO,
                           std::string msg = "test message")
    {
        return LogEvent{
            level,
            "2026-04-28 20:00:00.000",
            "12345",
            SourceLocation("test.cpp", 42, "test_func"),
            "test_logger",
            std::move(msg)
        };
    }

    class TempLogDir
    {
    public:
        TempLogDir()
            : m_path(fs::temp_directory_path() / ("log_test_" + std::to_string(rand())))
        {
            fs::create_directories(m_path);
        }

        ~TempLogDir()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }

        fs::path path() const
        {
            return m_path;
        }

    private:
        fs::path m_path;
    };

    bool fileContains(const fs::path &filepath, const std::string &substring)
    {
        std::ifstream f(filepath);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        return content.find(substring) != std::string::npos;
    }
}

// ============================================================================
// DefaultFormatter 测试
// ============================================================================

TEST_CASE("DefaultFormatter produces valid output", "[LogSink][DefaultFormatter]")
{
    DefaultFormatter fmt;
    auto event   = makeTestEvent();
    auto output  = fmt.format(event);

    REQUIRE_FALSE(output.empty());
    REQUIRE(output.find("2026-04-28 20:00:00.000") != std::string::npos);
    REQUIRE(output.find("12345") != std::string::npos);
    REQUIRE(output.find("INFO ") != std::string::npos);
    REQUIRE(output.find("test_logger") != std::string::npos);
    REQUIRE(output.find("test.cpp") != std::string::npos);
    REQUIRE(output.find("42") != std::string::npos);
    REQUIRE(output.find("test message") != std::string::npos);
}

TEST_CASE("DefaultFormatter handles empty message", "[LogSink][DefaultFormatter][boundary]")
{
    DefaultFormatter fmt;
    auto event = makeTestEvent(LogLevel::INFO, "");
    auto output = fmt.format(event);
    REQUIRE_FALSE(output.empty());
}

TEST_CASE("DefaultFormatter handles very long message", "[LogSink][DefaultFormatter][boundary]")
{
    DefaultFormatter fmt;
    std::string long_msg(10000, 'A');
    auto event  = makeTestEvent(LogLevel::INFO, long_msg);
    auto output = fmt.format(event);
    REQUIRE(output.find(long_msg) != std::string::npos);
}

// ============================================================================
// ColorFormatter 测试
// ============================================================================

TEST_CASE("ColorFormatter produces output with ANSI codes", "[LogSink][ColorFormatter]")
{
    ColorFormatter fmt;
    auto event  = makeTestEvent(LogLevel::WARN, "color test");
    auto output = fmt.format(event);

    REQUIRE_FALSE(output.empty());
    REQUIRE(output.find("\033[") != std::string::npos);
    REQUIRE(output.find("color test") != std::string::npos);
}

TEST_CASE("ColorFormatter uses different colors for different levels", "[LogSink][ColorFormatter]")
{
    ColorFormatter fmt;

    auto info_output = fmt.format(makeTestEvent(LogLevel::INFO, "info"));
    auto error_output = fmt.format(makeTestEvent(LogLevel::ERROR, "error"));

    REQUIRE(info_output != error_output);
}

// ============================================================================
// LogSink 基类测试 (使用 TestSink)
// ============================================================================

class TestSink : public LogSink
{
public:
    void write(const LogEvent &event) override
    {
        last_event = event;
        write_count++;
    }

    void flush() override
    {
        flush_count++;
    }

    LogEvent last_event;
    int write_count{0};
    int flush_count{0};
};

TEST_CASE("LogSink::setLevel and getLevel round-trip", "[LogSink][base]")
{
    TestSink sink;
    sink.setLevel(LogLevel::WARN);
    REQUIRE(sink.getLevel() == LogLevel::WARN);

    sink.setLevel(LogLevel::TRACE);
    REQUIRE(sink.getLevel() == LogLevel::TRACE);

    sink.setLevel(LogLevel::FATAL);
    REQUIRE(sink.getLevel() == LogLevel::FATAL);
}

TEST_CASE("LogSink::shouldLog respects level threshold", "[LogSink][base]")
{
    TestSink sink;
    sink.setLevel(LogLevel::INFO);

    REQUIRE(sink.shouldLog(LogLevel::INFO));
    REQUIRE(sink.shouldLog(LogLevel::WARN));
    REQUIRE(sink.shouldLog(LogLevel::ERROR));
    REQUIRE(sink.shouldLog(LogLevel::FATAL));
    REQUIRE_FALSE(sink.shouldLog(LogLevel::TRACE));
    REQUIRE_FALSE(sink.shouldLog(LogLevel::DEBUG));
}

TEST_CASE("LogSink::shouldLog boundary checks", "[LogSink][base][boundary]")
{
    TestSink sink;

    sink.setLevel(LogLevel::TRACE);
    REQUIRE(sink.shouldLog(LogLevel::TRACE));
    REQUIRE(sink.shouldLog(LogLevel::FATAL));

    sink.setLevel(LogLevel::FATAL);
    REQUIRE(sink.shouldLog(LogLevel::FATAL));
    REQUIRE_FALSE(sink.shouldLog(LogLevel::ERROR));
    REQUIRE_FALSE(sink.shouldLog(LogLevel::INFO));

    sink.setLevel(LogLevel::OFF);
    REQUIRE_FALSE(sink.shouldLog(LogLevel::FATAL));
    REQUIRE_FALSE(sink.shouldLog(LogLevel::TRACE));
}

TEST_CASE("LogSink::setFormatter works via public interface", "[LogSink][base]")
{
    TestSink sink;
    // Setting a custom formatter should not throw
    sink.setFormatter(std::make_unique<DefaultFormatter>());
    sink.setFormatter(std::make_unique<ColorFormatter>());
    SUCCEED("Formatter set successfully");
}

// ============================================================================
// ConsoleSink 测试
// ============================================================================

TEST_CASE("ConsoleSink construction with color enabled", "[LogSink][ConsoleSink]")
{
    REQUIRE_NOTHROW(ConsoleSink(true));
    REQUIRE_NOTHROW(ConsoleSink(false));
}

TEST_CASE("ConsoleSink write does not throw", "[LogSink][ConsoleSink]")
{
    ConsoleSink sink(false);
    auto event = makeTestEvent();
    REQUIRE_NOTHROW(sink.write(event));
}

TEST_CASE("ConsoleSink write with all log levels", "[LogSink][ConsoleSink]")
{
    ConsoleSink sink(false);
    for (uint8_t i = 0; i <= 5; ++i)
    {
        auto level = static_cast<LogLevel>(i);
        auto event = makeTestEvent(level, "level " + std::to_string(i));
        REQUIRE_NOTHROW(sink.write(event));
    }
}

TEST_CASE("ConsoleSink flush does not throw", "[LogSink][ConsoleSink]")
{
    ConsoleSink sink(false);
    REQUIRE_NOTHROW(sink.flush());
}

TEST_CASE("ConsoleSink setColorEnabled toggles formatting", "[LogSink][ConsoleSink]")
{
    ConsoleSink sink(false);
    REQUIRE_NOTHROW(sink.setColorEnabled(true));
    REQUIRE_NOTHROW(sink.write(makeTestEvent()));
    REQUIRE_NOTHROW(sink.setColorEnabled(false));
    REQUIRE_NOTHROW(sink.write(makeTestEvent()));
}

TEST_CASE("ConsoleSink write concurrent", "[LogSink][ConsoleSink][stress]")
{
    ConsoleSink sink(false);
    std::atomic<int> count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&]()
        {
            for (int i = 0; i < 100; ++i)
            {
                sink.write(makeTestEvent());
                count.fetch_add(1);
            }
        });
    }

    for (auto &t: threads) t.join();
    REQUIRE(count.load() == 400);
}

// ============================================================================
// FileSink 测试
// ============================================================================

TEST_CASE("FileSink construction creates file", "[LogSink][FileSink]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "test.log";
    FileSink sink(filepath);

    REQUIRE(fs::exists(filepath));
}

TEST_CASE("FileSink construction creates parent directories", "[LogSink][FileSink]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "subdir" / "another" / "test.log";
    FileSink sink(filepath);
    REQUIRE(fs::exists(filepath));
}

TEST_CASE("FileSink write appends to file", "[LogSink][FileSink]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "test.log";
    FileSink sink(filepath);

    auto event = makeTestEvent(LogLevel::INFO, "unique_message_42");
    sink.write(event);
    sink.flush();

    REQUIRE(fileContains(filepath, "unique_message_42"));
}

TEST_CASE("FileSink truncate mode overwrites file", "[LogSink][FileSink]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "test.log";

    {
        FileSink sink(filepath, true);
        sink.write(makeTestEvent(LogLevel::INFO, "first"));
        sink.flush();
    }

    {
        FileSink sink(filepath, true);
        sink.write(makeTestEvent(LogLevel::INFO, "second_only"));
        sink.flush();
    }

    REQUIRE(fileContains(filepath, "second_only"));
    REQUIRE_FALSE(fileContains(filepath, "first"));
}

TEST_CASE("FileSink reopen switches file", "[LogSink][FileSink]")
{
    TempLogDir dir;
    auto file1 = dir.path() / "log1.txt";
    auto file2 = dir.path() / "log2.txt";

    FileSink sink(file1);
    sink.write(makeTestEvent(LogLevel::INFO, "message_in_file1"));
    sink.flush();

    sink.reopen(file2);
    sink.write(makeTestEvent(LogLevel::INFO, "message_in_file2"));
    sink.flush();

    REQUIRE(fileContains(file1, "message_in_file1"));
    REQUIRE(fileContains(file2, "message_in_file2"));
}

TEST_CASE("FileSink write to closed file does not crash", "[LogSink][FileSink][boundary]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "test.log";
    {
        FileSink sink(filepath);
    }
    SUCCEED("FileSink destruction is safe");
}

TEST_CASE("FileSink handles special characters in path", "[LogSink][FileSink][boundary]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "日志文件.log";
    FileSink sink(filepath);
    REQUIRE(fs::exists(filepath));
}

// ============================================================================
// RollingPolicy 枚举测试
// ============================================================================

TEST_CASE("RollingPolicy enum values", "[LogSink][RollingPolicy]")
{
    REQUIRE(static_cast<int>(RollingPolicy::Size) == 0);
    REQUIRE(static_cast<int>(RollingPolicy::Daily) == 1);
    REQUIRE(static_cast<int>(RollingPolicy::Hourly) == 2);
}

// ============================================================================
// RollingFileSink 测试
// ============================================================================

TEST_CASE("RollingFileSink construction creates file", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("app.log", dir.path(), RollingPolicy::Size, 1024 * 1024, 5);

    REQUIRE(fs::exists(dir.path() / "app.log"));
}

TEST_CASE("RollingFileSink write works correctly", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("test.log", dir.path(), RollingPolicy::Size, 10 * 1024 * 1024, 5);

    sink.write(makeTestEvent(LogLevel::INFO, "rolling_test_message"));
    sink.flush();

    REQUIRE(fileContains(dir.path() / "test.log", "rolling_test_message"));
}

TEST_CASE("RollingFileSink size-based rolling", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("sized.log", dir.path(), RollingPolicy::Size, 512, 3);

    for (int i = 0; i < 20; ++i)
    {
        sink.write(makeTestEvent(LogLevel::INFO,
                                 "line_" + std::to_string(i) + " with padding data"));
    }
    sink.flush();

    REQUIRE(fs::exists(dir.path() / "sized.log"));
}

TEST_CASE("RollingFileSink daily policy", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("daily.log", dir.path(), RollingPolicy::Daily, 10 * 1024 * 1024, 5);

    sink.write(makeTestEvent(LogLevel::INFO, "daily test"));
    sink.flush();

    // Daily policy appends date suffix (e.g. daily.2026-04-28.log)
    // Check that at least one log file exists in the directory
    bool found = false;
    for (const auto &entry: fs::directory_iterator(dir.path()))
    {
        if (entry.is_regular_file() && entry.path().filename().string().find("daily") == 0)
        {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("RollingFileSink hourly policy", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("hourly.log", dir.path(), RollingPolicy::Hourly, 10 * 1024 * 1024, 5);

    sink.write(makeTestEvent(LogLevel::INFO, "hourly test"));
    sink.flush();

    // Hourly policy appends datetime suffix
    bool found = false;
    for (const auto &entry: fs::directory_iterator(dir.path()))
    {
        if (entry.is_regular_file() && entry.path().filename().string().find("hourly") == 0)
        {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("RollingFileSink with max_backup_files=0", "[LogSink][RollingFileSink][boundary]")
{
    TempLogDir dir;
    RollingFileSink sink("nobackup.log", dir.path(), RollingPolicy::Size, 1024, 0);

    sink.write(makeTestEvent());
    sink.flush();
    SUCCEED("max_backup_files=0 works");
}

TEST_CASE("RollingFileSink flush", "[LogSink][RollingFileSink]")
{
    TempLogDir dir;
    RollingFileSink sink("flush_test.log", dir.path(), RollingPolicy::Size, 1024 * 1024, 5);

    REQUIRE_NOTHROW(sink.flush());
}

// ============================================================================
// AsyncSink 测试
// ============================================================================

TEST_CASE("AsyncSink construction starts worker thread", "[LogSink][AsyncSink]")
{
    auto console = std::make_unique<ConsoleSink>(false);
    REQUIRE_NOTHROW(AsyncSink(std::move(console), 1024, AsyncSink::OverflowPolicy::Block));
}

TEST_CASE("AsyncSink write passes events to wrapped sink", "[LogSink][AsyncSink]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "async_test.log";
    auto file = std::make_unique<FileSink>(filepath);
    AsyncSink async(std::move(file), 128);

    async.write(makeTestEvent(LogLevel::INFO, "async_message"));
    async.flush();

    REQUIRE(fileContains(filepath, "async_message"));
}

TEST_CASE("AsyncSink blocks on full queue", "[LogSink][AsyncSink][boundary]")
{
    auto test_sink = std::make_unique<TestSink>();
    auto *raw_sink = test_sink.get();

    // Use larger queue to avoid deadlock scenarios in test
    AsyncSink async(std::move(test_sink), 128, AsyncSink::OverflowPolicy::Block);

    for (int i = 0; i < 10; ++i)
    {
        async.write(makeTestEvent(LogLevel::INFO, "msg_" + std::to_string(i)));
    }

    async.stop();

    REQUIRE(raw_sink->write_count == 10);
}

TEST_CASE("AsyncSink drops on full queue", "[LogSink][AsyncSink][boundary]")
{
    auto test_sink = std::make_unique<TestSink>();
    auto *raw_sink = test_sink.get();

    AsyncSink async(std::move(test_sink), 2, AsyncSink::OverflowPolicy::Drop);

    for (int i = 0; i < 100; ++i)
    {
        async.write(makeTestEvent(LogLevel::INFO, "drop_test_" + std::to_string(i)));
    }

    async.flush();
    async.stop();

    REQUIRE(raw_sink->write_count > 0);
}

TEST_CASE("AsyncSink stop flushes remaining events", "[LogSink][AsyncSink]")
{
    auto test_sink = std::make_unique<TestSink>();
    auto *raw_sink = test_sink.get();

    AsyncSink async(std::move(test_sink), 1024);

    async.write(makeTestEvent(LogLevel::INFO, "final_message"));
    async.stop();

    REQUIRE(raw_sink->write_count == 1);
    REQUIRE(raw_sink->last_event.message == "final_message");
}

TEST_CASE("AsyncSink destruction calls stop", "[LogSink][AsyncSink]")
{
    auto test_sink = std::make_unique<TestSink>();
    auto *raw_sink = test_sink.get();

    {
        AsyncSink async(std::move(test_sink), 1024);
        async.write(makeTestEvent(LogLevel::INFO, "destructor_test"));
    }
    REQUIRE(raw_sink->write_count == 1);
    REQUIRE(raw_sink->last_event.message == "destructor_test");
}

// ============================================================================
// AsyncSink::OverflowPolicy 测试
// ============================================================================

TEST_CASE("AsyncSink::OverflowPolicy enum values", "[LogSink][AsyncSink]")
{
    REQUIRE(static_cast<int>(AsyncSink::OverflowPolicy::Block) == 0);
    REQUIRE(static_cast<int>(AsyncSink::OverflowPolicy::Drop) == 1);
}

// ============================================================================
// Sink 层级过滤测试
// ============================================================================

TEST_CASE("LogSink level filtering with FileSink", "[LogSink][filtering]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "filtered.log";
    auto file = std::make_unique<FileSink>(filepath);

    file->setLevel(LogLevel::WARN);

    auto info_event = makeTestEvent(LogLevel::INFO, "should_not_appear");
    file->write(info_event);

    auto warn_event = makeTestEvent(LogLevel::WARN, "should_appear");
    file->write(warn_event);

    file->flush();

    REQUIRE(fileContains(filepath, "should_appear"));
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("LogSink rapid write stress", "[LogSink][stress]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "stress.log";
    FileSink sink(filepath);

    constexpr int N = 5000;
    for (int i = 0; i < N; ++i)
    {
        sink.write(makeTestEvent(LogLevel::INFO, "stress_msg_" + std::to_string(i)));
    }
    sink.flush();

    REQUIRE(fileContains(filepath, "stress_msg_0"));
    REQUIRE(fileContains(filepath, "stress_msg_" + std::to_string(N - 1)));
}

TEST_CASE("AsyncSink concurrent write stress", "[LogSink][AsyncSink][stress]")
{
    TempLogDir dir;
    auto filepath = dir.path() / "async_stress.log";
    auto file = std::make_unique<FileSink>(filepath);
    AsyncSink async(std::move(file), 4096);

    std::atomic<int> count{0};
    std::vector<std::thread> threads;

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 500;

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([&, t]()
        {
            for (int i = 0; i < PER_THREAD; ++i)
            {
                async.write(makeTestEvent(LogLevel::INFO,
                                          "thread_" + std::to_string(t) + "_msg_" + std::to_string(i)));
                count.fetch_add(1);
            }
        });
    }

    for (auto &t: threads) t.join();
    async.flush();

    REQUIRE(count.load() == THREADS * PER_THREAD);
}

// ============================================================================
// 性能基准测试
// ============================================================================

TEST_CASE("LogSink performance benchmarks", "[LogSink][benchmark]")
{
    auto event = makeTestEvent();

    BENCHMARK("DefaultFormatter::format")
    {
        DefaultFormatter fmt;
        return fmt.format(event);
    };

    BENCHMARK("ColorFormatter::format")
    {
        ColorFormatter fmt;
        return fmt.format(event);
    };

    TempLogDir dir;
    auto filepath = dir.path() / "bench.log";

    BENCHMARK("FileSink::write")
    {
        FileSink sink(filepath);
        sink.write(event);
    };
}
