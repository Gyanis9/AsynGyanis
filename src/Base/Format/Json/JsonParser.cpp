#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/TextEscapes.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
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
            // RFC 8259 §2：JSON 空白只有这四个字符
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

        /**
         * @brief 判断字节是否落在给定闭区间内
         * @param character 待判定字符
         * @param minimum 区间下界（含）
         * @param maximum 区间上界（含）
         * @return true 落在区间内
         */
        bool isByteInRange(const char character, const unsigned char minimum, const unsigned char maximum) noexcept
        {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= minimum && byte <= maximum;
        }
    } // namespace

    FormatValue JsonParser::parse(const std::string_view text)
    {
        return JsonParser(text, JsonParseOptions{}).parseDocument();
    }

    FormatValue JsonParser::parse(const std::string_view text, const JsonParseOptions &options)
    {
        return JsonParser(text, options).parseDocument();
    }

    JsonParser::JsonParser(const std::string_view text, const JsonParseOptions &options) :
        m_text(text)
        , m_options(options)
    {
    }

    FormatValue JsonParser::parseDocument()
    {
        // 输入长度上限最先判定：后续扫描与解码都以「已通过长度检查」为前提
        if (m_options.maximumInputLength != 0 && m_text.size() > m_options.maximumInputLength)
        {
            throw FormatError(FormatErrorKind::SizeExceeded, std::format("输入长度超出上限：{} 字节", m_options.maximumInputLength), currentPosition());
        }

        skipUtf8Bom();

        // BOM 之前视为未开始；只有 BOM（或空串）同样按「输入为空」处理
        if (m_index >= m_text.size())
        {
            throw FormatError(FormatErrorKind::EmptyInput, "输入为空", currentPosition());
        }

        FormatValue document = parseValue(0);

        skipWhitespace();
        if (m_index < m_text.size())
        {
            throw FormatError(FormatErrorKind::TrailingContent, "文档结束后出现意外内容", currentPosition());
        }

        return document;
    }

    FormatValue JsonParser::parseValue(const std::size_t nestingDepth)
    {
        skipWhitespace();

        const char currentCharacter = peekCurrent();

        // 字符串入口先判引号：单引号只在宽松模式下合法，严格模式下落回下方「应为 JSON 值」
        if (currentCharacter == '"' || (m_options.allowSingleQuotedStrings && currentCharacter == '\''))
        {
            return parseString();
        }

        switch (currentCharacter)
        {
            case '{':
                return parseObject(nestingDepth);
            case '[':
                return parseArray(nestingDepth);
            case 't':
            case 'f':
            case 'n':
                return parseLiteral(currentCharacter);
            default:
                if (currentCharacter == '-' || isDigit(currentCharacter))
                {
                    return parseNumber();
                }
                throw FormatError(FormatErrorKind::UnexpectedByte, "应为 JSON 值", currentPosition());
        }
    }

    FormatValue JsonParser::parseObject(const std::size_t nestingDepth)
    {
        if (m_options.maximumDepth != 0 && nestingDepth >= m_options.maximumDepth)
        {
            throw FormatError(FormatErrorKind::DepthExceeded, "嵌套深度超出限制", currentPosition());
        }

        expect('{');

        FormatValueObject members;
        skipWhitespace();
        if (peekCurrent() == '}')
        {
            advance();
            return FormatValue(std::move(members));
        }

        while (true)
        {
            skipWhitespace();
            if (!atStringOpeningQuote())
            {
                // 文档已到末尾说明对象括号没闭合，给出更准确的分类
                if (m_index >= m_text.size())
                {
                    throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
                }
                throw FormatError(FormatErrorKind::UnexpectedByte, m_options.allowSingleQuotedStrings ? "对象键必须是字符串" : "对象键必须是双引号字符串", currentPosition());
            }

            std::string keyName = parseStringText();
            skipWhitespace();
            expect(':');

            // 元素数上限：在解析并分配下一个成员之前拒绝
            if (m_options.maximumContainerElements != 0 && members.size() >= m_options.maximumContainerElements)
            {
                throw FormatError(FormatErrorKind::SizeExceeded, std::format("对象成员数量超出上限：{}", m_options.maximumContainerElements), currentPosition());
            }

            FormatValue value = parseValue(nestingDepth + 1);

            // 配置场景下重复键几乎都是笔误，直接报错而不是静默覆盖。
            // 用一次 emplace 的返回值同时完成插入与查重：命中已有键时其键文本与新键完全相同，
            // 因此报错文案与逐字比较时代保持一致，但少了一次哈希查找。
            // 键文本此处已无其它用途，按右值交给 emplace 直接移入节点，长键因此省掉一次堆分配
            if (const auto [insertedIterator, inserted] = members.emplace(std::move(keyName), std::move(value)); !inserted)
            {
                throw FormatError(FormatErrorKind::DuplicateKey, "对象存在重复键：" + insertedIterator->first, currentPosition());
            }

            skipWhitespace();
            const char separator = peekCurrent();
            if (separator == ',')
            {
                advance();
                skipWhitespace();
                // 尾逗号：宽松模式下 ',' 紧跟 '}' 视为结束
                if (m_options.allowTrailingCommas && peekCurrent() == '}')
                {
                    advance();
                    return FormatValue(std::move(members));
                }
                continue;
            }
            if (separator == '}')
            {
                advance();
                return FormatValue(std::move(members));
            }
            if (m_index >= m_text.size())
            {
                throw FormatError(FormatErrorKind::UnterminatedContainer, "对象未闭合", currentPosition());
            }

            throw FormatError(FormatErrorKind::UnexpectedByte, "对象内应为 ',' 或 '}'", currentPosition());
        }
    }

    FormatValue JsonParser::parseArray(const std::size_t nestingDepth)
    {
        if (m_options.maximumDepth != 0 && nestingDepth >= m_options.maximumDepth)
        {
            throw FormatError(FormatErrorKind::DepthExceeded, "嵌套深度超出限制", currentPosition());
        }

        expect('[');

        FormatValueArray elements;
        skipWhitespace();
        if (peekCurrent() == ']')
        {
            advance();
            return FormatValue(std::move(elements));
        }

        while (true)
        {
            // 元素数上限：在解析并分配下一个元素之前拒绝
            if (m_options.maximumContainerElements != 0 && elements.size() >= m_options.maximumContainerElements)
            {
                throw FormatError(FormatErrorKind::SizeExceeded, std::format("数组元素数量超出上限：{}", m_options.maximumContainerElements), currentPosition());
            }

            elements.push_back(parseValue(nestingDepth + 1));

            skipWhitespace();
            const char separator = peekCurrent();
            if (separator == ',')
            {
                advance();
                skipWhitespace();
                // 尾逗号：宽松模式下 ',' 紧跟 ']' 视为结束
                if (m_options.allowTrailingCommas && peekCurrent() == ']')
                {
                    advance();
                    return FormatValue(std::move(elements));
                }
                continue;
            }
            if (separator == ']')
            {
                advance();
                return FormatValue(std::move(elements));
            }
            if (m_index >= m_text.size())
            {
                throw FormatError(FormatErrorKind::UnterminatedContainer, "数组未闭合", currentPosition());
            }

            throw FormatError(FormatErrorKind::UnexpectedByte, "数组内应为 ',' 或 ']'", currentPosition());
        }
    }

    std::string JsonParser::parseStringText()
    {
        // 调用方已确认起始引号合法，双引号恒合法、单引号仅在宽松模式下合法
        const char         quote       = peekCurrent();
        const TextPosition stringStart = currentPosition();
        advance(); // 消费起始引号

        const std::size_t  bodyStart         = m_index;
        const TextPosition bodyStartPosition = currentPosition();

        while (true)
        {
            if (m_index >= m_text.size())
            {
                throw FormatError(FormatErrorKind::UnterminatedString, "字符串未闭合", currentPosition());
            }

            const char currentCharacter = m_text[m_index];
            if (currentCharacter == '\\')
            {
                // 转义引导符与紧随其后的字符一起越过：即便被转义的是收尾引号也不会误判
                advance();
                if (m_index >= m_text.size())
                {
                    throw FormatError(FormatErrorKind::InvalidEscape, "反斜杠后输入即结束", currentPosition());
                }
                advance();
                continue;
            }

            if (currentCharacter == quote)
            {
                break;
            }

            // RFC 8259 §7：字符串内 U+0000..U+001F 必须以转义形式出现
            if (static_cast<unsigned char>(currentCharacter) < 0x20U)
            {
                throw FormatError(FormatErrorKind::ControlCharacter, "字符串内不允许出现原始控制字符", currentPosition());
            }

            advance();
        }

        const std::size_t bodyEnd = m_index;

        // 长度上限按原文（含转义序列）字节数计：在解码分配之前即可拒绝超限字符串
        if (m_options.maximumStringLength != 0 && bodyEnd - bodyStart > m_options.maximumStringLength)
        {
            throw FormatError(FormatErrorKind::SizeExceeded, std::format("字符串长度超出上限：{} 字节", m_options.maximumStringLength), bodyStartPosition);
        }

        validateStringUtf8(bodyStart, bodyEnd, bodyStartPosition);

        const std::string_view body = m_text.substr(bodyStart, bodyEnd - bodyStart);
        advance(); // 消费收尾引号

        try
        {
            return TextEscapes::decodeQuotedBody(body, stringStart);
        } catch (const FormatError &error)
        {
            // TextEscapes 是 JSON 与 YAML 共用的原语，不认识 JSON 的错误分类；
            // 这里按原因文本补上分类后重抛，位置与抛出点沿用原错误，文本保持逐字不变
            const std::string &   reason = error.reason();
            const FormatErrorKind kind   = reason.find("代理项") != std::string::npos
                                             ? FormatErrorKind::SurrogatePairError
                                             : FormatErrorKind::InvalidEscape;
            throw FormatError(kind, reason, error.position(), error.location());
        }
    }

    FormatValue JsonParser::parseString()
    {
        // 字符串值：解码结果直接包成节点。对象成员名走 parseStringText()，
        // 避免为「只当键用」的字符串多构造一个 FormatValue 临时量
        return FormatValue(parseStringText());
    }

    FormatValue JsonParser::parseNumber()
    {
        const std::size_t start           = m_index;
        bool              isFloatingPoint = false;
        const bool        isNegative      = peekCurrent() == '-';

        if (isNegative)
        {
            advance();
        }

        // 整数部分：0 之后不能再跟数字（RFC 8259 §6 禁止 01 这类前导零）
        if (peekCurrent() == '0')
        {
            advance();
            if (isDigit(peekCurrent()))
            {
                throw FormatError(FormatErrorKind::InvalidNumber, "数字不得有前导零", currentPosition());
            }
        } else
        {
            if (!isDigit(peekCurrent()))
            {
                throw FormatError(FormatErrorKind::InvalidNumber, "数字必须以数字开头", currentPosition());
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
                throw FormatError(FormatErrorKind::InvalidNumber, "小数点后必须至少有一位数字", currentPosition());
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
                throw FormatError(FormatErrorKind::InvalidNumber, "指数部分必须至少有一位数字", currentPosition());
            }
            while (isDigit(peekCurrent()))
            {
                advance();
            }
        }

        const std::string_view token = m_text.substr(start, m_index - start);

        if (!isFloatingPoint)
        {
            if (!isNegative)
            {
                // 非负整数三档取值：Int → UInt → Double，宁可退化为浮点也不凭空造数
                std::int64_t signedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), signedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return FormatValue(signedInteger);
                }

                std::uint64_t unsignedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), unsignedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return FormatValue(unsignedInteger);
                }
            } else
            {
                std::int64_t signedInteger = 0;
                if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), signedInteger);
                    errorCode == std::errc() && pointer == token.data() + token.size())
                {
                    return FormatValue(signedInteger);
                }
            }

            // 超出 64 位整数范围：退化为 double，保证数值本身不丢失（精度按 IEEE 754 舍入）
            isFloatingPoint = true;
        }

        if (isFloatingPoint)
        {
            double floatingPoint = 0.0;
            if (const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), floatingPoint);
                errorCode == std::errc() && pointer == token.data() + token.size())
            {
                return FormatValue(floatingPoint);
            }
        }

        throw FormatError(FormatErrorKind::NumberOutOfRange, "数字超出范围或格式错误：" + std::string(token), currentPosition());
    }

    FormatValue JsonParser::parseLiteral(const char leadCharacter)
    {
        // 首字符已由 parseValue 限定为 t/f/n，据此唯一确定关键字文本。
        // 关键字表改为静态存储的 string_view：FormatValue 不是字面类型，原先那张
        // {text, FormatValue} 局部表每次解析都要在栈上构造并析构 3 个变体，
        // 而三个关键字的文本长度不同、比较次数也被首字符分派压到一次
        const std::string_view keywordText = leadCharacter == 't' ? std::string_view{"true"} : leadCharacter == 'f' ? std::string_view{"false"} : std::string_view{"null"};

        if (m_text.substr(m_index, keywordText.size()) != keywordText)
        {
            throw FormatError(FormatErrorKind::InvalidKeyword, "关键字必须是 true、false、null 之一", currentPosition());
        }

        // 关键字必须是完整词，truely 之类的后续字母属于非法输入
        const char characterAfterKeyword = m_index + keywordText.size() < m_text.size()
                                               ? m_text[m_index + keywordText.size()]
                                               : '\0';
        if (std::isalnum(static_cast<unsigned char>(characterAfterKeyword)) != 0)
        {
            throw FormatError(FormatErrorKind::InvalidKeyword, "关键字格式错误", currentPosition());
        }

        for (std::size_t offset = 0; offset < keywordText.size(); ++offset)
        {
            advance();
        }

        switch (leadCharacter)
        {
            case 't':
                return FormatValue(true);
            case 'f':
                return FormatValue(false);
            default:
                return FormatValue(nullptr);
        }
    }

    void JsonParser::skipWhitespace()
    {
        while (m_index < m_text.size())
        {
            const char currentCharacter = m_text[m_index];
            if (isJsonWhitespace(currentCharacter))
            {
                advance();
                continue;
            }

            // 注释只在宽松模式下生效，且必须成对引导，否则当普通非法字符留给上层报错
            if (m_options.allowComments && currentCharacter == '/' && m_index + 1 < m_text.size() &&
                (m_text[m_index + 1] == '/' || m_text[m_index + 1] == '*'))
            {
                skipComment();
                continue;
            }

            break;
        }
    }

    void JsonParser::skipComment()
    {
        const TextPosition commentStart = currentPosition();

        advance(); // 消费 '/'
        if (peekCurrent() == '/')
        {
            // 行注释：内容直到换行（不含）或文档末尾
            while (m_index < m_text.size() && m_text[m_index] != '\n')
            {
                advance();
            }
            return;
        }

        advance(); // 消费 '*'
        while (true)
        {
            // 需要同时看到 '*' 与 '/'，因此末尾不足两个字符即视为未闭合
            if (m_index + 1 >= m_text.size())
            {
                throw FormatError(FormatErrorKind::UnterminatedComment, "块注释未闭合", commentStart);
            }
            if (m_text[m_index] == '*' && m_text[m_index + 1] == '/')
            {
                advance();
                advance();
                return;
            }
            advance();
        }
    }

    void JsonParser::skipUtf8Bom() noexcept
    {
        if (!m_options.skipUtf8Bom)
        {
            return;
        }

        // EF BB BF 属于编码层标记而非 JSON 空白（RFC 8259 §8.1 指出实现不得自行添加），
        // 这里按选项跳过；行列号保持 1 起始，BOM 不参与计数以免污染诊断信息
        if (m_text.size() >= 3 &&
            static_cast<unsigned char>(m_text[0]) == 0xEFU &&
            static_cast<unsigned char>(m_text[1]) == 0xBBU &&
            static_cast<unsigned char>(m_text[2]) == 0xBFU)
        {
            m_index = 3;
        }
    }

    void JsonParser::validateStringUtf8(const std::size_t bodyStart, const std::size_t bodyEnd, const TextPosition &bodyStartPosition) const
    {
        // 正文中不可能出现换行（裸控制字符已被拒绝），因此出错位置只需按字节偏移平移列号
        const auto failurePosition = [bodyStart, &bodyStartPosition](const std::size_t failureIndex) noexcept
        {
            TextPosition position = bodyStartPosition;
            position.columnNumber += failureIndex - bodyStart;
            position.offset       = failureIndex;
            return position;
        };

        std::size_t index = bodyStart;
        while (index < bodyEnd)
        {
            const auto leadByte = static_cast<unsigned char>(m_text[index]);
            if (leadByte < 0x80U)
            {
                // ASCII 快路径；U+007F（DEL）在 RFC 8259 §7 下合法，原样保留
                ++index;
                continue;
            }

            // RFC 3629：由首字节决定序列长度，并给出第二个字节的合法区间
            std::size_t   sequenceLength    = 0;
            unsigned char secondByteMinimum = 0x80U;
            unsigned char secondByteMaximum = 0xBFU;

            if (leadByte >= 0xC2U && leadByte <= 0xDFU)
            {
                sequenceLength = 2; // C0/C1 属于过长编码，必须拒绝
            } else if (leadByte == 0xE0U)
            {
                sequenceLength    = 3;
                secondByteMinimum = 0xA0U; // 排除过长编码
            } else if (leadByte >= 0xE1U && leadByte <= 0xECU)
            {
                sequenceLength = 3;
            } else if (leadByte == 0xEDU)
            {
                sequenceLength    = 3;
                secondByteMaximum = 0x9FU; // 排除以 UTF-8 编码的代理项 U+D800..U+DFFF
            } else if (leadByte >= 0xEEU && leadByte <= 0xEFU)
            {
                sequenceLength = 3;
            } else if (leadByte == 0xF0U)
            {
                sequenceLength    = 4;
                secondByteMinimum = 0x90U; // 排除过长编码
            } else if (leadByte >= 0xF1U && leadByte <= 0xF3U)
            {
                sequenceLength = 4;
            } else if (leadByte == 0xF4U)
            {
                sequenceLength    = 4;
                secondByteMaximum = 0x8FU; // 上限 U+10FFFF
            } else
            {
                // 0x80..0xC1 与 0xF5..0xFF 都不是合法的首字节
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index));
            }

            if (index + sequenceLength > bodyEnd)
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中的 UTF-8 序列不完整", failurePosition(index));
            }
            if (!isByteInRange(m_text[index + 1], secondByteMinimum, secondByteMaximum))
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index));
            }
            for (std::size_t offset = 2; offset < sequenceLength; ++offset)
            {
                if (!isByteInRange(m_text[index + offset], 0x80U, 0xBFU))
                {
                    throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", failurePosition(index + offset));
                }
            }

            index += sequenceLength;
        }
    }

    bool JsonParser::atStringOpeningQuote() const noexcept
    {
        const char currentCharacter = peekCurrent();
        return currentCharacter == '"' || (m_options.allowSingleQuotedStrings && currentCharacter == '\'');
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
            throw FormatError(FormatErrorKind::UnexpectedByte, std::string("应为 '") + expected + "'", currentPosition());
        }
        advance();
    }

    TextPosition JsonParser::currentPosition() const noexcept
    {
        return TextPosition{.lineNumber = m_line, .columnNumber = m_column, .offset = m_index};
    }
} // namespace AsynGyanis::Base
