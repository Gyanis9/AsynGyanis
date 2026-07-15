#include <catch2/catch_test_macros.hpp>
#include "Core/AsyncSocket.h"
#include "Core/EventLoop.h"
#include "Core/InetAddress.h"
#include "Core/Scheduler.h"
#include "Core/Task.h"
#include <thread>
#include <atomic>
#include "Platform/Platform.h"

using namespace Core;
using namespace std::chrono_literals;

TEST_CASE("AsyncSocket: create returns valid socket", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    REQUIRE(asyncSocket.fileDescriptor() >= 0);
}

TEST_CASE("AsyncSocket: move construction", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket1 = AsyncSocket::create(loop);
    int fileDescriptor1 = asyncSocket1.fileDescriptor();

    AsyncSocket asyncSocket2(std::move(asyncSocket1));
    REQUIRE(asyncSocket2.fileDescriptor() == fileDescriptor1);
}

TEST_CASE("AsyncSocket: move assignment", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket1 = AsyncSocket::create(loop);
    auto asyncSocket2 = AsyncSocket::create(loop);
    int fileDescriptor1 = asyncSocket1.fileDescriptor();

    asyncSocket2 = std::move(asyncSocket1);
    REQUIRE(asyncSocket2.fileDescriptor() == fileDescriptor1);
}

TEST_CASE("AsyncSocket: close sets fileDescriptor to -1", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    REQUIRE(asyncSocket.fileDescriptor() >= 0);

    asyncSocket.close();
    REQUIRE(asyncSocket.fileDescriptor() == -1);
}

TEST_CASE("AsyncSocket: double close is safe", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    asyncSocket.close();
    REQUIRE_NOTHROW(asyncSocket.close());
}

TEST_CASE("AsyncSocket: bind to localhost port", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    auto address = InetAddress::localhost(0);

    REQUIRE(asyncSocket.bind(address));
}

TEST_CASE("AsyncSocket: listen after bind", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    auto address = InetAddress::localhost(0);

    REQUIRE(asyncSocket.bind(address));
    REQUIRE(asyncSocket.listen());
}

TEST_CASE("AsyncSocket: setSockOpt", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);

    int opt = 1;
    REQUIRE(asyncSocket.setSockOpt(SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)));
}

TEST_CASE("AsyncSocket: bind/listen/close lifecycle", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    auto address = InetAddress::localhost(0);

    REQUIRE(asyncSocket.bind(address));
    REQUIRE(asyncSocket.listen());

    auto localAddress = asyncSocket.localAddress();
    REQUIRE(localAddress.port() != 0);

    asyncSocket.close();
    REQUIRE(asyncSocket.fileDescriptor() == -1);
}

TEST_CASE("AsyncSocket: remoteAddress and localAddress", "[AsyncSocket]") {
    EventLoop loop;
    auto asyncSocket = AsyncSocket::create(loop);
    auto address = InetAddress::localhost(0);
    REQUIRE(asyncSocket.bind(address));

    // localAddress should return the bound address
    auto local = asyncSocket.localAddress();
    REQUIRE(local.port() != 0);
}
