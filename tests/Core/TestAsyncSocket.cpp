#include <catch2/catch_test_macros.hpp>
#include "Core/AsyncSocket.h"
#include "Core/EventLoop.h"
#include "Core/InetAddress.h"
#include "Core/Scheduler.h"
#include "Core/Task.h"
#include <thread>
#include <atomic>
#include <sys/socket.h>

using namespace Core;
using namespace std::chrono_literals;

TEST_CASE("AsyncSocket: create returns valid socket", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    REQUIRE(sock.fd() >= 0);
}

TEST_CASE("AsyncSocket: move construction", "[AsyncSocket]") {
    EventLoop loop;
    auto sock1 = AsyncSocket::create(loop);
    int fd = sock1.fd();

    AsyncSocket sock2(std::move(sock1));
    REQUIRE(sock2.fd() == fd);
}

TEST_CASE("AsyncSocket: move assignment", "[AsyncSocket]") {
    EventLoop loop;
    auto sock1 = AsyncSocket::create(loop);
    auto sock2 = AsyncSocket::create(loop);
    int fd1 = sock1.fd();

    sock2 = std::move(sock1);
    REQUIRE(sock2.fd() == fd1);
}

TEST_CASE("AsyncSocket: close sets fd to -1", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    REQUIRE(sock.fd() >= 0);

    sock.close();
    REQUIRE(sock.fd() == -1);
}

TEST_CASE("AsyncSocket: double close is safe", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    sock.close();
    REQUIRE_NOTHROW(sock.close());
}

TEST_CASE("AsyncSocket: bind to localhost port", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    auto addr = InetAddress::localhost(0);

    REQUIRE(sock.bind(addr));
}

TEST_CASE("AsyncSocket: listen after bind", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    auto addr = InetAddress::localhost(0);

    REQUIRE(sock.bind(addr));
    REQUIRE(sock.listen());
}

TEST_CASE("AsyncSocket: setSockOpt", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);

    int opt = 1;
    REQUIRE(sock.setSockOpt(SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)));
}

TEST_CASE("AsyncSocket: bind/listen/close lifecycle", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    auto addr = InetAddress::localhost(0);

    REQUIRE(sock.bind(addr));
    REQUIRE(sock.listen());

    auto localAddr = sock.localAddress();
    REQUIRE(localAddr.port() != 0);

    sock.close();
    REQUIRE(sock.fd() == -1);
}

TEST_CASE("AsyncSocket: remoteAddress and localAddress", "[AsyncSocket]") {
    EventLoop loop;
    auto sock = AsyncSocket::create(loop);
    auto addr = InetAddress::localhost(0);
    REQUIRE(sock.bind(addr));

    // localAddress should return the bound address
    auto local = sock.localAddress();
    REQUIRE(local.port() != 0);
}
