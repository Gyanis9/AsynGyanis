#include <catch2/catch_test_macros.hpp>
#include "Core/InetAddress.h"
#include <optional>
#include "Platform/Platform.h"

using namespace Core;

TEST_CASE("InetAddress: default construction", "[InetAddress]") {
    InetAddress address;
    REQUIRE(address.family() == AF_INET);
    REQUIRE(address.ip() == "0.0.0.0");
    REQUIRE(address.port() == 0);
}

TEST_CASE("InetAddress: port and IP constructor (IPv4)", "[InetAddress]") {
    InetAddress address(8080, "127.0.0.1");
    REQUIRE(address.family() == AF_INET);
    REQUIRE(address.ip() == "127.0.0.1");
    REQUIRE(address.port() == 8080);
    REQUIRE(address.toString() == "127.0.0.1:8080");
}

TEST_CASE("InetAddress: IP and port constructor (IPv4)", "[InetAddress]") {
    InetAddress address("192.168.1.1", 9090);
    REQUIRE(address.family() == AF_INET);
    REQUIRE(address.ip() == "192.168.1.1");
    REQUIRE(address.port() == 9090);
}

TEST_CASE("InetAddress: localhost factory", "[InetAddress]") {
    auto address = InetAddress::localhost(3000);
    REQUIRE(address.ip() == "127.0.0.1");
    REQUIRE(address.port() == 3000);
    REQUIRE(address.family() == AF_INET);
}

TEST_CASE("InetAddress: any factory", "[InetAddress]") {
    auto address = InetAddress::any(4000);
    REQUIRE(address.ip() == "0.0.0.0");
    REQUIRE(address.port() == 4000);
    REQUIRE(address.family() == AF_INET);
}

TEST_CASE("InetAddress: resolve localhost returns valid address", "[InetAddress]") {
    auto address = InetAddress::resolve("localhost", 8080);
    REQUIRE(address.has_value());
    REQUIRE(address->port() == 8080);
    // localhost should resolve to 127.0.0.1 or ::1
    REQUIRE((address->ip() == "127.0.0.1" || address->ip() == "::1"));
}

TEST_CASE("InetAddress: resolve invalid host returns nullopt", "[InetAddress]") {
    auto address = InetAddress::resolve("", 8080);
    REQUIRE_FALSE(address.has_value());
}

TEST_CASE("InetAddress: toString with IPv4", "[InetAddress]") {
    InetAddress address("10.0.0.1", 1234);
    REQUIRE(address.toString() == "10.0.0.1:1234");
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

    InetAddress address(sin);
    REQUIRE(address.family() == AF_INET);
    REQUIRE(address.port() == 5555);
    REQUIRE(address.ip() == "10.20.30.40");
}

TEST_CASE("InetAddress: nativeAddress() and nativeAddressLength()", "[InetAddress]") {
    InetAddress address(7777, "1.2.3.4");
    REQUIRE(address.nativeAddress() != nullptr);
    REQUIRE(address.nativeAddressLength() == sizeof(sockaddr_in));
}

TEST_CASE("InetAddress: IPv6 address", "[InetAddress]") {
    InetAddress address(8080, "::1");
    REQUIRE(address.family() == AF_INET6);
    REQUIRE(address.ip() == "::1");
    REQUIRE(address.port() == 8080);
}
