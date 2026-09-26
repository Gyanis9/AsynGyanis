#include "Net/WebSocket/WebSocketUtf8.h"

#include <cstdint>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 续字节的形状：高两位必须是 10（RFC 3629 §3）
        constexpr std::uint8_t kContinuationByteMask = 0xC0;

        /// 续字节与上面这个掩码按位与之后必须等于本值
        constexpr std::uint8_t kContinuationByteValue = 0x80;

        /// 代理区码点的下界：这个区间是 UTF-16 的专用表示，UTF-8 里不得出现（RFC 3629 §3）
        constexpr std::uint32_t kSurrogateMinimumCodePoint = 0xD800;

        /// 代理区码点的上界
        constexpr std::uint32_t kSurrogateMaximumCodePoint = 0xDFFF;

        /// Unicode 码点上限：4 字节序列可表示到 U+1FFFFF，越过它的都是非法编码（RFC 3629 §3）
        constexpr std::uint32_t kMaximumUnicodeCodePoint = 0x10FFFF;

        /**
         * @brief 按首字节的形状判断这个序列总共占几个字节
         * @param leadingByte 序列首字节
         * @return std::size_t 序列长度（1~4 字节）
         * @return 0 表示该字节不能做首字节：10xxxxxx 是续字节、11111xxx 是 RFC 3629 未定义的形状
         */
        [[nodiscard]] std::size_t utf8SequenceLength(const std::uint8_t leadingByte) noexcept
        {
            // 形状判据取自 RFC 3629 §3 的表：0xxxxxxx / 110xxxxx / 1110xxxx / 11110xxx
            if (leadingByte < 0x80U)
            {
                return 1;
            }
            if ((leadingByte & 0xE0U) == 0xC0U)
            {
                return 2;
            }
            if ((leadingByte & 0xF0U) == 0xE0U)
            {
                return 3;
            }
            if ((leadingByte & 0xF8U) == 0xF0U)
            {
                return 4;
            }
            return 0;
        }
    } // namespace

    std::size_t findInvalidWebSocketUtf8ByteOffset(const std::string_view text) noexcept
    {
        std::size_t offset = 0;
        while (offset < text.size())
        {
            // char 的符号性随平台而异：先转成无符号字节再判形状，否则高位字节会变成负值
            const auto        leadingByte    = static_cast<std::uint8_t>(text[offset]);
            const std::size_t sequenceLength = utf8SequenceLength(leadingByte);

            // 首字节形状非法、或剩余字节数不够这个序列的长度（截断）：违规点都算序列起点
            if (sequenceLength == 0 || sequenceLength > text.size() - offset)
            {
                return offset;
            }
            if (sequenceLength == 1)
            {
                // ASCII 单字节：7 位全用满，不存在过长编码这一说
                ++offset;
                continue;
            }

            // 首字节里的长度位不属于码点，先按序列长度掩掉：2 字节留 5 位、3 字节留 4 位、4 字节留 3 位
            std::uint32_t codePoint = static_cast<std::uint32_t>(leadingByte & static_cast<std::uint8_t>(0xFFU >> (sequenceLength + 1U)));
            for (std::size_t index = 1; index < sequenceLength; ++index)
            {
                const auto continuationByte = static_cast<std::uint8_t>(text[offset + index]);
                // 续字节必须形如 10xxxxxx：不满足说明对端这个序列的实际边界与本端判读的不一致
                if ((continuationByte & kContinuationByteMask) != kContinuationByteValue)
                {
                    return offset;
                }
                codePoint = (codePoint << 6) | static_cast<std::uint32_t>(continuationByte & 0x3FU);
            }

            // 过长编码：同一个码点用更长的序列写出会让不同实现得出不同结果（RFC 3629 §3）
            const std::uint32_t minimumCodePoint = sequenceLength == 2 ? 0x80U : (sequenceLength == 3 ? 0x800U : 0x10000U);
            if (codePoint < minimumCodePoint)
            {
                return offset;
            }
            // 代理区码点非法；码点上限用于拦下 0xF4 之上一档的首字节（如 0xF4 0x90 0x80 0x80）
            if (codePoint >= kSurrogateMinimumCodePoint && codePoint <= kSurrogateMaximumCodePoint)
            {
                return offset;
            }
            if (codePoint > kMaximumUnicodeCodePoint)
            {
                return offset;
            }

            offset += sequenceLength;
        }

        // 走到这里说明每个字节都落在某个合法序列内：npos 是「没有违规字节」的唯一表示
        return std::string_view::npos;
    }

    bool isValidWebSocketUtf8(const std::string_view text) noexcept
    {
        return findInvalidWebSocketUtf8ByteOffset(text) == std::string_view::npos;
    }
} // namespace AsynGyanis::Net
