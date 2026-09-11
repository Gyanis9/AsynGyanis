#include "Base/Parser/Json/JsonWriter.h"

#include "Base/Parser/Value/ParserValueType.h"

#include <charconv>
#include <cstdint>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 单个缩进层级使用的空格数量
         */
        constexpr std::size_t kIndentationWidth = 2;

        /**
         * @brief 把码位写入为 \uXXXX 形式的转义
         * @param output 输出缓冲
         * @param codeUnit 待转义的 16 位码元
         */
        void appendUnicodeEscape(std::string &output, const std::uint16_t codeUnit)
        {
            static constexpr char khexadecimalDigits[] = "0123456789abcdef";

            output += "\\u";
            output.push_back(khexadecimalDigits[(codeUnit >> 12) & 0xFU]);
            output.push_back(khexadecimalDigits[(codeUnit >> 8) & 0xFU]);
            output.push_back(khexadecimalDigits[(codeUnit >> 4) & 0xFU]);
            output.push_back(khexadecimalDigits[codeUnit & 0xFU]);
        }
    } // namespace

    std::string JsonWriter::write(const ParserValue &value, const bool indented)
    {
        std::string output;
        output.reserve(64);
        appendValue(output, value, 0, indented);
        return output;
    }

    void JsonWriter::appendValue(std::string &output, const ParserValue &value, const std::size_t indentationLevel,
                                 const bool   indented)
    {
        switch (value.type())
        {
            case ParserValueType::Null:
                output += "null";
                break;
            case ParserValueType::Bool:
                output += value.asBool() ? "true" : "false";
                break;
            case ParserValueType::Int:
                output += std::to_string(value.asInt());
                break;
            case ParserValueType::Double:
            {
                const double floatingPoint = value.asDouble();
                if (!std::isfinite(floatingPoint))
                {
                    // JSON 没有 NaN 与无穷大的数字表示，退回 null
                    output += "null";
                    break;
                }

                char       buffer[64];
                const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), floatingPoint);
                if (ec != std::errc())
                {
                    output += "null";
                    break;
                }

                const std::size_t numberStart = output.size();
                output.append(buffer, ptr);
                // 整数值的浮点必须带小数点或指数，否则会被重新解析成 int64_t
                if (output.find_first_of(".eE", numberStart) == std::string::npos)
                {
                    output += ".0";
                }
                break;
            }
            case ParserValueType::String:
                appendQuotedString(output, value.asString());
                break;
            case ParserValueType::Array:
            {
                const ParserValueArray &elements = value.asArray();
                if (elements.empty())
                {
                    output += "[]";
                    break;
                }

                output += indented ? "[\n" : "[";
                for (std::size_t index = 0; index < elements.size(); ++index)
                {
                    if (index > 0)
                    {
                        output += indented ? ",\n" : ",";
                    }
                    appendIndentation(output, indentationLevel + 1, indented);
                    appendValue(output, elements[index], indentationLevel + 1, indented);
                }
                output += indented ? "\n" : "";
                appendIndentation(output, indentationLevel, indented);
                output += ']';
                break;
            }
            case ParserValueType::Object:
            {
                const ParserValueObject &members = value.asObject();
                if (members.empty())
                {
                    output += "{}";
                    break;
                }

                output             += indented ? "{\n" : "{";
                bool isFirstMember = true;
                for (const auto &[key, member]: members)
                {
                    if (!isFirstMember)
                    {
                        output += indented ? ",\n" : ",";
                    }
                    isFirstMember = false;

                    appendIndentation(output, indentationLevel + 1, indented);
                    appendQuotedString(output, key);
                    output += indented ? ": " : ":";
                    appendValue(output, member, indentationLevel + 1, indented);
                }
                output += indented ? "\n" : "";
                appendIndentation(output, indentationLevel, indented);
                output += '}';
                break;
            }
            default:
                output += "null";
                break;
        }
    }

    void JsonWriter::appendQuotedString(std::string &output, const std::string &text)
    {
        output.push_back('"');

        for (const char character: text)
        {
            switch (character)
            {
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(character) < 0x20)
                    {
                        appendUnicodeEscape(output, static_cast<std::uint16_t>(static_cast<unsigned char>(character)));
                    } else
                    {
                        output.push_back(character);
                    }
                    break;
            }
        }

        output.push_back('"');
    }

    void JsonWriter::appendIndentation(std::string &output, const std::size_t indentationLevel, const bool indented)
    {
        if (!indented)
        {
            return;
        }
        output.append(indentationLevel * kIndentationWidth, ' ');
    }
} // namespace AsynGyanis::Base
