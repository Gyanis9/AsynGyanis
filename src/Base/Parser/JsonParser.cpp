#include "Base/Parser/JsonParser.h"

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

    ConfigValue JsonParser::parse(const std::string_view text)
    {
        return JsonParser(text).parseDocument();
    }

    JsonParser::JsonParser(const std::string_view text) :
        m_text(text)
    {
    }

    ConfigValue JsonParser::parseDocument()
    {
        if (m_text.empty())
        {
            throw ParserError("input is empty", currentPosition());
        }

        ConfigValue document = parseValue(0);

        skipWhitespace();
        if (m_index < m_text.size())
        {
            throw ParserError("unexpected content after the end of the document", currentPosition());
        }

        return document;
    }

    ConfigValue JsonParser::parseValue(const std::size_t nestingDepth)
    {
        skipWhitespace();

        const char currentCharacter = peekCurrent();
        switch (currentCharacter)
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
                throw ParserError("expected a JSON value", currentPosition());
        }
    }

    ConfigValue JsonParser::parseObject(const std::size_t nestingDepth)
    {
        if (nestingDepth >= kMaximumNestingDepth)
        {
            throw ParserError("nesting depth limit exceeded", currentPosition());
        }

        expect('{');

        ConfigObject members;
        skipWhitespace();
        if (peekCurrent() == '}')
        {
            advance();
            return ConfigValue(std::move(members));
        }

        while (true)
        {
            skipWhitespace();
            if (peekCurrent() != '"')
            {
                throw ParserError("object keys must be double-quoted strings", currentPosition());
            }

            const ConfigValue key = parseString();
            skipWhitespace();
            expect(':');

            ConfigValue value = parseValue(nestingDepth + 1);

            // 配置场景下重复键几乎都是笔误，直接报错而不是静默覆盖
            const std::string &keyName = key.asString();
            if (members.contains(keyName))
            {
                throw ParserError("duplicate object key: " + keyName, currentPosition());
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
                return ConfigValue(std::move(members));
            }

            throw ParserError("expected ',' or '}' inside an object", currentPosition());
        }
    }

    ConfigValue JsonParser::parseArray(const std::size_t nestingDepth)
    {
        if (nestingDepth >= kMaximumNestingDepth)
        {
            throw ParserError("nesting depth limit exceeded", currentPosition());
        }

        expect('[');

        ConfigArray elements;
        skipWhitespace();
        if (peekCurrent() == ']')
        {
            advance();
            return ConfigValue(std::move(elements));
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
                return ConfigValue(std::move(elements));
            }

            throw ParserError("expected ',' or ']' inside an array", currentPosition());
        }
    }

    ConfigValue JsonParser::parseString()
    {
        expect('"');

        const ParserPosition stringStart = currentPosition();
        const std::size_t bodyStart = m_index;
        while (true)
        {
            if (m_index >= m_text.size())
            {
                throw ParserError("unterminated string", currentPosition());
            }

            const char currentCharacter = m_text[m_index];
            if (currentCharacter == '\\')
            {
                advance();
                if (m_index >= m_text.size())
                {
                    throw ParserError("input ends right after a backslash", currentPosition());
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
                throw ParserError("raw control characters are not allowed inside strings", currentPosition());
            }

            advance();
        }

        const std::string_view body = m_text.substr(bodyStart, m_index - bodyStart);
        advance(); // 消费收尾引号

        return ConfigValue(ParserText::decodeQuotedBody(body, stringStart));
    }

    ConfigValue JsonParser::parseNumber()
    {
        const std::size_t start = m_index;
        bool isFloatingPoint    = false;

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
                throw ParserError("numbers must not have leading zeros", currentPosition());
            }
        }
        else
        {
            if (!isDigit(peekCurrent()))
            {
                throw ParserError("a number must start with a digit", currentPosition());
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
                throw ParserError("a decimal point must be followed by at least one digit", currentPosition());
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
                throw ParserError("an exponent must contain at least one digit", currentPosition());
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
                return ConfigValue(integer);
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
                return ConfigValue(floatingPoint);
            }
        }

        throw ParserError("number is out of range or malformed: " + std::string(token), currentPosition());
    }

    ConfigValue JsonParser::parseLiteral(const char leadCharacter)
    {
        struct Keyword
        {
            std::string_view text;    ///< 字面量文本
            ConfigValue value;        ///< 对应的配置值
        };

        const Keyword keywords[] = {
                Keyword{"true", ConfigValue(true)},
                Keyword{"false", ConfigValue(false)},
                Keyword{"null", ConfigValue(nullptr)},
        };

        for (const Keyword &keyword: keywords)
        {
            if (leadCharacter != keyword.text.front() || m_text.substr(m_index, keyword.text.size()) != keyword.text)
            {
                continue;
            }

            // 关键字必须是完整词，truely 之类的后续字母属于非法输入
            const char characterAfterKeyword = m_index + keyword.text.size() < m_text.size()
                                                   ? m_text[m_index + keyword.text.size()]
                                                   : '\0';
            if (std::isalnum(static_cast<unsigned char>(characterAfterKeyword)) != 0)
            {
                throw ParserError("malformed keyword", currentPosition());
            }

            for (std::size_t offset = 0; offset < keyword.text.size(); ++offset)
            {
                advance();
            }
            return keyword.value;
        }

        throw ParserError("keyword must be one of true, false, null", currentPosition());
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
        }
        else
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
            throw ParserError(std::string("expected '") + expected + "'", currentPosition());
        }
        advance();
    }

    ParserPosition JsonParser::currentPosition() const noexcept
    {
        return ParserPosition{m_line, m_column, m_index};
    }
} // namespace AsynGyanis::Base
