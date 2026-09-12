#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Value/FormatValueType.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 把 16 位码元写入为 \uXXXX 形式的转义
         * @param output 输出缓冲
         * @param codeUnit 待转义的 16 位码元
         */
        void appendUnicodeEscape(std::string &output, const std::uint16_t codeUnit)
        {
            static constexpr char kHexadecimalDigits[] = "0123456789abcdef";

            char escape[6] = {'\\', 'u', '\0', '\0', '\0', '\0'};
            escape[2]      = kHexadecimalDigits[(codeUnit >> 12) & 0xFU];
            escape[3]      = kHexadecimalDigits[(codeUnit >> 8) & 0xFU];
            escape[4]      = kHexadecimalDigits[(codeUnit >> 4) & 0xFU];
            escape[5]      = kHexadecimalDigits[codeUnit & 0xFU];
            output.append(escape, sizeof(escape));
        }

        /**
         * @brief 把码位写入为 \uXXXX（补充平面用代理对）
         * @param output 输出缓冲
         * @param codePoint 待写入的 Unicode 码位
         */
        void appendCodePointEscape(std::string &output, const std::uint32_t codePoint)
        {
            if (codePoint <= 0xFFFFU)
            {
                appendUnicodeEscape(output, static_cast<std::uint16_t>(codePoint));
                return;
            }

            // RFC 8259 §7：补充平面（U+10000..U+10FFFF）在 \u 转义中必须写成代理对
            const std::uint32_t offset = codePoint - 0x10000U;
            appendUnicodeEscape(output, static_cast<std::uint16_t>(0xD800U + (offset >> 10)));
            appendUnicodeEscape(output, static_cast<std::uint16_t>(0xDC00U + (offset & 0x3FFU)));
        }

        /**
         * @brief 取单字符的短转义形式
         * @param character 待判定字符
         * @return const char* 转义文本；无需转义时返回 nullptr
         */
        const char *shortEscapeFor(const char character) noexcept
        {
            switch (character)
            {
                case '"':
                    return "\\\"";
                case '\\':
                    return "\\\\";
                case '\b':
                    return "\\b";
                case '\f':
                    return "\\f";
                case '\n':
                    return "\\n";
                case '\r':
                    return "\\r";
                case '\t':
                    return "\\t";
                default:
                    return nullptr;
            }
        }

        /**
         * @brief 解码一个 UTF-8 序列
         * @details 依 RFC 3629 做完整校验：拒绝过长编码、越界码位、UTF-8 形式的代理项
         *          与截断序列。解析器产出的字符串必然合法，但手工构造的值未必，
         *          因此转义路径仍需自证合法。
         * @param text 源文本
         * @param index 序列起始偏移
         * @param codePoint 输出参数，解码得到的码位
         * @return std::size_t 序列占用的字节数
         * @throws FormatError 序列非法或截断（kind 为 InvalidUtf8）
         */
        std::size_t decodeUtf8Sequence(const std::string &text, const std::size_t index, std::uint32_t &codePoint)
        {
            const auto leadByte = static_cast<unsigned char>(text[index]);

            std::size_t   sequenceLength    = 0;
            std::uint32_t initialCodePoint  = 0;
            unsigned char secondByteMinimum = 0x80U;
            unsigned char secondByteMaximum = 0xBFU;

            if (leadByte >= 0xC2U && leadByte <= 0xDFU)
            {
                sequenceLength   = 2;
                initialCodePoint = leadByte & 0x1FU;
            } else if (leadByte == 0xE0U)
            {
                sequenceLength    = 3;
                initialCodePoint  = leadByte & 0x0FU;
                secondByteMinimum = 0xA0U;
            } else if (leadByte >= 0xE1U && leadByte <= 0xECU)
            {
                sequenceLength   = 3;
                initialCodePoint = leadByte & 0x0FU;
            } else if (leadByte == 0xEDU)
            {
                sequenceLength    = 3;
                initialCodePoint  = leadByte & 0x0FU;
                secondByteMaximum = 0x9FU;
            } else if (leadByte >= 0xEEU && leadByte <= 0xEFU)
            {
                sequenceLength   = 3;
                initialCodePoint = leadByte & 0x0FU;
            } else if (leadByte == 0xF0U)
            {
                sequenceLength    = 4;
                initialCodePoint  = leadByte & 0x07U;
                secondByteMinimum = 0x90U;
            } else if (leadByte >= 0xF1U && leadByte <= 0xF3U)
            {
                sequenceLength   = 4;
                initialCodePoint = leadByte & 0x07U;
            } else if (leadByte == 0xF4U)
            {
                sequenceLength    = 4;
                initialCodePoint  = leadByte & 0x07U;
                secondByteMaximum = 0x8FU;
            } else
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", TextPosition{});
            }

            if (index + sequenceLength > text.size())
            {
                throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中的 UTF-8 序列不完整", TextPosition{});
            }

            std::uint32_t codePointValue = initialCodePoint;
            for (std::size_t offset = 1; offset < sequenceLength; ++offset)
            {
                const auto          byte    = static_cast<unsigned char>(text[index + offset]);
                const unsigned char minimum = offset == 1 ? secondByteMinimum : 0x80U;
                const unsigned char maximum = offset == 1 ? secondByteMaximum : 0xBFU;
                if (byte < minimum || byte > maximum)
                {
                    throw FormatError(FormatErrorKind::InvalidUtf8, "字符串中出现非法的 UTF-8 编码字节", TextPosition{});
                }
                codePointValue = (codePointValue << 6) | (byte & 0x3FU);
            }

            codePoint = codePointValue;
            return sequenceLength;
        }

        /**
         * @brief 以最短文本追加整数
         * @tparam IntegerType 有符号或无符号整型
         * @param output 输出缓冲
         * @param value 待写入的整数值
         */
        template<typename IntegerType>
        void appendInteger(std::string &output, const IntegerType value)
        {
            static_assert(std::is_integral_v<IntegerType>, "appendInteger 只接受整数类型");

            // int64/uint64 的十进制最长 20 位，24 字节缓冲留出充足余量
            char       buffer[24];
            const auto [pointer, errorCode] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (errorCode != std::errc())
            {
                // 缓冲区足够，此分支只是兜底：宁可写成 null 也不写出半截数字
                output += "null";
                return;
            }
            output.append(buffer, pointer);
        }
    } // namespace

    std::string JsonWriter::write(const FormatValue &value, const JsonWriteOptions &options)
    {
        std::string output;
        // 按节点数与字符串总长粗估规模，避免大文档序列化时反复扩容
        output.reserve(estimateOutputSize(value));
        appendValue(output, value, 0, options);
        return output;
    }

    std::string JsonWriter::write(const FormatValue &value, const bool indented)
    {
        // 紧凑模式等价于 indentWidth = 0，其余默认值与历史输出逐字节一致
        JsonWriteOptions options;
        if (!indented)
        {
            options.indentWidth = 0;
        }
        return write(value, options);
    }

    void JsonWriter::appendValue(std::string &           output, const FormatValue &value, const std::size_t indentationLevel,
                                 const JsonWriteOptions &options)
    {
        const FormatValueType valueType = value.type();

        // 深度守护只作用于容器：与解析侧「容器所在层数」含义一致，因此默认选项下解析成功过
        // 的值必然能用默认选项写回；非有限深度的手搓值则在此被拦下，避免递归爆栈
        if ((valueType == FormatValueType::Array || valueType == FormatValueType::Object) &&
            options.maximumDepth != 0 && indentationLevel >= options.maximumDepth)
        {
            // 序列化没有输入文本可定位，位置取默认值（第 1 行第 1 列）
            throw FormatError(FormatErrorKind::DepthExceeded, "序列化嵌套深度超出上限", TextPosition{});
        }

        switch (valueType)
        {
            case FormatValueType::Null:
                output += "null";
                break;
            case FormatValueType::Bool:
                output += value.asBool() ? "true" : "false";
                break;
            case FormatValueType::Int:
                appendInteger(output, value.asInt());
                break;
            case FormatValueType::UInt:
                appendInteger(output, value.asUInt());
                break;
            case FormatValueType::Double:
            {
                const double floatingPoint = value.asDouble();
                if (!std::isfinite(floatingPoint))
                {
                    // JSON 没有 NaN 与无穷大的数字表示，退回 null
                    output += "null";
                    break;
                }

                char       buffer[64];
                const auto [pointer, errorCode] = std::to_chars(buffer, buffer + sizeof(buffer), floatingPoint);
                if (errorCode != std::errc())
                {
                    output += "null";
                    break;
                }

                const std::size_t numberStart = output.size();
                output.append(buffer, pointer);
                // 整数值的浮点必须带小数点或指数，否则会被重新解析成整数类型
                if (output.find_first_of(".eE", numberStart) == std::string::npos)
                {
                    output += ".0";
                }
                break;
            }
            case FormatValueType::String:
                appendQuotedString(output, value.asString(), options);
                break;
            case FormatValueType::Array:
            {
                const FormatValueArray &elements = value.asArray();
                if (elements.empty())
                {
                    output += "[]";
                    break;
                }

                const bool isIndented = options.indentWidth > 0;
                output                += isIndented ? "[\n" : "[";
                for (std::size_t index = 0; index < elements.size(); ++index)
                {
                    if (index > 0)
                    {
                        output += isIndented ? ",\n" : ",";
                    }
                    appendIndentation(output, indentationLevel + 1, options);
                    appendValue(output, elements[index], indentationLevel + 1, options);
                }
                if (isIndented)
                {
                    output += '\n';
                }
                appendIndentation(output, indentationLevel, options);
                output += ']';
                break;
            }
            case FormatValueType::Object:
            {
                const FormatValueObject &members = value.asObject();
                if (members.empty())
                {
                    output += "{}";
                    break;
                }

                const bool isIndented = options.indentWidth > 0;
                output                += isIndented ? "{\n" : "{";
                bool isFirstMember    = true;
                // FormatValueObject 是按键升序的 std::map：Sorted 与 AsIs 在当前值模型下
                // 输出完全一致（AsIs 表示「按容器当前迭代顺序输出」，天然就是键升序）；
                // 二者保留为独立枚举，便于将来换成保序容器后 AsIs 立即具有区分度
                for (const auto &[key, member]: members)
                {
                    if (!isFirstMember)
                    {
                        output += isIndented ? ",\n" : ",";
                    }
                    isFirstMember = false;

                    appendIndentation(output, indentationLevel + 1, options);
                    appendQuotedString(output, key, options);
                    output += isIndented ? ": " : ":";
                    appendValue(output, member, indentationLevel + 1, options);
                }
                if (isIndented)
                {
                    output += '\n';
                }
                appendIndentation(output, indentationLevel, options);
                output += '}';
                break;
            }
            default:
                output += "null";
                break;
        }
    }

    void JsonWriter::appendQuotedString(std::string &output, const std::string &text, const JsonWriteOptions &options)
    {
        output.push_back('"');

        const std::size_t textSize = text.size();
        std::size_t       runStart = 0; // 待成段拷贝的普通字符区间起点
        std::size_t       index    = 0;

        while (index < textSize)
        {
            const auto byte = static_cast<unsigned char>(text[index]);

            if (byte < 0x80U)
            {
                // ASCII 分支：只有短转义与控制字符需要打断成段拷贝
                if (const char *escape = shortEscapeFor(static_cast<char>(byte)); escape != nullptr)
                {
                    output.append(text, runStart, index - runStart);
                    output += escape;
                    ++index;
                    runStart = index;
                    continue;
                }
                if (byte < 0x20U)
                {
                    // RFC 8259 §7：U+0000..U+001F 必须转义，统一写成 \u00XX
                    output.append(text, runStart, index - runStart);
                    appendUnicodeEscape(output, byte);
                    ++index;
                    runStart = index;
                    continue;
                }
                ++index;
                continue;
            }

            if (!options.ensureAscii)
            {
                // 原样透传 UTF-8 字节：不打断成段拷贝，整段一次性写出
                ++index;
                continue;
            }

            // ensureAscii：整个码位转成 \uXXXX，因此必须解码序列长度
            output.append(text, runStart, index - runStart);
            std::uint32_t     codePoint      = 0;
            const std::size_t sequenceLength = decodeUtf8Sequence(text, index, codePoint);
            appendCodePointEscape(output, codePoint);
            index    += sequenceLength;
            runStart = index;
        }

        // 收尾：把最后一段普通字符一次性写出
        output.append(text, runStart, textSize - runStart);
        output.push_back('"');
    }

    void JsonWriter::appendIndentation(std::string &output, const std::size_t indentationLevel, const JsonWriteOptions &options)
    {
        if (options.indentWidth == 0)
        {
            return;
        }
        output.append(indentationLevel * options.indentWidth, ' ');
    }

    std::size_t JsonWriter::estimateOutputSize(const FormatValue &value)
    {
        // 粗估规则（宁大勿小）：标量按最坏文本长度，字符串按原文长度加两个引号，
        // 数组按每个元素 1 字节分隔符，对象按每个成员 5 字节（引号、冒号、逗号）
        std::size_t                      total = 0;
        std::vector<const FormatValue *> pending;
        pending.push_back(&value);

        while (!pending.empty())
        {
            const FormatValue *current = pending.back();
            pending.pop_back();

            switch (current->type())
            {
                case FormatValueType::Null:
                    total += 4;
                    break;
                case FormatValueType::Bool:
                    total += 5;
                    break;
                case FormatValueType::Int:
                case FormatValueType::UInt:
                    total += 24;
                    break;
                case FormatValueType::Double:
                    total += 32;
                    break;
                case FormatValueType::String:
                    total += current->asString().size() + 2;
                    break;
                case FormatValueType::Array:
                {
                    const FormatValueArray &elements = current->asArray();
                    total                            += 2 + elements.size();
                    for (const FormatValue &element: elements)
                    {
                        pending.push_back(&element);
                    }
                    break;
                }
                case FormatValueType::Object:
                {
                    const FormatValueObject &members = current->asObject();
                    total                            += 2;
                    for (const auto &[key, member]: members)
                    {
                        total += key.size() + 5;
                        pending.push_back(&member);
                    }
                    break;
                }
                default:
                    total += 4;
                    break;
            }
        }

        return total;
    }
} // namespace AsynGyanis::Base
