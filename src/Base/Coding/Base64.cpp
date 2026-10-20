#include "Base/Coding/Base64.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 标准字母表（RFC 4648 §4）
        constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        /// URL-safe 字母表（RFC 4648 §5）：'-' 与 '_' 顶掉 '+' 与 '/'，因为后者在 URL 里另有含义
        constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        /// 解码用的反向表：字母表外的字节为 -1，命中时为 0..63。256 项静态表把每字符一次线性查找换成一次下标读。
        struct Base64DecodeTable
        {
            std::array<std::int8_t, 256> values{};

            constexpr explicit Base64DecodeTable(const std::string_view alphabet)
            {
                values.fill(-1);
                for (std::size_t index = 0; index < alphabet.size(); ++index)
                {
                    values[static_cast<unsigned char>(alphabet[index])] = static_cast<std::int8_t>(index);
                }
            }
        };

        constexpr Base64DecodeTable kStandardDecodeTable{kBase64Alphabet}; ///< 标准字母表的反向表
        constexpr Base64DecodeTable kUrlDecodeTable{kBase64UrlAlphabet};   ///< URL-safe 字母表的反向表

        /// 一段编码末尾最多两个填充符
        constexpr std::size_t kMaximumBase64PaddingLength = 2;

        /// 无填充写法的标记值：编码器拿它判断末组要不要补填充
        constexpr char kNoPadding = '\0';

        /**
         * @brief 按给定字母表编码，是否补填充由 paddingCharacter 决定
         * @details 先按输出长度一次建好字符串，再按索引逐位写入，整趟编码只分配这一块缓冲。
         * @param bytes 待编码字节
         * @param alphabet 64 个字符的字母表
         * @param paddingCharacter 填充符，传 kNoPadding 表示末组不补
         * @return std::string 编码结果
         */
        [[nodiscard]] std::string encodeWithAlphabet(const std::string_view bytes, const std::string_view alphabet, const char paddingCharacter)
        {
            const std::size_t fullGroupCount = bytes.size() / 3U;
            const std::size_t remainder      = bytes.size() % 3U;
            const std::size_t encodedLength  = fullGroupCount * 4U + (remainder == 0U ? 0U : (paddingCharacter == kNoPadding ? remainder + 1U : 4U));
            std::string       encoded(encodedLength, '\0');

            std::size_t outputIndex = 0;
            // 每 3 字节编成 4 个字符：24 位正好切成四段 6 位
            for (std::size_t index = 0; index + 2U < bytes.size(); index += 3U)
            {
                const std::uint32_t groupValue = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16U) |
                                                 (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1U])) << 8U) |
                                                 static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2U]));
                encoded[outputIndex++]         = alphabet[(groupValue >> 18U) & 0x3FU];
                encoded[outputIndex++]         = alphabet[(groupValue >> 12U) & 0x3FU];
                encoded[outputIndex++]         = alphabet[(groupValue >> 6U) & 0x3FU];
                encoded[outputIndex++]         = alphabet[groupValue & 0x3FU];
            }
            if (remainder != 0U)
            {
                const std::uint32_t firstByte  = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[fullGroupCount * 3U]));
                const std::uint32_t secondByte = remainder > 1U ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[fullGroupCount * 3U + 1U])) : 0U;
                const std::uint32_t groupValue = (firstByte << 16U) | (secondByte << 8U);
                encoded[outputIndex++]         = alphabet[(groupValue >> 18U) & 0x3FU];
                encoded[outputIndex++]         = alphabet[(groupValue >> 12U) & 0x3FU];
                // 末组只剩 1 字节时第三、四个字符没有信息：按规范补 '='，按无填充写法则就此收笔
                if (remainder > 1U)
                {
                    encoded[outputIndex++] = alphabet[(groupValue >> 6U) & 0x3FU];
                } else if (paddingCharacter != kNoPadding)
                {
                    encoded[outputIndex++] = paddingCharacter;
                }
                if (paddingCharacter != kNoPadding)
                {
                    encoded[outputIndex++] = paddingCharacter;
                }
            }
            return encoded;
        }

        /**
         * @brief 按给定字母表严格解码
         * @param text 待解码文本
         * @param table 该字母表的反向查询表
         * @param maximumPaddingLength 末尾允许的填充符个数，0 表示这套写法根本不该出现填充
         * @return std::optional<std::string> 解码出的字节；输入非法时为空
         */
        [[nodiscard]] std::optional<std::string> decodeWithAlphabet(const std::string_view text, const Base64DecodeTable &table, const std::size_t maximumPaddingLength) noexcept
        {
            if (text.empty())
            {
                return std::nullopt;
            }

            // 长度 % 4 == 1 凑不出任何一整个字节（RFC 4648 Table 1 没这一格）；要求满 4 的倍数的那套
            // 写法在下面这条长度检查里已经一并挡住，两条规则因此不互相覆盖
            if (text.size() % 4 == 1 || (maximumPaddingLength > 0 && text.size() % 4 != 0))
            {
                return std::nullopt;
            }

            // 填充符只允许在末尾：先数出末尾有几个 '='，它们之前的一切都必须是字母表字符
            std::size_t paddingLength = 0;
            while (paddingLength < maximumPaddingLength && paddingLength < text.size() && text[text.size() - 1 - paddingLength] == '=')
            {
                ++paddingLength;
            }
            const std::size_t dataLength = text.size() - paddingLength;

            std::string decoded;
            decoded.reserve(dataLength / 4 * 3 + 3);

            std::uint32_t accumulator         = 0;
            std::size_t   accumulatorBitCount = 0;
            for (std::size_t index = 0; index < dataLength; ++index)
            {
                const std::int8_t encodedValue = table.values[static_cast<unsigned char>(text[index])];
                // 字母表之外的字符（含无填充写法里出现的 '='）只能出现在末尾的填充位置，出现在这里就是非法编码
                if (encodedValue < 0)
                {
                    return std::nullopt;
                }

                accumulator = (accumulator << 6) | static_cast<std::uint32_t>(encodedValue);
                accumulatorBitCount += 6;
                if (accumulatorBitCount >= 8)
                {
                    accumulatorBitCount -= 8;
                    decoded.push_back(static_cast<char>((accumulator >> accumulatorBitCount) & 0xFFU));
                    // 只保留尚未消费的低位：accumulator 会随左移不断溢出，靠这一步把它压回 12 位以内
                    accumulator &= (1U << accumulatorBitCount) - 1U;
                }
            }

            // 掩码后 accumulator 只剩不足一字节的填充位，非 0 说明填充位被置位，即非规范编码
            if (accumulator != 0)
            {
                return std::nullopt;
            }
            return decoded;
        }
    } // namespace

    std::string base64Encode(const std::string_view bytes)
    {
        return encodeWithAlphabet(bytes, kBase64Alphabet, '=');
    }

    std::optional<std::string> base64Decode(const std::string_view text) noexcept
    {
        return decodeWithAlphabet(text, kStandardDecodeTable, kMaximumBase64PaddingLength);
    }

    std::string base64UrlEncode(const std::string_view bytes)
    {
        return encodeWithAlphabet(bytes, kBase64UrlAlphabet, kNoPadding);
    }

    std::optional<std::string> base64UrlDecode(const std::string_view text) noexcept
    {
        // 填充上限传 0：无填充写法里出现的 '=' 走反向表查不到这条出口，与 '+' '/' 同样判非法
        return decodeWithAlphabet(text, kUrlDecodeTable, 0U);
    }
} // namespace AsynGyanis::Base
