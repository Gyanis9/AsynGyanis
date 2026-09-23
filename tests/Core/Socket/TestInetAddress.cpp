// InetAddress 单元测试：构造、工厂方法、DNS 解析与比较运算

#include "Core/Socket/InetAddress.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace AsynGyanis::Core
{
    /**
     * @brief 默认构造得到的是 IPv4 通配地址 0.0.0.0:0，而不是未初始化的地址字段
     */
    TEST(InetAddress, DefaultConstructionIsUnspecifiedIpv4Address)
    {
        const InetAddress address;
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "0.0.0.0");
        EXPECT_EQ(address.port(), 0);
    }

    /**
     * @brief (port, ip) 参数序能正确解析 IPv4 文本，且 toString() 输出 ip:port
     */
    TEST(InetAddress, PortThenIpConstructorParsesIpv4)
    {
        const InetAddress address(8080, "127.0.0.1");
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "127.0.0.1");
        EXPECT_EQ(address.port(), 8080);
        EXPECT_EQ(address.toString(), "127.0.0.1:8080");
    }

    /**
     * @brief (ip, port) 参数序同样可用：两种重载的解析结果一致，不因参数顺序不同而错位
     */
    TEST(InetAddress, IpThenPortConstructorParsesIpv4)
    {
        const InetAddress address("192.168.1.1", 9090);
        EXPECT_EQ(address.family(), AF_INET);
        EXPECT_EQ(address.ip(), "192.168.1.1");
        EXPECT_EQ(address.port(), 9090);
    }

    /**
     * @brief localhost() 工厂固定给出回环地址 127.0.0.1：不走 DNS，不受本机 hosts 配置影响
     */
    TEST(InetAddress, LocalhostFactoryReturnsLoopbackAddress)
    {
        const InetAddress address = InetAddress::localhost(3000);
        EXPECT_EQ(address.ip(), "127.0.0.1");
        EXPECT_EQ(address.port(), 3000);
        EXPECT_EQ(address.family(), AF_INET);
    }

    /**
     * @brief any() 工厂给出 0.0.0.0 通配地址：用于监听本机所有网卡
     */
    TEST(InetAddress, AnyFactoryReturnsWildcardAddress)
    {
        const InetAddress address = InetAddress::any(4000);
        EXPECT_EQ(address.ip(), "0.0.0.0");
        EXPECT_EQ(address.port(), 4000);
        EXPECT_EQ(address.family(), AF_INET);
    }

    /**
     * @brief resolve() 能解析本机名，且只保证落到回环语义上（127.0.0.1 或 ::1 都算通过，不硬编 DNS 结果）
     */
    TEST(InetAddress, ResolveLocalhostReturnsValidAddress)
    {
        const std::optional<InetAddress> address = InetAddress::resolve("localhost", 8080);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->port(), 8080);
        // localhost 可能解析为 127.0.0.1（IPv4）或 ::1（IPv6）
        EXPECT_TRUE(address->ip() == "127.0.0.1" || address->ip() == "::1");
    }

    /**
     * @brief 拒绝面：空主机名解析失败返回 nullopt，而不是被当成通配地址 0.0.0.0 悄悄接受
     */
    TEST(InetAddress, ResolveEmptyHostReturnsNullopt)
    {
        const std::optional<InetAddress> address = InetAddress::resolve("", 8080);
        EXPECT_FALSE(address.has_value());
    }

    /**
     * @brief toString() 的输出格式固定为 ip:port（调用方与日志都依赖这一格式）
     */
    TEST(InetAddress, ToStringFormatsIpv4Address)
    {
        const InetAddress address("10.0.0.1", 1234);
        EXPECT_EQ(address.toString(), "10.0.0.1:1234");
    }

    /**
     * @brief 相等比较同时看 IP 与端口：只差端口或只差 IP 都不算相等
     */
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

    /**
     * @brief 从 sockaddr_in 直接构造时原样采纳原始地址：族、端口、地址三段都对得上
     */
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

    /**
     * @brief nativeAddress() 暴露底层 sockaddr 存储及对应长度，可直接交给系统调用
     */
    TEST(InetAddress, NativeAddressExposesSockaddrStorage)
    {
        const InetAddress address(7777, "1.2.3.4");
        EXPECT_NE(address.nativeAddress(), nullptr);
        EXPECT_EQ(address.nativeAddressLength(), sizeof(sockaddr_in));
    }

    /**
     * @brief IPv6 文本按 AF_INET6 解析并能原样读回：族判定不是写死的 AF_INET
     */
    TEST(InetAddress, Ipv6AddressRoundTripsThroughConstructor)
    {
        const InetAddress address(8080, "::1");
        EXPECT_EQ(address.family(), AF_INET6);
        EXPECT_EQ(address.ip(), "::1");
        EXPECT_EQ(address.port(), 8080);
    }

    /**
     * @brief 拒绝面：既非 IPv4 也非 IPv6 的文本必须抛非法参数异常并给出可操作文案，不留 0.0.0.0 半成品
     * @details 半成品会让「解析失败」与「确实是 0.0.0.0」无法区分；主机名同样被拒（解析主机名是 resolve() 的职责）
     */
    TEST(InetAddress, RejectsTextThatIsNeitherIpv4NorIpv6)
    {
        // 拒绝面：既不是点分十进制也不是冒号十六进制时必须抛错，而不是留下一个 0.0.0.0 的
        // 半成品对象——那会让「解析失败」与「确实是 0.0.0.0」无法区分
        try
        {
            const InetAddress address(8080, "not-an-ip");
            FAIL() << "非法 IP 文本不应构造成功，实际得到 " << address.toString();
        } catch (const Base::InvalidArgumentException &exception)
        {
            const std::string message = exception.what();
            // 文案要带上原始输入与可接受写法，调用方才能自己改对
            EXPECT_NE(message.find("not-an-ip"), std::string::npos) << message;
            EXPECT_NE(message.find("非法"), std::string::npos) << message;
            EXPECT_NE(message.find("resolve()"), std::string::npos) << message;
        }

        // 主机名同样不算合法 IP 文本（解析主机名是 resolve() 的职责）
        EXPECT_THROW(static_cast<void>(InetAddress("localhost", 80)), Base::InvalidArgumentException);
    }

    /**
     * @brief 拒绝面：含内嵌 NUL 的地址文本必须在进入 inet_pton 前失败，而不是被零终止语义静默截断成前半个合法 IP
     */
    TEST(InetAddress, RejectsEmbeddedNulInsteadOfSilentlyTruncating)
    {
        // 底层 inet_pton 按零终止语义解析，若不拦下内嵌 NUL，
        // "1.2.3.4\0evil" 会被当成 1.2.3.4 接受、后半段被静默丢弃——属于静默变形，
        // 因此必须在进入 C API 之前显式失败
        const std::string textWithNul("1.2.3.4\0evil", 12);
        EXPECT_THROW(static_cast<void>(InetAddress(8080, textWithNul)), Base::InvalidArgumentException);
    }

    /**
     * @brief 拒绝面：IPv4 的宽松变体写法一律拒收，不许被读成「同一个地址的另一种写法」
     * @details 走 inet_aton 那类宽松解析时 "0177.0.0.1" 会读成 127.0.0.1、"1.2.3.04" 会读成
     *          1.2.3.4（八进制/前导零），按地址文本做的放行与限额就能被换个写法绕过。本类用的是
     *          严格文法的 inet_pton：Windows 与 glibc 实测对这四种写法的判定完全一致。
     */
    TEST(InetAddress, RejectsAlternateEncodingsOfIpv4)
    {
        for (const char *ipText: {"0177.0.0.1", "1.2.3.04", "1.2.3.4 ", "256.1.1.1"})
        {
            EXPECT_THROW(static_cast<void>(InetAddress(80, ipText)), Base::InvalidArgumentException)
                << "该文本不该被当成合法 IPv4：" << ipText;
        }
    }

    /**
     * @brief IPv4 映射写法按 IPv6 收下，文本里带 ::ffff: 前缀而不是折回四段十进制
     * @details 双栈监听器上接到的 IPv4 对端就是这个形状（内核把 v4 地址放进 v6 的映射前缀），
     *          因此拿 ip() 当键的消费方不能指望它和纯 IPv4 字符串相等；两平台的 inet_ntop
     *          对该形状的输出一致（实测），故这里可以直接断言文本。
     */
    TEST(InetAddress, KeepsIpv4MappedIpv6TextAsIpv6Address)
    {
        const InetAddress address(80, "::ffff:1.2.3.4");
        EXPECT_EQ(address.family(), AF_INET6) << "映射前缀是 IPv6 地址，不该被折成 AF_INET";
        EXPECT_EQ(address.ip(), "::ffff:1.2.3.4");
        EXPECT_EQ(address.nativeAddressLength(), sizeof(sockaddr_in6));
    }

    /**
     * @brief 边界：IPv6 的作用域号既不出现在文本里，也不能从文本读回来
     * @details 内核在链路本地对端上会填 sin6_scope_id，而 inet_ntop 不输出「%接口」后缀、
     *          inet_pton 也不接受它（两平台实测一致）。后果：两块网卡上同名的 fe80:: 对端在
     *          **文本面**撞成同一个串，按 ip() 做键的限额会把它们当成同一个来源；而对象级比较
     *          （operator==）比的是原始字节，作用域号不同仍然区分得开。本用例钉住这两条现状。
     */
    TEST(InetAddress, Ipv6ScopeIdSurvivesInRawAddressButNotInText)
    {
        sockaddr_in6 rawAddress{};
        rawAddress.sin6_family   = AF_INET6;
        rawAddress.sin6_port     = htons(1234);
        rawAddress.sin6_scope_id = 3;
        ASSERT_GT(inet_pton(AF_INET6, "fe80::1", &rawAddress.sin6_addr), 0);

        const InetAddress withScope(rawAddress);
        EXPECT_EQ(withScope.ip(), "fe80::1") << "文本里不该混进作用域号：那会让 toString() 不再是可解析的写法";
        EXPECT_EQ(withScope.toString(), "[fe80::1]:1234");

        // 只有作用域号不同的两个对端（两块网卡上的同名链路本地地址）必须仍可区分
        sockaddr_in6 otherRawAddress = rawAddress;
        otherRawAddress.sin6_scope_id = 7;
        EXPECT_NE(withScope, InetAddress(otherRawAddress));

        // 反向不成立：带「%接口」后缀的文本被拒，因此 ip() 的输出与该文本不是同一条通道
        EXPECT_THROW(static_cast<void>(InetAddress(1234, "fe80::1%3")), Base::InvalidArgumentException);
    }

    /**
     * @brief 拒绝面：(sockaddr_storage, 长度) 这对参数里长度越界必须拒绝，而不是按容量截断照抄
     * @details 长度由调用方给出（多半来自内核回填的 socklen_t），直接照着 memcpy 会越界写；
     *          静默截断更糟——地址被悄悄改短，比对与限额都跟着错。
     */
    TEST(InetAddress, RejectsRawStorageLengthOutsideItsCapacity)
    {
        sockaddr_storage storage{};
        storage.ss_family = AF_INET;

        EXPECT_THROW(static_cast<void>(InetAddress(storage, 0)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(InetAddress(storage, static_cast<socklen_t>(sizeof(sockaddr_storage) + 1))),
                     Base::InvalidArgumentException);
        // 恰好放得下的两种真实长度都应当被接受：IPv4 与 IPv6 的地址长度不同，不能只认一种
        EXPECT_NO_THROW(static_cast<void>(InetAddress(storage, sizeof(sockaddr_in))));
        EXPECT_NO_THROW(static_cast<void>(InetAddress(storage, sizeof(sockaddr_storage))));
    }
} // namespace AsynGyanis::Core
