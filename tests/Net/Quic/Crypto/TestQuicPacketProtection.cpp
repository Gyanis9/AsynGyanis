// TestQuicPacketProtection.cpp —— QUIC 包保护（RFC 9001 §5.3）的用例
//
// 覆盖三块：
//   1) 官方向量：附录 A.5 的 ChaCha20-Poly1305 小包（AAD 4200bff4 + 明文 01 + 包号 654360564 →
//      密文带标签 655e5c…5bfb），以及附录 A.2 的客户端 Initial（AES-128-GCM，1162 字节明文 →
//      1178 字节密文），后者用的密钥是**从 deriveQuicInitialPacketKeys 现推**的，顺带验两层能接上；
//   2) 解开的逆运算：同一组输入解回原明文；
//   3) 认证性的拒绝面：改一个密文字节、包号差一、AAD 少一位，都必须判 AuthenticationFailed
//      ——这三条正是 AEAD 存在的理由，任一失守都意味着 nonce/AAD/标签没被真正绑进来。
//      另外核包号上限（2^62-1 可用、2^62 抛）、以及密文短于标签时判截断。
// 用例不建 SSL、不起网络，纯计算。

#include "Net/Quic/Crypto/QuicPacketProtection.h"

#include "Net/Quic/Codec/QuicVariableLengthInteger.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "QuicRfcVectors.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::buildAppendixA2Plaintext;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;

        /**
         * @brief 用十六进制直接摆一组 AEAD 密钥与 IV
         * @param cipherSuite 套件
         * @param keyHex AEAD 密钥
         * @param initializationVectorHex 12 字节 IV
         * @return QuicPacketKeys 密钥组（头部保护密钥本层用不到，留空）
         */
        QuicPacketKeys makeKeys(const QuicCipherSuite cipherSuite, const std::string_view keyHex,
                                const std::string_view initializationVectorHex)
        {
            QuicPacketKeys keys;
            keys.cipherSuite = cipherSuite;
            const auto keyBytes = makeBytesFromHex(keyHex);
            const auto vectorBytes = makeBytesFromHex(initializationVectorHex);
            std::copy(keyBytes.begin(), keyBytes.end(), keys.encryptionKey.begin());
            std::copy(vectorBytes.begin(), vectorBytes.end(), keys.initializationVector.begin());
            return keys;
        }

        // 向量本身统一放在 QuicRfcVectors.h（从 rfc9001.txt 机器抽取），这里只留旧名字做别名：
        // 两处各抄一份长字面量，改一处忘另一处时的表现是「实现看着对，包却解不开」
        constexpr std::string_view kAppendixA5Key = TestSupport::kAppendixA5KeyHex;

        constexpr std::string_view kAppendixA5InitializationVector = TestSupport::kAppendixA5InitializationVectorHex;

        constexpr std::string_view kAppendixA5AdditionalData = TestSupport::kAppendixA5AdditionalDataHex;

        constexpr std::string_view kAppendixA5Plaintext = TestSupport::kAppendixA5PlaintextHex;

        constexpr std::string_view kAppendixA5ProtectedPayload = TestSupport::kAppendixA5ProtectedPayloadHex;

        constexpr std::uint64_t kAppendixA5PacketNumber = TestSupport::kAppendixA5PacketNumber;

        constexpr std::string_view kAppendixA2AdditionalData = TestSupport::kAppendixA2AdditionalDataHex;

        constexpr std::string_view kAppendixA2ProtectedPayload = TestSupport::kAppendixA2ProtectedPayloadHex;

        constexpr std::uint64_t kAppendixA2PacketNumber = TestSupport::kAppendixA2PacketNumber;

        /// A.1 的客户端 Initial 密钥与 IV：直接给常量是为了把包保护一层单独钉住，
        /// 不与密钥层的正确性绑在一起
        constexpr std::string_view kClientInitialKey = TestSupport::kClientInitialKeyHex;

        constexpr std::string_view kClientInitialInitializationVector = TestSupport::kClientInitialInitializationVectorHex;

        const std::vector<std::uint8_t> &kSampleDestinationConnectionId = TestSupport::kDestinationConnectionIdBytes;

        /**
         * @brief 加密一段载荷并交出字节
         * @param keys 密钥组
         * @param packetNumber 包号
         * @param additionalDataHex 头部
         * @param plaintext 明文
         * @return std::vector<std::uint8_t> 密文加标签
         */
        std::vector<std::uint8_t> sealToBytes(const QuicPacketKeys &keys, const std::uint64_t packetNumber,
                                              const std::vector<std::uint8_t> &additionalData,
                                              const std::vector<std::uint8_t> &plaintext)
        {
            std::string output;
            appendQuicProtectedPayload(output, keys, packetNumber, additionalData, plaintext);
            return toUnsignedBytes(output);
        }
    } // namespace

    /**
     * @brief 附录 A.5 的 ChaCha20 包：密文加标签必须逐字节等于 RFC 给的结果
     */
    TEST(QuicPacketProtection, SealsChaCha20PayloadPerAppendixA5)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        const auto plaintext = makeBytesFromHex(kAppendixA5Plaintext);

        EXPECT_EQ(sealToBytes(keys, kAppendixA5PacketNumber, additionalData, plaintext),
                  makeBytesFromHex(kAppendixA5ProtectedPayload))
                << "nonce = IV XOR 包号、AAD = 头部，任一处算错都产不出这条密文";
    }

    /**
     * @brief 同一组输入解密要拿回原明文，且交回的字节数与明文一致
     */
    TEST(QuicPacketProtection, OpensChaCha20PayloadPerAppendixA5)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        const auto protectedPayload = makeBytesFromHex(kAppendixA5ProtectedPayload);

        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const auto opened = openQuicProtectedPayload(plaintext, keys, kAppendixA5PacketNumber, additionalData,
                                                     protectedPayload);
        ASSERT_TRUE(opened.has_value()) << opened.error().message;
        EXPECT_EQ(*opened, 1U);
        EXPECT_EQ(plaintext, makeBytesFromHex(kAppendixA5Plaintext));
    }

    /**
     * @brief 附录 A.2 的客户端 Initial：密钥现从密钥表推，密文要逐字节对上 RFC
     */
    TEST(QuicPacketProtection, SealsAes128GcmClientInitialPerAppendixA2)
    {
        const auto keys = deriveQuicInitialPacketKeys(kSampleDestinationConnectionId,
                                                      QuicPacketDirection::ClientToServer);
        const auto additionalData = makeBytesFromHex(kAppendixA2AdditionalData);
        const auto plaintext = buildAppendixA2Plaintext();
        const auto protectedPayload = makeBytesFromHex(kAppendixA2ProtectedPayload);

        EXPECT_EQ(sealToBytes(keys, kAppendixA2PacketNumber, additionalData, plaintext), protectedPayload);
    }

    /**
     * @brief 反向：RFC 的保护态载荷解回来要等于未保护明文
     */
    TEST(QuicPacketProtection, OpensAes128GcmClientInitialPerAppendixA2)
    {
        const auto keys = deriveQuicInitialPacketKeys(kSampleDestinationConnectionId,
                                                      QuicPacketDirection::ClientToServer);
        const auto additionalData = makeBytesFromHex(kAppendixA2AdditionalData);
        const auto protectedPayload = makeBytesFromHex(kAppendixA2ProtectedPayload);

        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const auto opened = openQuicProtectedPayload(plaintext, keys, kAppendixA2PacketNumber, additionalData,
                                                     protectedPayload);
        ASSERT_TRUE(opened.has_value()) << opened.error().message;
        EXPECT_EQ(plaintext, buildAppendixA2Plaintext());
    }

    /**
     * @brief 密文被改一个字节必须解不开：这是 AEAD 的完整性保证
     */
    TEST(QuicPacketProtection, RejectsTamperedCiphertext)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        auto protectedPayload = makeBytesFromHex(kAppendixA5ProtectedPayload);
        protectedPayload[0] = static_cast<std::uint8_t>(protectedPayload[0] ^ 0x01);

        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const auto opened = openQuicProtectedPayload(plaintext, keys, kAppendixA5PacketNumber, additionalData,
                                                     protectedPayload);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().kind, QuicDecodeErrorKind::AuthenticationFailed);
    }

    /**
     * @brief 包号差一必须解不开：nonce 里异或的就是它，猜包号不等于能解密
     */
    TEST(QuicPacketProtection, RejectsMismatchedPacketNumber)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        const auto protectedPayload = makeBytesFromHex(kAppendixA5ProtectedPayload);

        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const auto opened = openQuicProtectedPayload(plaintext, keys, kAppendixA5PacketNumber + 1ULL, additionalData,
                                                     protectedPayload);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().kind, QuicDecodeErrorKind::AuthenticationFailed) << "换个包号就解开，说明 nonce 根本没把包号算进去";
    }

    /**
     * @brief 头部（AAD）被改也必须解不开：AAD 不加密但要认证
     */
    TEST(QuicPacketProtection, RejectsTamperedAdditionalData)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        const auto protectedPayload = makeBytesFromHex(kAppendixA5ProtectedPayload);
        // 改掉自旋位：它不属于被保护字段，解包时也不会被掩码改动，正是「只改 AAD」的干净样本
        additionalData[0] = static_cast<std::uint8_t>(additionalData[0] ^ 0x20);

        std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
        const auto opened = openQuicProtectedPayload(plaintext, keys, kAppendixA5PacketNumber, additionalData,
                                                     protectedPayload);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().kind, QuicDecodeErrorKind::AuthenticationFailed);
    }

    /**
     * @brief 密文短于 16 字节标签属于结构性不可能，判截断而不是丢给 OpenSSL
     */
    TEST(QuicPacketProtection, RejectsPayloadShorterThanTag)
    {
        const auto keys = makeKeys(QuicCipherSuite::Aes128Gcm, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto shortPayload = makeBytesFromHex("00112233445566778899");
        std::vector<std::uint8_t> plaintext(10);

        const auto opened = openQuicProtectedPayload(plaintext, keys, 0ULL, shortPayload, shortPayload);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().kind, QuicDecodeErrorKind::Truncated);
    }

    /**
     * @brief 包号上限：2^62-1 可加密且能原样解回，越界当场抛
     */
    TEST(QuicPacketProtection, HandlesMaximumPacketNumberAndRejectsBeyondIt)
    {
        const auto keys = makeKeys(QuicCipherSuite::Aes128Gcm, kClientInitialKey, kClientInitialInitializationVector);
        const auto additionalData = makeBytesFromHex("c000000001");
        const auto plaintext = makeBytesFromHex("01020304");

        const auto protectedPayload = sealToBytes(keys, kQuicMaximumIntegerValue, additionalData, plaintext);
        std::vector<std::uint8_t> openedPlaintext(plaintext.size());
        const auto opened = openQuicProtectedPayload(openedPlaintext, keys, kQuicMaximumIntegerValue, additionalData,
                                                     protectedPayload);
        ASSERT_TRUE(opened.has_value()) << opened.error().message << "：最高位包号要能自洽（IV 拼接的移位边界）";
        EXPECT_EQ(openedPlaintext, plaintext);

        // 包号不同就必须解不开，否则说明高位那几个字节根本没进 nonce
        const auto mismatched = openQuicProtectedPayload(openedPlaintext, keys, kQuicMaximumIntegerValue - 1ULL, additionalData,
                                                         
                                                         protectedPayload);
        EXPECT_FALSE(mismatched.has_value());

        EXPECT_THROW(static_cast<void>(sealToBytes(keys, kQuicMaximumIntegerValue + 1ULL, additionalData, plaintext)),
                     Base::InvalidArgumentException);
    }

    /**
     * @brief 明文缓冲长度与密文不符属于本端用法错误，抛出来而不是写越界
     */
    TEST(QuicPacketProtection, RejectsOutputBufferWithWrongLength)
    {
        const auto keys = makeKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5Key, kAppendixA5InitializationVector);
        const auto additionalData = makeBytesFromHex(kAppendixA5AdditionalData);
        const auto protectedPayload = makeBytesFromHex(kAppendixA5ProtectedPayload);
        // A.5 的明文只有 1 字节：零长缓冲与 8 字节缓冲都不是「密文长减 16」
        std::vector<std::uint8_t> tooSmall(0);
        std::vector<std::uint8_t> tooLarge(8);

        EXPECT_THROW(static_cast<void>(openQuicProtectedPayload(tooSmall, keys, kAppendixA5PacketNumber, additionalData, protectedPayload)),
                     
                     Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(openQuicProtectedPayload(tooLarge, keys, kAppendixA5PacketNumber, additionalData, protectedPayload)),
                     
                     Base::InvalidArgumentException);
    }
} // namespace AsynGyanis::Net
