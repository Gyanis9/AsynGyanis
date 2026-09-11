/**
 * @file TestInetAddress.cpp
 * @brief InetAddress 单元测试：构造、工厂方法、DNS 解析与比较运算
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/InetAddress.h"

#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <optional>

namespace AsynGyanis::Core
{
    TEST(InetAddress, DefaultConstructionIsUnspecifiedIpv4Address)
    {
        const InetAddress address;
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "0.0.0.0");
        EXPECT_EQ(address.port(), 0);
    }

    TEST(InetAddress, PortThenIpConstructorParsesIpv4)
    {
        const InetAddress address(8080, "127.0.0.1");
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "127.0.0.1");
        EXPECT_EQ(address.port(), 8080);
        EXPECT_EQ(address.toString(), "127.0.0.1:8080");
    }

    TEST(InetAddress, IpThenPortConstructorParsesIpv4)
    {
        const InetAddress address("192.168.1.1", 9090);
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "192.168.1.1");
        EXPECT_EQ(address.port(), 9090);
    }

    TEST(InetAddress, LocalhostFactoryReturnsLoopbackAddress)
    {
        const InetAddress address = InetAddress::localhost(3000);
        EXPECT_EQ(address.ip(), "127.0.0.1");
        EXPECT_EQ(address.port(), 3000);
        EXPECT_EQ(address.family(), AF_INET);
    }

    TEST(InetAddress, AnyFactoryReturnsWildcardAddress)
    {
        const InetAddress address = InetAddress::any(4000);
        EXPECT_EQ(address.ip(), "0.0.0.0");
        EXPECT_EQ(address.port(), 4000);
        EXPECT_EQ(address.family(), AF_INET);
    }

    TEST(InetAddress, ResolveLocalhostReturnsValidAddress)
    {
        const std::optional<InetAddress> address = InetAddress::resolve("localhost", 8080);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->port(), 8080);
        // localhost 可能解析为 127.0.0.1（IPv4）或 ::1（IPv6）
        EXPECT_TRUE(address->ip() == "127.0.0.1" || address->ip() == "::1");
    }

    TEST(InetAddress, ResolveEmptyHostReturnsNullopt)
    {
        const std::optional<InetAddress> address = InetAddress::resolve("", 8080);
        EXPECT_FALSE(address.has_value());
    }

    TEST(InetAddress, ToStringFormatsIpv4Address)
    {
        const InetAddress address("10.0.0.1", 1234);
        EXPECT_EQ(address.toString(), "10.0.0.1:1234");
    }

    TEST(InetAddress, EqualityOperatorsCompareIpAndPort)
    {
        const InetAddress sameFirst(8080, "127.0.0.1");
        const InetAddress sameSecond(8080, "127.0.0.1");
        const InetAddress differentPort(9090, "127.0.0.1");
        const InetAddress differentIp(8080, "192.168.1.1");

        EXPECT_TRUE(sameFirst == sameSecond);
        EXPECT_TRUE(sameFirst != differentPort);
        EXPECT_TRUE(sameFirst != differentIp);
    }

    TEST(InetAddress, SockaddrInConstructorAdoptsRawAddress)
    {
        sockaddr_in rawAddress{};
        rawAddress.sin_family = AF_INET;
        rawAddress.sin_port   = htons(5555);
        inet_pton(AF_INET, "10.20.30.40", &rawAddress.sin_addr);

        const InetAddress address(rawAddress);
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.port(), 5555);
        EXPECT_EQ(address.ip(), "10.20.30.40");
    }

    TEST(InetAddress, NativeAddressExposesSockaddrStorage)
    {
        const InetAddress address(7777, "1.2.3.4");
        EXPECT_NE(address.nativeAddress(), nullptr);
        EXPECT_EQ(address.nativeAddressLength(), sizeof(sockaddr_in));
    }

    TEST(InetAddress, Ipv6AddressRoundTripsThroughConstructor)
    {
        const InetAddress address(8080, "::1");
        EXPECT_EQ(address.family(), AF_INET6);
        EXPECT_EQ(address.ip(), "::1");
        EXPECT_EQ(address.port(), 8080);
    }
}
