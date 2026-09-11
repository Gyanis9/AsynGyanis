#include <catch2/catch_test_macros.hpp>
#include "Core/ConnectionManager.h"
#include "Core/Connection.h"
#include "Core/EventLoop.h"
#include "Core/AsyncSocket.h"
#include <thread>

using namespace Core;

namespace {
    std::shared_ptr<Connection> makeDummyConn() {
        static EventLoop loop;
        return std::make_shared<Connection>(AsyncSocket(loop, -1));
    }
}

TEST_CASE("ConnectionManager: add and activeCount", "[ConnectionManager]") {
    ConnectionManager mgr;
    REQUIRE(mgr.activeCount() == 0);

    auto conn = makeDummyConn();
    mgr.add(conn);
    REQUIRE(mgr.activeCount() == 1);
}

TEST_CASE("ConnectionManager: remove by pointer", "[ConnectionManager]") {
    ConnectionManager mgr;
    auto conn = makeDummyConn();
    mgr.add(conn);
    REQUIRE(mgr.activeCount() == 1);

    mgr.remove(conn.get());
    REQUIRE(mgr.activeCount() == 0);
}

TEST_CASE("ConnectionManager: remove null pointer does nothing", "[ConnectionManager]") {
    ConnectionManager mgr;
    REQUIRE_NOTHROW(mgr.remove(nullptr));
}

TEST_CASE("ConnectionManager: add null pointer does nothing", "[ConnectionManager]") {
    ConnectionManager mgr;
    mgr.add(nullptr);
    REQUIRE(mgr.activeCount() == 0);
}

TEST_CASE("ConnectionManager: multiple connections", "[ConnectionManager]") {
    ConnectionManager mgr;
    auto c1 = makeDummyConn();
    auto c2 = makeDummyConn();
    auto c3 = makeDummyConn();

    mgr.add(c1);
    mgr.add(c2);
    mgr.add(c3);
    REQUIRE(mgr.activeCount() == 3);

    mgr.remove(c2.get());
    REQUIRE(mgr.activeCount() == 2);
}

TEST_CASE("ConnectionManager: shutdown requests stop on all", "[ConnectionManager]") {
    ConnectionManager mgr;
    auto c1 = makeDummyConn();
    auto c2 = makeDummyConn();

    mgr.add(c1);
    mgr.add(c2);

    mgr.shutdown();
    REQUIRE(c1->cancelable().isStopRequested());
    REQUIRE(c2->cancelable().isStopRequested());
}

TEST_CASE("ConnectionManager: waitAll blocks until empty", "[ConnectionManager]") {
    ConnectionManager mgr;
    auto conn = makeDummyConn();
    mgr.add(conn);

    std::atomic<bool> done{false};
    std::thread t([&]() {
        mgr.waitAll();
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(done.load());

    mgr.remove(conn.get());
    t.join();
    REQUIRE(done.load());
}

TEST_CASE("ConnectionManager: remove non-existent pointer is no-op", "[ConnectionManager]") {
    ConnectionManager mgr;
    auto conn = makeDummyConn();
    mgr.add(conn);

    auto other = makeDummyConn();
    mgr.remove(other.get()); // different pointer
    REQUIRE(mgr.activeCount() == 1);
}
