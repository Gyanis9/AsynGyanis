#include "Net/Quic/QuicPacketBuilder.h"

#include "Base/Exception/Exception.h"
#include "Net/Quic/Codec/QuicRawBytes.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"
#include "Net/Quic/Crypto/QuicHeaderProtection.h"
#include "Net/Quic/Crypto/QuicPacketProtection.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <ranges>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// PADDING 帧的类型字节：用它凑够头部保护的取样长度，对端只跳过不解释（RFC 9000 §19.1）
        inline constexpr std::uint8_t kQuicPaddingFrameType = 0x00;

        /// 长头首字节的固定位：头部形式位 0x80 + 固定位 0x40（RFC 9000 §17.2）
        inline constexpr std::uint8_t kQuicLongHeaderFirstByteBase = kQuicLongHeaderFlagBit | kQuicFixedBit;

        /// 长头类型值在首字节里的位移：表 5 的 2 位放在 0x30
        inline constexpr std::uint8_t kQuicLongPacketTypeShift = 4;

        /// Retry 完整性标签的 AEAD 密钥（RFC 9001 §5.8 为版本 1 定死的 16 字节取值）
        inline constexpr std::array<std::uint8_t, 16> kQuicRetryIntegrityKeyBytes{0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e};

        /// Retry 完整性标签的 AEAD 初始化向量（同节定死的 12 字节取值）
        inline constexpr std::array<std::uint8_t, kQuicInitializationVectorByteLength> kQuicRetryIntegrityVectorBytes{0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63,
                                                                                                                      0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};

        /// Retry 首字节取 0xFF：头部形式位与固定位按 §17.2 置起、类型位是 3，剩下 4 位是 Unused——
        /// 规范说这一档由服务端任意定、客户端 MUST 忽略（§17.2.5）。取全 1 是为了与 RFC 9001 附录 A.4
        /// 那条官方样例逐字相同：完整性标签的关联数据含这一字节，取别的值就没法拿官方向量钉住这条报文
        inline constexpr std::uint8_t kQuicRetryFirstByte =
                kQuicLongHeaderFirstByteBase | (static_cast<std::uint8_t>(QuicLongPacketType::Retry) << kQuicLongPacketTypeShift) | 0x0FU;

        /// 短头首字节的固定位：只有固定位 0x40（头部形式位为 0，RFC 9000 §17.3.1）
        inline constexpr std::uint8_t kQuicShortHeaderFirstByteBase = kQuicFixedBit;

        /**
         * @brief 算出让「包号 + 受保护载荷」够取 16 字节样本所需的明文最短长度
         * @details 样本从「包号起点 + 4」起取 16 字节，而密文 = 明文 + 16 字节标签，因此要求
         *          包号长度 + 明文长度 + 16 ≥ 4 + 16，即明文至少 4 − 包号长度字节（RFC 9001 §5.4.2）。
         *          不够就补 PADDING 帧，而不是把样本不足的包发出去让对端整包丢。
         * @param packetNumberByteCount 包号字段字节数（1..4）
         * @return std::size_t 明文的最小字节数
         */
        std::size_t minimumPlaintextByteCount(const std::size_t packetNumberByteCount) noexcept
        {
            return packetNumberByteCount >= kQuicMaximumPacketNumberByteCount ? 0 : kQuicMaximumPacketNumberByteCount - packetNumberByteCount;
        }

        /**
         * @brief 算出一条出站包会往缓冲里追加多少字节
         * @details 与 `appendQuicPacket` 的写入顺序逐项对应，用来一次预留到位。算多算少都只影响
         *          这次预留的收益，不影响线上字节。
         * @param packet 明文侧描述
         * @param plaintextByteCount 补齐 PADDING 之后的明文长度
         * @return std::size_t 本包的总字节数
         */
        std::size_t outboundPacketByteCount(const QuicOutboundPacket &packet, const std::size_t plaintextByteCount) noexcept
        {
            // Length 域覆盖的正是「包号 + 密文 + 标签」这三项，与下面要写的字节数同源
            const std::size_t protectedPayloadByteCount = packet.packetNumberByteCount + plaintextByteCount + kQuicAuthenticationTagByteLength;
            // 首字节与目的标识两种头部都要写；目的标识的长度字节只有长头才有
            std::size_t byteCount = 1U + packet.destinationConnectionId.size();
            if (!packet.isLongHeader)
            {
                return byteCount + protectedPayloadByteCount;
            }
            byteCount += 4U + 1U + 1U + packet.sourceConnectionId.size() + quicVariableLengthIntegerByteCount(protectedPayloadByteCount);
            if (packet.longPacketType == QuicLongPacketType::Initial)
            {
                byteCount += quicVariableLengthIntegerByteCount(packet.token.size()) + packet.token.size();
            }
            return byteCount + protectedPayloadByteCount;
        }

        /**
         * @brief 把首字节写成线上要的样子（保护前）
         * @param packet 明文侧描述
         * @param packetNumberByteCount 包号字段字节数
         * @return std::uint8_t 首字节；保留位按 §17.2 要求为 0
         */
        std::uint8_t makeFirstByte(const QuicOutboundPacket &packet, const std::size_t packetNumberByteCount) noexcept
        {
            const std::uint8_t packetNumberLengthBits = static_cast<std::uint8_t>(packetNumberByteCount - 1);
            if (!packet.isLongHeader)
            {
                return static_cast<std::uint8_t>(kQuicShortHeaderFirstByteBase | (packet.isKeyPhaseBitSet ? kQuicKeyPhaseBitMask : 0) | packetNumberLengthBits);
            }
            return static_cast<std::uint8_t>(kQuicLongHeaderFirstByteBase | (static_cast<std::uint8_t>(packet.longPacketType) << kQuicLongPacketTypeShift) |
                                             packetNumberLengthBits);
        }

        /**
         * @brief 校验明文侧描述能否落到线上
         * @param packet 明文侧描述
         * @throws Base::InvalidArgumentException 任一字段超出 v1 允许的范围
         */
        void validateOutboundPacket(const QuicOutboundPacket &packet)
        {
            // 载荷为空会产出一个「没有帧」的包，对端按 §12.4 必须判 PROTOCOL_VIOLATION
            if (packet.frames.empty())
            {
                throw Base::InvalidArgumentException("QUIC 组包失败：载荷至少要含一个帧（RFC 9000 §12.4）："
                                                     "只想填充的话请发一个 PADDING 帧，别交空序列");
            }
            if (packet.packetNumber > kQuicMaximumIntegerValue)
            {
                throw Base::InvalidArgumentException(std::format("QUIC 组包失败：包号 {} 超过 2^62-1 的上限（RFC 9000 §12.3）："
                                                                 "这条连接该按收包号上限处理，不能继续发",
                                                                 packet.packetNumber));
            }
            if (packet.packetNumberByteCount < 1 || packet.packetNumberByteCount > kQuicMaximumPacketNumberByteCount)
            {
                throw Base::InvalidArgumentException(
                        std::format("QUIC 组包失败：包号字段字节数 {} 不在 1..{} 内（RFC 9000 §17.1）", packet.packetNumberByteCount, kQuicMaximumPacketNumberByteCount));
            }
            if (packet.destinationConnectionId.size() > kQuicMaximumConnectionIdLength ||
                (packet.isLongHeader && packet.sourceConnectionId.size() > kQuicMaximumConnectionIdLength))
            {
                throw Base::InvalidArgumentException(std::format("QUIC 组包失败：连接标识长度（目的 {} 字节、源 {} 字节）超过版本 1 的上限 {} 字节"
                                                                 "（RFC 9000 §5.1.1、§17.2）",
                                                                 packet.destinationConnectionId.size(), packet.sourceConnectionId.size(), kQuicMaximumConnectionIdLength));
            }
            // 只有 Initial 有线上的 Token 字段；给 Handshake/0-RTT 带 Token 会把后面的字段整体错位
            if (!packet.token.empty() && (!packet.isLongHeader || packet.longPacketType != QuicLongPacketType::Initial))
            {
                throw Base::InvalidArgumentException(std::format("QUIC 组包失败：给{}带了 {} 字节 Token，而 Token 只存在于 Initial 的长头里"
                                                                 "（RFC 9000 §17.2.2 图 15）",
                                                                 packet.isLongHeader ? "长头" : "短头", packet.token.size()));
            }
        }
        /// 校验 Retry 的明文侧描述能否落到线上（与 validateOutboundPacket 同一手法：先全数查完再动缓冲）
        void validateRetryPacket(const QuicRetryPacket &packet)
        {
            if (packet.destinationConnectionId.size() > kQuicMaximumConnectionIdLength || packet.sourceConnectionId.size() > kQuicMaximumConnectionIdLength ||
                packet.originalDestinationConnectionId.size() > kQuicMaximumConnectionIdLength)
            {
                throw Base::InvalidArgumentException(std::format("QUIC Retry 组包失败：连接标识长度（目的 {} 字节、源 {} 字节、原始目的 {} 字节）超过版本 1 的"
                                                                 "上限 {} 字节（RFC 9000 §5.1.1、§17.2）",
                                                                 packet.destinationConnectionId.size(), packet.sourceConnectionId.size(),
                                                                 packet.originalDestinationConnectionId.size(), kQuicMaximumConnectionIdLength));
            }
            // 零长令牌的 Retry 客户端必须丢（§17.2.5.2），发出去只是白跑一趟，还多喂对端一个包
            if (packet.token.empty())
            {
                throw Base::InvalidArgumentException("QUIC Retry 组包失败：地址验证令牌为空（RFC 9000 §17.2.5.2 要求客户端丢弃这种 Retry）："
                                                     "请先用 QuicAddressValidationTokenCodec 签一张，再组这条报文");
            }
            // 本端新选的标识与客户端 Initial 的目的标识相同就等于没换：§17.2.5.1 的 MUST NOT，
            // 换标识正是这条报文「本端确实收过第一包」的凭据之一
            if (packet.sourceConnectionId.size() == packet.originalDestinationConnectionId.size() &&
                std::ranges::equal(packet.sourceConnectionId, packet.originalDestinationConnectionId))
            {
                throw Base::InvalidArgumentException(std::format("QUIC Retry 组包失败：本端新选的源连接标识与客户端 Initial 的目的标识逐字节相同（{} 字节），"
                                                                 "违反 RFC 9000 §17.2.5.1：请另取一条随机标识",
                                                                 packet.sourceConnectionId.size()));
            }
        }

        /**
         * @brief Retry 完整性标签用的那把密钥与 IV（RFC 9001 §5.8 为版本 1 定死的两个常量）
         * @details 本端别处的包密钥都由 TLS 握手算出，唯独这一条与连接无关：标签的作用是让客户端确认
         *          「这条 Retry 出自看过我第一个 Initial 的那台服务端」，而 Initial 此刻还不存在，
         *          所以规范直接把取值写死在版本里。伪包与空明文的算法仍走公共的 AEAD 出口，不在这里重抄一遍。
         */
        const QuicPacketKeys &retryIntegrityKeys()
        {
            static const QuicPacketKeys keys = []
            {
                QuicPacketKeys retryKeys{};
                retryKeys.cipherSuite = QuicCipherSuite::Aes128Gcm;
                std::ranges::copy(kQuicRetryIntegrityKeyBytes, retryKeys.encryptionKey.begin());
                std::ranges::copy(kQuicRetryIntegrityVectorBytes, retryKeys.initializationVector.begin());
                return retryKeys;
            }();
            return keys;
        }
    } // namespace

    void appendQuicPacket(std::string &datagram, const QuicOutboundPacket &packet, const QuicPacketKeys &keys)
    {
        validateOutboundPacket(packet);

        const std::size_t  packetNumberByteCount = packet.packetNumberByteCount;
        const std::uint8_t firstByte             = makeFirstByte(packet, packetNumberByteCount);

        // 明文不够取样本时补 PADDING 帧；补多少要先定下来，Length 域才有确定的值可写
        std::vector<std::uint8_t>     paddedPlaintext;
        std::span<const std::uint8_t> plaintext                  = packet.frames;
        const std::size_t             requiredPlaintextByteCount = minimumPlaintextByteCount(packetNumberByteCount);
        if (plaintext.size() < requiredPlaintextByteCount)
        {
            paddedPlaintext.assign(plaintext.begin(), plaintext.end());
            paddedPlaintext.resize(requiredPlaintextByteCount, kQuicPaddingFrameType);
            plaintext = std::span<const std::uint8_t>(paddedPlaintext);
        }

        const std::size_t packetStartOffset = datagram.size();
        // 一次预留到本包的精确长度：这条路径每包都走，交给几何扩容就是把整包重复拷贝三四遍
        datagram.reserve(packetStartOffset + outboundPacketByteCount(packet, plaintext.size()));
        datagram.push_back(static_cast<char>(firstByte));
        if (packet.isLongHeader)
        {
            for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
            {
                // 版本是 32 位定长大端，不是变长整数（§16 末段把版本排除在外）
                datagram.push_back(static_cast<char>((packet.version >> ((3 - byteIndex) * 8)) & 0xFFU));
            }
            // 只有长头带标识长度字节：短头的目的标识长度由连接本身约定（RFC 9000 §17.3.1）
            datagram.push_back(static_cast<char>(packet.destinationConnectionId.size()));
        }
        appendQuicRawBytes(datagram, packet.destinationConnectionId);
        if (packet.isLongHeader)
        {
            datagram.push_back(static_cast<char>(packet.sourceConnectionId.size()));
            appendQuicRawBytes(datagram, packet.sourceConnectionId);
            if (packet.longPacketType == QuicLongPacketType::Initial)
            {
                appendQuicVariableLengthInteger(datagram, packet.token.size());
                appendQuicRawBytes(datagram, packet.token);
            }
            // Length 覆盖「包号 + 密文 + 标签」，此刻三项长度都已知，因此不需要先占位再回填
            appendQuicVariableLengthInteger(datagram, packetNumberByteCount + plaintext.size() + kQuicAuthenticationTagByteLength);
        }

        const std::size_t packetNumberOffset = datagram.size() - packetStartOffset;
        appendQuicTruncatedPacketNumber(datagram, packet.packetNumber, packetNumberByteCount);

        // 加密会让 datagram 扩容，所以 AAD 必须交一份副本而不是指向它的 span。短头与不带 Token 的
        // 长头都在 64 字节内，走栈上的定长缓冲；Initial 的 Token 可能很长，超出 64 字节时才退回堆
        constexpr std::size_t                           kStackAdditionalDataByteCount = 64;
        const std::size_t                               additionalDataByteCount       = datagram.size() - packetStartOffset;
        std::array<char, kStackAdditionalDataByteCount> stackAdditionalData{};
        std::string                                     heapAdditionalData;
        std::span<const std::uint8_t>                   additionalData;
        if (additionalDataByteCount <= kStackAdditionalDataByteCount)
        {
            std::memcpy(stackAdditionalData.data(), datagram.data() + packetStartOffset, additionalDataByteCount);
            additionalData = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(stackAdditionalData.data()), additionalDataByteCount);
        } else
        {
            heapAdditionalData = datagram.substr(packetStartOffset);
            additionalData     = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(heapAdditionalData.data()), heapAdditionalData.size());
        }
        appendQuicProtectedPayload(datagram, keys, packet.packetNumber, additionalData, plaintext);

        // 加密之后才能取样本：此刻本包的全部字节都在缓冲尾部
        std::span<std::uint8_t> builtPacket(reinterpret_cast<std::uint8_t *>(datagram.data()) + packetStartOffset, datagram.size() - packetStartOffset);
        QuicPacketHeader        header;
        header.isLongHeader       = packet.isLongHeader;
        header.firstByte          = firstByte;
        header.packetNumberOffset = packetNumberOffset;

        const auto sample = extractQuicHeaderProtectionSample(builtPacket, header);
        if (!sample.has_value())
        {
            // 上面已经按 §5.4.2 补过 PADDING，走不到这里；真走到说明长度算错了，不能悄悄发一个解不开的包
            throw Base::Exception(std::format("QUIC 组包失败：补齐 PADDING 之后仍取不满头部保护样本（{}）：请核对包号与载荷长度的算法", sample.error().message));
        }
        const QuicHeaderProtectionMask mask = generateQuicHeaderProtectionMask(keys, *sample);
        if (!applyQuicHeaderProtection(builtPacket, header, mask).has_value())
        {
            throw Base::Exception("QUIC 组包失败：加头部保护时报文长度不自洽：请核对包号字节的写入长度与 Length 域是否一致");
        }
    }

    void appendQuicRetryPacket(std::string &datagram, const QuicRetryPacket &packet)
    {
        validateRetryPacket(packet);

        // 先全数拼在这条局部缓冲里，成了才整段交出去：中途抛出（AEAD 故障之类）不能让调用方的数据报
        // 留下一条缺了尾部标签的半截报文
        std::string built;
        built.reserve(1U + 4U + 1U + packet.destinationConnectionId.size() + 1U + packet.sourceConnectionId.size() + packet.token.size() + kQuicAuthenticationTagByteLength);
        built.push_back(static_cast<char>(kQuicRetryFirstByte));
        for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
        {
            // 版本是 32 位定长大端，与变长整数无关（§16 末段把版本排除在外）
            built.push_back(static_cast<char>((packet.version >> ((3U - byteIndex) * 8U)) & 0xFFU));
        }
        built.push_back(static_cast<char>(packet.destinationConnectionId.size()));
        appendQuicRawBytes(built, packet.destinationConnectionId);
        built.push_back(static_cast<char>(packet.sourceConnectionId.size()));
        appendQuicRawBytes(built, packet.sourceConnectionId);
        // 令牌是变长字段但没有长度前缀：它一直占到标签之前，客户端按整条数据报的末尾倒推 16 字节就是标签
        appendQuicRawBytes(built, packet.token);

        // 伪包＝「原始目的标识长度 + 原始目的标识」接在上面这段之前（RFC 9001 §5.8 图 8）。
        // 那一段不上线，作用是让标签把客户端的第一个 Initial 也绑进来
        std::string pseudoPacket;
        pseudoPacket.reserve(1U + packet.originalDestinationConnectionId.size() + built.size());
        pseudoPacket.push_back(static_cast<char>(packet.originalDestinationConnectionId.size()));
        appendQuicRawBytes(pseudoPacket, packet.originalDestinationConnectionId);
        pseudoPacket += built;

        // 明文为空、关联数据是伪包：AEAD 的输出恰好就是那 16 字节标签（§5.8 的 P 是空的）。
        // 包号取 0：这条报文没有包号字段， nonce 因此就是那个定值 IV（标签不参与任何确认与重传）
        appendQuicProtectedPayload(built, retryIntegrityKeys(), 0, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pseudoPacket.data()), pseudoPacket.size()),
                                   {});

        datagram += built;
    }
} // namespace AsynGyanis::Net
