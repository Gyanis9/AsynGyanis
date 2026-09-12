/**
 * @file YamlParser.cpp
 * @brief 严格 YAML 1.2 解析器实现：消费扫描器事件并组装配置值模型
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlParser.h"

#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/Yaml/YamlEvent.h"
#include "Base/Format/Yaml/YamlReader.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 核心 schema 各标签的完整文本：直接比较 string_view，
        /// 避免每次判断都拼接前缀（原实现每比较一次就要构造两个字符串）
        constexpr std::string_view kCoreTagString = "tag:yaml.org,2002:str";
        constexpr std::string_view kCoreTagNull   = "tag:yaml.org,2002:null";
        constexpr std::string_view kCoreTagBool   = "tag:yaml.org,2002:bool";
        constexpr std::string_view kCoreTagInt    = "tag:yaml.org,2002:int";
        constexpr std::string_view kCoreTagFloat  = "tag:yaml.org,2002:float";
        constexpr std::string_view kCoreTagBinary = "tag:yaml.org,2002:binary";
        constexpr std::string_view kCoreTagMerge  = "tag:yaml.org,2002:merge";
        constexpr std::string_view kCoreTagSequence = "tag:yaml.org,2002:seq";
        constexpr std::string_view kCoreTagMapping  = "tag:yaml.org,2002:map";

        /// 合并键标签（YAML 1.1 的类型库标签，本解析器按惯例支持）
        constexpr std::string_view kMergeTag = "tag:yaml.org,2002:merge";

        /// 合并键的裸标量写法
        constexpr std::string_view kMergeKeyText = "<<";

        /**
         * @brief 解析标准 Base64 文本（供 `!!binary` 使用）
         * @details 忽略空白字符，支持 `=` 填充；遇到非法字符报错。
         * @param text Base64 原文
         * @param position 出错位置
         * @return std::string 解码后的字节串
         * @throws FormatError 出现非法字符或长度非法
         */
        std::string decodeBase64(const std::string_view text, const TextPosition &position)
        {
            // 标准 Base64 字母表：A-Z a-z 0-9 + /，用查表避免逐个区间比较
            int          table[256];
            std::fill(std::begin(table), std::end(table), -1);
            for (int index = 0; index < 26; ++index)
            {
                table[static_cast<std::size_t>('A' + index)] = index;
                table[static_cast<std::size_t>('a' + index)] = index + 26;
            }
            for (int index = 0; index < 10; ++index)
            {
                table[static_cast<std::size_t>('0' + index)] = index + 52;
            }
            table[static_cast<std::size_t>('+')] = 62;
            table[static_cast<std::size_t>('/')] = 63;

            std::string result;
            result.reserve(text.size() / 4 * 3);

            std::uint32_t accumulator = 0;
            int           bitCount    = 0;
            std::size_t   paddingSeen = 0;

            for (const char character : text)
            {
                if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
                {
                    continue; // 规范允许 Base64 文本换行
                }
                if (character == '=')
                {
                    ++paddingSeen;
                    continue;
                }
                if (paddingSeen > 0)
                {
                    throw FormatError(FormatErrorKind::InvalidKeyword, "!!binary 的 Base64 填充符之后仍出现数据", position);
                }

                const int value = table[static_cast<unsigned char>(character)];
                if (value < 0)
                {
                    throw FormatError(FormatErrorKind::InvalidKeyword, "!!binary 含有非法 Base64 字符", position);
                }

                accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
                bitCount += 6;
                if (bitCount >= 8)
                {
                    bitCount -= 8;
                    result.push_back(static_cast<char>((accumulator >> bitCount) & 0xFFU));
                }
            }

            if (paddingSeen > 2)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "!!binary 的 Base64 填充符过多", position);
            }
            return result;
        }

        /**
         * @brief 按指定进制解析无符号整数（供 0x/0o/0b 使用）
         * @param digits 数字部分（不含前缀）
         * @param base 进制（16/8/2）
         * @param result 输出参数，解析成功时写入 Int 或 UInt
         * @return true 解析成功
         */
        bool parseUnsignedInBase(const std::string_view digits, const std::uint32_t base, FormatValue &result)
        {
            if (digits.empty())
            {
                return false;
            }

            std::uint64_t value = 0;
            for (const char character : digits)
            {
                int digit = -1;
                if (character >= '0' && character <= '9')
                {
                    digit = character - '0';
                } else if (character >= 'a' && character <= 'f')
                {
                    digit = character - 'a' + 10;
                } else if (character >= 'A' && character <= 'F')
                {
                    digit = character - 'A' + 10;
                }
                if (digit < 0 || static_cast<std::uint32_t>(digit) >= base)
                {
                    return false;
                }

                // 溢出检查：value*base+digit 超过 uint64 上限时按规范降级为字符串
                if (value > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(digit)) / base)
                {
                    return false;
                }
                value = value * base + static_cast<std::uint64_t>(digit);
            }

            // 落在 int64 正区间仍按 Int 落地，超出才用 UInt
            if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                result = FormatValue(static_cast<std::int64_t>(value));
            } else
            {
                result = FormatValue(value);
            }
            return true;
        }

        /**
         * @brief 按 YAML 1.2 核心 schema 尝试解析整数
         * @details 十进制可带 `+`/`-`；`0x`/`0o`/`0b` 前缀不带符号；前导零按十进制处理
         *          （`0755` 是十进制 755）。十进制依次尝试 int64 → uint64 → double，
         *          超出 uint64 才降级为 double。
         * @param text 标量文本
         * @param result 输出参数，成功时写入数值
         * @return true 文本确实是整数
         */
        bool tryParseCoreInteger(const std::string_view text, FormatValue &result)
        {
            if (text.empty())
            {
                return false;
            }

            // 进制前缀（§10.2.1.2 的核心 schema 整数正则）
            if (text.size() > 2 && text[0] == '0')
            {
                const char marker = text[1];
                if (marker == 'x' || marker == 'X')
                {
                    return parseUnsignedInBase(text.substr(2), 16, result);
                }
                if (marker == 'o' || marker == 'O')
                {
                    return parseUnsignedInBase(text.substr(2), 8, result);
                }
                if (marker == 'b' || marker == 'B')
                {
                    // 1.1 的二进制前缀，1.2 核心 schema 未收录；此处按扩展支持并已在文档写明
                    return parseUnsignedInBase(text.substr(2), 2, result);
                }
            }

            std::size_t index    = 0;
            bool        negative = false;
            if (text[0] == '+' || text[0] == '-')
            {
                negative = text[0] == '-';
                index    = 1;
            }
            if (index >= text.size())
            {
                return false;
            }
            for (std::size_t scan = index; scan < text.size(); ++scan)
            {
                if (text[scan] < '0' || text[scan] > '9')
                {
                    return false; // 出现非数字字符（如 `1:30`、`1.2.3`）即不是整数
                }
            }

            const char  *begin = text.data() + index;
            const char  *end   = text.data() + text.size();
            std::uint64_t magnitude = 0;
            if (const auto [pointer, error] = std::from_chars(begin, end, magnitude); error == std::errc() && pointer == end)
            {
                if (!negative)
                {
                    if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    {
                        result = FormatValue(static_cast<std::int64_t>(magnitude));
                    } else
                    {
                        result = FormatValue(magnitude);
                    }
                    return true;
                }

                // 负号：恰好等于 INT64_MAX+1 时是 INT64_MIN，其余按常规取负
                constexpr std::uint64_t kInt64MinimumMagnitude = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL;
                if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    result = FormatValue(-static_cast<std::int64_t>(magnitude));
                    return true;
                }
                if (magnitude == kInt64MinimumMagnitude)
                {
                    result = FormatValue(std::numeric_limits<std::int64_t>::min());
                    return true;
                }
            }

            // 整数语法合法但超出整数范围时降级为 double（§10.2.1.2 未规定上限）
            double floatingValue = 0.0;
            if (const auto [pointer, error] = std::from_chars(begin, end, floatingValue); error == std::errc() && pointer == end)
            {
                result = FormatValue(negative ? -floatingValue : floatingValue);
                return true;
            }
            return false;
        }

        /**
         * @brief 按 YAML 1.2 核心 schema 尝试解析浮点
         * @details 支持 `1.5`、`.5`、`1.`、`1e3`、`.inf/-.inf/+.inf`、`.nan`；
         *          纯整数文本不算浮点（由整数路径接管）。
         * @param text 标量文本
         * @param result 输出参数，成功时写入 Double
         * @return true 文本确实是浮点
         */
        bool tryParseCoreFloat(const std::string_view text, FormatValue &result)
        {
            if (text.empty())
            {
                return false;
            }

            // 特殊的无穷与 NaN：核心 schema 要求带前导点
            const std::string_view unsignedText = (text[0] == '+' || text[0] == '-') ? text.substr(1) : text;
            const bool             isNegative   = text[0] == '-';
            if (unsignedText == ".inf" || unsignedText == ".Inf" || unsignedText == ".INF")
            {
                result = FormatValue(isNegative ? -std::numeric_limits<double>::infinity()
                                                : std::numeric_limits<double>::infinity());
                return true;
            }
            if (unsignedText == ".nan" || unsignedText == ".NaN" || unsignedText == ".NAN")
            {
                result = FormatValue(std::numeric_limits<double>::quiet_NaN());
                return true;
            }

            // 手工按核心 schema 正则校验，避免 from_chars 接受超出正则的形式
            std::size_t index = 0;
            if (text[index] == '+' || text[index] == '-')
            {
                ++index;
            }
            bool hasDigitBefore = false;
            bool hasDigitAfter  = false;
            bool hasDot         = false;
            bool hasExponent    = false;

            while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            {
                hasDigitBefore = true;
                ++index;
            }
            if (index < text.size() && text[index] == '.')
            {
                hasDot = true;
                ++index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9')
                {
                    hasDigitAfter = true;
                    ++index;
                }
            }
            if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
            {
                hasExponent = true;
                ++index;
                if (index < text.size() && (text[index] == '+' || text[index] == '-'))
                {
                    ++index;
                }
                bool hasExponentDigit = false;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9')
                {
                    hasExponentDigit = true;
                    ++index;
                }
                if (!hasExponentDigit)
                {
                    return false;
                }
            }

            // 必须整体匹配，且含小数点或指数，并且至少有一位数字
            if (index != text.size() || (!hasDot && !hasExponent) || (!hasDigitBefore && !hasDigitAfter))
            {
                return false;
            }

            // 归一化：`.5` 补前导 0；`1.` 补小数位 0，使 from_chars 在各标准库实现上行为一致
            std::string       normalized(text);
            const std::size_t signLength = (normalized[0] == '+' || normalized[0] == '-') ? 1 : 0;
            if (normalized.size() > signLength && normalized[signLength] == '.')
            {
                normalized.insert(signLength, "0");
            }
            const std::size_t dotPosition = normalized.find('.');
            if (dotPosition != std::string::npos)
            {
                const bool nextIsDigit = dotPosition + 1 < normalized.size() &&
                                         normalized[dotPosition + 1] >= '0' && normalized[dotPosition + 1] <= '9';
                if (!nextIsDigit)
                {
                    normalized.insert(dotPosition + 1, "0");
                }
            }

            double floatingValue = 0.0;
            if (const auto [pointer, error] = std::from_chars(normalized.data(), normalized.data() + normalized.size(),
                                                              floatingValue, std::chars_format::general);
                error == std::errc() && pointer == normalized.data() + normalized.size())
            {
                result = FormatValue(floatingValue);
                return true;
            }
            return false;
        }

        /**
         * @brief 按 YAML 1.2 核心 schema 推断裸标量类型
         * @details 顺序即优先级：null → bool → 整数 → 浮点 → 字符串。
         *          `yes/no/on/off/y/n` 在 1.2 下是字符串（1.1 才是 bool），`1:30` 同样是字符串。
         * @param text 裸标量文本
         * @return FormatValue 推断得到的值
         */
        FormatValue inferCoreSchema(const std::string_view text)
        {
            if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL")
            {
                return FormatValue(nullptr);
            }
            if (text == "true" || text == "True" || text == "TRUE")
            {
                return FormatValue(true);
            }
            if (text == "false" || text == "False" || text == "FALSE")
            {
                return FormatValue(false);
            }

            FormatValue parsed;
            if (tryParseCoreInteger(text, parsed))
            {
                return parsed;
            }
            if (tryParseCoreFloat(text, parsed))
            {
                return parsed;
            }
            return FormatValue(std::string(text));
        }

        /**
         * @brief 应用显式标签解析标量
         * @details 支持核心 schema 的全部标量标签；本地标签与未知全局标签按字符串落地
         *          （规范允许应用自定义标签，此处选择「保留文本、不猜类型」）。
         * @param tag 展开后的标签
         * @param text 标量文本
         * @param position 出错位置
         * @return FormatValue 解析结果
         * @throws FormatError 标签与文本不匹配
         */
        FormatValue resolveTaggedScalar(const std::string_view tag, const std::string_view text, const TextPosition &position)
        {
            if (tag == kCoreTagString)
            {
                return FormatValue(std::string(text));
            }
            if (tag == kCoreTagNull)
            {
                // 显式 !!null 一律是 null，文本只是书写形式
                return FormatValue(nullptr);
            }
            if (tag == kCoreTagBool)
            {
                if (text == "true" || text == "True" || text == "TRUE")
                {
                    return FormatValue(true);
                }
                if (text == "false" || text == "False" || text == "FALSE")
                {
                    return FormatValue(false);
                }
                throw FormatError(FormatErrorKind::InvalidKeyword,
                                  "!!bool 只接受 true/false（不支持 yes/no/on/off）",
                                  position);
            }
            if (tag == kCoreTagInt)
            {
                FormatValue parsed;
                if (!tryParseCoreInteger(text, parsed))
                {
                    throw FormatError(FormatErrorKind::InvalidNumber, "!!int 文本不是合法整数：" + std::string(text), position);
                }
                return parsed;
            }
            if (tag == kCoreTagFloat)
            {
                FormatValue parsed;
                if (tryParseCoreFloat(text, parsed))
                {
                    return parsed;
                }
                // 核心 schema 的浮点正则同样接受纯整数写法（如 `!!float 1` 即 1.0）
                if (tryParseCoreInteger(text, parsed))
                {
                    if (parsed.type() == FormatValueType::Int)
                    {
                        return FormatValue(static_cast<double>(parsed.asInt()));
                    }
                    if (parsed.type() == FormatValueType::UInt)
                    {
                        return FormatValue(static_cast<double>(parsed.asUInt()));
                    }
                    if (parsed.type() == FormatValueType::Double)
                    {
                        return parsed;
                    }
                }
                throw FormatError(FormatErrorKind::InvalidNumber, "!!float 文本不是合法浮点：" + std::string(text), position);
            }
            if (tag == kCoreTagBinary)
            {
                // FormatValue 没有二进制类型，按惯例落地为「已解码字节的字符串」
                return FormatValue(decodeBase64(text, position));
            }
            if (tag == kCoreTagMerge)
            {
                return FormatValue(std::string(text));
            }
            if (tag == kCoreTagSequence || tag == kCoreTagMapping)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword,
                                  "标量不能标注集合标签：" + std::string(tag),
                                  position);
            }

            // 本地标签（!foo）与未知全局标签：保留文本，不猜类型
            return FormatValue(std::string(text));
        }

        /**
         * @brief 把标量键转成规范文本
         * @details null → "null"，bool → "true"/"false"，整数 → 十进制文本，
         *          浮点 → 最短往返文本，字符串 → 原文。
         * @param key 已解析的键值（可写：字符串键的文本直接移出节点，键节点随后即被丢弃，
         *            因此这里省掉一次字符串拷贝，长键也就省掉一次堆分配）
         * @return std::string 规范键文本；键是集合时返回空串（由调用方报错）
         */
        std::string canonicalKeyText(FormatValue &key)
        {
            switch (key.type())
            {
                case FormatValueType::String:
                    return std::move(key.as<std::string>());
                case FormatValueType::Null:
                    return "null";
                case FormatValueType::Bool:
                    return key.asBool() ? "true" : "false";
                case FormatValueType::Int:
                {
                    char buffer[32];
                    const auto [pointer, error] = std::to_chars(std::begin(buffer), std::end(buffer), key.asInt());
                    static_cast<void>(error);
                    return std::string(buffer, pointer);
                }
                case FormatValueType::UInt:
                {
                    char buffer[32];
                    const auto [pointer, error] = std::to_chars(std::begin(buffer), std::end(buffer), key.asUInt());
                    static_cast<void>(error);
                    return std::string(buffer, pointer);
                }
                case FormatValueType::Double:
                {
                    char buffer[64];
                    const auto [pointer, error] = std::to_chars(std::begin(buffer), std::end(buffer), key.asDouble());
                    static_cast<void>(error);
                    return std::string(buffer, pointer);
                }
                default:
                    return {};
            }
        }

        /**
         * @brief 统计值的节点数（用于别名展开预算）
         * @param value 待统计数据
         * @return std::size_t 节点总数（标量计 1，容器计自身加子节点）
         */
        std::size_t countNodes(const FormatValue &value)
        {
            switch (value.type())
            {
                case FormatValueType::Array:
                {
                    std::size_t total = 1;
                    for (const FormatValue &element : value.asArray())
                    {
                        total += countNodes(element);
                    }
                    return total;
                }
                case FormatValueType::Object:
                {
                    std::size_t total = 1;
                    for (const auto &[key, member] : value.asObject())
                    {
                        static_cast<void>(key);
                        total += countNodes(member);
                    }
                    return total;
                }
                default:
                    return 1;
            }
        }
    } // namespace

    namespace
    {
        /**
         * @brief YAML DOM 组装器：消费事件流并构建 FormatValue
         *
         * @details 用显式栈代替递归，逐条消费 YamlReader 事件。映射键值按「先键后值」交替
         *          落地，集合结束后统一应用合并键。锚点在节点完成时登记，别名解析为深拷贝。
         */
        class YamlComposer
        {
        public:
            /**
             * @brief 绑定解析选项
             * @param options 解析选项
             */
            explicit YamlComposer(const YamlParseOptions &options) :
                m_options(options)
            {
            }

            /**
             * @brief 消费读取器的全部事件并组装各文档根值
             * @param reader 已完成扫描的读取器
             * @return std::vector<FormatValue> 各文档根值，按出现顺序
             * @throws FormatError 键类型不支持、别名未定义、合并键非法或展开超限
             */
            std::vector<FormatValue> compose(YamlReader &reader)
            {
                std::vector<FormatValue> documents;
                while (reader.hasNext())
                {
                    std::optional<YamlEvent> event = reader.nextEvent();
                    if (!event.has_value())
                    {
                        break;
                    }
                    handleEvent(*event, documents);
                }
                return documents;
            }

        private:
            /**
             * @brief 一个尚未结束的容器节点
             */
            struct Frame
            {
                bool                           isMapping{false};         ///< true 为映射，false 为序列
                FormatValueObject              members;                  ///< 映射的显式成员
                std::vector<FormatValueObject> merges;                   ///< 映射收集到的合并源（按出现顺序）
                FormatValueArray               elements;                 ///< 序列的元素
                std::optional<std::string>     anchor;                   ///< 该容器登记的锚点名
                bool                           hasPendingKey{false};     ///< 是否已读入键而等待值
                bool                           pendingKeyIsMerge{false}; ///< 待值键是否为合并键 `<<`
                std::string                    pendingKey;               ///< 待值键的规范文本
                TextPosition                 pendingKeyPosition;       ///< 待值键的位置（重复键报错用）
            };

            /**
             * @brief 处理单条事件
             * @param event 事件
             * @param documents 输出参数，文档完成时追加根值
             */
            void handleEvent(const YamlEvent &event, std::vector<FormatValue> &documents)
            {
                switch (event.type)
                {
                    case YamlEventType::StreamStart:
                        m_anchors.clear();
                        m_pendingAnchor.reset();
                        break;
                    case YamlEventType::DocumentStart:
                        // 锚点作用域限于单份文档（YAML 1.2 §3.2.2.2）
                        m_frames.clear();
                        m_anchors.clear();
                        m_pendingAnchor.reset();
                        m_root.reset();
                        m_aliasExpansion = 0;
                        break;
                    case YamlEventType::DocumentEnd:
                        finishDocument(documents);
                        break;
                    case YamlEventType::MappingStart:
                        beginContainer(event, true);
                        break;
                    case YamlEventType::SequenceStart:
                        beginContainer(event, false);
                        break;
                    case YamlEventType::MappingEnd:
                        endContainer(event, true);
                        break;
                    case YamlEventType::SequenceEnd:
                        endContainer(event, false);
                        break;
                    case YamlEventType::Scalar:
                    {
                        const std::optional<std::string> anchor = consumeAnchor(event);
                        FormatValue value = resolveScalar(event);
                        attach(std::move(value), &event, event.position, anchor);
                        break;
                    }
                    case YamlEventType::Alias:
                    {
                        FormatValue value = resolveAlias(event);
                        attach(std::move(value), nullptr, event.position, std::nullopt);
                        break;
                    }
                    case YamlEventType::Anchor:
                        m_pendingAnchor = event.anchor;
                        break;
                    case YamlEventType::StreamEnd:
                        break;
                }
            }

            /**
             * @brief 收束当前文档
             * @param documents 输出参数，根值追加到此处
             */
            void finishDocument(std::vector<FormatValue> &documents)
            {
                if (m_root.has_value())
                {
                    documents.push_back(std::move(*m_root));
                } else
                {
                    // 兜底：读取器保证每份文档至少有一个根节点事件
                    documents.emplace_back(nullptr);
                }
                m_root.reset();
                m_frames.clear();
            }

            /**
             * @brief 开始一个容器节点
             * @param event 容器开始事件
             * @param isMapping 是否为映射
             */
            void beginContainer(const YamlEvent &event, const bool isMapping)
            {
                Frame frame;
                frame.isMapping = isMapping;
                frame.anchor    = consumeAnchor(event);
                m_frames.push_back(std::move(frame));
            }

            /**
             * @brief 结束一个容器节点并挂载
             * @param event 容器结束事件
             * @param isMapping 是否为映射
             * @throws FormatError 栈不匹配或合并键非法
             */
            void endContainer(const YamlEvent &event, const bool isMapping)
            {
                if (m_frames.empty() || m_frames.back().isMapping != isMapping)
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte, "容器结束事件与开始事件不匹配", event.position);
                }

                Frame frame = std::move(m_frames.back());
                m_frames.pop_back();

                FormatValue value = frame.isMapping ? buildMapping(frame, event.position)
                                                    : FormatValue(std::move(frame.elements));
                attach(std::move(value), nullptr, event.position, frame.anchor);
            }

            /**
             * @brief 由映射栈帧组装对象（先合并、后覆盖）
             * @param frame 映射栈帧
             * @param position 结束位置
             * @return FormatValue 组装好的对象
             * @throws FormatError 存在只有键没有值的悬挂条目
             */
            FormatValue buildMapping(Frame &frame, const TextPosition &position)
            {
                if (frame.hasPendingKey)
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte, "映射在缺少值的情况下结束", position);
                }

                FormatValueObject result;
                // 合并源按出现顺序先入，emplace 保留先到者，因此靠前的合并源优先
                for (const auto &source : frame.merges)
                {
                    for (const auto &[key, member] : source)
                    {
                        result.emplace(key, member);
                    }
                }
                // 显式键最后写入，无论书写顺序都覆盖合并结果
                for (auto &[key, member] : frame.members)
                {
                    result.insert_or_assign(key, std::move(member));
                }
                return FormatValue(std::move(result));
            }

            /**
             * @brief 取出并清空待生效的锚点名
             * @param event 当前节点事件
             * @return std::optional<std::string> 锚点名（Anchor 事件优先，其次节点自带）
             */
            std::optional<std::string> consumeAnchor(const YamlEvent &event)
            {
                if (m_pendingAnchor.has_value())
                {
                    std::optional<std::string> anchor = std::move(m_pendingAnchor);
                    m_pendingAnchor.reset();
                    return anchor;
                }
                if (!event.anchor.empty())
                {
                    return event.anchor;
                }
                return std::nullopt;
            }

            /**
             * @brief 把完成的节点挂载到父容器或作为文档根
             * @param value 已完成的节点值
             * @param origin 产生该值的标量事件（容器与别名为 nullptr）
             * @param position 节点位置
             * @param anchor 该节点登记的锚点名
             * @throws FormatError 集合键、重复键或合并键值非法
             */
            void attach(FormatValue value, const YamlEvent *origin, const TextPosition &position,
                        const std::optional<std::string> &anchor)
            {
                // 先登记锚点再移动值：FormatValue 是值语义，登记的是当时的深拷贝
                if (anchor.has_value())
                {
                    m_anchors[*anchor] = value;
                }

                if (m_frames.empty())
                {
                    m_root = std::move(value);
                    return;
                }

                Frame &frame = m_frames.back();
                if (!frame.isMapping)
                {
                    frame.elements.push_back(std::move(value));
                    return;
                }

                if (!frame.hasPendingKey)
                {
                    // 键：先判断是否是合并键，其余标量键转规范文本
                    frame.pendingKeyIsMerge = isMergeKey(value, origin);
                    if (!frame.pendingKeyIsMerge)
                    {
                        if (value.type() == FormatValueType::Array || value.type() == FormatValueType::Object)
                        {
                            throw FormatError(FormatErrorKind::UnexpectedByte,
                                              "映射键暂不支持集合类型（FormatValueObject 的键必须是字符串）",
                                              position);
                        }
                        frame.pendingKey = canonicalKeyText(value);
                    }
                    frame.pendingKeyPosition = position;
                    frame.hasPendingKey       = true;
                    return;
                }

                // 值：合并键收集来源，普通键写入成员并做重复检测
                if (frame.pendingKeyIsMerge)
                {
                    collectMergeSources(value, position, frame.merges);
                } else
                {
                    // 一次 try_emplace 同时完成插入与判重：命中已有键时它不会移动任何实参，
                    // 因此报错文案可以取已存节点的键（与新键等价、文本逐字相同）；
                    // 相比 contains + insert_or_assign 少一次红黑树查找，键也按右值直接移入节点
                    const auto [memberIterator, inserted] =
                            frame.members.try_emplace(std::move(frame.pendingKey), std::move(value));
                    if (!inserted)
                    {
                        if (!m_options.allowDuplicateKeys)
                        {
                            throw FormatError(FormatErrorKind::DuplicateKey,
                                              "重复的键：" + memberIterator->first,
                                              frame.pendingKeyPosition);
                        }
                        // 允许重复键时后到者覆盖先到者，与 insert_or_assign 的语义一致
                        memberIterator->second = std::move(value);
                    }
                }

                frame.hasPendingKey       = false;
                frame.pendingKeyIsMerge   = false;
                frame.pendingKey.clear();
            }

            /**
             * @brief 判断键是否为合并键 `<<`
             * @details 判定条件：键是字符串 `<<`，且标签为 `!!merge`，或既无标签又是 plain 风格
             *          （带引号的 `"<<"` 是普通键）。
             * @param key 已解析的键值
             * @param origin 键的标量事件，非标量为 nullptr
             * @return true 是合并键
             */
            [[nodiscard]] static bool isMergeKey(const FormatValue &key, const YamlEvent *origin) noexcept
            {
                if (origin == nullptr || origin->type != YamlEventType::Scalar)
                {
                    return false;
                }
                if (key.type() != FormatValueType::String || std::string_view(key.asString()) != kMergeKeyText)
                {
                    return false;
                }
                if (origin->tag == kMergeTag)
                {
                    return true;
                }
                return origin->tag.empty() && origin->style == YamlScalarStyle::Plain;
            }

            /**
             * @brief 收集合并键的值作为合并源
             * @param value 合并键的值
             * @param position 位置
             * @param merges 输出参数，合并源按顺序追加
             * @throws FormatError 值不是映射或映射序列
             */
            void collectMergeSources(const FormatValue &value, const TextPosition &position,
                                     std::vector<FormatValueObject> &merges)
            {
                if (value.type() == FormatValueType::Object)
                {
                    consumeExpansionBudget(countNodes(value), position);
                    merges.push_back(value.asObject());
                    return;
                }
                if (value.type() == FormatValueType::Array)
                {
                    for (const FormatValue &element : value.asArray())
                    {
                        if (element.type() != FormatValueType::Object)
                        {
                            throw FormatError(FormatErrorKind::InvalidKeyword,
                                              "合并键 '<<' 的序列元素必须是映射",
                                              position);
                        }
                        consumeExpansionBudget(countNodes(element), position);
                        merges.push_back(element.asObject());
                    }
                    return;
                }

                throw FormatError(FormatErrorKind::InvalidKeyword,
                                  "合并键 '<<' 的值必须是映射或映射序列",
                                  position);
            }

            /**
             * @brief 解析别名引用
             * @param event 别名事件
             * @return FormatValue 被引用子树的值拷贝
             * @throws FormatError 别名未定义或展开总量超限
             */
            FormatValue resolveAlias(const YamlEvent &event)
            {
                const auto iterator = m_anchors.find(event.alias);
                if (iterator == m_anchors.end())
                {
                    throw FormatError(FormatErrorKind::InvalidKeyword, "未定义的别名：" + event.alias, event.position);
                }
                // 别名炸弹防线：按被引用子树的节点数累计展开成本
                consumeExpansionBudget(countNodes(iterator->second), event.position);
                return iterator->second;
            }

            /**
             * @brief 累加别名/合并展开成本并在超限时报错
             * @param cost 本次展开的节点数
             * @param position 位置
             * @throws FormatError 累计成本超出 maximumAliasCount
             */
            void consumeExpansionBudget(const std::size_t cost, const TextPosition &position)
            {
                if (m_options.maximumAliasCount == 0)
                {
                    return;
                }
                m_aliasExpansion += cost;
                if (m_aliasExpansion > m_options.maximumAliasCount)
                {
                    throw FormatError(FormatErrorKind::SizeExceeded,
                                      std::format("别名展开总量超出上限 {}", m_options.maximumAliasCount),
                                      position);
                }
            }

            /**
             * @brief 按标签与风格解析标量
             * @details 显式标签优先；否则引号与块标量一律字符串（核心 schema 的默认标签为 !!str）；
             *          只有 plain 标量参与核心 schema 类型推断。
             * @param event 标量事件
             * @return FormatValue 解析结果
             * @throws FormatError 标签与文本不匹配
             */
            [[nodiscard]] static FormatValue resolveScalar(const YamlEvent &event)
            {
                // `!` 是非特定标签，按规范对集合之外的标量解析为 !!str
                if (event.tag == "!")
                {
                    return FormatValue(event.text);
                }
                if (!event.tag.empty())
                {
                    return resolveTaggedScalar(event.tag, event.text, event.position);
                }
                if (event.style != YamlScalarStyle::Plain)
                {
                    return FormatValue(event.text);
                }
                return inferCoreSchema(event.text);
            }

            const YamlParseOptions            &m_options;              ///< 解析选项
            std::vector<Frame>                 m_frames;               ///< 未结束容器的栈
            std::optional<FormatValue>          m_root;                 ///< 当前文档根值
            std::map<std::string, FormatValue, std::less<> > m_anchors; ///< 当前文档的锚点表
            std::optional<std::string>         m_pendingAnchor;        ///< 待绑定到下一个节点的锚点
            std::size_t                        m_aliasExpansion{0};    ///< 别名/合并累计展开节点数
        };
    } // namespace

    FormatValue YamlParser::parse(const std::string_view text)
    {
        return parse(text, YamlParseOptions{});
    }

    FormatValue YamlParser::parse(const std::string_view text, const YamlParseOptions &options)
    {
        YamlReader reader(text, options);
        YamlComposer composer(options);
        std::vector<FormatValue> documents = composer.compose(reader);

        // 单文档语义：取首份文档；流中零文档（空输入/仅注释）时按规范是 null
        if (documents.empty())
        {
            return FormatValue(nullptr);
        }
        return std::move(documents.front());
    }

    std::vector<FormatValue> YamlParser::parseAll(const std::string_view text)
    {
        return parseAll(text, YamlParseOptions{});
    }

    std::vector<FormatValue> YamlParser::parseAll(const std::string_view text, const YamlParseOptions &options)
    {
        YamlReader reader(text, options);
        YamlComposer composer(options);
        return composer.compose(reader);
    }
} // namespace AsynGyanis::Base
