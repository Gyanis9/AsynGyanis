// InetAddress 单元测试：构造、工厂方法、DNS 解析与比较运算

#include "Core/Socket/InetAddress.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Platform/IO/NetworkInterface.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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
     * @brief platformAddress() 交出的「存储 + 长度」必须成对，且换回来还是同一个地址
     * @details 这层换算的失败形状是静默的：长度少给一字节，内核就把半个地址当成完整地址用，
     *          绑定与比对都看不出错，因此互指认要按 IPv4、IPv6 各钉一次。
     */
    TEST(InetAddress, PlatformAddressCarriesTheSameStorageAndLength)
    {
        const InetAddress             addressV4(7777, "1.2.3.4");
        const Platform::SocketAddress platformAddressV4 = addressV4.platformAddress();
        EXPECT_EQ(platformAddressV4.length, sizeof(sockaddr_in));
        EXPECT_EQ(InetAddress(platformAddressV4.storage, platformAddressV4.length), addressV4) << "换算过去再换算回来，地址不是同一个";

        // 作用域号是 IPv6 地址的一部分：漏在换算之外，两块网卡上的同名链路本地地址就会撞成同一个键
        const InetAddress             addressV6(8080, "fe80::1%3");
        const Platform::SocketAddress platformAddressV6 = addressV6.platformAddress();
        EXPECT_EQ(platformAddressV6.length, sizeof(sockaddr_in6));
        EXPECT_EQ(InetAddress(platformAddressV6.storage, platformAddressV6.length), addressV6) << "IPv6 的作用域号在换算里丢了";

        // 默认构造的 0.0.0.0:0 也带着成对的长度：长度为 0 的地址交给平台层会被判成
        // 「地址没给」，而不是「绑到任意地址」，这两种含义不能混
        EXPECT_EQ(InetAddress().platformAddress().length, sizeof(sockaddr_in));
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
            EXPECT_THROW(static_cast<void>(InetAddress(80, ipText)), Base::InvalidArgumentException) << "该文本不该被当成合法 IPv4：" << ipText;
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
     * @brief IPv6 的作用域号在原始地址与文本两个面都留得住：两块网卡上的同名链路本地地址不撞成一条
     * @details 内核在链路本地对端上会填 sin6_scope_id，而 inet_ntop 不输出「%接口」后缀（两平台实测
     *          一致），故本层自己补上数字形式。旧断言「ip() 里不该混进作用域号」换成新语义：
     *          ip()/toString() 带 %<接口号>，且这份文本能被本层原样读回——依据是 RFC 4007 §11 的
     *          文本形状（数字作用域标识是合法写法），以及「按地址文本做键」的消费方必须分得开
     *          两块网卡上的同名地址（否则按来源限额会把两个来源并成一个）。
     */
    TEST(InetAddress, Ipv6ScopeIdSurvivesInBothRawAddressAndText)
    {
        sockaddr_in6 rawAddress{};
        rawAddress.sin6_family   = AF_INET6;
        rawAddress.sin6_port     = htons(1234);
        rawAddress.sin6_scope_id = 3;
        ASSERT_GT(inet_pton(AF_INET6, "fe80::1", &rawAddress.sin6_addr), 0);

        const InetAddress withScope(rawAddress);
        EXPECT_EQ(withScope.scopeId(), 3U);
        EXPECT_EQ(withScope.ip(), "fe80::1%3");
        EXPECT_EQ(withScope.toString(), "[fe80::1%3]:1234");

        // 输出的数字形式与构造用的文本是同一条通道：ip() 的产物要能被 parseLiteral 原样读回
        const std::optional<InetAddress> reparsed = InetAddress::parseLiteral(withScope.ip(), 1234);
        ASSERT_TRUE(reparsed.has_value());
        EXPECT_EQ(*reparsed, withScope);

        // 只有作用域号不同的两个对端（两块网卡上的同名链路本地地址）必须仍可区分
        sockaddr_in6 otherRawAddress  = rawAddress;
        otherRawAddress.sin6_scope_id = 7;
        EXPECT_NE(withScope, InetAddress(otherRawAddress));
        EXPECT_NE(InetAddress(otherRawAddress).ip(), withScope.ip()) << "文本面也必须分得开";

        // 未带作用域时不许多出后缀：否则所有既有 IPv6 文本断言与日志形状都会跟着变
        EXPECT_EQ(InetAddress(1234, "fe80::1").ip(), "fe80::1");
        EXPECT_EQ(InetAddress(1234, "fe80::1").scopeId(), 0U);
    }

    /**
     * @brief 「%数字」写法落进 sockaddr_in6::sin6_scope_id：那是 bind/connect 唯一认的位置
     * @details 只断言 ip() 的文本不够——文本对了但原始地址没填，内核照样把连接派到错的网卡上。
     *          作用域号不校验本机是否存在（校验要枚举全部网卡），故 7 号这种不存在的接口也照样收下，
     *          由真正的系统调用以 EADDRNOTAVAIL/ENODEV 明确报出来。
     */
    TEST(InetAddress, NumericZoneSuffixBecomesScopeIdInNativeAddress)
    {
        const InetAddress address(8080, "fe80::1%7");
        EXPECT_EQ(address.family(), AF_INET6);
        EXPECT_EQ(address.scopeId(), 7U);
        EXPECT_EQ(address.ip(), "fe80::1%7");
        EXPECT_EQ(address.port(), 8080);

        const auto *native = reinterpret_cast<const sockaddr_in6 *>(address.nativeAddress());
        EXPECT_EQ(native->sin6_scope_id, 7U) << "作用域号必须落在内核读的那个字段上";
        EXPECT_EQ(native->sin6_port, htons(8080));

        // %0 = RFC 4007 的「无作用域」：收下，但文本里不许多出「%0」后缀
        const InetAddress zeroScope(8080, "fe80::1%0");
        EXPECT_EQ(zeroScope.scopeId(), 0U);
        EXPECT_EQ(zeroScope.ip(), "fe80::1");

        // 前导零与超短写法都是同一个接口号，不该被判成别的接口
        EXPECT_EQ(InetAddress(8080, "fe80::1%007").scopeId(), 7U);

        // 全局单播地址带作用域也照收：本层不替调用方判定「这个地址该不该有作用域」
        EXPECT_EQ(InetAddress(80, "2001:db8::1%2").scopeId(), 2U);

        // 两个参数序的构造函数共用同一份判据
        EXPECT_EQ(InetAddress("fe80::1%2", 2).scopeId(), 2U);
    }

    /**
     * @brief 「%接口名」写法换算成本机接口索引，且输出文本用数字形式
     * @details 配置文件里写的多半是 `fe80::1%eth0` 这类人名，而内核只认数字，换算必须在这一层做完。
     *          接口名是本机状态，用例不硬编某个名字：先用 if_indextoname 取一个真实存在的名字，
     *          再要求本层给出同一个索引；取不到任何接口时（无网卡的容器）按跳过处理。
     */
    TEST(InetAddress, InterfaceNameZoneResolvesToItsIndex)
    {
        std::string interfaceName;
        unsigned    interfaceIndex = 0;
        for (unsigned candidate = 1; candidate <= 8; ++candidate)
        {
            std::array<char, 256> nameBuffer{};
            if (if_indextoname(candidate, nameBuffer.data()) != nullptr && Platform::interfaceIndexOfName(nameBuffer.data()) == candidate)
            {
                interfaceName  = nameBuffer.data();
                interfaceIndex = candidate;
                break;
            }
        }
        if (interfaceName.empty())
        {
            GTEST_SKIP() << "本机取不到可用网卡，接口名换算这条判据无法验证";
        }

        const InetAddress address(8080, "fe80::1%" + interfaceName);
        EXPECT_EQ(address.scopeId(), interfaceIndex) << "接口名「" << interfaceName << "」应换算成它的索引";
        EXPECT_EQ(address.ip(), "fe80::1%" + std::to_string(interfaceIndex)) << "输出用数字形式，接口名不外溢成第二份本机状态";

        const auto *native = reinterpret_cast<const sockaddr_in6 *>(address.nativeAddress());
        EXPECT_EQ(native->sin6_scope_id, interfaceIndex);
    }

    /**
     * @brief 拒绝面：作用域写法不可用时逐条拒收，不静默丢掉后缀也不退化成「无作用域」
     * @details 丢掉后缀就等于把「哪块网卡」这一层信息抹掉——链路本地地址没有作用域时根本连不上，
     *          静默退化只会把失败推后到一条看不出原因的超时上。
     */
    TEST(InetAddress, RejectsUnusableZoneIdentifiers)
    {
        // IPv4 没有作用域这个概念（RFC 4007 §11 只给 IPv6）
        EXPECT_THROW(static_cast<void>(InetAddress(80, "1.2.3.4%eth0")), Base::InvalidArgumentException);

        // 空标识、查不到的接口名、超出 uint32 的数字都算不可用
        for (const char *ipText: {"fe80::1%", "fe80::1%no-such-iface-99", "fe80::1%4294967296", "fe80::1%3%4", "fe80::1%+3"})
        {
            EXPECT_FALSE(InetAddress::parseLiteral(ipText, 80).has_value()) << "该写法不该被接受：" << ipText;
            EXPECT_THROW(static_cast<void>(InetAddress(80, ipText)), Base::InvalidArgumentException) << ipText;
        }

        // 文案要指认是哪一段不可用，配置写错的人才能自己改对
        try
        {
            static_cast<void>(InetAddress(80, "fe80::1%no-such-iface-99"));
            FAIL() << "查不到的接口名不应构造成功";
        } catch (const Base::InvalidArgumentException &exception)
        {
            const std::string message = exception.what();
            EXPECT_NE(message.find("no-such-iface-99"), std::string::npos) << message;
            EXPECT_NE(message.find("作用域标识"), std::string::npos) << message;
        }
    }

    /**
     * @brief parseLiteral() 是「问一句话就答有没有」的通道：不抛异常、不查 DNS
     * @details 构造函数对同样的文本抛异常，非此即彼；上层（URL 解析、按主机名分流 IP/DNS）只想知道
     *          「这是不是字面量」，为此就得有一份不吃异常入口的判据，且两路口径必须完全一致。
     */
    TEST(InetAddress, ParseLiteralReportsAbsenceInsteadOfThrowing)
    {
        EXPECT_TRUE(InetAddress::parseLiteral("192.0.2.1", 80).has_value());
        EXPECT_TRUE(InetAddress::parseLiteral("::1", 80).has_value());
        EXPECT_TRUE(InetAddress::parseLiteral("fe80::1%3", 80).has_value());

        // 这些文本在构造函数那一侧是拒绝条件，在这里都只是「不是字面量」
        for (const char *ipText: {"", "localhost", "0177.0.0.1", "256.1.1.1", "not-an-ip", "1.2.3.4%eth0", "fe80::1%none-such"})
        {
            EXPECT_FALSE(InetAddress::parseLiteral(ipText, 80).has_value()) << ipText;
        }

        // 端口与族都按字面量填好：这是给内核用的地址，不是只做一次格式检查
        const std::optional<InetAddress> address = InetAddress::parseLiteral("203.0.113.7", 9999);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->family(), AF_INET);
        EXPECT_EQ(address->port(), 9999);
    }

    /**
     * @brief resolve() 对带作用域的字面量不走 DNS：字面量不需要问任何人
     * @details 两平台的 getaddrinfo 对「fe80::1%5」是否受理、接口查不到时报哪个码都不一致；本层
     *          先按字面量自解，解析结果因此不随平台变。这条用例同时钉住 resolve() 与
     *          parseLiteral() 是同一套判据。
     */
    TEST(InetAddress, ResolveHandlesScopedLiteralWithoutLookup)
    {
        const std::optional<InetAddress> address = InetAddress::resolve("fe80::1%5", 8080);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->scopeId(), 5U);
        EXPECT_EQ(address->ip(), "fe80::1%5");
        EXPECT_EQ(address->port(), 8080);

        // 查不到的接口名不是字面量，会落到 DNS 那一路；本机没有这个域名，因此是失败而不是回落成 0.0.0.0
        EXPECT_FALSE(InetAddress::resolve("fe80::1%no-such-iface-99", 8080).has_value());
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
        EXPECT_THROW(static_cast<void>(InetAddress(storage, static_cast<socklen_t>(sizeof(sockaddr_storage) + 1))), Base::InvalidArgumentException);
        // 恰好放得下的两种真实长度都应当被接受：IPv4 与 IPv6 的地址长度不同，不能只认一种
        EXPECT_NO_THROW(static_cast<void>(InetAddress(storage, sizeof(sockaddr_in))));
        EXPECT_NO_THROW(static_cast<void>(InetAddress(storage, sizeof(sockaddr_storage))));
    }
} // namespace AsynGyanis::Core
