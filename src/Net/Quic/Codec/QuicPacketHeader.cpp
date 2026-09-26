#include "Net/Quic/Codec/QuicPacketHeader.h"

#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 按大端读出定长整数字段
         * @details QUIC 线上整数一律网络序（RFC 9000 §17 开头），且**不用**变长整数编码——
         *          版本、连接标识长度与包号都是这种定长字段（§16 末段明确把它们排除在外）。
         * @param bytes 字段字节，调用方保证够 byteCount 个
         * @param byteCount 字段字节数（1 到 8）
         * @return std::uint64_t 读出的数值
         */
        std::uint64_t readBigEndianInteger(const std::span<const std::uint8_t> bytes, const std::size_t byteCount) noexcept
        {
            std::uint64_t value = 0;
            for (std::size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex)
            {
                value = (value << 8) | static_cast<std::uint64_t>(bytes[byteIndex]);
            }
            return value;
        }

        /**
         * @brief 造一条截断类错误
         * @param fieldDescription 出错字段的中文名，写进文案便于定位
         * @param detail 具体差在哪里的补充说明
         * @return QuicDecodeError 类别为 Truncated 的错误
         */
        QuicDecodeError makeTruncatedError(std::string_view fieldDescription, std::string detail)
        {
            return QuicDecodeError{QuicDecodeErrorKind::Truncated, std::format("{}：{}；本包只能整包丢弃", fieldDescription, detail)};
        }

        /**
         * @brief 造一条报文违规类错误
         * @param detail 违反的规则与实测取值的说明
         * @return QuicDecodeError 类别为 Malformed 的错误
         */
        QuicDecodeError makeMalformedError(std::string detail)
        {
            return QuicDecodeError{QuicDecodeErrorKind::Malformed, std::move(detail)};
        }

        /**
         * @brief 从首字节取包号字段字节数
         * @param firstByte 首字节（去头部保护之后的才可信）
         * @return std::size_t 1 到 4：低 2 位的取值加一（RFC 9000 §17.2/§17.3.1）
         */
        std::size_t packetNumberByteCountFromFirstByte(const std::uint8_t firstByte) noexcept
        {
            return std::size_t{1} + static_cast<std::size_t>(firstByte & kQuicPacketNumberLengthBitMask);
        }

        /**
         * @brief 读长头里的一个连接标识字段（8 位长度 + 内容）
         * @param datagram 整条数据报
         * @param readOffset 读位置，成功时前移到字段之后
         * @param fieldDescription 字段中文名，用于文案
         * @param connectionId 输出：指向数据报的视图
         * @return std::expected<void, QuicDecodeError> 失败原样带回错误
         */
        std::expected<void, QuicDecodeError> readLongHeaderConnectionId(std::span<const std::uint8_t> datagram, std::size_t &readOffset, std::string_view fieldDescription,
                                                                        std::span<const std::uint8_t> &connectionId)
        {
            if (readOffset >= datagram.size())
            {
                return std::unexpected(makeTruncatedError(fieldDescription, std::format("连长度字节都没有（数据报只有 {} 字节）", datagram.size())));
            }
            const std::size_t length = static_cast<std::size_t>(datagram[readOffset]);
            ++readOffset;
            // v1 规定长头里的连接标识不得超过 20 字节（RFC 9000 §17.2）；放行会让路由键长度失控
            if (length > kQuicMaximumConnectionIdLength)
            {
                return std::unexpected(makeMalformedError(std::format("{}的长度 {} 字节超过版本 1 的上限 {} 字节（RFC 9000 §17.2）："
                                                                      "本包按非法处理",
                                                                      fieldDescription, length, kQuicMaximumConnectionIdLength)));
            }
            if (datagram.size() - readOffset < length)
            {
                return std::unexpected(makeTruncatedError(fieldDescription, std::format("长度声明 {} 字节，数据报只剩 {} 字节", length, datagram.size() - readOffset)));
            }
            connectionId = datagram.subspan(readOffset, length);
            readOffset += length;
            return {};
        }

        /**
         * @brief 解长头（RFC 9000 §17.2）：版本、两条连接标识、Initial 的 Token、Length 域
         * @param header 输出参数，进入时只需 firstByte 已填
         * @param datagram 整条数据报
         * @return std::expected<QuicPacketHeader, QuicDecodeError> 成功返回填好明文部分的报文头
         */
        std::expected<QuicPacketHeader, QuicDecodeError> decodeLongHeaderPacket(QuicPacketHeader header, std::span<const std::uint8_t> datagram)
        {
            // 首字节 + 32 位版本：不足 5 字节连版本都读不出来
            if (datagram.size() < 5)
            {
                return std::unexpected(makeTruncatedError("长头报文的版本字段", std::format("需要 5 字节，实收 {} 字节", datagram.size())));
            }
            header.version = static_cast<std::uint32_t>(readBigEndianInteger(datagram.subspan(1), 4));
            if (header.version == kQuicVersionNegotiationVersion)
            {
                // 版本协商只在客户端侧出现，且它的连接标识长度不受 v1 的 20 字节限制约束
                // （RFC 9000 §17.2.1），本端作为服务端收到它只能是噪声
                return std::unexpected(makeMalformedError("收到版本协商报文（版本字段为 0）：本实现只跑服务端，不解版本协商的内容，请丢弃"));
            }
            if (header.version != kQuicVersion1)
            {
                return std::unexpected(makeMalformedError(std::format("不支持的 QUIC 版本 0x{:08X}：本实现只支持版本 1（0x00000001），"
                                                                      "请丢弃；发版本协商报文是服务端的职责，不在解码层做",
                                                                      header.version)));
            }
            // 长头的固定位必须为 1，为 0 的报文在本版本里不是合法报文（RFC 9000 §17.2）
            if ((header.firstByte & kQuicFixedBit) == 0)
            {
                return std::unexpected(makeMalformedError(std::format("长头首字节 0x{:02X} 的固定位为 0（RFC 9000 §17.2）："
                                                                      "本版本里这不是合法报文，请丢弃",
                                                                      static_cast<unsigned int>(header.firstByte))));
            }
            header.longPacketType = static_cast<QuicLongPacketType>((header.firstByte & kQuicLongPacketTypeBitMask) >> 4);
            if (header.longPacketType == QuicLongPacketType::Retry)
            {
                // Retry 没有 Length 与包号，尾部是 Retry Token + 16 字节完整性标签（§17.2.5）；
                // 服务端只会发 Retry 而不会收，本层因此刻意不解释它的字段
                return std::unexpected(makeMalformedError("收到 Retry 报文：本实现只跑服务端，未解 Retry 的令牌与完整性标签，请丢弃"));
            }

            std::size_t readOffset = 5;
            if (const auto result = readLongHeaderConnectionId(datagram, readOffset, "目的连接标识", header.destinationConnectionId); !result.has_value())
            {
                return std::unexpected(result.error());
            }
            if (const auto result = readLongHeaderConnectionId(datagram, readOffset, "源连接标识", header.sourceConnectionId); !result.has_value())
            {
                return std::unexpected(result.error());
            }

            // 只有 Initial 带 Token（§17.2.2）；0-RTT 与 Handshake 直接进 Length 字段
            if (header.longPacketType == QuicLongPacketType::Initial)
            {
                const auto tokenLength = decodeQuicVariableLengthInteger(datagram.subspan(readOffset));
                if (!tokenLength.has_value())
                {
                    return std::unexpected(makeTruncatedError("Initial 的 Token 长度字段", tokenLength.error().message));
                }
                readOffset += tokenLength->byteCount;
                if (datagram.size() - readOffset < tokenLength->value)
                {
                    return std::unexpected(
                            makeTruncatedError("Initial 的 Token", std::format("长度 {} 字节，数据报只剩 {} 字节", tokenLength->value, datagram.size() - readOffset)));
                }
                header.token = datagram.subspan(readOffset, static_cast<std::size_t>(tokenLength->value));
                readOffset += static_cast<std::size_t>(tokenLength->value);
            }

            const auto lengthField = decodeQuicVariableLengthInteger(datagram.subspan(readOffset));
            if (!lengthField.has_value())
            {
                return std::unexpected(makeTruncatedError("长头的 Length 字段", lengthField.error().message));
            }
            readOffset += lengthField->byteCount;
            // Length 覆盖「包号 + 受保护载荷」，而包号至少 1 字节，因此 0 就是不可能的取值（§17.2、§17.2.2 图 15）
            if (lengthField->value == 0)
            {
                return std::unexpected(makeMalformedError("长头的 Length 为 0，容不下至少 1 字节的包号（RFC 9000 §17.2）：本包非法，请丢弃"));
            }
            if (lengthField->value > datagram.size() - readOffset)
            {
                return std::unexpected(
                        makeTruncatedError("长头声明的报文长度", std::format("Length 为 {} 字节，去掉头部后数据报只剩 {} 字节", lengthField->value, datagram.size() - readOffset)));
            }

            header.packetNumberOffset              = readOffset;
            header.packetNumberAndPayloadByteCount = static_cast<std::size_t>(lengthField->value);
            header.packetByteCount                 = readOffset + static_cast<std::size_t>(lengthField->value);
            return header;
        }

        /**
         * @brief 解短头（RFC 9000 §17.3.1）：目的连接标识按本端约定的长度取
         * @param header 输出参数，进入时只需 firstByte 已填
         * @param datagram 整条数据报
         * @param destinationConnectionIdLength 本端签发标识的字节数
         * @return std::expected<QuicPacketHeader, QuicDecodeError> 成功返回填好明文部分的报文头
         */
        std::expected<QuicPacketHeader, QuicDecodeError> decodeShortHeaderPacket(QuicPacketHeader header, std::span<const std::uint8_t> datagram,
                                                                                 const std::size_t destinationConnectionIdLength)
        {
            if ((header.firstByte & kQuicFixedBit) == 0)
            {
                return std::unexpected(makeMalformedError(std::format("短头首字节 0x{:02X} 的固定位为 0（RFC 9000 §17.3.1）："
                                                                      "本版本里这不是合法报文，请丢弃",
                                                                      static_cast<unsigned int>(header.firstByte))));
            }
            if (datagram.size() - 1 < destinationConnectionIdLength)
            {
                return std::unexpected(
                        makeTruncatedError("短头的目的连接标识", std::format("按本端约定需要 {} 字节，数据报只剩 {} 字节", destinationConnectionIdLength, datagram.size() - 1)));
            }
            header.destinationConnectionId = datagram.subspan(1, destinationConnectionIdLength);
            // 短头不带版本（§17.3.1），保持 0 而不是谎报成 1：调用方要用的是建连接时谈定的版本
            header.version            = 0;
            header.packetNumberOffset = 1 + destinationConnectionIdLength;
            // 短头没有 Length 域，本包吃掉数据报的剩余全部字节（§17.3.1）
            header.packetNumberAndPayloadByteCount = datagram.size() - header.packetNumberOffset;
            header.packetByteCount                 = datagram.size();
            return header;
        }
    } // namespace

    std::expected<QuicPacketHeader, QuicDecodeError> decodeQuicPacketHeader(const std::span<const std::uint8_t> datagram,
                                                                            const std::size_t                   shortHeaderDestinationConnectionIdLength)
    {
        if (shortHeaderDestinationConnectionIdLength > kQuicMaximumConnectionIdLength)
        {
            // 这是本端配置错了而不是对端发了坏包：让调用方的 bug 冒出去，不冒充可恢复的解码失败
            throw Base::InvalidArgumentException(std::format("短头目的连接标识长度 {} 超过版本 1 的上限 {} 字节（RFC 9000 §5.1.1、§17.2）："
                                                             "请检查本端签发连接标识时用的长度",
                                                             shortHeaderDestinationConnectionIdLength, kQuicMaximumConnectionIdLength));
        }
        if (datagram.empty())
        {
            return std::unexpected(makeTruncatedError("报文首字节", "数据报为空"));
        }

        QuicPacketHeader header;
        header.firstByte    = datagram[0];
        header.isLongHeader = (datagram[0] & kQuicLongHeaderFlagBit) != 0;

        return header.isLongHeader ? decodeLongHeaderPacket(header, datagram) : decodeShortHeaderPacket(header, datagram, shortHeaderDestinationConnectionIdLength);
    }

    std::expected<void, QuicDecodeError> refreshQuicPacketHeader(QuicPacketHeader &header, const std::uint8_t unmaskedFirstByte, const std::span<const std::uint8_t> datagram)
    {
        header.firstByte             = unmaskedFirstByte;
        header.packetNumberByteCount = packetNumberByteCountFromFirstByte(unmaskedFirstByte);
        header.isSpinBitSet          = (unmaskedFirstByte & kQuicSpinBitMask) != 0;
        header.isKeyPhaseBitSet      = (unmaskedFirstByte & kQuicKeyPhaseBitMask) != 0;

        // 先判偏移本身是否还在数据报内：后面的减法都建立在这个前提上，否则无符号回绕会把结论反过来
        if (header.packetNumberOffset > datagram.size())
        {
            return std::unexpected(makeTruncatedError("包号字段", std::format("起始偏移 {} 已越过数据报末尾（共 {} 字节）", header.packetNumberOffset, datagram.size())));
        }
        // 包号长度那两位也在被保护的范围内：掩出来的值若大于 Length 域，本包自相矛盾（§17.2）
        if (header.packetNumberByteCount > header.packetNumberAndPayloadByteCount)
        {
            return std::unexpected(makeMalformedError(std::format("包号字段需要 {} 字节，而报文长度只剩 {} 字节（RFC 9000 §17.2 的 Length 含包号）："
                                                                  "去掉头部保护后两者不自洽，本包非法",
                                                                  header.packetNumberByteCount, header.packetNumberAndPayloadByteCount)));
        }
        if (header.packetNumberByteCount > datagram.size() - header.packetNumberOffset)
        {
            return std::unexpected(makeTruncatedError("包号字段", std::format("需要 {} 字节，数据报在偏移 {} 处只剩 {} 字节", header.packetNumberByteCount,
                                                                              header.packetNumberOffset, datagram.size() - header.packetNumberOffset)));
        }
        header.packetNumber = readBigEndianInteger(datagram.subspan(header.packetNumberOffset), header.packetNumberByteCount);
        return {};
    }

    void appendQuicTruncatedPacketNumber(std::string &bytes, const std::uint64_t packetNumber, const std::size_t packetNumberByteCount)
    {
        if (packetNumberByteCount < 1 || packetNumberByteCount > kQuicMaximumPacketNumberByteCount)
        {
            throw Base::InvalidArgumentException(std::format("包号字段字节数 {} 不在 1..{} 内（RFC 9000 §17.1）："
                                                             "首字节的包号长度位只表达得到这四档",
                                                             packetNumberByteCount, kQuicMaximumPacketNumberByteCount));
        }

        // 定长大端：只交最低的几个字节，高位由对端按窗口还原，因此这里刻意不检查值是否放得下
        for (std::size_t byteIndex = 0; byteIndex < packetNumberByteCount; ++byteIndex)
        {
            const std::size_t shiftBitCount = (packetNumberByteCount - 1 - byteIndex) * 8;
            bytes.push_back(static_cast<char>((packetNumber >> shiftBitCount) & 0xFFULL));
        }
    }

    std::uint64_t restoreQuicPacketNumber(const std::uint64_t largestReceivedPacketNumber, const std::uint64_t truncatedPacketNumber, const std::size_t packetNumberByteCount)
    {
        if (packetNumberByteCount < 1 || packetNumberByteCount > 4)
        {
            throw Base::InvalidArgumentException(std::format("包号字段字节数 {} 不在 1..4 内（RFC 9000 §17.1）："
                                                             "请先按首字节的包号长度位算出真实字节数再还原",
                                                             packetNumberByteCount));
        }

        const std::uint64_t windowSize = 1ULL << (packetNumberByteCount * 8ULL);
        const std::uint64_t halfWindow = windowSize / 2ULL;
        const std::uint64_t windowMask = windowSize - 1ULL;
        const std::uint64_t expected   = largestReceivedPacketNumber + 1ULL;
        const std::uint64_t candidate  = (expected & ~windowMask) | truncatedPacketNumber;

        // 附录 A.3 的第一支：候选值落在窗口下沿之下，说明高位借错了一位，补回一个窗口。
        // 判据写成 candidate + halfWindow <= expected 而不是 candidate <= expected - halfWindow，
        // 因为后者在 expected 小于半窗口时无符号回绕，会把结论整个反过来
        if (candidate + halfWindow <= expected && candidate < (1ULL << 62) - windowSize)
        {
            return candidate + windowSize;
        }
        // 第二支：候选值越过窗口上沿，退一个窗口（RFC 9000 §A.3）
        if (candidate > expected + halfWindow && candidate >= windowSize)
        {
            return candidate - windowSize;
        }
        return candidate;
    }
} // namespace AsynGyanis::Net
