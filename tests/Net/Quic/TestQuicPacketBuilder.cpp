// TestQuicPacketBuilder.cpp —— QUIC 发包组包（RFC 9001 §5.3/§5.4）的用例
//
// 覆盖四块：
//   1) 端到端字节比对：附录 A.2 的客户端 Initial（1200 字节）与附录 A.5 的 ChaCha20 短头包（21 字节）。
//      组包器一次要过四层——头部字段编码、Length 域、AEAD、头部保护掩码——所以这两条相等就是
//      四层的联合证明，任何一层错位都会整包对不上；
//   2) 补 PADDING：包号短到取不满 16 字节样本时，凑长度的 PADDING 帧必须在**密文内部**，
//      因此这条用「组包 → 解码 → 去保护 → 解密」的完整回路验，看到明文尾部的 0x00 才算数；
//   3) 合包：一个数据报里连发 Initial + Handshake，第二个包要能被第一个包的 packetByteCount 定位到；
//   4) 拒绝面：空帧序列、包号字节数越界、连接标识超 20 字节、给非 Initial 带 Token、包号超 62 位上限。
// 用例不建 SSL、不起网络，纯计算。

#include "Net/Quic/QuicPacketBuilder.h"

#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicKeySchedule.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"
#include "QuicRfcVectors.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::buildAppendixA2Plaintext;
        using AsynGyanis::Net::TestSupport::kAppendixA2AdditionalDataHex;
        using AsynGyanis::Net::TestSupport::kAppendixA2PacketNumber;
        using AsynGyanis::Net::TestSupport::kAppendixA2ProtectedPacketHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5AdditionalDataHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5HeaderProtectionKeyHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5InitializationVectorHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5KeyHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5PacketNumber;
        using AsynGyanis::Net::TestSupport::kAppendixA5PlaintextHex;
        using AsynGyanis::Net::TestSupport::kAppendixA5ProtectedPacketHex;
        using AsynGyanis::Net::TestSupport::kVectorDestinationConnectionIdHex;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using AsynGyanis::Net::TestSupport::toUnsignedBytes;

        /**
         * @brief 把一组三件套密钥按十六进制摆好
         * @param cipherSuite 套件
         * @param keyHex AEAD 密钥
         * @param initializationVectorHex 12 字节 IV
         * @param headerProtectionKeyHex 头部保护密钥
         * @return QuicPacketKeys 密钥组
         */
        QuicPacketKeys makeFullKeys(const QuicCipherSuite cipherSuite, const std::string_view keyHex,
                                    const std::string_view initializationVectorHex, const std::string_view headerProtectionKeyHex)
        {
            QuicPacketKeys keys;
            keys.cipherSuite = cipherSuite;
            const auto keyBytes = makeBytesFromHex(keyHex);
            const auto vectorBytes = makeBytesFromHex(initializationVectorHex);
            const auto protectionKeyBytes = makeBytesFromHex(headerProtectionKeyHex);
            std::copy(keyBytes.begin(), keyBytes.end(), keys.encryptionKey.begin());
            std::copy(vectorBytes.begin(), vectorBytes.end(), keys.initializationVector.begin());
            std::copy(protectionKeyBytes.begin(), protectionKeyBytes.end(), keys.headerProtectionKey.begin());
            return keys;
        }

        /// 附录 A.5 那组 ChaCha20 密钥（密钥、IV、头部保护密钥都取自 RFC 原文）
        QuicPacketKeys chacha20Keys()
        {
            return makeFullKeys(QuicCipherSuite::ChaCha20Poly1305, kAppendixA5KeyHex, kAppendixA5InitializationVectorHex,
                                kAppendixA5HeaderProtectionKeyHex);
        }

        /**
         * @brief 把一个完整包从数据报里解回明文
         * @details 走的是「解码 → 去头部保护 → 解密」三步，与组包器互为逆运算；
         *          补 PADDING 是否真落在密文内部，只有走这一条路才看得见。
         * @param datagram 已编好的数据报字节
         * @param keys 该方向的密钥组
         * @param localConnectionIdLength 本端签发的连接标识长度（短头报文必须给）
         * @param packetOffset 本包在数据报里的起始偏移
         * @return std::vector<std::uint8_t> 解出的明文帧；任何一步失败都返回空
         */
        std::vector<std::uint8_t> openPacketAt(const std::vector<std::uint8_t> &datagram, const QuicPacketKeys &keys,
                                               const std::size_t localConnectionIdLength, const std::size_t packetOffset)
        {
            const std::span<const std::uint8_t> packetSpan(datagram.data() + packetOffset, datagram.size() - packetOffset);
            const auto decoded = decodeQuicPacketHeader(packetSpan, localConnectionIdLength);
            if (!decoded.has_value())
            {
                return {};
            }
            const auto sample = extractQuicHeaderProtectionSample(packetSpan, *decoded);
            if (!sample.has_value())
            {
                return {};
            }
            std::vector<std::uint8_t> mutablePacket(packetSpan.begin(), packetSpan.end());
            const QuicHeaderProtectionMask mask = generateQuicHeaderProtectionMask(keys, *sample);
            QuicPacketHeader header = *decoded;
            const auto unmaskedFirstByte = removeQuicHeaderProtection(mutablePacket, header, mask);
            if (!unmaskedFirstByte.has_value() ||
                !refreshQuicPacketHeader(header, *unmaskedFirstByte, mutablePacket).has_value())
            {
                return {};
            }
            // 附加认证数据 = 未保护头部到包号末尾，取去保护之后的字节才对得上
            const std::span<const std::uint8_t> additionalData(mutablePacket.data(),
                                                               header.packetNumberOffset + header.packetNumberByteCount);
            const std::span<const std::uint8_t> protectedPayload(mutablePacket.data() + header.packetNumberOffset +
                                                                         header.packetNumberByteCount,
                                                                 header.packetNumberAndPayloadByteCount - header.packetNumberByteCount);
            std::vector<std::uint8_t> plaintext(protectedPayload.size() - kQuicAuthenticationTagByteLength);
            if (!openQuicProtectedPayload(plaintext, keys, header.packetNumber, additionalData, protectedPayload).has_value())
            {
                return {};
            }
            return plaintext;
        }
    } // namespace

    /**
     * @brief 附录 A.2 的客户端 Initial：组出来的 1200 字节必须与 RFC 逐字节相同
     */
    TEST(QuicPacketBuilder, BuildsAppendixA2ClientInitialByteForByte)
    {
        const auto destinationConnectionId = makeBytesFromHex(kVectorDestinationConnectionIdHex);
        const auto keys = deriveQuicInitialPacketKeys(destinationConnectionId, QuicPacketDirection::ClientToServer);
        const auto plaintext = buildAppendixA2Plaintext();

        QuicOutboundPacket packet;
        packet.destinationConnectionId = destinationConnectionId;
        packet.packetNumber = kAppendixA2PacketNumber;
        packet.packetNumberByteCount = 4;
        packet.frames = plaintext;

        std::string datagram;
        appendQuicPacket(datagram, packet, keys);

        EXPECT_EQ(toUnsignedBytes(datagram), makeBytesFromHex(kAppendixA2ProtectedPacketHex))
                << "头部编码、Length 域、AEAD 与头部保护四层里任一处错位，整包都对不上";
    }

    /**
     * @brief 附录 A.5 的 ChaCha20 短头包：空目的标识 + 3 字节包号，21 字节逐字节相同
     */
    TEST(QuicPacketBuilder, BuildsAppendixA5ShortPacketByteForByte)
    {
        const auto plaintext = makeBytesFromHex(kAppendixA5PlaintextHex);
        QuicOutboundPacket packet;
        packet.isLongHeader = false;
        packet.packetNumber = kAppendixA5PacketNumber;
        packet.packetNumberByteCount = 3;
        packet.frames = plaintext;

        std::string datagram;
        appendQuicPacket(datagram, packet, chacha20Keys());

        EXPECT_EQ(toUnsignedBytes(datagram), makeBytesFromHex(kAppendixA5ProtectedPacketHex));
        // 首字节的包号长度位（0x03 掩出来是 10）说明写的是 3 字节包号，短头的标识由连接本身决定
        EXPECT_EQ(makeBytesFromHex(kAppendixA5AdditionalDataHex).size(), 4U);
    }

    /**
     * @brief 包号短到会缺样本时补 PADDING 帧，且补齐部分必须在密文内部
     */
    TEST(QuicPacketBuilder, PadsShortPacketsInsideTheEncryptedPayload)
    {
        const auto destinationConnectionId = makeBytesFromHex(kVectorDestinationConnectionIdHex);
        const auto keys = deriveQuicInitialPacketKeys(destinationConnectionId, QuicPacketDirection::ServerToClient);
        const auto pingFrame = makeBytesFromHex("01");

        QuicOutboundPacket packet;
        packet.isLongHeader = false;
        packet.destinationConnectionId = destinationConnectionId;
        packet.packetNumber = 1;
        packet.packetNumberByteCount = 1;
        packet.frames = pingFrame;

        std::string datagram;
        appendQuicPacket(datagram, packet, keys);

        const auto bytes = toUnsignedBytes(datagram);
        // 1 字节首字节 + 8 字节目的标识 + 1 字节包号 + 3 字节明文（PING + 补的两个 PADDING）+ 16 字节标签
        EXPECT_EQ(bytes.size(), 1U + 8U + 1U + 3U + kQuicAuthenticationTagByteLength) << "补 PADDING 后整包长度";

        const auto plaintext = openPacketAt(bytes, keys, destinationConnectionId.size(), 0);
        ASSERT_EQ(plaintext.size(), 3U) << "去保护并解密后要拿到补齐的 3 字节明文";
        EXPECT_EQ(plaintext, makeBytesFromHex("010000")) << "PING 之后要有两个 PADDING 帧，且它们在密文里";
    }

    /**
     * @brief 合包：同一数据报里连发 Initial 与 Handshake，第二个包要能按第一包的长度定位
     */
    TEST(QuicPacketBuilder, CoalescesLongHeaderPacketsIntoOneDatagram)
    {
        const auto keys = deriveQuicInitialPacketKeys(makeBytesFromHex(kVectorDestinationConnectionIdHex),
                                                      QuicPacketDirection::ServerToClient);
        const auto cryptoFrame = makeBytesFromHex("06000141");
        // 目的/源标识要先存进具名容器：QuicOutboundPacket 存的是视图，直接从临时量赋值会在
        // 语句结束时悬空（ASan 实测 heap-use-after-free）
        const auto peerConnectionId = makeBytesFromHex(kVectorDestinationConnectionIdHex);
        const auto localConnectionId = makeBytesFromHex("f0eec687a7eb7f48");

        QuicOutboundPacket initial;
        initial.longPacketType = QuicLongPacketType::Initial;
        initial.destinationConnectionId = peerConnectionId;
        initial.sourceConnectionId = localConnectionId;
        initial.packetNumber = 0;
        initial.frames = cryptoFrame;

        QuicOutboundPacket handshake;
        handshake.longPacketType = QuicLongPacketType::Handshake;
        handshake.destinationConnectionId = peerConnectionId;
        handshake.sourceConnectionId = localConnectionId;
        handshake.packetNumber = 1;
        handshake.frames = cryptoFrame;

        std::string datagram;
        appendQuicPacket(datagram, initial, keys);
        const std::size_t initialByteCount = datagram.size();
        appendQuicPacket(datagram, handshake, keys);

        const auto bytes = toUnsignedBytes(datagram);
        const auto firstPlaintext = openPacketAt(bytes, keys, 0, 0);
        ASSERT_FALSE(firstPlaintext.empty()) << "第一个包解不开";
        EXPECT_EQ(firstPlaintext, cryptoFrame);

        // 第二包的起点只能由第一包的 packetByteCount 给出，这一条同时验 Length 域算得对
        const auto secondPlaintext = openPacketAt(bytes, keys, 0, initialByteCount);
        ASSERT_FALSE(secondPlaintext.empty()) << "第二个包解不开，说明第一包的 Length 与真实长度不一致";
        EXPECT_EQ(secondPlaintext, cryptoFrame);
        // Initial 多一个 Token 长度字段，两包本来就不等长；要看的是第一包的 Length 域
        // 是否等于它的真实长度——对端定位第二包全靠这个数
        const auto firstHeader = decodeQuicPacketHeader(bytes, 0);
        ASSERT_TRUE(firstHeader.has_value()) << firstHeader.error().message;
        EXPECT_EQ(firstHeader->packetByteCount, initialByteCount) << "Length 域与真实长度不一致，对端就找不到下一个包";
    }

    /**
     * @brief 五个非法的明文侧描述当场抛，不发对端必然判错的包
     */
    TEST(QuicPacketBuilder, RejectsInvalidOutboundPackets)
    {
        const auto destinationConnectionId = makeBytesFromHex(kVectorDestinationConnectionIdHex);
        const auto keys = deriveQuicInitialPacketKeys(destinationConnectionId, QuicPacketDirection::ServerToClient);
        const auto frame = makeBytesFromHex("01");
        const auto oversizedConnectionId = makeBytesFromHex("00112233445566778899aabbccddeeff0011223344");
        const auto token = makeBytesFromHex("aabb");

        QuicOutboundPacket packet;
        packet.destinationConnectionId = destinationConnectionId;
        packet.frames = frame;

        std::string datagram;
        packet.frames = {};
        EXPECT_THROW(appendQuicPacket(datagram, packet, keys), Base::InvalidArgumentException);
        packet.frames = frame;

        packet.packetNumberByteCount = 5;
        EXPECT_THROW(appendQuicPacket(datagram, packet, keys), Base::InvalidArgumentException);
        packet.packetNumberByteCount = 1;

        packet.destinationConnectionId = oversizedConnectionId;
        EXPECT_THROW(appendQuicPacket(datagram, packet, keys), Base::InvalidArgumentException);
        packet.destinationConnectionId = destinationConnectionId;

        // 只有 Initial 的长头里有 Token 字段
        packet.longPacketType = QuicLongPacketType::Handshake;
        packet.token = token;
        EXPECT_THROW(appendQuicPacket(datagram, packet, keys), Base::InvalidArgumentException);
        packet.longPacketType = QuicLongPacketType::Initial;
        packet.token = {};

        packet.packetNumber = kQuicMaximumIntegerValue + 1ULL;
        EXPECT_THROW(appendQuicPacket(datagram, packet, keys), Base::InvalidArgumentException);
        EXPECT_TRUE(datagram.empty()) << "校验在建头之前，五种非法输入都不该留下半个字节";
    }
} // namespace AsynGyanis::Net
