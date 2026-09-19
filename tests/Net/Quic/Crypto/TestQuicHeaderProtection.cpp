// TestQuicHeaderProtection.cpp —— QUIC 头部保护（RFC 9001 §5.4）的用例
//
// 覆盖三块，全部拿 RFC 的官方向量逐字节比：
//   1) 掩码本身：附录 A.2 的 AES-128 例（样本 d1b1c98d… → 掩码 437b9aec36）与附录 A.5 的
//      ChaCha20 例（样本 5e5cd55c… → 掩码 aefefe7d03），两条不同算法路径各钉一个；
//   2) 去保护的端到端结果：不只是掩码对，还要「保护态报文 → 去保护」后逐字节等于 RFC 给的
//      未保护头部（A.2 的 c3…00000002 与 A.5 的 4200bff4），并把包号交回 Codec 层的 refresh
//      确认能还原成 2 与 49140；
//   3) 取样偏移与加/去保护互逆：样本起点是「包号起点 + 4」而不是真实包号长度（§5.4.2），
//      短到取不满样本的报文一律丢弃。
// 用例不建 SSL、不起网络，纯计算。

#include "Net/Quic/Crypto/QuicHeaderProtection.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::containsText;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;

        /**
         * @brief 造一组只填了头部保护密钥的密钥组
         * @param cipherSuite 套件，决定走 AES-ECB 还是 ChaCha20
         * @param headerProtectionKeyHex 密钥的十六进制文本
         * @return QuicPacketKeys 密钥组（AEAD 部分本层用不到，留空）
         */
        QuicPacketKeys keysWithHeaderProtectionKey(const QuicCipherSuite cipherSuite, const std::string_view headerProtectionKeyHex)
        {
            QuicPacketKeys keys;
            keys.cipherSuite = cipherSuite;
            const auto keyBytes = makeBytesFromHex(headerProtectionKeyHex);
            std::copy(keyBytes.begin(), keyBytes.end(), keys.headerProtectionKey.begin());
            return keys;
        }

        /// 附录 A.1 客户端 Initial 的头部保护密钥
        constexpr const char *kClientInitialHeaderProtectionKey = "9f50449e04a0e810283a1e9933adedd2";

        /// 附录 A.5 的 ChaCha20 头部保护密钥
        constexpr const char *kChaCha20HeaderProtectionKey = "25a282b9e82f06f21f488917a4fc8f1b"
                                                             "73573685608597d0efcb076b0ab7a7a4";

        /// 附录 A.2：保护态头部（18 字节）+ 保护态包号 + 密文开头的 16 字节样本
        std::vector<std::uint8_t> buildAppendixA2ProtectedPacket()
        {
            auto packet = makeBytesFromHex("c000000001088394c8f03e5157080000449e"
                                           "7b9aec34"
                                           "d1b1c98dd7689fb8ec11d242b123dc9b");
            // Length 声明 1182 = 4 字节包号 + 载荷，整包 1200 字节；尾部内容对本层无意义
            packet.resize(1200);
            return packet;
        }
    } // namespace

    /**
     * @brief 附录 A.2 的 AES-128 掩码必须逐字节等于 437b9aec36
     */
    TEST(QuicHeaderProtection, GeneratesAesMaskPerAppendixA2)
    {
        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::Aes128Gcm, kClientInitialHeaderProtectionKey);
        const auto sample = makeBytesFromHex("d1b1c98dd7689fb8ec11d242b123dc9b");

        const auto mask = generateQuicHeaderProtectionMask(keys, sample);
        EXPECT_EQ(std::vector<std::uint8_t>(mask.first(5).begin(), mask.first(5).end()),
                  makeBytesFromHex("437b9aec36"));
    }

    /**
     * @brief 附录 A.5 的 ChaCha20 掩码必须逐字节等于 aefefe7d03
     */
    TEST(QuicHeaderProtection, GeneratesChaCha20MaskPerAppendixA5)
    {
        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::ChaCha20Poly1305, kChaCha20HeaderProtectionKey);
        const auto sample = makeBytesFromHex("5e5cd55c41f69080575d7999c25a5bfb");

        const auto mask = generateQuicHeaderProtectionMask(keys, sample);
        EXPECT_EQ(std::vector<std::uint8_t>(mask.bytes.begin(), mask.bytes.end()), makeBytesFromHex("aefefe7d03"));
    }

    /**
     * @brief 附录 A.2 端到端：取样→算掩码→去保护，报文头部要还原成 RFC 给的未保护字节
     */
    TEST(QuicHeaderProtection, RemovesProtectionFromAppendixA2ClientInitial)
    {
        auto packet = buildAppendixA2ProtectedPacket();
        const auto decoded = decodeQuicPacketHeader(packet, 8);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        QuicPacketHeader header = *decoded;

        const auto sample = extractQuicHeaderProtectionSample(packet, header);
        ASSERT_TRUE(sample.has_value()) << sample.error().message;
        // 期望值直接取 RFC 自己给的样本，而不是复述实现的偏移
        EXPECT_EQ(std::vector<std::uint8_t>(sample->begin(), sample->end()),
                  makeBytesFromHex("d1b1c98dd7689fb8ec11d242b123dc9b")) << "样本起点应是包号偏移 18 + 4";

        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::Aes128Gcm, kClientInitialHeaderProtectionKey);
        const auto mask = generateQuicHeaderProtectionMask(keys, *sample);
        const auto unmaskedFirstByte = removeQuicHeaderProtection(packet, header, mask);
        ASSERT_TRUE(unmaskedFirstByte.has_value()) << unmaskedFirstByte.error().message;
        EXPECT_EQ(*unmaskedFirstByte, 0xc3);

        const std::vector<std::uint8_t> headerBytes(packet.begin(), packet.begin() + 22);
        EXPECT_EQ(headerBytes, makeBytesFromHex("c300000001088394c8f03e5157080000449e00000002"))
                << "去保护后的头部必须逐字节等于 RFC 的未保护头部";

        // 还原出的首字节交给 Codec 层，包号才能读成 2
        ASSERT_TRUE(refreshQuicPacketHeader(header, *unmaskedFirstByte, packet).has_value());
        EXPECT_EQ(header.packetNumberByteCount, 4U);
        EXPECT_EQ(header.packetNumber, 2ULL);
        EXPECT_TRUE(header.areReservedBitsClear());
    }

    /**
     * @brief 附录 A.5 端到端：短头的样本是偏移 5 起的 16 字节，去保护后头部为 4200bff4
     */
    TEST(QuicHeaderProtection, RemovesProtectionFromAppendixA5ShortPacket)
    {
        auto packet = makeBytesFromHex("4cfe4189655e5cd55c41f69080575d7999c25a5bfb");
        const auto decoded = decodeQuicPacketHeader(packet, 0);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        QuicPacketHeader header = *decoded;

        const auto sample = extractQuicHeaderProtectionSample(packet, header);
        ASSERT_TRUE(sample.has_value()) << sample.error().message;
        EXPECT_EQ(std::vector<std::uint8_t>(sample->begin(), sample->end()),
                  makeBytesFromHex("5e5cd55c41f69080575d7999c25a5bfb")) << "样本跳过 1 字节，从包号起点 + 4 处取（§5.4.2）";

        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::ChaCha20Poly1305, kChaCha20HeaderProtectionKey);
        const auto mask = generateQuicHeaderProtectionMask(keys, *sample);
        const auto unmaskedFirstByte = removeQuicHeaderProtection(packet, header, mask);
        ASSERT_TRUE(unmaskedFirstByte.has_value()) << unmaskedFirstByte.error().message;

        const std::vector<std::uint8_t> headerBytes(packet.begin(), packet.begin() + 4);
        EXPECT_EQ(headerBytes, makeBytesFromHex("4200bff4"));

        ASSERT_TRUE(refreshQuicPacketHeader(header, *unmaskedFirstByte, packet).has_value());
        EXPECT_EQ(header.packetNumber, 49140ULL);
        EXPECT_FALSE(header.isKeyPhaseBitSet);
    }

    /**
     * @brief 加保护是去保护的逆运算：同一掩码正反各跑一次要回到原字节
     */
    TEST(QuicHeaderProtection, ApplyIsTheInverseOfRemove)
    {
        const auto protectedBytes = makeBytesFromHex("4cfe4189655e5cd55c41f69080575d7999c25a5bfb");
        auto unprotectedPacket = makeBytesFromHex("4200bff4655e5cd55c41f69080575d7999c25a5bfb");

        // 明文态：首字节与包号都按未保护的样子写好
        QuicPacketHeader plainHeader;
        plainHeader.isLongHeader = false;
        plainHeader.firstByte = 0x42;
        plainHeader.packetNumberOffset = 1;

        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::ChaCha20Poly1305, kChaCha20HeaderProtectionKey);
        const auto sample = std::span<const std::uint8_t>(unprotectedPacket).subspan(5, 16);
        const auto mask = generateQuicHeaderProtectionMask(keys, sample);
        const auto protectedFirstByte = applyQuicHeaderProtection(unprotectedPacket, plainHeader, mask);
        ASSERT_TRUE(protectedFirstByte.has_value()) << protectedFirstByte.error().message;
        EXPECT_EQ(*protectedFirstByte, 0x4c);
        EXPECT_EQ(unprotectedPacket, protectedBytes) << "加保护的结果必须与 RFC 的保护态报文逐字节相同";

        // 再用同一条掩码解回去，验证两侧确实是同一个双射
        const auto restored = removeQuicHeaderProtection(unprotectedPacket, plainHeader, mask);
        ASSERT_TRUE(restored.has_value()) << restored.error().message;
        EXPECT_EQ(*restored, 0x42);
    }

    /**
     * @brief 报文短到取不满样本时判截断，让调用方整包丢弃（§5.4.2 明确要求）
     */
    TEST(QuicHeaderProtection, RejectsPacketTooShortForSample)
    {
        auto packet = makeBytesFromHex("4cfe4189655e5cd55c");
        const auto decoded = decodeQuicPacketHeader(packet, 0);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

        const auto sample = extractQuicHeaderProtectionSample(packet, *decoded);
        ASSERT_FALSE(sample.has_value());
        EXPECT_EQ(sample.error().kind, QuicDecodeErrorKind::Truncated);
        EXPECT_TRUE(containsText(sample.error().message, "丢弃")) << sample.error().message;
    }

    /**
     * @brief 样本长度不是 16 字节属于用法错误，当场抛而不是算出一个错掩码
     */
    TEST(QuicHeaderProtection, RejectsSampleWithWrongLength)
    {
        const auto keys = keysWithHeaderProtectionKey(QuicCipherSuite::Aes128Gcm, kClientInitialHeaderProtectionKey);
        const auto shortSample = makeBytesFromHex("d1b1c98dd7689fb8ec11d242b123dc");
        EXPECT_THROW(static_cast<void>(generateQuicHeaderProtectionMask(keys, shortSample)), Base::InvalidArgumentException);
    }
} // namespace AsynGyanis::Net
