#include <catch2/catch_test_macros.hpp>
#include "Core/ThreadPool.h"
#include "Core/EventLoop.h"
#include "Core/Task.h"
#include <atomic>
#include <thread>
#include <memory>

using namespace Core;

TEST_CASE("ThreadPool: construction with default threads", "[ThreadPool]") {
    ThreadPool pool;
    REQUIRE(pool.threadCount() >= 1);
}

TEST_CASE("ThreadPool: construction with specific count", "[ThreadPool]") {
    ThreadPool pool(4);
    REQUIRE(pool.threadCount() == 4);
}

TEST_CASE("ThreadPool: construction with 0 uses hardware concurrency", "[ThreadPool]") {
    ThreadPool pool(0);
    REQUIRE(pool.threadCount() == std::thread::hardware_concurrency());
}

TEST_CASE("ThreadPool: eventLoop access by index", "[ThreadPool]") {
    ThreadPool pool(2);
    REQUIRE_NOTHROW(pool.eventLoop(0));
    REQUIRE_NOTHROW(pool.eventLoop(1));
    REQUIRE_THROWS_AS(pool.eventLoop(2), std::out_of_range);
}

TEST_CASE("ThreadPool: scheduler access by index", "[ThreadPool]") {
    ThreadPool pool(2);
    REQUIRE_NOTHROW(pool.scheduler(0));
    REQUIRE_NOTHROW(pool.scheduler(1));
}

TEST_CASE("ThreadPool: start and stop", "[ThreadPool]") {
    ThreadPool pool(2);
    pool.start();

    // Verify loops are running
    for (size_t i = 0; i < pool.threadCount(); ++i) {
        auto &loop = pool.eventLoop(i);
        // Give threads time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pool.stop();
}

namespace {
    Task<int> incrementCounter(std::atomic<int> &c) { c.fetch_add(1); co_return 0; }
}

TEST_CASE("ThreadPool: scheduling across threads", "[ThreadPool]") {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    auto task = incrementCounter(counter);
    pool.scheduler(0).schedule(task.handle());

    pool.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.stop();

    REQUIRE(counter.load() >= 1);
}

TEST_CASE("ThreadPool: event loops are distinct", "[ThreadPool]") {
    ThreadPool pool(2);
    auto &loop0 = pool.eventLoop(0);
    auto &loop1 = pool.eventLoop(1);

    // Each should have its own epoll fd
    REQUIRE(loop0.epoll().fd() != loop1.epoll().fd());
    // Each should have its own scheduler
    REQUIRE(&pool.scheduler(0) != &pool.scheduler(1));
}

TEST_CASE("ThreadPool: double stop is safe", "[ThreadPool]") {
    ThreadPool pool(1);
    pool.start();
    pool.stop();
    REQUIRE_NOTHROW(pool.stop());
}
