#include "Base/Parser/Yaml/YamlParser.h"
#include "Base/Parser/ParserError.h"
#include "Base/Parser/ParserText.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 去掉首尾空白
         * @param text 原始文本
         * @return std::string_view 裁剪后的视图
         */
        std::string_view trim(const std::string_view text) noexcept
        {
            const std::size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const std::size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        /**
         * @brief 去掉前导空格
         * @param text 原始文本
         * @return std::string_view 裁剪后的视图
         */
        std::string_view trimLeft(const std::string_view text) noexcept
        {
            const std::size_t begin = text.find_first_not_of(' ');
            return begin == std::string_view::npos ? std::string_view{} : text.substr(begin);
        }

        /**
         * @brief 统计前导空格数量
         * @param text 原始文本
         * @return std::size_t 空格个数
         */
        std::size_t countLeadingSpaces(const std::string_view text) noexcept
        {
            return text.find_first_not_of(' ') == std::string_view::npos ? text.size() : text.find_first_not_of(' ');
        }

        /**
         * @brief 忽略大小写比较文本与给定字面量
         * @param text 待比较文本
         * @param keyword 全小写字面量
         * @return true 相等
         */
        bool equalsIgnoringCase(std::string_view text, std::string_view keyword) noexcept
        {
            return text.size() == keyword.size() &&
                   std::equal(text.begin(), text.end(), keyword.begin(),
                              [](const char left, const char right)
                              {
                                  return std::tolower(static_cast<unsigned char>(left)) == right;
                              });
        }

        /**
         * @brief 定位双引号字符串的收尾引号
         * @param text 输入文本
         * @param openingQuote 起始引号下标
         * @return std::size_t 收尾引号下标，未闭合返回 npos
         */
        std::size_t findClosingDoubleQuote(const std::string_view text, const std::size_t openingQuote) noexcept
        {
            for (std::size_t index = openingQuote + 1; index < text.size(); ++index)
            {
                if (text[index] == '\\')
                {
                    ++index;
                    continue;
                }
                if (text[index] == '"')
                {
                    return index;
                }
            }
            return std::string_view::npos;
        }

        /**
         * @brief 定位单引号字符串的收尾引号（'' 表示一个引号）
         * @param text 输入文本
         * @param openingQuote 起始引号下标
         * @return std::size_t 收尾引号下标，未闭合返回 npos
         */
        std::size_t findClosingSingleQuote(const std::string_view text, const std::size_t openingQuote) noexcept
        {
            for (std::size_t index = openingQuote + 1; index < text.size(); ++index)
            {
                if (text[index] != '\'')
                {
                    continue;
                }
                if (index + 1 < text.size() && text[index + 1] == '\'')
                {
                    ++index;
                    continue;
                }
                return index;
            }
            return std::string_view::npos;
        }

        /**
         * @brief 把单引号标量内的 '' 还原为单个引号
         * @param body 引号内正文
         * @return std::string 还原后的文本
         */
        std::string expandSingleQuoteEscapes(const std::string_view body)
        {
            std::string result;
            result.reserve(body.size());

            for (std::size_t index = 0; index < body.size(); ++index)
            {
                if (body[index] == '\'' && index + 1 < body.size() && body[index + 1] == '\'')
                {
                    result.push_back('\'');
                    ++index;
                    continue;
                }
                result.push_back(body[index]);
            }
            return result;
        }

        /**
         * @brief 构造一个指向行内某列的错误位置
         * @param number 行号
         * @param column 列号
         * @return ParserPosition 位置对象
         */
        ParserPosition makePosition(const std::size_t number, const std::size_t column) noexcept
        {
            return ParserPosition{number, column, 0};
        }

        /**
         * @brief 拒绝配置子集之外的 YAML 高级特性
         * @param featureName 特性名称
         * @param position 出错位置
         */
        [[noreturn]] void rejectUnsupportedFeature(const std::string &featureName, const ParserPosition &position)
        {
            throw ParserError("不支持的 YAML 特性：" + featureName, position);
        }
    } // namespace

    ParserValue YamlParser::parse(const std::string_view text)
    {
        YamlParser parser(text);
        parser.splitIntoLines();

        if (parser.m_lines.empty())
        {
            return ParserValue(ParserValueObject{});
        }

        std::size_t       index      = 0;
        const Line &      firstLine  = parser.m_lines.front();
        const std::size_t rootIndent = firstLine.indent;

        // 根节点直接按首行形态分派：parseBlock 的「必须比父级更深」判定不适用於顶层
        ParserValue root = startsSequenceEntry(firstLine.content)
                               ? parser.parseSequence(index, rootIndent, 0)
                               : parser.parseMapping(index, rootIndent, 0);

        if (index < parser.m_lines.size())
        {
            const Line &leftover = parser.m_lines[index];
            throw ParserError("文档根级出现意外的缩进", makePosition(leftover.number, leftover.indent + 1));
        }

        return root;
    }

    YamlParser::YamlParser(const std::string_view text) :
        m_text(text)
    {
    }

    void YamlParser::splitIntoLines()
    {
        std::size_t       number    = 0;
        std::size_t       start     = 0;
        const std::size_t inputSize = m_text.size();

        while (start <= inputSize)
        {
            ++number;

            const std::size_t lineEnd = m_text.find('\n', start);
            std::string_view  rawLine = lineEnd == std::string_view::npos ? m_text.substr(start) : m_text.substr(start, lineEnd - start);

            std::size_t indent = 0;
            while (indent < rawLine.size() && rawLine[indent] == ' ')
            {
                ++indent;
            }

            if (std::string_view content = trim(rawLine.substr(indent)); content.empty() || content.front() == '#')
            {
                // 空行与整行注释不参与结构，因此也不受制表符缩进规则约束
            } else if (indent < rawLine.size() && rawLine[indent] == '\t')
            {
                throw ParserError("缩进禁止使用制表符", makePosition(number, indent + 1));
            } else
            {
                if (content == "---" || content == "..." || content.substr(0, std::min<std::size_t>(content.size(), 4)) == "--- ")
                {
                    rejectUnsupportedFeature("多文档", makePosition(number, indent + 1));
                }
                if (content.front() == '%')
                {
                    rejectUnsupportedFeature("指令行", makePosition(number, indent + 1));
                }

                // 行尾注释只在引号之外生效
                bool        insideDoubleQuote = false;
                bool        insideSingleQuote = false;
                std::size_t commentStart      = std::string_view::npos;
                for (std::size_t index = 0; index < content.size(); ++index)
                {
                    const char character = content[index];
                    if (character == '"' && !insideSingleQuote)
                    {
                        if (!insideDoubleQuote)
                        {
                            const std::size_t closing = findClosingDoubleQuote(content, index);
                            if (closing == std::string_view::npos)
                            {
                                throw ParserError("双引号字符串未闭合", makePosition(number, indent + index + 1));
                            }
                            index = closing;
                        } else
                        {
                            insideDoubleQuote = false;
                        }
                        continue;
                    }
                    if (character == '\'' && !insideDoubleQuote)
                    {
                        if (!insideSingleQuote)
                        {
                            const std::size_t closing = findClosingSingleQuote(content, index);
                            if (closing == std::string_view::npos)
                            {
                                throw ParserError("单引号字符串未闭合", makePosition(number, indent + index + 1));
                            }
                            index = closing;
                        } else
                        {
                            insideSingleQuote = false;
                        }
                        continue;
                    }
                    if (character == '#' && !insideDoubleQuote && !insideSingleQuote)
                    {
                        // 只有前置空白时 # 才是注释起点，其余情况属于标量文本
                        if (index == 0 || content[index - 1] == ' ')
                        {
                            commentStart = index;
                            break;
                        }
                    }
                }

                if (commentStart != std::string_view::npos)
                {
                    content = trim(content.substr(0, commentStart));
                }

                if (!content.empty())
                {
                    m_lines.push_back(Line{.content = content, .indent = indent, .number = number});
                }
            }

            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            start = lineEnd + 1;
        }
    }

    ParserValue YamlParser::parseBlock(std::size_t &index, const std::size_t parentIndent, const std::size_t nestingDepth)
    {
        if (nestingDepth >= kMaximumNestingDepth)
        {
            throw ParserError("嵌套深度超出限制", index < m_lines.size()
                                                                  ? makePosition(m_lines[index].number, m_lines[index].indent + 1)
                                                                  : ParserPosition{});
        }

        if (index >= m_lines.size() || m_lines[index].indent <= parentIndent)
        {
            // 该层级没有任何内容，按 YAML 语义这是 null 而不是空映射
            return ParserValue(nullptr);
        }

        const std::size_t blockIndent = m_lines[index].indent;
        if (startsSequenceEntry(m_lines[index].content))
        {
            return parseSequence(index, blockIndent, nestingDepth + 1);
        }
        return parseMapping(index, blockIndent, nestingDepth + 1);
    }

    ParserValue YamlParser::parseMapping(std::size_t &index, const std::size_t blockIndent, const std::size_t nestingDepth)
    {
        ParserValueObject members;

        while (index < m_lines.size())
        {
            const auto &[content, indent, number] = m_lines[index];

            if (indent < blockIndent)
            {
                break;
            }
            if (indent > blockIndent)
            {
                throw ParserError("映射内缩进不一致", makePosition(number, indent + 1));
            }
            if (startsSequenceEntry(content))
            {
                throw ParserError("序列条目不能与映射键同级", makePosition(number, indent + 1));
            }

            const std::size_t separator = findKeyValueSeparator(content);
            if (separator == std::string_view::npos)
            {
                throw ParserError("应为 'key: value'", makePosition(number, indent + 1));
            }

            const std::string_view keyText = trim(content.substr(0, separator));
            if (keyText.empty())
            {
                throw ParserError("映射键不能为空", makePosition(number, indent + 1));
            }

            std::string_view  remainder     = content.substr(separator + 1);
            const std::size_t leadingSpaces = countLeadingSpaces(remainder);
            remainder                       = trim(remainder);
            ++index;

            ParserValue value;
            if (remainder.empty())
            {
                value = parseBlock(index, blockIndent, nestingDepth);
            } else
            {
                value = parseInlineValue(remainder, number, indent + separator + 1 + leadingSpaces + 1);
            }

            // 键文本允许带引号（书写含特殊字符的键），裸键一律按字面文本处理
            std::string keyName;
            if (keyText.front() == '"' || keyText.front() == '\'')
            {
                const ParserValue decodedKey = parseInlineValue(keyText, number, indent + 1);
                if (!decodedKey.is<std::string>())
                {
                    throw ParserError("映射键必须是字符串", makePosition(number, indent + 1));
                }
                keyName = decodedKey.asString();
            } else
            {
                keyName = std::string(keyText);
            }

            if (members.contains(keyName))
            {
                throw ParserError("重复的键：" + keyName, makePosition(number, indent + 1));
            }
            members.emplace(keyName, std::move(value));
        }

        return ParserValue(std::move(members));
    }

    ParserValue YamlParser::parseSequence(std::size_t &index, const std::size_t blockIndent, const std::size_t nestingDepth)
    {
        ParserValueArray elements;

        while (index < m_lines.size())
        {
            const auto &[content, indent, number] = m_lines[index];

            if (indent < blockIndent || !startsSequenceEntry(content))
            {
                if (indent > blockIndent)
                {
                    throw ParserError("序列内缩进不一致", makePosition(number, indent + 1));
                }
                break;
            }

            std::string_view  itemText      = trimLeft(content.substr(1));
            const std::size_t leadingSpaces = content.size() - itemText.size() - 1;

            if (itemText.empty())
            {
                ++index;
                elements.push_back(parseBlock(index, blockIndent, nestingDepth));
                continue;
            }

            const std::size_t itemIndent = indent + 1 + leadingSpaces;

            // 「- key: value」是内联开始的映射，就地改写这一行后按块解析，
            // 使后续同列的兄弟键自然并入同一个元素
            if (startsSequenceEntry(itemText) || findKeyValueSeparator(itemText) != std::string_view::npos)
            {
                m_lines[index] = Line{.content = itemText, .indent = itemIndent, .number = number};
                elements.push_back(parseBlock(index, blockIndent, nestingDepth));
                continue;
            }

            elements.push_back(parseInlineValue(itemText, number, itemIndent + 1));
            ++index;
        }

        return ParserValue(std::move(elements));
    }

    ParserValue YamlParser::parseInlineValue(std::string_view text, const std::size_t number, const std::size_t column)
    {
        const ParserPosition position = makePosition(number, column);

        if (text.empty())
        {
            return ParserValue(nullptr);
        }

        switch (text.front())
        {
            case '[':
            {
                std::size_t cursor = 0;
                ParserValue value  = parseFlowSequence(text, cursor, number);
                if (!trim(text.substr(cursor)).empty())
                {
                    throw ParserError("流式序列之后出现意外内容", position);
                }
                return value;
            }
            case '{':
            {
                std::size_t cursor = 0;
                ParserValue value  = parseFlowMapping(text, cursor, number);
                if (!trim(text.substr(cursor)).empty())
                {
                    throw ParserError("流式映射之后出现意外内容", position);
                }
                return value;
            }
            case '"':
            {
                const std::size_t closing = findClosingDoubleQuote(text, 0);
                if (closing == std::string_view::npos)
                {
                    throw ParserError("双引号字符串未闭合", position);
                }
                if (!trim(text.substr(closing + 1)).empty())
                {
                    throw ParserError("带引号标量之后出现意外内容", position);
                }
                return ParserValue(ParserText::decodeQuotedBody(text.substr(1, closing - 1), position));
            }
            case '\'':
            {
                const std::size_t closing = findClosingSingleQuote(text, 0);
                if (closing == std::string_view::npos)
                {
                    throw ParserError("单引号字符串未闭合", position);
                }
                if (!trim(text.substr(closing + 1)).empty())
                {
                    throw ParserError("带引号标量之后出现意外内容", position);
                }
                return ParserValue(expandSingleQuoteEscapes(text.substr(1, closing - 1)));
            }
            case '&':
            case '*':
                rejectUnsupportedFeature("锚点与别名", position);
            case '|':
            case '>':
                rejectUnsupportedFeature("块标量", position);
            default:
                break;
        }

        if (text.find("!!") != std::string_view::npos)
        {
            rejectUnsupportedFeature("显式类型标签", position);
        }

        return inferScalar(text);
    }

    ParserValue YamlParser::parseFlowValue(std::string_view text, std::size_t &cursor, const std::size_t number)
    {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
        {
            ++cursor;
        }

        if (cursor >= text.size())
        {
            throw ParserError("流式集合内出现意外的行尾", makePosition(number, cursor + 1));
        }

        switch (text[cursor])
        {
            case '[':
                return parseFlowSequence(text, cursor, number);
            case '{':
                return parseFlowMapping(text, cursor, number);
            case '"':
            {
                const std::size_t closing = findClosingDoubleQuote(text, cursor);
                if (closing == std::string_view::npos)
                {
                    throw ParserError("流式集合内双引号字符串未闭合", makePosition(number, cursor + 1));
                }
                ParserValue value(ParserText::decodeQuotedBody(text.substr(cursor + 1, closing - cursor - 1), makePosition(number, cursor + 1)));
                cursor = closing + 1;
                return value;
            }
            case '\'':
            {
                const std::size_t closing = findClosingSingleQuote(text, cursor);
                if (closing == std::string_view::npos)
                {
                    throw ParserError("流式集合内单引号字符串未闭合", makePosition(number, cursor + 1));
                }
                ParserValue value(expandSingleQuoteEscapes(text.substr(cursor + 1, closing - cursor - 1)));
                cursor = closing + 1;
                return value;
            }
            default:
                break;
        }

        const std::size_t valueStart = cursor;
        while (cursor < text.size() && text[cursor] != ',' && text[cursor] != ']' && text[cursor] != '}')
        {
            ++cursor;
        }

        return inferScalar(trim(text.substr(valueStart, cursor - valueStart)));
    }

    ParserValue YamlParser::parseFlowSequence(const std::string_view text, std::size_t &cursor, const std::size_t number)
    {
        ++cursor; // 跳过 '['
        ParserValueArray elements;

        while (true)
        {
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }

            if (cursor >= text.size())
            {
                throw ParserError("流式序列未闭合，应为 ']'", makePosition(number, cursor + 1));
            }
            if (text[cursor] == ']')
            {
                ++cursor;
                return ParserValue(std::move(elements));
            }

            elements.push_back(parseFlowValue(text, cursor, number));

            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }
            if (cursor < text.size() && text[cursor] == ',')
            {
                ++cursor;
                continue;
            }
        }
    }

    ParserValue YamlParser::parseFlowMapping(std::string_view text, std::size_t &cursor, const std::size_t number)
    {
        ++cursor; // 跳过 '{'
        ParserValueObject members;

        while (true)
        {
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }

            if (cursor >= text.size())
            {
                throw ParserError("流式映射未闭合，应为 '}'", makePosition(number, cursor + 1));
            }
            if (text[cursor] == '}')
            {
                ++cursor;
                return ParserValue(std::move(members));
            }

            // 键先扫到冒号，再作为独立文本解析（兼容引号键）
            const std::size_t keyStart = cursor;
            while (cursor < text.size() && text[cursor] != ':' && text[cursor] != ',' && text[cursor] != '}')
            {
                ++cursor;
            }
            if (cursor >= text.size() || text[cursor] != ':')
            {
                throw ParserError("流式映射条目必须写成 key: value", makePosition(number, keyStart + 1));
            }

            const ParserValue key = parseInlineValue(trim(text.substr(keyStart, cursor - keyStart)), number, keyStart + 1);
            if (!key.is<std::string>() && !key.isNull())
            {
                throw ParserError("流式映射键必须是字符串", makePosition(number, keyStart + 1));
            }

            ++cursor; // 消费 ':'
            ParserValue value = parseFlowValue(text, cursor, number);

            const std::string keyName = key.isNull() ? "null" : key.asString();
            if (members.contains(keyName))
            {
                throw ParserError("流式映射内存在重复键：" + keyName, makePosition(number, keyStart + 1));
            }
            members.emplace(keyName, std::move(value));

            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            {
                ++cursor;
            }
            if (cursor < text.size() && text[cursor] == ',')
            {
                ++cursor;
                continue;
            }
        }
    }

    ParserValue YamlParser::inferScalar(const std::string_view text)
    {
        if (text.empty() || text == "~" || equalsIgnoringCase(text, "null"))
        {
            return ParserValue(nullptr);
        }
        if (equalsIgnoringCase(text, "true") || equalsIgnoringCase(text, "yes") || equalsIgnoringCase(text, "on"))
        {
            return ParserValue(true);
        }
        if (equalsIgnoringCase(text, "false") || equalsIgnoringCase(text, "no") || equalsIgnoringCase(text, "off"))
        {
            return ParserValue(false);
        }

        const char *begin = text.data();
        const char *end   = text.data() + text.size();

        std::int64_t integer = 0;
        if (const auto [integerPointer, integerError] = std::from_chars(begin, end, integer);
            integerError == std::errc() && integerPointer == end)
        {
            return ParserValue(integer);
        }

        double floatingPoint = 0.0;
        if (const auto [floatPointer, floatError] = std::from_chars(begin, end, floatingPoint);
            floatError == std::errc() && floatPointer == end)
        {
            return ParserValue(floatingPoint);
        }

        return ParserValue(std::string(text));
    }

    bool YamlParser::startsSequenceEntry(const std::string_view content) noexcept
    {
        return content == "-" || (content.size() > 1 && content.front() == '-' && content[1] == ' ');
    }

    std::size_t YamlParser::findKeyValueSeparator(const std::string_view content) noexcept
    {
        for (std::size_t index = 0; index < content.size(); ++index)
        {
            const char character = content[index];

            if (character == '"')
            {
                const std::size_t closing = findClosingDoubleQuote(content, index);
                if (closing == std::string_view::npos)
                {
                    return std::string_view::npos;
                }
                index = closing;
                continue;
            }
            if (character == '\'')
            {
                const std::size_t closing = findClosingSingleQuote(content, index);
                if (closing == std::string_view::npos)
                {
                    return std::string_view::npos;
                }
                index = closing;
                continue;
            }
            if (character == '[' || character == '{')
            {
                // 值里的流式容器不参与键定位
                return std::string_view::npos;
            }

            if (character == ':' && (index + 1 == content.size() || content[index + 1] == ' '))
            {
                return index;
            }
        }
        return std::string_view::npos;
    }
} // namespace AsynGyanis::Base
