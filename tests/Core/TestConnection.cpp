#include <catch2/catch_test_macros.hpp>
#include "Core/Connection.h"
#include "Core/EventLoop.h"
#include "Core/AsyncSocket.h"
#include "Core/InetAddress.h"

using namespace Core;

TEST_CASE("Connection: construction", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1); // dummy fd
    Connection conn(std::move(sock));

    REQUIRE(conn.isAlive());
    REQUIRE(conn.socket().fd() == -1);
}

TEST_CASE("Connection: close sets not alive", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1);
    Connection conn(std::move(sock));

    conn.close();
    REQUIRE_FALSE(conn.isAlive());
}

TEST_CASE("Connection: cancelable requestStop after close", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1);
    Connection conn(std::move(sock));

    conn.close();
    REQUIRE(conn.cancelable().isStopRequested());
}

TEST_CASE("Connection: base start returns immediately", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1);
    Connection conn(std::move(sock));

    auto task = conn.start();
    task.handle().resume();
    REQUIRE(task.isReady());
}

TEST_CASE("Connection: move construction", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1);
    Connection conn1(std::move(sock));

    conn1.close();
    Connection conn2(std::move(conn1));
    REQUIRE_FALSE(conn2.isAlive());
}

TEST_CASE("Connection: move assignment", "[Connection]") {
    EventLoop loop;
    auto sock1 = AsyncSocket(loop, -1);
    auto sock2 = AsyncSocket(loop, -1);

    Connection conn1(std::move(sock1));
    Connection conn2(std::move(sock2));

    conn1.close();
    conn2 = std::move(conn1);
    REQUIRE_FALSE(conn2.isAlive());
}

TEST_CASE("Connection: cancelable propagates", "[Connection]") {
    EventLoop loop;
    AsyncSocket sock(loop, -1);
    Connection conn(std::move(sock));

    auto &cancelable = conn.cancelable();
    REQUIRE_FALSE(cancelable.isStopRequested());

    cancelable.requestStop();
    REQUIRE(cancelable.isStopRequested());
}
