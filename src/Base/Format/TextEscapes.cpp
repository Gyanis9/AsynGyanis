#include "Base/Format/TextEscapes.h"
#include "Base/Format/FormatError.h"

#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 判断字符是否为十六进制数字
         * @param character 待判定字符
         * @return true 属于 [0-9a-fA-F]
         */
        bool isHexDigit(const char character) noexcept
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        }

        /**
         * @brief 取单个十六进制字符的数值
         * @param character 十六进制字符
         * @return std::uint32_t 0 到 15 的数值
         */
        std::uint32_t hexValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<std::uint32_t>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<std::uint32_t>(character - 'a' + 10);
            }
            return static_cast<std::uint32_t>(character - 'A' + 10);
        }

        /// Unicode 代理区边界，用于合并 \\uD840\\uDC00 形式的代理对
        constexpr std::uint32_t kHighSurrogateMinimum = 0xD800;
        constexpr std::uint32_t kHighSurrogateMaximum = 0xDBFF;
        constexpr std::uint32_t kLowSurrogateMinimum  = 0xDC00;
        constexpr std::uint32_t kLowSurrogateMaximum  = 0xDFFF;
    } // namespace

    void TextEscapes::appendUtf8(std::string &output, const std::uint32_t codePoint)
    {
        if (codePoint < 0x80U)
        {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800U)
        {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint < 0x10000U)
        {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else
        {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    std::uint32_t TextEscapes::decodeHex(const std::string_view text, std::size_t &index, const std::size_t digitCount, const TextPosition &position)
    {
        if (index + digitCount > text.size())
        {
            throw FormatError("\\u 转义不完整，应为 4 位十六进制数字", position);
        }

        std::uint32_t value = 0;
        for (std::size_t digit = 0; digit < digitCount; ++digit)
        {
            const char character = text[index + digit];
            if (!isHexDigit(character))
            {
                throw FormatError("\\u 转义中存在无效的十六进制数字", position);
            }
            value = (value << 4) | hexValue(character);
        }

        index += digitCount;
        return value;
    }

    char TextEscapes::decodeSimpleEscape(const std::string_view text, std::size_t &index, const TextPosition &position)
    {
        if (index >= text.size())
        {
            throw FormatError("反斜杠后输入即结束", position);
        }

        const char escapeCharacter = text[index];
        ++index;

        switch (escapeCharacter)
        {
            case '"':
                return '"';
            case '\\':
                return '\\';
            case '/':
                return '/';
            case 'b':
                return '\b';
            case 'f':
                return '\f';
            case 'n':
                return '\n';
            case 'r':
                return '\r';
            case 't':
                return '\t';
            default:
                throw FormatError("不支持的转义序列", position);
        }
    }

    std::string TextEscapes::decodeQuotedBody(const std::string_view body, const TextPosition &position)
    {
        // 快路径：整段正文不含反斜杠时无需逐字符解码，直接按原文构造一份结果。
        // 常见配置文本（键名、路径、描述）都走这条路径，省掉逐字节 push_back 的循环
        if (body.find('\\') == std::string_view::npos)
        {
            return std::string(body);
        }

        std::string decoded;
        decoded.reserve(body.size());

        for (std::size_t index = 0; index < body.size();)
        {
            if (body[index] != '\\')
            {
                decoded.push_back(body[index]);
                ++index;
                continue;
            }

            ++index;

            if (index >= body.size())
            {
                throw FormatError("反斜杠后输入即结束", position);
            }

            if (body[index] == 'u')
            {
                ++index;
                const std::uint32_t codeUnit = decodeHex(body, index, 4, position);

                // 高位代理必须紧跟低位代理，合并成单个码位后再编码为 UTF-8
                if (codeUnit >= kHighSurrogateMinimum && codeUnit <= kHighSurrogateMaximum)
                {
                    if (index + 1 >= body.size() || body[index] != '\\' || body[index + 1] != 'u')
                    {
                        throw FormatError("\\u 转义中出现孤立的高代理项", position);
                    }

                    index                            += 2;
                    const std::uint32_t lowSurrogate = decodeHex(body, index, 4, position);
                    if (lowSurrogate < kLowSurrogateMinimum || lowSurrogate > kLowSurrogateMaximum)
                    {
                        throw FormatError("\\u 转义中存在无效的低代理项", position);
                    }

                    const std::uint32_t mergedCodePoint =
                            0x10000U + ((codeUnit - kHighSurrogateMinimum) << 10) + (lowSurrogate - kLowSurrogateMinimum);
                    appendUtf8(decoded, mergedCodePoint);
                    continue;
                }

                if (codeUnit >= kLowSurrogateMinimum && codeUnit <= kLowSurrogateMaximum)
                {
                    throw FormatError("\\u 转义中出现孤立的低代理项", position);
                }

                appendUtf8(decoded, codeUnit);
                continue;
            }

            decoded.push_back(decodeSimpleEscape(body, index, position));
        }

        return decoded;
    }
} // namespace AsynGyanis::Base
