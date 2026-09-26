// TestQuicKeySchedule.cpp —— QUIC 密钥表（RFC 9001 §5.1/§5.2）的推导用例
//
// 覆盖四块：
//   1) Initial 密钥：RFC 9001 附录 A.1 给的就是这条链路——从目的连接标识 8394c8f03e515708 抽出
//      PRK，再按 "client in"/"server in" 各自扩出 32 字节秘密，最后导出 "quic key"/"quic iv"/"quic hp"
//      三段；六个期望值逐字节比，盐、前缀、标签任一抄错都会立刻对不上；
//   2) 1-RTT/Handshake 通路：附录 A.5 的 ChaCha20 三件套（同一套标签、密钥 32 字节档），
//      证明 deriveQuicPacketKeys 的长度与哈希是按套件走的；
//   3) 套件参数表：密钥/头部保护密钥/流量秘密三个长度取值；
//   4) 拒绝面：流量秘密长度与套件不符（拿错方向的秘密是最容易犯的错）、以及零长连接标识
//      （服务端发了零长 SCID 的 Retry 之后会出现）也要能正常导出。
// 用例全是纯计算，不建 SSL、不起网络，因此没有时序与外部依赖问题。

#include "Net/Quic/Crypto/QuicKeySchedule.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;

        /// 附录 A.1 与 A.3 的样例目的连接标识：Initial 密钥的输入就是它
        const std::vector<std::uint8_t> kSampleDestinationConnectionId = makeBytesFromHex("8394c8f03e515708");

        /**
         * @brief 把密钥视图拷成可比对的容器
         * @param bytes 密钥字节视图
         * @return std::vector<std::uint8_t> 内容副本
         */
        std::vector<std::uint8_t> asVector(const std::span<const std::uint8_t> bytes)
        {
            return {bytes.begin(), bytes.end()};
        }
    } // namespace

    /**
     * @brief 客户端方向的 Initial 三件套必须逐字节等于附录 A.1
     */
    TEST(QuicKeySchedule, DerivesClientInitialKeysPerAppendixA1)
    {
        const auto keys = deriveQuicInitialPacketKeys(kSampleDestinationConnectionId, QuicPacketDirection::ClientToServer);
        EXPECT_EQ(keys.cipherSuite, QuicCipherSuite::Aes128Gcm) << "Initial 一律用 AES_128_GCM，与协商结果无关（RFC 9001 §5.2）";
        EXPECT_EQ(asVector(keys.encryptionKeyBytes()), makeBytesFromHex("1f369613dd76d5467730efcbe3b1a22d"));
        EXPECT_EQ(asVector(keys.initializationVectorBytes()), makeBytesFromHex("fa044b2f42a3fd3b46fb255c"));
        EXPECT_EQ(asVector(keys.headerProtectionKeyBytes()), makeBytesFromHex("9f50449e04a0e810283a1e9933adedd2"));
    }

    /**
     * @brief 服务端方向的 Initial 三件套必须逐字节等于附录 A.1，且与客户端方向完全分离
     */
    TEST(QuicKeySchedule, DerivesServerInitialKeysPerAppendixA1)
    {
        const auto keys = deriveQuicInitialPacketKeys(kSampleDestinationConnectionId, QuicPacketDirection::ServerToClient);
        EXPECT_EQ(asVector(keys.encryptionKeyBytes()), makeBytesFromHex("cf3a5331653c364c88f0f379b6067e37"));
        EXPECT_EQ(asVector(keys.initializationVectorBytes()), makeBytesFromHex("0ac1493ca1905853b0bba03e"));
        EXPECT_EQ(asVector(keys.headerProtectionKeyBytes()), makeBytesFromHex("c206b8d9b9f0f37644430b490eeaa314"));

        // 同一目的标识下两个方向的密钥必须不同：若哪天盐或标签被写成与方向无关，这里会红
        const auto clientKeys = deriveQuicInitialPacketKeys(kSampleDestinationConnectionId, QuicPacketDirection::ClientToServer);
        EXPECT_NE(asVector(keys.encryptionKeyBytes()), asVector(clientKeys.encryptionKeyBytes()));
    }

    /**
     * @brief 零长目的标识（服务端发了带零长 SCID 的 Retry 之后）也要能正常导出密钥
     */
    TEST(QuicKeySchedule, DerivesInitialKeysForEmptyDestinationConnectionId)
    {
        const auto keys = deriveQuicInitialPacketKeys(std::span<const std::uint8_t>{}, QuicPacketDirection::ClientToServer);
        EXPECT_EQ(keys.encryptionKeyBytes().size(), 16U);
        EXPECT_EQ(keys.initializationVectorBytes().size(), 12U);
        EXPECT_EQ(keys.headerProtectionKeyBytes().size(), 16U);
    }

    /**
     * @brief 1-RTT 通路：附录 A.5 的 ChaCha20 三件套，密钥与头部保护密钥都是 32 字节档
     */
    TEST(QuicKeySchedule, DerivesChaCha20KeyTriplePerAppendixA5)
    {
        const auto secret = makeBytesFromHex("9ac312a7f877468ebe69422748ad00a1"
                                             "5443f18203a07d6060f688f30f21632b");
        const auto keys   = deriveQuicPacketKeys(QuicCipherSuite::ChaCha20Poly1305, secret);
        EXPECT_EQ(asVector(keys.encryptionKeyBytes()), makeBytesFromHex("c6d98ff3441c3fe1b2182094f69caa2e"
                                                                        "d4b716b65488960a7a984979fb23e1c8"));
        EXPECT_EQ(asVector(keys.initializationVectorBytes()), makeBytesFromHex("e0459b3474bdd0e44a41c144"));
        EXPECT_EQ(asVector(keys.headerProtectionKeyBytes()), makeBytesFromHex("25a282b9e82f06f21f488917a4fc8f1b"
                                                                              "73573685608597d0efcb076b0ab7a7a4"));
    }

    /**
     * @brief 三个长度表：密钥/头部保护密钥/流量秘密按套件取值，抄错会让导出长度整体错位
     */
    /**
     * @brief 密钥更新的递推：「quic ku」出来的下一代秘密对上附录 A.5 的取值，头部保护密钥不跟着换
     */
    TEST(QuicKeySchedule, AdvancesApplicationSecretPerAppendixA5)
    {
        const auto secret  = makeBytesFromHex("9ac312a7f877468ebe69422748ad00a1"
                                              "5443f18203a07d6060f688f30f21632b");
        const auto current = deriveQuicPacketKeys(QuicCipherSuite::ChaCha20Poly1305, secret);
        const auto updated = deriveQuicUpdatedPacketKeys(current);

        const auto                      expectedNextSecret = makeBytesFromHex("1223504755036d556342ee9361d25342"
                                                                              "1a826c9ecdf3c7148684b36b714881f9");
        const std::vector<std::uint8_t> actualNextSecret(updated.generationSecretBytes().begin(), updated.generationSecretBytes().end());
        EXPECT_EQ(actualNextSecret, expectedNextSecret) << "下一代流量秘密与 RFC 9001 附录 A.5 的 ku 不符";
        // §6.1：只有 AEAD 密钥与 IV 换，头部保护密钥原样带走
        EXPECT_EQ(updated.headerProtectionKey, current.headerProtectionKey);
        EXPECT_NE(updated.encryptionKey, current.encryptionKey);
        EXPECT_NE(updated.initializationVector, current.initializationVector);
        EXPECT_EQ(updated.cipherSuite, current.cipherSuite);
    }

    TEST(QuicKeySchedule, CipherSuiteLengthsMatchRfc9001)
    {
        EXPECT_EQ(quicCipherSuiteKeyByteLength(QuicCipherSuite::Aes128Gcm), 16U);
        EXPECT_EQ(quicCipherSuiteKeyByteLength(QuicCipherSuite::Aes256Gcm), 32U);
        EXPECT_EQ(quicCipherSuiteKeyByteLength(QuicCipherSuite::ChaCha20Poly1305), 32U);

        EXPECT_EQ(quicCipherSuiteHeaderProtectionKeyByteLength(QuicCipherSuite::Aes256Gcm), 32U);
        EXPECT_EQ(quicCipherSuiteHeaderProtectionKeyByteLength(QuicCipherSuite::Aes128Gcm), 16U);

        // 只有 TLS_AES_256_GCM_SHA384 走 SHA-384，其余三档的哈希都是 32 字节
        EXPECT_EQ(quicCipherSuiteSecretByteLength(QuicCipherSuite::Aes256Gcm), 48U);
        EXPECT_EQ(quicCipherSuiteSecretByteLength(QuicCipherSuite::Aes128Gcm), 32U);
        EXPECT_EQ(quicCipherSuiteSecretByteLength(QuicCipherSuite::ChaCha20Poly1305), 32U);
    }

    /**
     * @brief 流量秘密长度与套件不符时当场拒绝：交错方向的秘密不该被当成合法输入继续导出
     */
    TEST(QuicKeySchedule, RejectsTrafficSecretWithMismatchedLength)
    {
        const auto sha256SizedSecret = makeBytesFromHex("9ac312a7f877468ebe69422748ad00a1"
                                                        "5443f18203a07d6060f688f30f21632b");
        EXPECT_THROW(static_cast<void>(deriveQuicPacketKeys(QuicCipherSuite::Aes256Gcm, sha256SizedSecret)), Base::InvalidArgumentException);

        const auto shortSecret = makeBytesFromHex("00112233");
        EXPECT_THROW(static_cast<void>(deriveQuicPacketKeys(QuicCipherSuite::Aes128Gcm, shortSecret)), Base::InvalidArgumentException);
    }
} // namespace AsynGyanis::Net
