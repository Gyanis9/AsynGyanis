#include <catch2/catch_test_macros.hpp>
#include "Core/IoContext.h"
#include "Core/Task.h"
#include <thread>
#include <atomic>
#include <memory>

using namespace Core;

TEST_CASE("IoContext: construction", "[IoContext]") {
    IoContext ctx(2);
    auto &pool = ctx.threadPool();
    REQUIRE(pool.threadCount() == 2);
}

TEST_CASE("IoContext: threadPool access", "[IoContext]") {
    IoContext ctx(2);
    auto &pool = ctx.threadPool();
    REQUIRE(pool.threadCount() == 2);
}

TEST_CASE("IoContext: mainScheduler returns scheduler 0", "[IoContext]") {
    IoContext ctx(2);
    auto &s = ctx.mainScheduler();
    REQUIRE_FALSE(s.hasWork());
}

TEST_CASE("IoContext: stop without run", "[IoContext]") {
    IoContext ctx(1);
    ctx.stop(); // Should not deadlock
}

TEST_CASE("IoContext: run blocks until stop", "[IoContext]") {
    IoContext ctx(1);
    std::atomic<bool> running{false};

    std::thread t([&]() {
        running.store(true);
        ctx.run();
    });

    // Wait for run to start
    while (!running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ctx.stop();
    t.join();
}

namespace {
    Task<int> setIoValue(std::atomic<int> &v) { v.store(99); co_return 0; }
}

TEST_CASE("IoContext: scheduling task on mainScheduler before run", "[IoContext]") {
    IoContext ctx(1);
    std::atomic<int> value{0};

    auto task = setIoValue(value);
    ctx.mainScheduler().schedule(task.handle());

    std::thread t([&]() { ctx.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ctx.stop();
    t.join();

    REQUIRE(value.load() == 99);
}

TEST_CASE("IoContext: default constructor uses hardware concurrency", "[IoContext]") {
    IoContext ctx;
    auto &pool = ctx.threadPool();
    REQUIRE(pool.threadCount() == std::thread::hardware_concurrency());
}
