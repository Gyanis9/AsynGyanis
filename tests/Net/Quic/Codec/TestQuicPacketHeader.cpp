// TestQuicPacketHeader.cpp —— QUIC 报文头（RFC 9000 §17.2/§17.3）解码用例
//
// 覆盖四块：
//   1) 官方向量：RFC 9001 附录 A.2 的客户端 Initial（18 字节头部、Length=1182、整包 1200 字节）与
//      附录 A.5 的 ChaCha20 短头包（21 字节、空目的连接标识、3 字节包号 49140），逐字段核偏移与长度；
//   2) 两趟解的契约：首字节的保留位与包号长度受头部保护（RFC 9001 §5.4），所以第一趟不许给包号；
//      并刻意断言「拿保护后的首字节去 refresh 会得到错误的包号长度」，把这条钉死；
//   3) 短头没有长度字段：目的连接标识长度必须由调用方给。用例同时演示给错的后果——把包号字节
//      并进目的标识、读位置整体偏移，路由表从此命不中；
//   4) 拒绝面与截断：固定位为 0、非 v1 版本、版本协商与 Retry（本层不解释）、连接标识超 20 字节、
//      Length 为 0、各字段越出数据报末尾、以及一个数据报里合包时本包只按 Length 前进。
// 用例全是纯计算：不起网络、不依赖外部服务，因此没有等待时序的问题。

