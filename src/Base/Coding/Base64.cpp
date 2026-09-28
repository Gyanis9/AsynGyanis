#include "Base/Coding/Base64.h"

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

        /// 一段编码末尾最多两个填充符
        constexpr std::size_t kMaximumBase64PaddingLength = 2;

        /// 无填充写法的占位值：push_back 见到它就不写任何字符
        constexpr char kNoPadding = '\0';

        /**
         * @brief 按给定字母表编码，是否补填充由 paddingCharacter 决定
         * @param bytes 待编码字节
         * @param alphabet 64 个字符的字母表
         * @param paddingCharacter 填充符，传 kNoPadding 表示末组不补
         * @return std::string 编码结果
         */
        [[nodiscard]] std::string encodeWithAlphabet(const std::string_view bytes, const std::string_view alphabet, const char paddingCharacter)
        {
            std::string encoded;
            encoded.reserve((bytes.size() + 2) / 3 * 4);

            // 每 3 字节编成 4 个字符：24 位正好切成四段 6 位
            for (std::size_t index = 0; index < bytes.size(); index += 3)
            {
                const std::size_t   remainingLength = bytes.size() - index;
                const std::uint32_t firstByte       = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
                const std::uint32_t secondByte      = remainingLength > 1 ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1])) : 0U;
                const std::uint32_t thirdByte       = remainingLength > 2 ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2])) : 0U;
                const std::uint32_t groupValue      = (firstByte << 16) | (secondByte << 8) | thirdByte;

                encoded.push_back(alphabet[(groupValue >> 18) & 0x3FU]);
                encoded.push_back(alphabet[(groupValue >> 12) & 0x3FU]);
                // 末组只剩 1 字节时第三、四个字符没有信息：按规范补 '='，按无填充写法则就此收笔
                if (remainingLength > 1)
                {
                    encoded.push_back(alphabet[(groupValue >> 6) & 0x3FU]);
                } else if (paddingCharacter != kNoPadding)
                {
                    encoded.push_back(paddingCharacter);
                }
                if (remainingLength > 2)
                {
                    encoded.push_back(alphabet[groupValue & 0x3FU]);
                } else if (paddingCharacter != kNoPadding)
                {
                    encoded.push_back(paddingCharacter);
                }
            }
            return encoded;
        }

        /**
         * @brief 按给定字母表严格解码
         * @param text 待解码文本
         * @param alphabet 64 个字符的字母表
         * @param maximumPaddingLength 末尾允许的填充符个数，0 表示这套写法根本不该出现填充
         * @return std::optional<std::string> 解码出的字节；输入非法时为空
         */
        [[nodiscard]] std::optional<std::string> decodeWithAlphabet(const std::string_view text, const std::string_view alphabet, const std::size_t maximumPaddingLength) noexcept
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
                const std::size_t encodedValue = alphabet.find(text[index]);
                // 字母表之外的字符（含无填充写法里出现的 '='）只能出现在末尾的填充位置，出现在这里就是非法编码
                if (encodedValue == std::string_view::npos)
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
        return decodeWithAlphabet(text, kBase64Alphabet, kMaximumBase64PaddingLength);
    }

    std::string base64UrlEncode(const std::string_view bytes)
    {
        return encodeWithAlphabet(bytes, kBase64UrlAlphabet, kNoPadding);
    }

    std::optional<std::string> base64UrlDecode(const std::string_view text) noexcept
    {
        // 填充上限传 0：无填充写法里出现的 '=' 走字母表查找失败这条出口，与 '+' '/' 同样判非法
        return decodeWithAlphabet(text, kBase64UrlAlphabet, 0U);
    }
} // namespace AsynGyanis::Base
