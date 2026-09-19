#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <array>
#include <format>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 一档编码宽度的三样属性：占几字节、首字节的长度前缀、该档能表示的最大值
         */
        struct IntegerWidthProfile
        {
            std::size_t byteWidth;      ///< 该档占用的字节数
            std::uint8_t lengthPrefix;  ///< 首字节高 2 位：字节数以 2 为底的对数
            std::uint64_t maximumValue; ///< 该档剩下的位数能表示的最大值
        };

        /// 四档属性逐条抄自 RFC 9000 §16 表 4（可用位数 6/14/30/62），宽度递增排列
        constexpr std::array<IntegerWidthProfile, 4> kIntegerWidthProfiles{{
                {1, 0x00, kQuicMaximumOneByteIntegerValue},
                {2, 0x40, kQuicMaximumTwoByteIntegerValue},
                {4, 0x80, kQuicMaximumFourByteIntegerValue},
                {8, 0xC0, kQuicMaximumIntegerValue},
        }};

        /**
         * @brief 按字节数查该档属性
         * @param byteWidth 待查的宽度
         * @return const IntegerWidthProfile* 命中时返回该档；宽度是 3/5/6/7 这类线上无法表达的值时返回 nullptr
         */
        const IntegerWidthProfile *findWidthProfile(const std::size_t byteWidth) noexcept
        {
            for (const IntegerWidthProfile &profile: kIntegerWidthProfiles)
            {
                if (profile.byteWidth == byteWidth)
                {
                    return &profile;
                }
            }
            return nullptr;
        }
    } // namespace

    void appendQuicVariableLengthInteger(std::string &bytes, const std::uint64_t value)
    {
        // 单参重载就是「取最少档位」的特例：超上限的值让三参重载当场拒绝，不在这里另判一套
        appendQuicVariableLengthInteger(bytes, value, quicVariableLengthIntegerByteCount(value));
    }

    void appendQuicVariableLengthInteger(std::string &bytes, const std::uint64_t value, const std::size_t byteWidth)
    {
        // 先判值域再判宽度：quicVariableLengthIntegerByteCount 对超限值返回 0，若先查宽度表，
        // 报出来的会是「宽度非法」这种把责任推给调用方的错文案
        if (value > kQuicMaximumIntegerValue)
        {
            throw Base::InvalidArgumentException(std::format("值 {} 超过 QUIC 变长整数的上限 {}（2^62-1，RFC 9000 §16 表 4）："
                                                             "这个量级的数值在本协议里没有合法编码，请在写入前先把它压进上限之内",
                                                             value, kQuicMaximumIntegerValue));
        }

        const IntegerWidthProfile *const profile = findWidthProfile(byteWidth);
        if (profile == nullptr)
        {
            throw Base::InvalidArgumentException(std::format("变长整数的宽度 {} 不是合法档位：只能是 1、2、4、8 之一"
                                                             "（RFC 9000 §16 只定义这四种长度，首字节的 2 位前缀表达不了别的）",
                                                             byteWidth));
        }
        if (value > profile->maximumValue)
        {
            throw Base::InvalidArgumentException(std::format("{} 字节档最多表示 {}，本次要写 {}：请放宽一档宽度，"
                                                             "或先核对这个值是不是算错了（RFC 9000 §16 表 4）",
                                                             byteWidth, profile->maximumValue, value));
        }

        std::array<std::uint8_t, 8> encoded{};
        // 线上是大端：先写的是高位字节，因此每轮右移的位数由剩余字节数决定
        for (std::size_t byteIndex = 0; byteIndex < profile->byteWidth; ++byteIndex)
        {
            const std::size_t shiftBitCount = (profile->byteWidth - 1 - byteIndex) * 8;
            encoded[byteIndex]              = static_cast<std::uint8_t>((value >> shiftBitCount) & 0xFFULL);
        }
        // 上面的档位校验保证了首字节只用了低 (8*宽度-2) 位，高 2 位仍是空的，可以直接并上前缀
        encoded[0] = static_cast<std::uint8_t>(encoded[0] | profile->lengthPrefix);
        bytes.append(reinterpret_cast<const char *>(encoded.data()), profile->byteWidth);
    }

    std::expected<QuicDecodedInteger, QuicDecodeError> decodeQuicVariableLengthInteger(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(QuicDecodeError{
                    QuicDecodeErrorKind::Truncated,
                    "变长整数至少需要 1 个字节，当前一个字节都没有：请确认确实还有数据要解"});
        }

        // 首字节高 2 位是「字节数以 2 为底的对数」，左移即得本数宽度（RFC 9000 §16 与附录 A.1 的样例算法）
        const std::size_t byteWidth = std::size_t{1} << (static_cast<std::size_t>(bytes[0]) >> 6);
        if (bytes.size() < byteWidth)
        {
            return std::unexpected(QuicDecodeError{
                    QuicDecodeErrorKind::Truncated,
                    std::format("变长整数的首字节 0x{:02X} 声明本数占 {} 字节，但只剩 {} 字节：报文在此处断了，本包只能整包丢弃",
                                static_cast<unsigned int>(bytes[0]), byteWidth, bytes.size())});
        }

        // 先抹掉高 2 位的前缀，再把其余字节按网络序并进低位（附录 A.1：v = v & 0x3f 后逐字节左移相加）
        std::uint64_t value = static_cast<std::uint64_t>(bytes[0] & 0x3FU);
        for (std::size_t byteIndex = 1; byteIndex < byteWidth; ++byteIndex)
        {
            value = (value << 8) | static_cast<std::uint64_t>(bytes[byteIndex]);
        }
        // 这里刻意不校验「是否用了最短编码」：§16 末段写明除帧类型外都合法，帧类型那一档的
        // 从严判断归帧解码器做，本层若一起拒，会把合法的 Length 域回填写法全部打死
        return QuicDecodedInteger{value, byteWidth};
    }
} // namespace AsynGyanis::Net