#include "Net/Quic/Codec/QuicPacketHeader.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 由字节序列拼出待解码的字节容器（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 把报文头里的一条连接标识/Token 视图拷成可比对的容器
         * @param bytes 指向数据报的视图
         * @return std::vector<std::uint8_t> 视图内容的副本
         */
        std::vector<std::uint8_t> copyOf(const std::span<const std::uint8_t> bytes)
        {
            return {bytes.begin(), bytes.end()};
        }

        /// RFC 9001 附录 A.2 的客户端 Initial：首字节 c0、版本 1、8 字节目的标识、空源标识、
        /// 无 Token（Token 长度 00）、Length 域 449e（=1182），其后是 4 字节包号
        constexpr const char *kAppendixA2ProtectedHeader = "c000000001088394c8f03e5157080000449e";

        /// 同一条报文去掉头部保护后的样子：首字节回到 c3，包号是未截断的 00000002
        constexpr const char *kAppendixA2UnprotectedPrefix = "c300000001088394c8f03e5157080000449e00000002";

        /// RFC 9001 附录 A.5 的保护后短头包（21 字节）与它的未保护首字节序列
        constexpr const char *kAppendixA5ProtectedShortPacket = "4cfe4189655e5cd55c41f69080575d7999c25a5bfb";

        constexpr const char *kAppendixA5UnprotectedShortPacket = "4200bff4655e5cd55c41f69080575d7999c25a5bfb";
    } // namespace

    /**
     * @brief 附录 A.2 的客户端 Initial：明文能确定的字段逐个对上，且第一趟不许给包号
     */
    TEST(QuicPacketHeader, DecodesAppendixA2ClientInitialFraming)
    {
        auto packet = makeBytesFromHex(kAppendixA2ProtectedHeader);
        packet.resize(1200);

        const auto header = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_TRUE(header->isLongHeader);
        EXPECT_EQ(header->longPacketType, QuicLongPacketType::Initial);
        EXPECT_EQ(header->version, kQuicVersion1);
        EXPECT_EQ(copyOf(header->destinationConnectionId), makeBytesFromHex("8394c8f03e515708"));
        EXPECT_TRUE(header->sourceConnectionId.empty());
        EXPECT_TRUE(header->token.empty());
        // RFC 的 "header[18..21] ^= mask[1..4]" 正是本偏移，能直接对上原文
        EXPECT_EQ(header->packetNumberOffset, 18U);
        EXPECT_EQ(header->packetNumberAndPayloadByteCount, 1182U);
        EXPECT_EQ(header->packetByteCount, 1200U);
        EXPECT_EQ(header->packetNumberByteCount, 0U) << "包号长度那两位受头部保护，去保护之前不许给值";
        EXPECT_EQ(header->packetNumber, 0U);
    }

    /**
     * @brief refresh 用去掉头部保护后的首字节，才能读到正确的包号长度与包号
     */
    TEST(QuicPacketHeader, RefreshAfterHeaderProtectionRecoversPacketNumber)
    {
        auto packet = makeBytesFromHex(kAppendixA2UnprotectedPrefix);
        packet.resize(1200);

        const auto decoded = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        QuicPacketHeader header = *decoded;
        ASSERT_TRUE(refreshQuicPacketHeader(header, 0xc3, packet).has_value());
        EXPECT_EQ(header.packetNumberByteCount, 4U);
        EXPECT_EQ(header.packetNumber, 2ULL);
        EXPECT_TRUE(header.areReservedBitsClear());
    }

    /**
     * @brief 拿保护后的首字节去 refresh 会得到错的包号长度——这条把「必须掩回原值」钉住
     */
    TEST(QuicPacketHeader, RefreshWithProtectedFirstByteGivesWrongPacketNumber)
    {
        auto packet = makeBytesFromHex(kAppendixA2ProtectedHeader);
        packet.resize(1200);

        const auto decoded = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(decoded.has_value());
        QuicPacketHeader header = *decoded;
        ASSERT_TRUE(refreshQuicPacketHeader(header, 0xc0, packet).has_value());
        EXPECT_EQ(header.packetNumberByteCount, 1U) << "c0 的低 2 位是 00，会被当成 1 字节包号";
        EXPECT_NE(header.packetNumber, 2ULL) << "真包号 2 要按 4 字节读；按 1 字节读到的是包号首字节";
    }

    /**
     * @brief 附录 A.5 的短头包：目的标识按本端约定的长度取，本包吃掉数据报剩余全部字节
     */
    TEST(QuicPacketHeader, DecodesAppendixA5ShortHeaderPacket)
    {
        const auto packet = makeBytesFromHex(kAppendixA5ProtectedShortPacket);
        ASSERT_EQ(packet.size(), 21U);

        const auto header = decodeQuicPacketHeader(packet, 0);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_FALSE(header->isLongHeader);
        EXPECT_EQ(header->version, 0U) << "短头线上不带版本，保持 0 而不是谎报成 1";
        EXPECT_TRUE(header->destinationConnectionId.empty());
        EXPECT_EQ(header->packetNumberOffset, 1U);
        EXPECT_EQ(header->packetNumberAndPayloadByteCount, 20U);
        EXPECT_EQ(header->packetByteCount, 21U) << "短头没有 Length 域，本包吃掉整个数据报余下部分（RFC 9000 §17.3.1）";
    }

    /**
     * @brief 短头去保护后：3 字节包号 49140、自旋位与密钥相位位都按位取
     */
    TEST(QuicPacketHeader, RefreshShortHeaderReadsTruncatedPacketNumber)
    {
        const auto packet = makeBytesFromHex(kAppendixA5UnprotectedShortPacket);

        const auto decoded = decodeQuicPacketHeader(packet, 0);
        ASSERT_TRUE(decoded.has_value());
        QuicPacketHeader header = *decoded;
        ASSERT_TRUE(refreshQuicPacketHeader(header, 0x42, packet).has_value());
        EXPECT_EQ(header.packetNumberByteCount, 3U);
        // 654360564 的低 3 字节是 0x00bff4 = 49140：包号是定长大端，不带变长整数前缀
        EXPECT_EQ(header.packetNumber, 49140ULL);
        EXPECT_FALSE(header.isSpinBitSet);
        EXPECT_FALSE(header.isKeyPhaseBitSet);
        EXPECT_TRUE(header.areReservedBitsClear());
    }

    /**
     * @brief 自旋位与密钥相位位置位时按位读出，保留位仍要求为 0
     */
    TEST(QuicPacketHeader, ReadsSpinAndKeyPhaseBitsFromShortHeader)
    {
        // 0x66 = 短头 + 固定位 1 + 自旋 1 + 保留 00 + 密钥相位 1 + 包号长度 10（3 字节）
        auto packet = makeBytesFromHex("6600000000");
        packet.resize(20);

        const auto decoded = decodeQuicPacketHeader(packet, 0);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        QuicPacketHeader header = *decoded;
        ASSERT_TRUE(refreshQuicPacketHeader(header, 0x66, packet).has_value());
        EXPECT_TRUE(header.isSpinBitSet);
        EXPECT_TRUE(header.isKeyPhaseBitSet);
        EXPECT_TRUE(header.areReservedBitsClear());
    }

    /**
     * @brief 短头目的标识长度给错的后果：标识内容被污染、读位置整体偏移，路由必然命不中
     */
    TEST(QuicPacketHeader, ShortHeaderConnectionIdLengthMustMatchLocalIssuedLength)
    {
        // 真实目的标识 18 字节 + 2 字节包号 + 16 字节标签
        auto packet = makeBytesFromHex("40aabbccddeeff00112233445566778899aabb 0a0b");
        packet.resize(1 + 18 + 2 + 16);

        const auto correct = decodeQuicPacketHeader(packet, 18);
        ASSERT_TRUE(correct.has_value()) << correct.error().message;
        EXPECT_EQ(copyOf(correct->destinationConnectionId), makeBytesFromHex("aabbccddeeff00112233445566778899aabb"));
        EXPECT_EQ(correct->packetNumberOffset, 19U);

        const auto tooLong = decodeQuicPacketHeader(packet, 20);
        ASSERT_TRUE(tooLong.has_value()) << "按 20 字节读只是内容错，不会当场失败——正因如此必须靠断言钉住";
        EXPECT_NE(copyOf(tooLong->destinationConnectionId), copyOf(correct->destinationConnectionId)) << "多吃的 2 字节把包号并进了目的连接标识";
        EXPECT_NE(tooLong->packetNumberOffset, correct->packetNumberOffset);
    }

    /**
     * @brief 短头按声明长度取不到足够的字节时判截断，而不是解出一个残缺标识
     */
    TEST(QuicPacketHeader, ShortHeaderWithTooLittleRoomIsTruncated)
    {
        const auto packet = makeBytesFromHex("40aabbcc");
        const auto header = decodeQuicPacketHeader(packet, 18);
        ASSERT_FALSE(header.has_value());
        EXPECT_EQ(header.error().kind, QuicDecodeErrorKind::Truncated);
    }

    /**
     * @brief 附录 A.3 的包号还原：16 位 0x9b32 在最大包号 0xa82f30ea 之后还原为 0xa82f9b32
     */
    TEST(QuicPacketHeader, RestoresPacketNumberPerAppendixA3)
    {
        EXPECT_EQ(restoreQuicPacketNumber(0xa82f30eaULL, 0x9b32ULL, 2), 0xa82f9b32ULL);
        EXPECT_EQ(restoreQuicPacketNumber(100ULL, 101ULL, 1), 101ULL);
        // 1 字节窗口回绕：最大包号 250 之后的截断值 3 应该是 259 而不是 3
        EXPECT_EQ(restoreQuicPacketNumber(250ULL, 3ULL, 1), 259ULL);
        // 期望值小于半窗口时，判据不能因无符号回绕而反过来：最大包号 0 之后的截断值 0 仍是 0
        EXPECT_EQ(restoreQuicPacketNumber(0ULL, 0ULL, 1), 0ULL);
        EXPECT_THROW(static_cast<void>(restoreQuicPacketNumber(0ULL, 0ULL, 0)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(restoreQuicPacketNumber(0ULL, 0ULL, 5)), Base::InvalidArgumentException);
    }

    /**
     * @brief 一个数据报里合多包时，本包只按 Length 前进，剩余字节留给下一包
     */
    TEST(QuicPacketHeader, CoalescedPacketsAdvanceByOwnLengthOnly)
    {
        auto packet = makeBytesFromHex(kAppendixA2ProtectedHeader);
        packet.resize(1200 + 50);

        const auto header = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(header->packetByteCount, 1200U) << "多出来的 50 字节属于同一数据报里的下一个报文（RFC 9000 §12.2）";
    }

    /**
     * @brief Initial 带 Token 时按 Token 长度字段切出内容，其后的 Length 域才接着读
     */
    TEST(QuicPacketHeader, ReadsInitialTokenBeforeLengthField)
    {
        auto packet = makeBytesFromHex("c000000001088394c8f03e5157080004deadbeef449e00000002");
        packet.resize(1220);

        const auto header = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(copyOf(header->token), makeBytesFromHex("deadbeef"));
        EXPECT_EQ(header->packetNumberOffset, 22U);
        EXPECT_EQ(header->packetNumberAndPayloadByteCount, 1182U);
        EXPECT_EQ(header->packetByteCount, 1204U);
    }

    /**
     * @brief 违反 v1 硬性规则的长头一律判 Malformed：固定位为 0、未知版本、版本协商、Retry、标识超 20 字节
     */
    TEST(QuicPacketHeader, RejectsLongHeaderViolations)
    {
        struct Case
        {
            const char *hex;
            const char *expectedFragment;
        };

        const std::vector<Case> cases{
                // 固定位为 0（版本字段合法）：不是本版本的合法报文
                {"8000000001 00 00", "固定位"},
                // 版本不是 1
                {"c0000000ff08"
                 "3333333333333333"
                 "0000",
                 "版本"},
                // 版本协商：版本字段为 0
                {"ff000000000833333333333333330000000001", "版本协商"},
                // Retry：本层不解释令牌与完整性标签
                {"f00000000108333333333333333300", "Retry"},
                // 目的连接标识长度 21 字节，超过 v1 上限
                {"c0000000011500112233445566778899aabbccddeeff0011223300 00", "上限"},
                // Length 为 0：容不下至少 1 字节的包号
                {"c20000000100000000", "包号"},
        };

        for (const Case &testCase: cases)
        {
            const auto header = decodeQuicPacketHeader(makeBytesFromHex(testCase.hex), 8);
            ASSERT_FALSE(header.has_value()) << "以下报文必须被拒：" << testCase.hex;
            EXPECT_EQ(header.error().kind, QuicDecodeErrorKind::Malformed);
            EXPECT_TRUE(containsText(header.error().message, testCase.expectedFragment)) << "文案要能指到违规处：" << header.error().message;
        }
    }

    /**
     * @brief 字段越出数据报末尾一律判截断：版本、连接标识、Token 长度、Length 字段与声明长度各处都要拦下
     */
    TEST(QuicPacketHeader, RejectsTruncatedLongHeaders)
    {
        const std::vector<const char *> cases{
                "",                                               // 空数据报：连首字节都没有
                "c000",                                           // 版本读不满 4 字节
                "c0000000010883",                                 // 目的标识声明 8 字节只剩 1
                "c000000001088394c8f03e515708",                   // 源标识的长度字节缺失
                "c000000001088394c8f03e51570800c2",               // Token 长度声明 8 字节却没给够
                "e000000001088394c8f03e515708 00",                // Handshake 无 Token，Length 字段一个字节都没有
                "c000000001088394c8f03e5157080040ff449e00000002", // Token 声明 255 字节越出数据报末尾
        };

        for (const char *hex: cases)
        {
            const auto header = decodeQuicPacketHeader(makeBytesFromHex(hex), 8);
            ASSERT_FALSE(header.has_value()) << "以下报文必须被拒：" << hex;
            EXPECT_EQ(header.error().kind, QuicDecodeErrorKind::Truncated) << hex << " → " << header.error().message;
        }
    }

    /**
     * @brief 本端配置的连接标识长度超过 v1 上限是用法错误，抛异常而不是冒充可恢复的解码失败
     */
    TEST(QuicPacketHeader, RejectsLocalConnectionIdLengthAboveRange)
    {
        const auto packet = makeBytesFromHex("4000112233");
        EXPECT_THROW(static_cast<void>(decodeQuicPacketHeader(packet, kQuicMaximumConnectionIdLength + 1)), Base::InvalidArgumentException);
    }

    /**
     * @brief Length 域容不下真实包号长度时，refresh 判不自洽而不是算出负的载荷长度
     */
    TEST(QuicPacketHeader, RefreshRejectsInconsistentLengthField)
    {
        // 头部 Length=1（只够 1 字节），但首字节低 2 位掩出来是 3 → 需要 4 字节包号
        auto       packet  = makeBytesFromHex("c300000001088394c8f03e515708000001ff");
        const auto decoded = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        QuicPacketHeader header    = *decoded;
        const auto       refreshed = refreshQuicPacketHeader(header, 0xc3, packet);
        ASSERT_FALSE(refreshed.has_value());
        EXPECT_EQ(refreshed.error().kind, QuicDecodeErrorKind::Malformed);
        EXPECT_TRUE(containsText(refreshed.error().message, "不自洽")) << refreshed.error().message;
    }
} // namespace AsynGyanis::Net
