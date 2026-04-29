#include <catch2/catch_test_macros.hpp>
#include "Core/InetAddress.h"
#include <optional>
#include <arpa/inet.h>

using namespace Core;

TEST_CASE("InetAddress: default construction", "[InetAddress]") {
    InetAddress addr;
    REQUIRE(addr.family() == AF_INET);
    REQUIRE(addr.ip() == "0.0.0.0");
    REQUIRE(addr.port() == 0);
}

TEST_CASE("InetAddress: port and IP constructor (IPv4)", "[InetAddress]") {
    InetAddress addr(8080, "127.0.0.1");
    REQUIRE(addr.family() == AF_INET);
    REQUIRE(addr.ip() == "127.0.0.1");
    REQUIRE(addr.port() == 8080);
    REQUIRE(addr.toString() == "127.0.0.1:8080");
}

TEST_CASE("InetAddress: IP and port constructor (IPv4)", "[InetAddress]") {
    InetAddress addr("192.168.1.1", 9090);
    REQUIRE(addr.family() == AF_INET);
    REQUIRE(addr.ip() == "192.168.1.1");
    REQUIRE(addr.port() == 9090);
}

TEST_CASE("InetAddress: localhost factory", "[InetAddress]") {
    auto addr = InetAddress::localhost(3000);
    REQUIRE(addr.ip() == "127.0.0.1");
    REQUIRE(addr.port() == 3000);
    REQUIRE(addr.family() == AF_INET);
}

TEST_CASE("InetAddress: any factory", "[InetAddress]") {
    auto addr = InetAddress::any(4000);
    REQUIRE(addr.ip() == "0.0.0.0");
    REQUIRE(addr.port() == 4000);
    REQUIRE(addr.family() == AF_INET);
}

TEST_CASE("InetAddress: resolve localhost returns valid address", "[InetAddress]") {
    auto addr = InetAddress::resolve("localhost", 8080);
    REQUIRE(addr.has_value());
    REQUIRE(addr->port() == 8080);
    // localhost should resolve to 127.0.0.1 or ::1
    REQUIRE((addr->ip() == "127.0.0.1" || addr->ip() == "::1"));
}

TEST_CASE("InetAddress: resolve invalid host returns nullopt", "[InetAddress]") {
    auto addr = InetAddress::resolve("invalid.host.that.does.not.exist.test", 8080);
    REQUIRE_FALSE(addr.has_value());
}

TEST_CASE("InetAddress: toString with IPv4", "[InetAddress]") {
    InetAddress addr("10.0.0.1", 1234);
    REQUIRE(addr.toString() == "10.0.0.1:1234");
}

TEST_CASE("InetAddress: equality operators", "[InetAddress]") {
    InetAddress a(8080, "127.0.0.1");
    InetAddress b(8080, "127.0.0.1");
    InetAddress c(9090, "127.0.0.1");
    InetAddress d(8080, "192.168.1.1");

    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a != d);
}

TEST_CASE("InetAddress: sockaddr_in constructor", "[InetAddress]") {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(5555);
    inet_pton(AF_INET, "10.20.30.40", &sin.sin_addr);

    InetAddress addr(sin);
    REQUIRE(addr.family() == AF_INET);
    REQUIRE(addr.port() == 5555);
    REQUIRE(addr.ip() == "10.20.30.40");
}

TEST_CASE("InetAddress: addr() and addrLen()", "[InetAddress]") {
    InetAddress addr(7777, "1.2.3.4");
    REQUIRE(addr.addr() != nullptr);
    REQUIRE(addr.addrLen() == sizeof(sockaddr_in));
}

TEST_CASE("InetAddress: IPv6 address", "[InetAddress]") {
    InetAddress addr(8080, "::1");
    REQUIRE(addr.family() == AF_INET6);
    REQUIRE(addr.ip() == "::1");
    REQUIRE(addr.port() == 8080);
}
