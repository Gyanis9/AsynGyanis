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

        /// 附录 A.5：ChaCha20-Poly1305 的最小包
        constexpr const char *kAppendixA5Key = "c6d98ff3441c3fe1b2182094f69caa2e"
                                               "d4b716b65488960a7a984979fb23e1c8";

        constexpr const char *kAppendixA5InitializationVector = "e0459b3474bdd0e44a41c144";

        constexpr const char *kAppendixA5AdditionalData = "4200bff4";

        constexpr const char *kAppendixA5Plaintext = "01";

        constexpr const char *kAppendixA5ProtectedPayload = "655e5cd55c41f69080575d7999c25a5bfb";

        constexpr std::uint64_t kAppendixA5PacketNumber = 654360564ULL;

        /// 附录 A.2：客户端 Initial 的头部（未保护态，含 4 字节包号）就是 AEAD 的 AAD
        constexpr const char *kAppendixA2AdditionalData = "c300000001088394c8f03e5157080000449e00000002";

        /// 附录 A.2 只列出前 245 字节的帧，余下说明是「补足 1162 字节的 PADDING 帧」，
        /// 而 PADDING 帧的类型就是 0x00，所以补齐部分是一串零字节
        constexpr const char *kAppendixA2FramesHex =
            "060040f1010000ed0303ebf8fa56f12939b9584a3896472ec40bb863"
            "cfd3e86804fe3a47f06a2b69484c00000413011302010000c0000000"
            "10000e00000b6578616d706c652e636f6dff01000100000a00080006"
            "001d0017001800100007000504616c706e0005000501000000000033"
            "00260024001d00209370b2c9caa47fbabaf4559fedba753de171fa71"
            "f50f1ce15d43e994ec74d748002b0003020304000d0010000e040305"
            "0306030203080408050806002d00020101001c000240010039003204"
            "08ffffffffffffffff05048000ffff07048000ffff08011001048000"
            "75300901100f088394c8f03e51570806048000ffff";

        constexpr std::size_t kAppendixA2PayloadByteCount = 1162;

        /// 附录 A.2 保护态报文的密文加标签共 1178 字节（整包 1200 减去 22 字节保护态头部）
        constexpr const char *kAppendixA2ProtectedPayload =
                                                "d1b1c98dd7689fb8ec11d242b123dc9bd8bab936b47d92ec356c0bab7df5976d"
                                                    "27cd449f63300099f3991c260ec4c60d17b31f8429157bb35a1282a643a8d226"
                                                    "2cad67500cadb8e7378c8eb7539ec4d4905fed1bee1fc8aafba17c750e2c7ace"
                                                    "01e6005f80fcb7df621230c83711b39343fa028cea7f7fb5ff89eac2308249a0"
                                                    "2252155e2347b63d58c5457afd84d05dfffdb20392844ae812154682e9cf012f"
                                                    "9021a6f0be17ddd0c2084dce25ff9b06cde535d0f920a2db1bf362c23e596d11"
                                                    "a4f5a6cf3948838a3aec4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3f"
                                                    "af6ad760b7bad1db4ba3485e8a94dc250ae3fdb41ed15fb6a8e5eba0fc3dd60b"
                                                    "c8e30c5c4287e53805db059ae0648db2f64264ed5e39be2e20d82df566da8dd5"
                                                    "998ccabdae053060ae6c7b4378e846d29f37ed7b4ea9ec5d82e7961b7f25a932"
                                                    "3851f681d582363aa5f89937f5a67258bf63ad6f1a0b1d96dbd4faddfcefc526"
                                                    "6ba6611722395c906556be52afe3f565636ad1b17d508b73d8743eeb524be22b"
                                                    "3dcbc2c7468d54119c7468449a13d8e3b95811a198f3491de3e7fe942b330407"
                                                    "abf82a4ed7c1b311663ac69890f4157015853d91e923037c227a33cdd5ec281c"
                                                    "a3f79c44546b9d90ca00f064c99e3dd97911d39fe9c5d0b23a229a234cb36186"
                                                    "c4819e8b9c5927726632291d6a418211cc2962e20fe47feb3edf330f2c603a9d"
                                                    "48c0fcb5699dbfe5896425c5bac4aee82e57a85aaf4e2513e4f05796b07ba2ee"
                                                    "47d80506f8d2c25e50fd14de71e6c418559302f939b0e1abd576f279c4b2e0fe"
                                                    "b85c1f28ff18f58891ffef132eef2fa09346aee33c28eb130ff28f5b76695333"
                                                    "4113211996d20011a198e3fc433f9f2541010ae17c1bf202580f6047472fb368"
                                                    "57fe843b19f5984009ddc324044e847a4f4a0ab34f719595de37252d6235365e"
                                                    "9b84392b061085349d73203a4a13e96f5432ec0fd4a1ee65accdd5e3904df54c"
                                                    "1da510b0ff20dcc0c77fcb2c0e0eb605cb0504db87632cf3d8b4dae6e705769d"
                                                    "1de354270123cb11450efc60ac47683d7b8d0f811365565fd98c4c8eb936bcab"
                                                    "8d069fc33bd801b03adea2e1fbc5aa463d08ca19896d2bf59a071b851e6c2390"
                                                    "52172f296bfb5e72404790a2181014f3b94a4e97d117b438130368cc39dbb2d1"
                                                    "98065ae3986547926cd2162f40a29f0c3c8745c0f50fba3852e566d44575c29d"
                                                    "39a03f0cda721984b6f440591f355e12d439ff150aab7613499dbd49adabc867"
                                                    "6eef023b15b65bfc5ca06948109f23f350db82123535eb8a7433bdabcb909271"
                                                    "a6ecbcb58b936a88cd4e8f2e6ff5800175f113253d8fa9ca8885c2f552e657dc"
                                                    "603f252e1a8e308f76f0be79e2fb8f5d5fbbe2e30ecadd220723c8c0aea8078c"
                                                    "dfcb3868263ff8f0940054da48781893a7e49ad5aff4af300cd804a6b6279ab3"
                                                    "ff3afb64491c85194aab760d58a606654f9f4400e8b38591356fbf6425aca26d"
                                                    "c85244259ff2b19c41b9f96f3ca9ec1dde434da7d2d392b905ddf3d1f9af93d1"
                                                    "af5950bd493f5aa731b4056df31bd267b6b90a079831aaf579be0a39013137aa"
                                                    "c6d404f518cfd46840647e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d169a44"
                                                    "e55c88d4a9a7f9474241e221af44860018ab0856972e194cd934";

        constexpr std::uint64_t kAppendixA2PacketNumber = 2ULL;

        /// 附录 A.1 的客户端 Initial AEAD 密钥与 IV（等价于 deriveQuicInitialPacketKeys 的结果，
        /// 这里直接给常量是为了把包保护一层单独钉住，不与密钥层的正确性绑在一起）
        constexpr const char *kClientInitialKey = "1f369613dd76d5467730efcbe3b1a22d";

        constexpr const char *kClientInitialInitializationVector = "fa044b2f42a3fd3b46fb255c";

        /// 目的连接标识：Initial 密钥的输入，A.1/A.2/A.3 用的是同一个值
        const std::vector<std::uint8_t> kSampleDestinationConnectionId = makeBytesFromHex("8394c8f03e515708");

        /**
         * @brief 拼出附录 A.2 的完整明文载荷
         * @details 用 resize 补零而不是贴两千多个字符：余下 917 字节全是 PADDING 帧，
         *          写成一串零字面量既看不出来源也容易被格式化工具折断。
         * @return std::vector<std::uint8_t> 1162 字节载荷
         */
        std::vector<std::uint8_t> buildAppendixA2Plaintext()
        {
            auto plaintext = makeBytesFromHex(kAppendixA2FramesHex);
            plaintext.resize(kAppendixA2PayloadByteCount);
            return plaintext;
        }

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
