#include <catch2/catch_test_macros.hpp>
#include "Core/Connection.h"
#include "Core/EventLoop.h"
#include "Core/AsyncSocket.h"
#include "Core/InetAddress.h"

using namespace Core;

TEST_CASE("Connection: construction", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1); // dummy file descriptor
    Connection connection(std::move(asyncSocket));

    REQUIRE(connection.isAlive());
    REQUIRE(connection.socket().fileDescriptor() == -1);
}

TEST_CASE("Connection: close sets not alive", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1);
    Connection connection(std::move(asyncSocket));

    connection.close();
    REQUIRE_FALSE(connection.isAlive());
}

TEST_CASE("Connection: cancelable requestStop after close", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1);
    Connection connection(std::move(asyncSocket));

    connection.close();
    REQUIRE(connection.cancelable().isStopRequested());
}

TEST_CASE("Connection: base start returns immediately", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1);
    Connection connection(std::move(asyncSocket));

    auto task = connection.start();
    task.handle().resume();
    REQUIRE(task.isReady());
}

TEST_CASE("Connection: move construction", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1);
    Connection connection1(std::move(asyncSocket));

    connection1.close();
    Connection connection2(std::move(connection1));
    REQUIRE_FALSE(connection2.isAlive());
}

TEST_CASE("Connection: move assignment", "[Connection]") {
    EventLoop loop;
    auto asyncSocket1 = AsyncSocket(loop, -1);
    auto asyncSocket2 = AsyncSocket(loop, -1);

    Connection connection1(std::move(asyncSocket1));
    Connection connection2(std::move(asyncSocket2));

    connection1.close();
    connection2 = std::move(connection1);
    REQUIRE_FALSE(connection2.isAlive());
}

TEST_CASE("Connection: cancelable propagates", "[Connection]") {
    EventLoop loop;
    AsyncSocket asyncSocket(loop, -1);
    Connection connection(std::move(asyncSocket));

    auto &cancelable = connection.cancelable();
    REQUIRE_FALSE(cancelable.isStopRequested());

    cancelable.requestStop();
    REQUIRE(cancelable.isStopRequested());
}
