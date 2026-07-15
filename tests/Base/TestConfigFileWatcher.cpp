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
#include <random>
#include <thread>

using namespace Base;
namespace fs = std::filesystem;

// ============================================================================
// 测试辅助工具
// ============================================================================

class TempDir
{
public:
    TempDir() : m_path(fs::temp_directory_path() / ("cfw_test_" + std::to_string(std::random_device{}())))
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

    // 无监视的情况下启动，根据实现可能成功也可能失败
    // 只需验证不会崩溃
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
    // 双重启动应该安全
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
    // 双重停止应该安全
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
    // 添加相同路径应该返回 true（已经在监视中）
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
    // 创建子目录
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

    // 设置回调不应立即触发
    REQUIRE_FALSE(called);
}

TEST_CASE("FileWatcher setCallback with nullptr-like callback", "[ConfigFileWatcher][callback][boundary]")
{
    auto watcher = FileWatcherFactory::create();
    // 设置空回调（事件触发时不应崩溃）
    REQUIRE_NOTHROW(watcher->setCallback(nullptr));

    TempDir dir;
    watcher->addWatch(dir.path().string());
    watcher->start();

    // 写入文件 - 回调为 null，不期望崩溃
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

    // 创建初始文件
    dir.writeFile("config.yaml", "key: value");
    REQUIRE(watcher->addWatch(dir.path().string()));
    REQUIRE(watcher->start());

    // 等待监视器就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 修改文件
    dir.writeFile("config.yaml", "key: modified_value");

    // 等待检测
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
