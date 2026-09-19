#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <array>
#include <format>

namespace AsynGyanis::Net
{
    void appendQuicVariableLengthInteger(std::string &bytes, const std::uint64_t value)
    {
        const std::size_t byteWidth = quicVariableLengthIntegerByteCount(value);
        // 先判值域：byteCount 对超限值返回 0，若直接拿去查档位表，报出来的会是「宽度非法」这种
        // 把责任推给调用方的错文案
        if (byteWidth == 0)
        {
            throw Base::InvalidArgumentException(std::format("值 {} 超过 QUIC 变长整数的上限 {}（2^62-1，RFC 9000 §16 表 4）："
                                                             "这个量级的数值在本协议里没有合法编码，请在写入前先把它压进上限之内",
                                                             value, kQuicMaximumIntegerValue));
        }

        // 首字节高 2 位是「字节数以 2 为底的对数」：1→00、2→01、4→10、8→11（RFC 9000 §16 表 4）
        std::size_t remainingWidth = byteWidth;
        std::uint8_t lengthPrefix = 0;
        while (remainingWidth > 1)
        {
            remainingWidth >>= 1;
            lengthPrefix = static_cast<std::uint8_t>(lengthPrefix + 0x40);
        }

        std::array<std::uint8_t, 8> encoded{};
        // 线上是大端：先写的是高位字节，因此每轮右移的位数由剩余字节数决定
        for (std::size_t byteIndex = 0; byteIndex < byteWidth; ++byteIndex)
        {
            const std::size_t shiftBitCount = (byteWidth - 1 - byteIndex) * 8;
            encoded[byteIndex] = static_cast<std::uint8_t>((value >> shiftBitCount) & 0xFFULL);
        }
        // 档位选择保证了值只用到低 (8*宽度-2) 位，首字节的高 2 位仍是空的，可以直接并上前缀
        encoded[0] = static_cast<std::uint8_t>(encoded[0] | lengthPrefix);
        bytes.append(reinterpret_cast<const char *>(encoded.data()), byteWidth);
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
