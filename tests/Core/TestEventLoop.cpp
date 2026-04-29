#include <catch2/catch_test_macros.hpp>
#include "Core/EventLoop.h"
#include "Core/Task.h"
#include "Core/Scheduler.h"
#include <thread>
#include <atomic>

using namespace Core;

TEST_CASE("EventLoop: construction", "[EventLoop]") {
    EventLoop loop;
    REQUIRE(loop.epoll().fd() >= 0);
    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("EventLoop: run and stop", "[EventLoop]") {
    EventLoop loop;
    std::atomic<bool> started{false};
    std::thread worker([&]() { started.store(true); loop.run(); });
    while (!started.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    REQUIRE(loop.isRunning());
    loop.stop();
    worker.join();
    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("EventLoop: scheduler is accessible", "[EventLoop]") {
    EventLoop loop;
    REQUIRE_FALSE(loop.scheduler().hasWork());
}

TEST_CASE("EventLoop: wake interrupts epoll_wait", "[EventLoop]") {
    EventLoop loop;
    std::atomic<bool> loopStarted{false};
    std::thread worker([&]() { loopStarted.store(true); loop.run(); });
    while (!loopStarted.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    loop.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loop.stop();
    worker.join();
}

namespace {
    Task<int> setValue(std::atomic<int> &v) { v.store(42); co_return 0; }
}

TEST_CASE("EventLoop: scheduling coroutine via scheduler executes it", "[EventLoop]") {
    EventLoop loop;
    std::atomic<int> value{0};

    auto task = setValue(value);
    REQUIRE(task.handle() != nullptr);
    loop.scheduler().schedule(task.handle());

    bool ran = loop.scheduler().runOne();
    REQUIRE(ran);
    REQUIRE(value.load() == 42);
}

TEST_CASE("EventLoop: multiple coroutines in order", "[EventLoop]") {
    EventLoop loop;
    int sequence = 0;
    int results[3]{};
    std::vector<Task<void>> tasks;

    auto t0 = [&]() -> Task<void> { sequence++; results[0] = 1; co_return; }();
    auto t1 = [&]() -> Task<void> { sequence++; results[1] = 2; co_return; }();
    auto t2 = [&]() -> Task<void> { sequence++; results[2] = 3; co_return; }();

    loop.scheduler().schedule(t0.handle());
    loop.scheduler().schedule(t1.handle());
    loop.scheduler().schedule(t2.handle());
    tasks.push_back(std::move(t0));
    tasks.push_back(std::move(t1));
    tasks.push_back(std::move(t2));

    loop.scheduler().runAll();
    REQUIRE(results[0] == 1);
    REQUIRE(results[1] == 2);
    REQUIRE(results[2] == 3);
}

TEST_CASE("EventLoop: stop from outside thread", "[EventLoop]") {
    EventLoop loop;
    std::thread worker([&]() { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(loop.isRunning());
    loop.stop();
    worker.join();
    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("EventLoop: double stop is safe", "[EventLoop]") {
    EventLoop loop;
    std::thread worker([&]() { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    loop.stop();
    loop.stop();
    worker.join();
}
