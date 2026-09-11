#include "Base/Parser/Json/JsonParser.h"
#include "Base/Parser/ParserError.h"
#include "Base/Parser/ParserText.h"

#include <cctype>
#include <charconv>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 判断字符是否为 JSON 空白
         * @param character 待判定字符
         * @return true 属于空格、制表、换行或回车
         */
        bool isJsonWhitespace(const char character) noexcept
        {
            return character == ' ' || character == '\t' || character == '\n' || character == '\r';
        }

        /**
         * @brief 判断字符是否为十进制数字
         * @param character 待判定字符
         * @return true 属于 [0-9]
         */
        bool isDigit(const char character) noexcept
        {
            return character >= '0' && character <= '9';
        }
    } // namespace

    ParserValue JsonParser::parse(const std::string_view text)
    {
        return JsonParser(text).parseDocument();
    }

    JsonParser::JsonParser(const std::string_view text) :
        m_text(text)
    {
    }

    ParserValue JsonParser::parseDocument()
    {
        if (m_text.empty())
        {
            throw ParserError("输入为空", currentPosition());
        }

        ParserValue document = parseValue(0);

        skipWhitespace();
        if (m_index < m_text.size())
        {
            throw ParserError("文档结束后出现意外内容", currentPosition());
        }

        return document;
    }

    ParserValue JsonParser::parseValue(const std::size_t nestingDepth)
    {
        skipWhitespace();

        switch (const char currentCharacter = peekCurrent())
        {
            case '{':
                return parseObject(nestingDepth);
            case '[':
                return parseArray(nestingDepth);
            case '"':
                return parseString();
            case 't':
            case 'f':
            case 'n':
                return parseLiteral(currentCharacter);
            default:
                if (currentCharacter == '-' || isDigit(currentCharacter))
                {
                    return parseNumber();
                }
                throw ParserError("应为 JSON 值", currentPosition());
        }
    }

    ParserValue JsonParser::parseObject(const std::size_t nestingDepth)
    {
        if (nestingDepth >= kMaximumNestingDepth)
        {
            throw ParserError("嵌套深度超出限制", currentPosition());
        }

        expect('{');

        ParserValueObject members;
        skipWhitespace();
        if (peekCurrent() == '}')
        {
            advance();
            return ParserValue(std::move(members));
        }

        while (true)
        {
            skipWhitespace();
            if (peekCurrent() != '"')
            {
                throw ParserError("对象键必须是双引号字符串", currentPosition());
            }

            const ParserValue key = parseString();
            skipWhitespace();
            expect(':');

            ParserValue value = parseValue(nestingDepth + 1);

            // 配置场景下重复键几乎都是笔误，直接报错而不是静默覆盖
            const std::string &keyName = key.asString();
            if (members.contains(keyName))
            {
                throw ParserError("对象存在重复键：" + keyName, currentPosition());
            }
            members.emplace(keyName, std::move(value));

            skipWhitespace();
            const char separator = peekCurrent();
            if (separator == ',')
            {
                advance();
                continue;
            }
            if (separator == '}')
            {
                advance();
                return ParserValue(std::move(members));
            }

            throw ParserError("对象内应为 ',' 或 '}'", currentPosition());
        }
    }

    ParserValue JsonParser::parseArray(const std::size_t nestingDepth)
    {
        if (nestingDepth >= kMaximumNestingDepth)
        {
            throw ParserError("嵌套深度超出限制", currentPosition());
        }

        expect('[');

        ParserValueArray elements;
        skipWhitespace();
        if (peekCurrent() == ']')
        {
            advance();
            return ParserValue(std::move(elements));
        }

        while (true)
        {
            elements.push_back(parseValue(nestingDepth + 1));

            skipWhitespace();
            const char separator = peekCurrent();
            if (separator == ',')
            {
                advance();
                continue;
            }
            if (separator == ']')
            {
                advance();
                return ParserValue(std::move(elements));
            }

            throw ParserError("数组内应为 ',' 或 ']'", currentPosition());
        }
    }

    ParserValue JsonParser::parseString()
    {
        expect('"');

        const ParserPosition stringStart = currentPosition();
        const std::size_t    bodyStart   = m_index;
        while (true)
        {
            if (m_index >= m_text.size())
            {
                throw ParserError("字符串未闭合", currentPosition());
            }

            const char currentCharacter = m_text[m_index];
            if (currentCharacter == '\\')
            {
                advance();
                if (m_index >= m_text.size())
                {
                    throw ParserError("反斜杠后输入即结束", currentPosition());
                }
                advance();
                continue;
            }

            if (currentCharacter == '"')
            {
                break;
            }

            if (static_cast<unsigned char>(currentCharacter) < 0x20)
            {
                throw ParserError("字符串内不允许出现原始控制字符", currentPosition());
            }

            advance();
        }

        const std::string_view body = m_text.substr(bodyStart, m_index - bodyStart);
        advance(); // 消费收尾引号

        return ParserValue(ParserText::decodeQuotedBody(body, stringStart));
    }

    ParserValue JsonParser::parseNumber()
    {
        const std::size_t start           = m_index;
        bool              isFloatingPoint = false;

        if (peekCurrent() == '-')
        {
            advance();
        }

        // 整数部分：0 之后不能再跟数字（禁止 01 这类前导零）
        if (peekCurrent() == '0')
        {
            advance();
            if (isDigit(peekCurrent()))
            {
                throw ParserError("数字不得有前导零", currentPosition());
            }
        } else
        {
            if (!isDigit(peekCurrent()))
            {
                throw ParserError("数字必须以数字开头", currentPosition());
            }
            while (isDigit(peekCurrent()))
            {
                advance();
            }
        }

        if (peekCurrent() == '.')
        {
            isFloatingPoint = true;
            advance();
            if (!isDigit(peekCurrent()))
            {
                throw ParserError("小数点后必须至少有一位数字", currentPosition());
            }
            while (isDigit(peekCurrent()))
            {
                advance();
            }
        }

        if (peekCurrent() == 'e' || peekCurrent() == 'E')
        {
            isFloatingPoint = true;
            advance();
            if (peekCurrent() == '+' || peekCurrent() == '-')
            {
                advance();
            }
            if (!isDigit(peekCurrent()))
            {
                throw ParserError("指数部分必须至少有一位数字", currentPosition());
            }
            while (isDigit(peekCurrent()))
            {
                advance();
            }
        }

        const std::string_view token = m_text.substr(start, m_index - start);

        if (!isFloatingPoint)
        {
            std::int64_t integer = 0;
            if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), integer);
                errorCode == std::errc() && pointer == token.data() + token.size())
            {
                return ParserValue(integer);
            }
            // 超出 int64 范围的整数按浮点处理，保证数值不丢失
            isFloatingPoint = true;
        }

        if (isFloatingPoint)
        {
            double floatingPoint = 0.0;
            if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), floatingPoint);
                errorCode == std::errc() && pointer == token.data() + token.size())
            {
                return ParserValue(floatingPoint);
            }
        }

        throw ParserError("数字超出范围或格式错误：" + std::string(token), currentPosition());
    }

    ParserValue JsonParser::parseLiteral(const char leadCharacter)
    {
        struct Keyword
        {
            std::string_view text;  ///< 字面量文本
            ParserValue      value; ///< 对应的配置值
        };

        const Keyword keywords[] = {
                Keyword{.text = "true", .value = ParserValue(true)},
                Keyword{.text = "false", .value = ParserValue(false)},
                Keyword{.text = "null", .value = ParserValue(nullptr)},
        };

        for (const auto &[text, value]: keywords)
        {
            if (leadCharacter != text.front() || m_text.substr(m_index, text.size()) != text)
            {
                continue;
            }

            // 关键字必须是完整词，truely 之类的后续字母属于非法输入
            const char characterAfterKeyword = m_index + text.size() < m_text.size()
                                                   ? m_text[m_index + text.size()]
                                                   : '\0';
            if (std::isalnum(static_cast<unsigned char>(characterAfterKeyword)) != 0)
            {
                throw ParserError("关键字格式错误", currentPosition());
            }

            for (std::size_t offset = 0; offset < text.size(); ++offset)
            {
                advance();
            }
            return value;
        }

        throw ParserError("关键字必须是 true、false、null 之一", currentPosition());
    }

    void JsonParser::skipWhitespace()
    {
        while (m_index < m_text.size() && isJsonWhitespace(m_text[m_index]))
        {
            advance();
        }
    }

    char JsonParser::peekCurrent() const noexcept
    {
        return m_index < m_text.size() ? m_text[m_index] : '\0';
    }

    void JsonParser::advance() noexcept
    {
        if (m_index >= m_text.size())
        {
            return;
        }

        if (m_text[m_index] == '\n')
        {
            ++m_line;
            m_column = 1;
        } else
        {
            ++m_column;
        }
        ++m_index;
    }

    void JsonParser::expect(const char expected)
    {
        skipWhitespace();
        if (peekCurrent() != expected)
        {
            throw ParserError(std::string("应为 '") + expected + "'", currentPosition());
        }
        advance();
    }

    ParserPosition JsonParser::currentPosition() const noexcept
    {
        return ParserPosition{.lineNumber = m_line, .columnNumber = m_column, .offset = m_index};
    }
} // namespace AsynGyanis::Base
