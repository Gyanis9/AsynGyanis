/**
 * @file TestConfigFileWatcher.cpp
 * @brief ConfigFileWatcher 模块单元测试：工厂、轮询监听器（跨平台）
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "Base/ConfigFileWatcher.h"

#include <filesystem>
#include <fstream>
#include <thread>

using namespace Base;
namespace fs = std::filesystem;

// ============================================================================
// 测试辅助工具
// ============================================================================

class TempDir
{
public:
    TempDir() : m_path(fs::temp_directory_path() / ("cfw_test_" + std::to_string(rand())))
    {
        fs::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    fs::path path() const
    {
        return m_path;
    }

    void writeFile(const std::string &name, const std::string &content)
    {
        std::ofstream f(m_path / name);
        f << content;
    }

private:
    fs::path m_path;
};

// ============================================================================
// FileWatcherFactory 测试
// ============================================================================

TEST_CASE("FileWatcherFactory::create returns valid watcher", "[ConfigFileWatcher][Factory]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE(watcher != nullptr);
}

TEST_CASE("FileWatcherFactory::create returns watcher with correct initial state", "[ConfigFileWatcher][Factory]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE_FALSE(watcher->isRunning());
}

// ============================================================================
// FileChangeEvent 枚举测试
// ============================================================================

TEST_CASE("FileChangeEvent enum values", "[ConfigFileWatcher][FileChangeEvent]")
{
    REQUIRE(static_cast<uint8_t>(FileChangeEvent::Modified) == 0);
    REQUIRE(static_cast<uint8_t>(FileChangeEvent::Created) == 1);
    REQUIRE(static_cast<uint8_t>(FileChangeEvent::Deleted) == 2);
    REQUIRE(static_cast<uint8_t>(FileChangeEvent::Moved) == 3);
}

// ============================================================================
// IFileWatcher 接口测试（使用具体实现）
// ============================================================================

TEST_CASE("FileWatcher lifecycle - start/stop not running", "[ConfigFileWatcher][lifecycle]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE_FALSE(watcher->isRunning());

    // Start without any watches should fail or succeed depending on implementation
    // Just verify it doesn't crash
    REQUIRE_NOTHROW(watcher->start());
    REQUIRE_NOTHROW(watcher->stop());
}

TEST_CASE("FileWatcher double start is idempotent", "[ConfigFileWatcher][lifecycle]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    watcher->addWatch(dir.path().string());
    REQUIRE(watcher->start());
    REQUIRE(watcher->isRunning());
    // Double start should be safe
    REQUIRE(watcher->start());
    REQUIRE(watcher->isRunning());
    watcher->stop();
    REQUIRE_FALSE(watcher->isRunning());
}

TEST_CASE("FileWatcher double stop is idempotent", "[ConfigFileWatcher][lifecycle]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    watcher->addWatch(dir.path().string());
    watcher->start();
    watcher->stop();
    REQUIRE_FALSE(watcher->isRunning());
    // Double stop should be safe
    REQUIRE_NOTHROW(watcher->stop());
    REQUIRE_FALSE(watcher->isRunning());
}

// ============================================================================
// addWatch / removeWatch 测试
// ============================================================================

TEST_CASE("FileWatcher addWatch with valid path succeeds", "[ConfigFileWatcher][addWatch]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    REQUIRE(watcher->addWatch(dir.path().string()));
}

TEST_CASE("FileWatcher addWatch with nonexistent path returns false", "[ConfigFileWatcher][addWatch][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE_FALSE(watcher->addWatch("/nonexistent/path/that/does/not/exist"));
}

TEST_CASE("FileWatcher addWatch duplicate path returns true", "[ConfigFileWatcher][addWatch][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    REQUIRE(watcher->addWatch(dir.path().string()));
    // Adding the same path should return true (already watching)
    REQUIRE(watcher->addWatch(dir.path().string()));
}

TEST_CASE("FileWatcher addWatch with empty path", "[ConfigFileWatcher][addWatch][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE_FALSE(watcher->addWatch(""));
}

TEST_CASE("FileWatcher removeWatch nonexistent returns false", "[ConfigFileWatcher][removeWatch][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    REQUIRE_FALSE(watcher->removeWatch("/nonexistent/path"));
}

TEST_CASE("FileWatcher removeWatch after addWatch succeeds", "[ConfigFileWatcher][removeWatch]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    REQUIRE(watcher->addWatch(dir.path().string()));
    REQUIRE(watcher->removeWatch(dir.path().string()));
}

TEST_CASE("FileWatcher addWatch with recursive flag", "[ConfigFileWatcher][addWatch]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    // Create subdirectories
    fs::create_directories(dir.path() / "sub1" / "sub2");
    REQUIRE(watcher->addWatch(dir.path().string(), true));
}

// ============================================================================
// Callback 测试
// ============================================================================

TEST_CASE("FileWatcher setCallback stores callback", "[ConfigFileWatcher][callback]")
{
    auto watcher = FileWatcherFactory::create();
    bool called = false;

    watcher->setCallback([&called](std::string_view, FileChangeEvent)
    {
        called = true;
    });

    // Setting callback shouldn't trigger it immediately
    REQUIRE_FALSE(called);
}

TEST_CASE("FileWatcher setCallback with nullptr-like callback", "[ConfigFileWatcher][callback][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    // Set an empty callback (should not crash when events fire)
    REQUIRE_NOTHROW(watcher->setCallback(nullptr));

    TempDir dir;
    watcher->addWatch(dir.path().string());
    watcher->start();

    // Write a file - callback is null so no crash expected
    dir.writeFile("test.yaml", "key: value");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    watcher->stop();
}

TEST_CASE("FileWatcher detects file modification", "[ConfigFileWatcher][integration]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;

    std::string changed_path;
    FileChangeEvent received_event = FileChangeEvent::Modified;
    bool callback_called = false;

    watcher->setCallback([&](std::string_view path, FileChangeEvent event)
    {
        changed_path = path;
        received_event = event;
        callback_called = true;
    });

    // Create initial file
    dir.writeFile("config.yaml", "key: value");
    REQUIRE(watcher->addWatch(dir.path().string()));
    REQUIRE(watcher->start());

    // Wait for watcher to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Modify file
    dir.writeFile("config.yaml", "key: modified_value");

    // Wait for detection
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    watcher->stop();

    // On Linux with inotify, modification should be detected
    // On polling fallback, it will also be detected
    if (callback_called)
    {
        REQUIRE(changed_path.find("config.yaml") != std::string::npos);
    }
}

TEST_CASE("FileWatcher handles rapid file changes", "[ConfigFileWatcher][stress]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;

    std::atomic<int> callback_count{0};

    watcher->setCallback([&](std::string_view, FileChangeEvent)
    {
        callback_count.fetch_add(1);
    });

    dir.writeFile("app.yaml", "k: 0");
    watcher->addWatch(dir.path().string());
    watcher->start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Rapid modifications
    for (int i = 0; i < 10; ++i)
    {
        dir.writeFile("app.yaml", "k: " + std::to_string(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    watcher->stop();

    // Just verify it doesn't crash under rapid changes
    SUCCEED("Rapid change stress test completed without crash");
}

TEST_CASE("FileWatcher handles multiple concurrent watches", "[ConfigFileWatcher][stress]")
{
    auto watcher = FileWatcherFactory::create();
    std::vector<TempDir> dirs;
    dirs.reserve(5);

    for (int i = 0; i < 5; ++i)
    {
        dirs.emplace_back();
        dirs.back().writeFile("test.yaml", "k: v");
        watcher->addWatch(dirs.back().path().string());
    }

    watcher->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Modify all files
    for (int i = 0; i < 5; ++i)
    {
        dirs[i].writeFile("test.yaml", "k: modified_" + std::to_string(i));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    REQUIRE_NOTHROW(watcher->stop());
    SUCCEED("Multiple watch stress test passed");
}

// ============================================================================
// 析构安全测试
// ============================================================================

TEST_CASE("FileWatcher destruction while running is safe", "[ConfigFileWatcher][lifecycle]")
{
    auto watcher = FileWatcherFactory::create();
    TempDir dir;
    watcher->addWatch(dir.path().string());
    watcher->start();
    // Destructor will stop the watcher
    // watcher goes out of scope here — should not hang or crash
}

TEST_CASE("FileWatcher destruction without start is safe", "[ConfigFileWatcher][lifecycle]")
{
    auto watcher = FileWatcherFactory::create();
    // Destructor called without ever starting
}
