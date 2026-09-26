#include "Base/Coding/Base64.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 标准字母表（RFC 4648 §4）
        constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        /// 一段编码末尾最多两个填充符
        constexpr std::size_t kMaximumBase64PaddingLength = 2;
    } // namespace

    std::string base64Encode(const std::string_view bytes)
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

            encoded.push_back(kBase64Alphabet[(groupValue >> 18) & 0x3FU]);
            encoded.push_back(kBase64Alphabet[(groupValue >> 12) & 0x3FU]);
            // 末组只剩 1 字节时第三、四个字符没有信息，按规范用 '=' 占位
            encoded.push_back(remainingLength > 1 ? kBase64Alphabet[(groupValue >> 6) & 0x3FU] : '=');
            encoded.push_back(remainingLength > 2 ? kBase64Alphabet[groupValue & 0x3FU] : '=');
        }
        return encoded;
    }

    std::optional<std::string> base64Decode(const std::string_view text) noexcept
    {
        if (text.empty() || text.size() % 4 != 0)
        {
            return std::nullopt;
        }

        // 填充符只允许在末尾：先数出末尾有几个 '='，它们之前的一切都必须是字母表字符
        std::size_t paddingLength = 0;
        while (paddingLength < kMaximumBase64PaddingLength && paddingLength < text.size() && text[text.size() - 1 - paddingLength] == '=')
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
            const std::size_t encodedValue = kBase64Alphabet.find(text[index]);
            // '=' 与字母表之外的字符都只能出现在末尾的填充位置，出现在这里就是非法编码
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
} // namespace AsynGyanis::Base
