#include "Base/Format/Json/JsonPointer.h"

#include "Base/Format/FormatError.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 把数组下标 token 解析为下标
         * @details 严格按 RFC 6901 §4 校验数组定位 token：
         *          - 空 token 与 `-` 都不是可定位的下标（`-` 代表「末尾之后」的不存在元素）；
         *          - 除 `0` 自身外不允许前导零（`01`、`00` 均非法）；
         *          - 必须是纯十进制数字且不超出 size_t 可表示范围。
         * @param token 引用 token（已反转义）
         * @return std::optional<std::size_t> 合法下标；非法或 `-` 返回空
         */
        std::optional<std::size_t> parseArrayIndexToken(const std::string_view token) noexcept
        {
            if (token.empty() || token == "-")
            {
                return std::nullopt;
            }

            // 前导零：RFC 6901 §4 明确 "leading zeros are not allowed"，只有单个 "0" 合法
            if (token.size() > 1 && token.front() == '0')
            {
                return std::nullopt;
            }

            std::size_t value = 0;
            const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), value);
            // from_chars 对无符号目标天然拒绝 '-' 与 '+'，此处只需确认全量消费且未越界
            if (errorCode != std::errc() || pointer != token.data() + token.size())
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    JsonPointer::JsonPointer(std::vector<std::string> tokens) noexcept :
        m_tokens(std::move(tokens))
    {
    }

    JsonPointer JsonPointer::parse(const std::string_view text)
    {
        std::vector<std::string> tokens;

        // 空串是合法指针：RFC 6901 §5 示例 "" 指向整个文档
        if (text.empty())
        {
            return JsonPointer(std::move(tokens));
        }

        if (text.front() != '/')
        {
            // RFC 6901 §3：ABNF 要求要么整体为空，要么以 '/' 开头；这属于指针语法非法
            throw FormatError(FormatErrorKind::InvalidPointer,
                              "JSON Pointer 必须以 '/' 开头或为空串",
                              TextPosition{.lineNumber = 1, .columnNumber = 1, .offset = 0});
        }

        std::size_t index = 1;
        while (true)
        {
            const std::size_t slash = text.find('/', index);
            const std::string_view token = slash == std::string_view::npos
                                               ? text.substr(index)
                                               : text.substr(index, slash - index);

            // token 起始偏移 = index（首个 token 从 1 开始），用于报错的列号定位
            tokens.push_back(unescapeToken(token, index));

            if (slash == std::string_view::npos)
            {
                break;
            }
            index = slash + 1;
        }

        return JsonPointer(std::move(tokens));
    }

    std::string JsonPointer::escapeToken(const std::string_view token)
    {
        std::string escaped;
        escaped.reserve(token.size());

        for (const char character: token)
        {
            // RFC 6901 §3 的编码方向：'~' → "~0"、'/' → "~1"；单趟替换天然满足「先 ~ 后 /」
            if (character == '~')
            {
                escaped += "~0";
            } else if (character == '/')
            {
                escaped += "~1";
            } else
            {
                escaped.push_back(character);
            }
        }

        return escaped;
    }

    std::string JsonPointer::unescapeToken(const std::string_view token)
    {
        return unescapeToken(token, 0);
    }

    std::string JsonPointer::unescapeToken(const std::string_view token, const std::size_t tokenOffsetInPointer)
    {
        std::string unescaped;
        unescaped.reserve(token.size());

        for (std::size_t index = 0; index < token.size();)
        {
            if (token[index] != '~')
            {
                unescaped.push_back(token[index]);
                ++index;
                continue;
            }

            // RFC 6901 §3：escaped = "~" ( "0" / "1" )，孤立的 '~' 属于非法指针
            if (index + 1 >= token.size())
            {
                throw FormatError(FormatErrorKind::InvalidPointer,
                                  "JSON Pointer 中的 '~' 之后必须是 '0' 或 '1'",
                                  TextPosition{.lineNumber = 1,
                                                 .columnNumber = tokenOffsetInPointer + index + 1,
                                                 .offset = tokenOffsetInPointer + index});
            }

            const char escapedCharacter = token[index + 1];
            if (escapedCharacter == '0')
            {
                unescaped.push_back('~');
            } else if (escapedCharacter == '1')
            {
                unescaped.push_back('/');
            } else
            {
                throw FormatError(FormatErrorKind::InvalidPointer,
                                  std::format("JSON Pointer 中出现非法的转义序列：~{}", escapedCharacter),
                                  TextPosition{.lineNumber = 1,
                                                 .columnNumber = tokenOffsetInPointer + index + 1,
                                                 .offset = tokenOffsetInPointer + index});
            }

            // 单趟读取 "~x" 两字符，等价于官方要求的「先还原 ~1 再还原 ~0」，
            // 因此 "~01" 会被正确解码为 "~1" 而不会被二次还原成 "/"
            index += 2;
        }

        return unescaped;
    }

    const std::vector<std::string> &JsonPointer::tokens() const noexcept
    {
        return m_tokens;
    }

    bool JsonPointer::empty() const noexcept
    {
        return m_tokens.empty();
    }

    std::size_t JsonPointer::size() const noexcept
    {
        return m_tokens.size();
    }

    std::string JsonPointer::toString() const
    {
        std::string text;
        for (const std::string &token: m_tokens)
        {
            text.push_back('/');
            text += escapeToken(token);
        }
        return text;
    }

    bool JsonPointer::isProperPrefixOf(const JsonPointer &other) const noexcept
    {
        // 真前缀：token 个数更少，且公共前缀逐段相同
        return m_tokens.size() < other.m_tokens.size() &&
               std::equal(m_tokens.begin(), m_tokens.end(), other.m_tokens.begin());
    }

    const FormatValue *JsonPointer::evaluate(const FormatValue &document) const noexcept
    {
        const FormatValue *current = &document;

        for (const std::string &token: m_tokens)
        {
            if (current->isObject())
            {
                current = current->find(token);
            } else if (current->isArray())
            {
                const std::optional<std::size_t> index = parseArrayIndexToken(token);
                // 非法下标（前导零、非数字）与 '-' 都不定位任何元素，按未命中处理（RFC 6901 §4）
                if (!index.has_value())
                {
                    return nullptr;
                }
                current = current->find(*index);
            } else
            {
                // 非容器上取成员：RFC 6901 §4 定义为求值失败，此处按未命中返回空
                return nullptr;
            }

            if (current == nullptr)
            {
                return nullptr;
            }
        }

        return current;
    }

    std::optional<std::reference_wrapper<const FormatValue> > JsonPointer::tryEvaluate(const FormatValue &document) const noexcept
    {
        const FormatValue *resolved = evaluate(document);
        if (resolved == nullptr)
        {
            return std::nullopt;
        }
        return std::cref(*resolved);
    }

    const FormatValue &JsonPointer::resolve(const FormatValue &document) const
    {
        const FormatValue *resolved = evaluate(document);
        if (resolved == nullptr)
        {
            // 指针语法本身已在 parse() 阶段校验通过，此处只可能是「语法合法但定位不到值」，
            // 因此不区分「父级缺失」「类型不符」「数组下标非法」，统一上报 PatchTargetMissing
            throw FormatError(FormatErrorKind::PatchTargetMissing,
                              "JSON Pointer 未指向任何值：" + toString(),
                              TextPosition{});
        }
        return *resolved;
    }

    FormatValue *JsonPointer::evaluateForWrite(FormatValue &document) const noexcept
    {
        FormatValue *current = &document;

        for (const std::string &token: m_tokens)
        {
            if (current->isObject())
            {
                current = current->find(token);
            } else if (current->isArray())
            {
                const std::optional<std::size_t> index = parseArrayIndexToken(token);
                if (!index.has_value())
                {
                    return nullptr;
                }
                current = current->find(*index);
            } else
            {
                return nullptr;
            }

            if (current == nullptr)
            {
                return nullptr;
            }
        }

        return current;
    }
} // namespace AsynGyanis::Base
