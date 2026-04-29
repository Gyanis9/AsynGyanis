#include <catch2/catch_test_macros.hpp>
#include "Core/Task.h"
#include "Core/Scheduler.h"

using namespace Core;

namespace {
    Task<int> simpleValueTask() {
        co_return 42;
    }

    Task<void> simpleVoidTask() {
        co_return;
    }

    Task<int> throwingTask() {
        throw std::runtime_error("test error");
        co_return 0;
    }

    Task<int> chainedTask(int x) {
        int val = co_await simpleValueTask();
        co_return val + x;
    }
}

TEST_CASE("Task: simple value return", "[Task]") {
    auto task = simpleValueTask();
    REQUIRE_FALSE(task.isReady());

    // Resume the coroutine
    task.handle().resume();
    REQUIRE(task.isReady());

    int result = task.handle().promise().result();
    REQUIRE(result == 42);
}

TEST_CASE("Task: void task returns cleanly", "[Task]") {
    auto task = simpleVoidTask();
    REQUIRE_FALSE(task.isReady());

    task.handle().resume();
    REQUIRE(task.isReady());

    // Should not throw
    REQUIRE_NOTHROW(task.handle().promise().result());
}

TEST_CASE("Task: exception handling", "[Task]") {
    auto task = throwingTask();
    task.handle().resume();
    REQUIRE(task.isReady());
    REQUIRE_THROWS_AS(task.handle().promise().result(), std::runtime_error);
}

TEST_CASE("Task: move construction", "[Task]") {
    auto task1 = simpleValueTask();
    auto handle = task1.handle();

    Task<int> task2(std::move(task1));
    REQUIRE(task2.handle() == handle);
}

TEST_CASE("Task: move assignment", "[Task]") {
    auto task1 = simpleValueTask();
    auto task2 = simpleValueTask();

    auto h1 = task1.handle();
    task2 = std::move(task1);
    REQUIRE(task2.handle() == h1);
}

TEST_CASE("Task: isReady returns false before resume", "[Task]") {
    auto task = simpleValueTask();
    REQUIRE_FALSE(task.isReady());
}

TEST_CASE("Task: isReady returns true after completion", "[Task]") {
    auto task = simpleValueTask();
    task.handle().resume();
    REQUIRE(task.isReady());
}

TEST_CASE("Task: await_resume returns value for Task<int>", "[Task]") {
    auto task = simpleValueTask();
    task.handle().resume();

    int val = task.await_resume();
    REQUIRE(val == 42);
}

TEST_CASE("Task: await_ready reflects done state", "[Task]") {
    auto task = simpleValueTask();
    REQUIRE_FALSE(task.await_ready());
    task.handle().resume();
    REQUIRE(task.await_ready());
}

TEST_CASE("Task: moved-from task has null handle", "[Task]") {
    auto task1 = simpleValueTask();
    Task<int> task2(std::move(task1));
    REQUIRE(task1.handle() == nullptr);
}
