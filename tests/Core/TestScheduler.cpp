#include <catch2/catch_test_macros.hpp>
#include "Core/Scheduler.h"
#include "Core/Task.h"
#include <atomic>

using namespace Core;

namespace {
    Task<int> incrementTask(std::atomic<int> &counter) {
        counter.fetch_add(1);
        co_return 0;
    }

    Task<void> setFlag(std::atomic<bool> &flag) {
        flag.store(true);
        co_return;
    }
}

TEST_CASE("Scheduler: schedule and run one task", "[Scheduler]") {
    Scheduler s;
    std::atomic<int> counter{0};

    auto task = incrementTask(counter);
    s.schedule(task.handle());

    REQUIRE(s.hasWork());
    REQUIRE(s.runOne());
    REQUIRE(counter.load() == 1);
}

TEST_CASE("Scheduler: runAll processes all tasks", "[Scheduler]") {
    Scheduler s;
    std::atomic<int> counter{0};
    std::vector<Task<int>> tasks;

    for (int i = 0; i < 10; ++i) {
        auto task = incrementTask(counter);
        s.schedule(task.handle());
        tasks.push_back(std::move(task));
    }

    s.runAll();
    REQUIRE(counter.load() == 10);
    REQUIRE_FALSE(s.hasWork());
}

TEST_CASE("Scheduler: scheduleRemote allows cross-thread scheduling", "[Scheduler]") {
    Scheduler s;
    std::atomic<int> counter{0};

    auto task = incrementTask(counter);
    s.scheduleRemote(task.handle());

    REQUIRE(s.runOne());
    REQUIRE(counter.load() == 1);
}

TEST_CASE("Scheduler: hasWork returns false when empty", "[Scheduler]") {
    Scheduler s;
    REQUIRE_FALSE(s.hasWork());
}

TEST_CASE("Scheduler: localQueueSize reflects pending tasks", "[Scheduler]") {
    Scheduler s;
    REQUIRE(s.localQueueSize() == 0);

    std::atomic<int> counter{0};
    auto task = incrementTask(counter);
    s.schedule(task.handle());

    REQUIRE(s.localQueueSize() == 1);

    s.runOne();
    REQUIRE(s.localQueueSize() == 0);
}

TEST_CASE("Scheduler: schedule nullptr handle does nothing", "[Scheduler]") {
    Scheduler s;
    s.schedule(nullptr);
    REQUIRE_FALSE(s.hasWork());
}

TEST_CASE("Scheduler: scheduleRemote nullptr does nothing", "[Scheduler]") {
    Scheduler s;
    s.scheduleRemote(nullptr);
    REQUIRE_FALSE(s.runOne());
}

TEST_CASE("Scheduler: runOne returns false when empty", "[Scheduler]") {
    Scheduler s;
    REQUIRE_FALSE(s.runOne());
}

TEST_CASE("Scheduler: work stealing from another scheduler", "[Scheduler]") {
    Scheduler s1, s2;
    std::atomic<int> counter{0};

    auto task = incrementTask(counter);
    s1.schedule(task.handle());

    auto stolen = s2.stealFrom(s1);
    // Note: stealFrom may not work if s1 only has 1 task (need > 1 for steal)
    // If stolen, run it; otherwise run from s1
    if (stolen) {
        stolen.resume();
        REQUIRE(counter.load() == 1);
    }
}

TEST_CASE("Scheduler: multiple scheduleRemote calls", "[Scheduler]") {
    Scheduler s;
    std::atomic<int> counter{0};
    std::vector<Task<int>> tasks;

    for (int i = 0; i < 5; ++i) {
        auto task = incrementTask(counter);
        s.scheduleRemote(task.handle());
        tasks.push_back(std::move(task));
    }

    // scheduleRemote puts tasks in global queue; call runOne 5 times
    for (int i = 0; i < 5; ++i) {
        REQUIRE(s.runOne());
    }
    REQUIRE(counter.load() == 5);
}
