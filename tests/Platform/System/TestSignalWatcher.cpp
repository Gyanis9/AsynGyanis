// SignalWatcher 单元测试：集合内容、屏蔽、真实 SIGHUP 交付与平台能力差异
#include "Platform/System/SignalWatcher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    TEST(SignalWatcher, ShutdownSetMatchesPlatform)
    {
#if ASYN_PLATFORM_WIN32
        const SignalSet signals = SignalSet::forShutdown();
        EXPECT_TRUE(signals.isEmpty());
        EXPECT_TRUE(signals.capturesConsoleShutdown());
#else
        const SignalSet signals = SignalSet::forShutdown();
        EXPECT_TRUE(signals.contains(SIGINT));
        EXPECT_TRUE(signals.contains(SIGTERM));
        EXPECT_FALSE(signals.contains(SIGHUP));
        EXPECT_FALSE(signals.capturesConsoleShutdown());
#endif
    }

    TEST(SignalWatcher, ReloadSetMatchesPlatform)
    {
        const SignalSet signals = SignalSet::forReload();
#if ASYN_PLATFORM_WIN32
        EXPECT_TRUE(signals.isEmpty());
        EXPECT_FALSE(signals.capturesConsoleShutdown());
#else
        EXPECT_TRUE(signals.contains(SIGHUP));
        EXPECT_FALSE(signals.isEmpty());
#endif
    }

    TEST(SignalWatcher, ShutdownWatcherInstallsOnBothPlatforms)
    {
        SignalWatcher watcher(SignalSet::forShutdown(), [](SignalEvent) noexcept { return true; });
        EXPECT_TRUE(watcher.isInstalled());
    }

    TEST(SignalWatcher, ReloadChannelIsUnavailableOnWindows)
    {
#if ASYN_PLATFORM_WIN32
        SignalWatcher watcher(SignalSet::forReload(), [](SignalEvent) noexcept { return true; });
        EXPECT_FALSE(watcher.isInstalled());
#else
        GTEST_SKIP() << "POSIX 有 SIGHUP，由下一条用例验证";
#endif
    }

#if !ASYN_PLATFORM_WIN32
    TEST(SignalWatcher, DeliversARealSighupToTheHandler)
    {
        std::atomic<bool> sawReload{false};
        SignalWatcher     watcher(
                SignalSet::forReload(),
                [&sawReload](const SignalEvent event) noexcept
                {
                    if (event == SignalEvent::Reload)
                    {
                        sawReload.store(true, std::memory_order_release);
                    }
                    return true;
                },
                [](const std::string_view) {});
        ASSERT_TRUE(watcher.isInstalled());

        ASSERT_EQ(::kill(::getpid(), SIGHUP), 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sawReload.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(sawReload.load(std::memory_order_acquire)) << "SIGHUP 没有被观察者取走";
    }
#endif
} // namespace AsynGyanis::Platform
