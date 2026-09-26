// PROXY 协议读侧解析用例：v1 文本行与 v2 二进制块的合法形状、越界形状，以及分帧判定
//
// 全部走纯函数：解析与分帧都不碰套接字，因此这些用例在任何机器上都是确定的，
// 而且能把「什么样的字节算一条合法头」逐条钉住——这部分一旦被放宽，服务器就会把不是头的
// 字节当成身份来源记账，所以宁可在这里把边界写死。

#include "Net/Proxy/ProxyProtocol.h"

#include "Core/Socket/InetAddress.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// v2 的 12 字节签名，用例里按原文拼
        const std::string kSignature{"\r\n\r\n\0\r\nQUIT\n", 12};

        /**
         * @brief 拼一条 v2 头
         * @param command 低 4 位命令（0=LOCAL、1=PROXY、2=UNKNOWN）
         * @param family 高 4 位地址族（0=UNSPEC、1=IPv4、2=IPv6、3=UNIX）
         * @param addressBlock 地址块（含 TLV），长度字段按它的字节数自动写
         * @param declaredOverride 不为空时用它当长度字段，用来构造「谎报长度」的畸形头
         * @return std::string 完整的头字节
         */
        std::string makeV2Header(const int command, const int family, const std::string &addressBlock, const std::size_t declaredOverride = 0U)
        {
            const std::size_t declared = declaredOverride != 0U ? declaredOverride : addressBlock.size();
            std::string       header   = kSignature;
            header += static_cast<char>(0x20 | (command & 0x0F));
            header += static_cast<char>((family << 4) | 0x01); // 传输协议一律按 STREAM 填
            header += static_cast<char>((declared >> 8) & 0xFF);
            header += static_cast<char>(declared & 0xFF);
            header += addressBlock;
            return header;
        }

        /**
         * @brief 拼 v2 的 IPv4 地址块：源、目的各 4 字节，再加两个端口
         */
        std::string makeV2Ipv4Block(const std::vector<int> &source, const std::vector<int> &destination, const int sourcePort, const int destinationPort)
        {
            std::string block;
            for (const int octet: source)
            {
                block += static_cast<char>(octet);
            }
            for (const int octet: destination)
            {
                block += static_cast<char>(octet);
            }
            block += static_cast<char>((sourcePort >> 8) & 0xFF);
            block += static_cast<char>(sourcePort & 0xFF);
            block += static_cast<char>((destinationPort >> 8) & 0xFF);
            block += static_cast<char>(destinationPort & 0xFF);
            return block;
        }

        /**
         * @brief 拼 v2 的 IPv6 地址块：16 + 16 + 2 + 2
         * @param sourceTail 源地址的最后一字节（前缀固定 2001:db8::），够区分两条不同地址
         * @param destinationTail 目的地址的最后一字节
         */
        std::string makeV2Ipv6Block(const int sourceTail, const int destinationTail, const int sourcePort, const int destinationPort)
        {
            std::string block(36U, '\0');
            // 2001:db8::/32 是文档前缀：前 4 字节固定填上，末尾再带一个可辨认的尾号
            block[0U]  = static_cast<char>(0x20);
            block[1U]  = static_cast<char>(0x01);
            block[2U]  = static_cast<char>(0x0D);
            block[3U]  = static_cast<char>(0xB8);
            block[16U] = static_cast<char>(0x20);
            block[17U] = static_cast<char>(0x01);
            block[18U] = static_cast<char>(0x0D);
            block[19U] = static_cast<char>(0xB8);
            block[15U] = static_cast<char>(sourceTail);
            block[31U] = static_cast<char>(destinationTail);
            block[32U] = static_cast<char>((sourcePort >> 8) & 0xFF);
            block[33U] = static_cast<char>(sourcePort & 0xFF);
            block[34U] = static_cast<char>((destinationPort >> 8) & 0xFF);
            block[35U] = static_cast<char>(destinationPort & 0xFF);
            return block;
        }

        /// 解析一条完整的头并交出消费字节数
        std::optional<ProxyEndpoint> parse(const std::string_view header, std::size_t &consumed)
        {
            return parseProxyHeader(header, consumed);
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // v1：文本行
    // ---------------------------------------------------------------------------

    TEST(ProxyProtocol, ParsesV1Tcp4Line)
    {
        const std::string header   = "PROXY TCP4 203.0.113.9 198.51.100.7 65535 80\r\n";
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_TRUE(endpoint->hasAddresses);
        EXPECT_EQ(endpoint->source.ip(), "203.0.113.9");
        EXPECT_EQ(endpoint->source.port(), 65535);
        EXPECT_EQ(endpoint->destination.ip(), "198.51.100.7");
        EXPECT_EQ(endpoint->destination.port(), 80);
        EXPECT_EQ(consumed, header.size()) << "消费字节数要含结尾的 CRLF";
    }

    /**
     * @brief 规范原文给出的那条例子必须逐字段解析成它自己标注的样子
     * @details 这一条的用例数据不来自本仓库的写法：字节串与「谁是谁」都照 HAProxy 的
     *          `proxy-protocol.txt` §version 1 原文（"PROXY TCP4 192.168.0.1 192.168.0.11 56324 443"），
     *          为的是让判据独立于实现——其余用例的地址是本仓库自己编的，只能证明「自洽」
     */
    TEST(ProxyProtocol, ParsesTheSpecVersionOneExample)
    {
        const std::string header   = "PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r\n";
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_TRUE(endpoint->hasAddresses);
        EXPECT_EQ(endpoint->source.ip(), "192.168.0.1") << "第一个地址是来源，不是目的";
        EXPECT_EQ(endpoint->source.port(), 56324);
        EXPECT_EQ(endpoint->destination.ip(), "192.168.0.11");
        EXPECT_EQ(endpoint->destination.port(), 443);
        EXPECT_EQ(consumed, header.size());
    }

    TEST(ProxyProtocol, ParsesV1Tcp6Line)
    {
        const std::string header   = "PROXY TCP6 2001:db8::1 2001:db8::2 12345 443\r\n";
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_EQ(endpoint->source.ip(), "2001:db8::1");
        EXPECT_EQ(endpoint->destination.port(), 443);
        EXPECT_EQ(consumed, header.size());
    }

    /**
     * @brief v1 的 UNKNOWN 只声明「前面有个代理」，没有身份可当客户端来源
     * @details 这一条必须单独钉：把 UNKNOWN 的地址当真实来源记账，等于用一个不存在的 IP 做限额
     */
    TEST(ProxyProtocol, TreatsV1UnknownAsNoAddresses)
    {
        const std::string header   = "PROXY UNKNOWN\r\n";
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_FALSE(endpoint->hasAddresses);
        EXPECT_EQ(consumed, header.size());
    }

    TEST(ProxyProtocol, StopsV1AtLineEndAndLeavesTheRest)
    {
        // 代理把头和请求写在同一次发送里：解析只能吃到 CRLF，剩下的要原样留给协议解析
        const std::string header   = "PROXY TCP4 192.0.2.1 192.0.2.2 1 2\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_EQ(consumed, header.find("\r\n") + 2U) << "消费数应正好到第一个 CRLF 末尾";
        EXPECT_EQ(header.substr(consumed), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }

    TEST(ProxyProtocol, RejectsMalformedV1Lines)
    {
        const std::vector<std::string> bad = {
                "PROXY TCP4 192.0.2.1 192.0.2.2 1\r\n",           // 字段不够
                "PROXY TCP4 192.0.2.1 192.0.2.2 1 2 3\r\n",       // 字段太多
                "PROXY TCP4 192.0.2.1 192.0.2.2 65536 2\r\n",     // 端口越界
                "PROXY TCP4 192.0.2.1 192.0.2.2 abc 2\r\n",       // 端口不是数字
                "PROXY TCP4 2001:db8::1 ::1 1 2\r\n",             // 声明 TCP4 却给 IPv6
                "PROXY TCP6 192.0.2.1 192.0.2.2 1 2\r\n",         // 声明 TCP6 却给 IPv4
                "PROXY TCP4 hostname.example 192.0.2.2 1 2\r\n",  // 主机名不是 IP
                "PROXY UNKNOWN TCP4 192.0.2.1 192.0.2.2 1 2\r\n", // 规范里没有这种形式
                "proxy TCP4 192.0.2.1 192.0.2.2 1 2\r\n",         // 前缀大小写敏感
                "PROXY TCP4 192.0.2.1 192.0.2.2 1 2\n",           // 只换行不收 CRLF
        };
        for (const std::string &header: bad)
        {
            std::size_t consumed = 12345U;
            const auto  endpoint = parse(header, consumed);
            EXPECT_FALSE(endpoint.has_value()) << "不该被接受的 v1 头：" << header;
            EXPECT_EQ(consumed, 0U) << "解析失败必须归零消费数，不能让调用方误吃掉字节：" << header;
        }
    }

    // ---------------------------------------------------------------------------
    // v2：二进制块
    // ---------------------------------------------------------------------------

    TEST(ProxyProtocol, ParsesV2Ipv4ProxyCommand)
    {
        const std::string header   = makeV2Header(1, 1, makeV2Ipv4Block({203, 0, 113, 9}, {198, 51, 100, 7}, 44000, 443));
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_TRUE(endpoint->hasAddresses);
        EXPECT_EQ(endpoint->source.ip(), "203.0.113.9");
        EXPECT_EQ(endpoint->source.port(), 44000);
        EXPECT_EQ(endpoint->destination.ip(), "198.51.100.7");
        EXPECT_EQ(endpoint->destination.port(), 443);
        EXPECT_EQ(consumed, header.size());
    }

    TEST(ProxyProtocol, ParsesV2Ipv6ProxyCommand)
    {
        const std::string header   = makeV2Header(1, 2, makeV2Ipv6Block(0x01, 0x02, 1234, 80));
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_TRUE(endpoint->hasAddresses);
        EXPECT_EQ(endpoint->source.ip(), "2001:db8::1");
        EXPECT_EQ(endpoint->destination.ip(), "2001:db8::2");
        EXPECT_EQ(endpoint->source.port(), 1234);
        EXPECT_EQ(consumed, header.size());
    }

    /**
     * @brief v2 的地址块之后可以挂 TLV：长度字段算的是整段正文，解析要按它消费而不是按固定尺寸
     */
    TEST(ProxyProtocol, SkipsTlvsAfterTheV2AddressBlock)
    {
        std::string block = makeV2Ipv4Block({192, 0, 2, 1}, {192, 0, 2, 2}, 1000, 80);
        block += static_cast<char>(0x04); // TLV 类型：阿里云 VPC 之类都用自定义号段，这里只验「会跳过」
        block += static_cast<char>(0x00);
        block += static_cast<char>(0x02);
        block += std::string{'a', 'b'};
        const std::string header = makeV2Header(1, 1, block);

        std::size_t consumed = 0U;
        const auto  endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_EQ(endpoint->source.ip(), "192.0.2.1");
        EXPECT_EQ(consumed, header.size()) << "带 TLV 时消费数要含 TLV";
    }

    /**
     * @brief LOCAL 与 UNKNOWN 命令都不给身份
     * @details LOCAL 是代理自己发的（健康检查一类），地址字段按规范填 0/127.0.0.1；把它当客户端来源
     *          会让所有健康检查共享一个名额，或让限额指向一个假来源
     */
    TEST(ProxyProtocol, TreatsV2LocalAndUnknownCommandsAsNoAddresses)
    {
        for (const int command: {0, 2})
        {
            const std::string header   = makeV2Header(command, 1, makeV2Ipv4Block({127, 0, 0, 1}, {127, 0, 0, 1}, 1, 1));
            std::size_t       consumed = 0U;
            const auto        endpoint = parse(header, consumed);
            ASSERT_TRUE(endpoint.has_value()) << "命令 " << command << " 是合法的，只是不带可信地址";
            EXPECT_FALSE(endpoint->hasAddresses) << "命令 " << command << " 不许交出身份";
            EXPECT_EQ(consumed, header.size());
        }
    }

    TEST(ProxyProtocol, AcceptsV2UnspecAndUnixFamiliesWithoutIdentity)
    {
        for (const int family: {0, 3})
        {
            const std::string header   = makeV2Header(1, family, std::string{});
            std::size_t       consumed = 0U;
            const auto        endpoint = parse(header, consumed);
            ASSERT_TRUE(endpoint.has_value()) << "地址族 " << family << " 应当被认出，只是没有 IP 身份";
            EXPECT_FALSE(endpoint->hasAddresses) << "地址族 " << family << " 不该交出 IP";
        }
    }

    TEST(ProxyProtocol, RejectsMalformedV2Headers)
    {
        const std::vector<std::string> bad = {
                // 版本号写成 1：v2 的正文形状不该被 1 号版本套用
                kSignature + std::string("\x11\x11\x00\x0c", 4) + makeV2Ipv4Block({1, 1, 1, 1}, {2, 2, 2, 2}, 1, 2),
                // 长度字段短于该地址族必需的地址块
                makeV2Header(1, 1, makeV2Ipv4Block({1, 1, 1, 1}, {2, 2, 2, 2}, 1, 2), 8U),
                // 正文没有长度字段所说的那么多字节：头不完整，不该被当成一条已读到的头
                kSignature + std::string("\x21\x11\x00\x0c\x01\x02", 6),
        };
        for (const std::string &header: bad)
        {
            std::size_t consumed = 0U;
            EXPECT_FALSE(parse(header, consumed).has_value()) << "不该被接受的 v2 头，长度=" << header.size();
        }
    }

    /**
     * @brief 规范之外的命令号按「有代理但没身份」处理
     * @details 不拒绝整条连接（代理以后新增命令号时不该把流量全断），但绝不能把它交出的地址当真实
     *          来源记账——那等于让一个没人认领的命令决定限额算在谁头上
     */
    TEST(ProxyProtocol, TreatsUndefinedV2CommandAsNoAddresses)
    {
        const std::string header   = makeV2Header(3, 1, makeV2Ipv4Block({1, 1, 1, 1}, {2, 2, 2, 2}, 1, 2));
        std::size_t       consumed = 0U;
        const auto        endpoint = parse(header, consumed);
        ASSERT_TRUE(endpoint.has_value());
        EXPECT_FALSE(endpoint->hasAddresses);
        EXPECT_EQ(consumed, header.size());
    }

    // ---------------------------------------------------------------------------
    // 分帧：读侧靠它决定「继续读」还是「当场判死」
    // ---------------------------------------------------------------------------

    TEST(ProxyProtocol, FramesV1OnlyWhenTheWholeLineArrives)
    {
        EXPECT_TRUE(frameProxyHeader("").isStillPlausible);

        const auto partial = frameProxyHeader("PROXY TCP4 192.0.2.1 192.0.2.2 1 2");
        EXPECT_TRUE(partial.isStillPlausible);
        EXPECT_FALSE(partial.totalLength.has_value()) << "没有 CRLF 就定不出总长";

        const auto complete = frameProxyHeader("PROXY TCP4 192.0.2.1 192.0.2.2 1 2\r\nGET / HTTP/1.1\r\n");
        EXPECT_TRUE(complete.isStillPlausible);
        ASSERT_TRUE(complete.totalLength.has_value());
        EXPECT_EQ(*complete.totalLength, 36U) << "长度只到第一个 CRLF，后面是请求";
    }

    /**
     * @brief 不是头的字节要**立刻**判死
     * @details 第一个字节就能否掉：直接把服务器暴露在公网上时，对端发的是 `GET /`，
     *          慢慢等满整个缓冲才拒绝等于给「不配头的监听端口」留了一段可被填满的窗口
     */
    TEST(ProxyProtocol, RejectsNonHeaderPrefixImmediately)
    {
        EXPECT_FALSE(frameProxyHeader("G").isStillPlausible);
        EXPECT_FALSE(frameProxyHeader("GET / HTTP/1.1\r\n").isStillPlausible);
        EXPECT_FALSE(frameProxyHeader("\r\nX").isStillPlausible);
        // 但 CR 开头的确可能是 v2 签名，不能据此判死
        EXPECT_TRUE(frameProxyHeader("\r").isStillPlausible);
        EXPECT_TRUE(frameProxyHeader("\r\n\r\n").isStillPlausible);
    }

    TEST(ProxyProtocol, FramesV2FromTheDeclaredLength)
    {
        EXPECT_FALSE(frameProxyHeader(kSignature).totalLength.has_value()) << "签名读满但定长段还没齐";

        const std::string header  = makeV2Header(1, 1, makeV2Ipv4Block({1, 2, 3, 4}, {5, 6, 7, 8}, 9, 10));
        const auto        framing = frameProxyHeader(header);
        EXPECT_TRUE(framing.isStillPlausible);
        ASSERT_TRUE(framing.totalLength.has_value());
        EXPECT_EQ(*framing.totalLength, header.size());
    }

    /**
     * @brief 长度字段谎报要判死：读侧的上界就是接收缓冲的上界
     */
    TEST(ProxyProtocol, RejectsOversizedHeadersInsteadOfBufferingThem)
    {
        const std::string oversizedV2 = makeV2Header(1, 1, std::string{}, 60000U);
        EXPECT_FALSE(frameProxyHeader(oversizedV2).isStillPlausible);

        std::string oversizedV1 = "PROXY TCP4 ";
        oversizedV1.append(kMaximumProxyHeaderV1Bytes, 'a');
        oversizedV1 += "\r\n";
        EXPECT_FALSE(frameProxyHeader(oversizedV1).isStillPlausible);
    }

    /**
     * @brief 突变自检：把 v1 的行尾判据改坏时，这条用例必须红
     * @details 直接验证 consumed 与 hasAddresses 不是恒等断言——同一份字节里换掉一个字符，
     *          结论就要翻。这里用「TCP4 但地址是 IPv6」这种必须被拒的形状做对照
     */
    TEST(ProxyProtocol, DistinguishesLookalikeV1Lines)
    {
        std::size_t consumed = 0U;
        EXPECT_TRUE(parse("PROXY TCP4 192.0.2.1 192.0.2.2 1 2\r\n", consumed).has_value());
        consumed = 0U;
        EXPECT_FALSE(parse("PROXY TCP4 2001:db8::1 192.0.2.2 1 2\r\n", consumed).has_value());
        consumed = 0U;
        EXPECT_FALSE(parse("PROXY TCP6 192.0.2.1 192.0.2.2 1 2\r\n", consumed).has_value());
    }
} // namespace AsynGyanis::Net
