#include "Base/Format/Yaml/YamlWriter.h"

#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/TextPosition.h"
#include "Base/Format/Value/FormatValueType.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 行分隔符：YAML 1.2 §5.4 允许 LF 或 CRLF，本输出器统一写 LF
        constexpr char kLineBreak = '\n';

        /// 块标量的显式缩进指示符候选字符（§8.1.1.1 只允许 1~9，不允许 0）
        constexpr char kIndentIndicatorDigits[] = "123456789";

        /// FNV-1a 哈希偏移基准，用于锚点复用前的重复子树检测
        constexpr std::size_t kHashOffsetBasis = static_cast<std::size_t>(0x811C9DC5U);

        /// FNV-1a 哈希乘子
        constexpr std::size_t kHashPrime = static_cast<std::size_t>(16777619U);

        /**
         * @brief 判断字符是否为空格或制表符
         * @param character 待判定字符
         * @return true 是空格或制表符
         */
        [[nodiscard]] constexpr bool isSpaceOrTab(const char character) noexcept
        {
            return character == ' ' || character == '\t';
        }

        /**
         * @brief 判断字符是否为控制字符
         * @details 覆盖 C0 控制符（U+0000..U+001F）与 DEL（U+007F）：它们既不能出现在裸标量里，
         *          写进引号标量时也必须转义（§5.1 的可打印字符要求）。
         * @param character 待判定字符
         * @return true 是控制字符
         */
        [[nodiscard]] constexpr bool isControlCharacter(const char character) noexcept
        {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20U || byte == 0x7FU;
        }

        /**
         * @brief 判断字符是否为 YAML 指示符
         * @details 完整的 `c-indicator` 集合：这些字符出现在标量**开头**时会被扫描器分派成结构语法
         *          （§6.2 的指示符表、§7.3.3 的裸标量首字符限制）。
         * @param character 待判定字符
         * @return true 是指示符
         */
        [[nodiscard]] constexpr bool isYamlIndicator(const char character) noexcept
        {
            switch (character)
            {
                case '-':
                case '?':
                case ':':
                case ',':
                case '[':
                case ']':
                case '{':
                case '}':
                case '#':
                case '&':
                case '*':
                case '!':
                case '|':
                case '>':
                case '\'':
                case '"':
                case '%':
                case '@':
                case '`':
                    return true;
                default:
                    return false;
            }
        }

        /**
         * @brief 判断文本是否非空且全为十进制数字
         * @param text 待判定文本
         * @return true 全部由 0~9 组成
         */
        [[nodiscard]] bool isAllDigits(const std::string_view text) noexcept
        {
            return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
        }

        /**
         * @brief 判断文本是否非空且字符都落在给定字母表内
         * @param text 待判定文本
         * @param alphabet 允许出现的字符集合
         * @return true 全部命中字母表
         */
        [[nodiscard]] bool isAllCharactersFrom(const std::string_view text, const std::string_view alphabet) noexcept
        {
            return !text.empty() && text.find_first_not_of(alphabet) == std::string_view::npos;
        }

        /**
         * @brief 判断两段文本是否在忽略 ASCII 大小写后相等
         * @details 只折叠 ASCII 字母，与核心 schema 的 `null|Null|NULL` 之类写法保持一致。
         * @param left 左文本
         * @param right 右文本
         * @return true 忽略大小写后相等
         */
        [[nodiscard]] bool equalsAsciiIgnoreCase(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                char leftCharacter  = left[index];
                char rightCharacter = right[index];
                if (leftCharacter >= 'A' && leftCharacter <= 'Z')
                {
                    leftCharacter = static_cast<char>(leftCharacter - 'A' + 'a');
                }
                if (rightCharacter >= 'A' && rightCharacter <= 'Z')
                {
                    rightCharacter = static_cast<char>(rightCharacter - 'A' + 'a');
                }
                if (leftCharacter != rightCharacter)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 判断文本是否形如整数
         * @details 与解析器的核心 schema 整数判定（§10.2.1.2）逐条对齐：十进制可带 `+`/`-`，
         *          `0x`/`0o`/`0b` 前缀不带符号，前导零按十进制处理。
         * @param text 待判定文本，必须非空
         * @return true 会被解析成整数
         */
        [[nodiscard]] bool isIntegerLookingText(const std::string_view text) noexcept
        {
            if (text.size() > 2 && text[0] == '0')
            {
                const std::string_view digits = text.substr(2);
                switch (text[1])
                {
                    case 'x':
                    case 'X':
                        return isAllCharactersFrom(digits, "0123456789abcdefABCDEF");
                    case 'o':
                    case 'O':
                        return isAllCharactersFrom(digits, "01234567");
                    case 'b':
                    case 'B':
                        return isAllCharactersFrom(digits, "01");
                    default:
                        break;
                }
            }

            const std::size_t start = (text[0] == '+' || text[0] == '-') ? 1 : 0;
            return isAllDigits(text.substr(start));
        }

        /**
         * @brief 判断文本是否形如浮点
         * @details 与解析器的核心 schema 浮点判定（§10.2.1.3）逐条对齐：必须整体匹配，
         *          且含小数点或指数；纯整数文本由整数路径接管，不算浮点。
         * @param text 待判定文本，必须非空
         * @return true 会被解析成浮点
         */
        [[nodiscard]] bool isFloatLookingText(const std::string_view text) noexcept
        {
            std::size_t index = (text[0] == '+' || text[0] == '-') ? 1 : 0;

            bool hasDigitBeforeDot = false;
            bool hasDigitAfterDot  = false;
            bool hasDot            = false;
            bool hasExponent       = false;

            while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            {
                hasDigitBeforeDot = true;
                ++index;
            }
            if (index < text.size() && text[index] == '.')
            {
                hasDot = true;
                ++index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9')
                {
                    hasDigitAfterDot = true;
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
                    return false; // 指数缺少数字，不是合法浮点
                }
            }

            return index == text.size() && (hasDot || hasExponent) && (hasDigitBeforeDot || hasDigitAfterDot);
        }

        /**
         * @brief 判断字符串不加引号时是否会被解析成非字符串
         * @details 命中即必须加引号，否则类型当场改变。判定范围刻意比 YAML 1.2 核心 schema
         *          （§10.2.1）更宽：`yes/no/on/off/y/n` 在 1.2 下是字符串，但大量下游仍按 1.1 读，
         *          显式引号更安全，故一并纳入。
         * @param text 待判定文本
         * @return true 需要引号才能保持字符串语义
         */
        [[nodiscard]] bool looksLikeNonStringScalar(const std::string_view text) noexcept
        {
            if (text.empty() || text == "~")
            {
                return true; // 裸写的空标量是 null，不是空字符串
            }
            if (equalsAsciiIgnoreCase(text, "null") || equalsAsciiIgnoreCase(text, "true") ||
                equalsAsciiIgnoreCase(text, "false"))
            {
                return true;
            }
            if (equalsAsciiIgnoreCase(text, "yes") || equalsAsciiIgnoreCase(text, "no") ||
                equalsAsciiIgnoreCase(text, "on") || equalsAsciiIgnoreCase(text, "off") ||
                equalsAsciiIgnoreCase(text, "y") || equalsAsciiIgnoreCase(text, "n"))
            {
                return true;
            }

            // 特殊浮点：核心 schema 要求带前导点，允许正负号
            const std::string_view magnitude = (text[0] == '+' || text[0] == '-') ? text.substr(1) : text;
            if (equalsAsciiIgnoreCase(magnitude, ".inf") || equalsAsciiIgnoreCase(magnitude, ".nan"))
            {
                return true;
            }

            return isIntegerLookingText(text) || isFloatLookingText(text);
        }

        /**
         * @brief 判断文本是否形如文档边界标记（`---` 或 `...`）
         * @details 与扫描器的判定逐条对齐（§9.1.3）：标记之后要么就是行尾，要么隔一个分隔空白。
         *          `---` 的首字符已由指示符表拦下，`...` 的首字符 `.` 却是普通字符，
         *          必须显式排除，否则整行会被当成文档结束标记而不是标量内容。
         *
         *          适用范围仅限**裸标量**（isPlainScalarSafe）：裸标量写在所在行的当前列上，
         *          根位置即列 0，与「只在列 0 生效」的标记正面相撞。块标量则不需要这一判定——
         *          其内容一律缩进 contentStep ≥ 1 列（§8.1.1.1），永远够不到列 0。
         * @param text 待判定文本
         * @return true 形如文档起始或结束标记
         */
        [[nodiscard]] bool isDocumentMarkerLooking(const std::string_view text) noexcept
        {
            if (text.size() < 3)
            {
                return false;
            }
            const std::string_view marker = text.substr(0, 3);
            if (marker != "---" && marker != "...")
            {
                return false;
            }
            return text.size() == 3 || isSpaceOrTab(text[3]);
        }

        /**
         * @brief 判断文本能否安全地写成裸标量
         * @details 逐条对齐扫描器的裸标量规则（§7.3.3 的 `c-ns-plain-safe`）：首字符不得是指示符
         *          或空白（首尾空白会被裁掉）；不得含控制字符；` #`、`: ` 分别是注释引导与键值分隔
         *          （§6.2、§7.4.2）；含引号则排除（扫描器定位键值分隔时会先做引号配对）；flow 上下文
         *          额外排除 `,` `[` `]` `{` `}` `:` `#`（§7.4）。
         * @param text 待判定文本
         * @param flowContext 是否处于 flow 容器内部
         * @return true 可以裸写
         */
        [[nodiscard]] bool isPlainScalarSafe(const std::string_view text, const bool flowContext) noexcept
        {
            if (text.empty())
            {
                return false;
            }
            if (isYamlIndicator(text.front()) || isSpaceOrTab(text.front()) || isSpaceOrTab(text.back()))
            {
                return false;
            }
            if (isDocumentMarkerLooking(text))
            {
                return false; // 形如 `...` / `... x` 的整行会被当成文档边界（§9.1.3），不是标量
            }

            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const char character = text[index];
                if (isControlCharacter(character))
                {
                    return false;
                }
                if (character == '#' && index > 0 && isSpaceOrTab(text[index - 1]))
                {
                    return false;
                }
                if (character == ':' && (index + 1 == text.size() || isSpaceOrTab(text[index + 1])))
                {
                    return false;
                }
                if (character == '\'' || character == '"')
                {
                    return false;
                }
                if (flowContext && (character == ',' || character == '[' || character == ']' || character == '{' ||
                                    character == '}' || character == ':' || character == '#'))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 判断文本是否含换行
         * @param text 待判定文本
         * @return true 含至少一个换行
         */
        [[nodiscard]] bool hasLineBreak(const std::string_view text) noexcept
        {
            return text.find(kLineBreak) != std::string_view::npos;
        }

        /**
         * @brief 统计文本尾部的换行个数
         * @details 该数量决定块标量的 chomping 指示（§8.1.1.2）：0 → `-`，1 → 默认 clip，>1 → `+`。
         * @param text 待统计文本
         * @return std::size_t 尾部连续换行数
         */
        [[nodiscard]] std::size_t trailingLineBreakCount(const std::string_view text) noexcept
        {
            std::size_t count = 0;
            std::size_t index = text.size();
            while (index > 0 && text[index - 1] == kLineBreak)
            {
                --index;
                ++count;
            }
            return count;
        }

        /**
         * @brief 判断文本能否用块标量无损表示
         * @details 三类情形会让块标量失真，必须退回引号标量：以换行开场（chomping 只能按「有内容的
         *          末行」计数，§8.1.1.2）；含制表符等控制字符（行首 tab 是非法缩进，§6.1）；存在仅由
         *          空格/制表符构成的行（会被当作空行丢掉）。内容由 appendBlockScalarText 统一缩进
         *          contentStep ≥ 1 列（§8.1.1.1），够不到只在**列 0** 成立的文档标记与指令（§9.1.3、§6.8）。
         * @param text 待判定文本，必须含换行
         * @return true 可以用 `|` 或 `>` 表示
         */
        [[nodiscard]] bool canUseBlockScalar(const std::string_view text) noexcept
        {
            if (text.empty() || text.front() == kLineBreak)
            {
                return false;
            }

            for (const char character: text)
            {
                if (character != kLineBreak && isControlCharacter(character))
                {
                    return false;
                }
            }

            std::size_t lineStart = 0;
            while (lineStart <= text.size())
            {
                const std::size_t      lineEnd = text.find(kLineBreak, lineStart);
                const std::size_t      stop    = lineEnd == std::string_view::npos ? text.size() : lineEnd;
                const std::string_view line    = text.substr(lineStart, stop - lineStart);
                if (!line.empty() && line.find_first_not_of(" \t") == std::string_view::npos)
                {
                    return false; // 仅空白行会被扫描器按空行丢掉
                }
                if (lineEnd == std::string_view::npos)
                {
                    break;
                }
                lineStart = lineEnd + 1;
            }
            return true;
        }

        /**
         * @brief 判断文本改用折叠块标量 `>` 后是否仍然无损
         * @details §8.1.3 规定折叠风格把「非空行之间的单个换行」折成空格，这一步不可逆。
         *          只有当换行全部位于尾部（由 chomping 承担）时才没有可折叠的位置，`>` 与 `|`
         *          才完全等价；其余情形一律退回字面风格，绝不为好看牺牲 round-trip。
         * @param text 待判定文本
         * @return true 可以无损使用 `>`
         */
        [[nodiscard]] bool foldedStyleIsLossless(const std::string_view text) noexcept
        {
            const std::size_t firstLineBreak = text.find(kLineBreak);
            if (firstLineBreak == std::string_view::npos)
            {
                return false; // 单行文本本就不该走块标量
            }
            return text.find_first_not_of(kLineBreak, firstLineBreak) == std::string_view::npos;
        }

        /**
         * @brief 判断文本里是否有会被扫描器当作注释引导的 `#`
         * @details 扫描器在进入引号标量之前会先对整行做一次「去掉行尾注释」：
         *          任何前置空白（或位于行首）的 `#` 都会把其后的内容整段砍掉（§6.2），
         *          引号并不能保护它。因此这类字符必须改写为 `\x23` 转义。
         * @param text 待判定文本
         * @return true 存在被前置空白引导的 `#`
         */
        [[nodiscard]] bool hasCommentLikeHash(const std::string_view text) noexcept
        {
            for (std::size_t index = 1; index < text.size(); ++index)
            {
                if (text[index] == '#' && isSpaceOrTab(text[index - 1]))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 判断文本能否安全地写成单引号标量
         * @details 单引号只需把 `'` 写成 `''`（§7.3.1），是引号家族里最省的一种；
         *          但控制字符（含制表符）必须走双引号的 `\t` / `\xXX` 转义（§7.3.2），
         *          会被当作注释引导的 `#` 同样只能靠双引号转义（单引号没有转义能力）。
         * @param text 待判定文本
         * @return true 可以单引号包裹
         */
        [[nodiscard]] bool isSingleQuotable(const std::string_view text) noexcept
        {
            for (const char character: text)
            {
                if (isControlCharacter(character))
                {
                    return false;
                }
            }
            return !hasCommentLikeHash(text);
        }

        /**
         * @brief 取单字符的短转义写法
         * @param character 待判定字符
         * @return const char* 转义文本；无短写法时返回 nullptr（交由 `\xXX` 兜底）
         */
        [[nodiscard]] const char *shortEscapeFor(const char character) noexcept
        {
            switch (character)
            {
                case '\0':
                    return "\\0";
                case '\x07':
                    return "\\a";
                case '\b':
                    return "\\b";
                case '\t':
                    return "\\t";
                case '\n':
                    return "\\n";
                case '\v':
                    return "\\v";
                case '\f':
                    return "\\f";
                case '\r':
                    return "\\r";
                case '\x1B':
                    return "\\e";
                case '"':
                    return "\\\"";
                case '\\':
                    return "\\\\";
                default:
                    return nullptr;
            }
        }

        /**
         * @brief 判断裸标量折行后的续行是否安全
         * @details 续行会被扫描器重新按行首语义审视：`-`/`?` 后跟空白会被当作序列条目或复杂键
         *          （§8.2.1、§8.2.2），`---`/`...`/`%` 开头会被当作文档边界或指令
         *          （§6.2、§9.1.3），这些位置一律不允许折行。
         * @param text 标量全文
         * @param position 续行首字符在全文中的下标
         * @return true 可以在该位置折行
         */
        [[nodiscard]] bool isFoldSafeContinuation(const std::string_view text, const std::size_t position) noexcept
        {
            if (position >= text.size())
            {
                return false;
            }

            const char lead = text[position];
            if (lead == '%')
            {
                return false;
            }
            if ((lead == '-' || lead == '?') && (position + 1 >= text.size() || isSpaceOrTab(text[position + 1])))
            {
                return false;
            }

            const std::string_view tail = text.substr(position);
            if (tail.size() >= 3 && (tail.substr(0, 3) == "---" || tail.substr(0, 3) == "..."))
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 判断值是否为容器（数组或对象）
         * @param value 待判定值
         * @return true 是数组或对象
         */
        [[nodiscard]] bool isContainerValue(const FormatValue &value) noexcept
        {
            const FormatValueType type = value.type();
            return type == FormatValueType::Array || type == FormatValueType::Object;
        }

        /**
         * @brief 取容器直接子节点数量
         * @param value 待统计值
         * @return std::size_t 子节点数；非容器为 0
         */
        [[nodiscard]] std::size_t childCountOf(const FormatValue &value) noexcept
        {
            switch (value.type())
            {
                case FormatValueType::Array:
                    return value.asArray().size();
                case FormatValueType::Object:
                    return value.asObject().size();
                default:
                    return 0;
            }
        }

        /**
         * @brief 把两个哈希值混合成一个
         * @param seed 累积哈希
         * @param value 待混入的哈希值
         * @return std::size_t 混合后的哈希
         */
        [[nodiscard]] std::size_t mixHash(const std::size_t seed, const std::size_t value) noexcept
        {
            return (seed ^ value) * kHashPrime;
        }

        /**
         * @brief 计算子树的递归结构哈希
         * @details 只用于把「可能相等」的候选归到同一个桶，最终是否复用仍由
         *          FormatValue::operator== 精确裁决，因此哈希碰撞不影响正确性。
         * @param value 待哈希值
         * @return std::size_t 结构哈希
         */
        [[nodiscard]] std::size_t hashOfValue(const FormatValue &value) noexcept
        {
            const FormatValueType type = value.type();
            std::size_t           seed = mixHash(kHashOffsetBasis, static_cast<std::size_t>(type));

            switch (type)
            {
                case FormatValueType::Bool:
                    return mixHash(seed, value.asBool() ? 1U : 0U);
                case FormatValueType::Int:
                    return mixHash(seed, static_cast<std::size_t>(value.asInt()));
                case FormatValueType::UInt:
                    return mixHash(seed, static_cast<std::size_t>(value.asUInt()));
                case FormatValueType::Double:
                    return mixHash(seed, std::hash<double>{}(value.asDouble()));
                case FormatValueType::String:
                    return mixHash(seed, std::hash<std::string_view>{}(*value.getStringView()));
                case FormatValueType::Array:
                {
                    const FormatValueArray &elements = value.asArray();
                    seed                             = mixHash(seed, elements.size());
                    for (const FormatValue &element: elements)
                    {
                        seed = mixHash(seed, hashOfValue(element));
                    }
                    return seed;
                }
                case FormatValueType::Object:
                {
                    const FormatValueObject &members = value.asObject();
                    seed                             = mixHash(seed, members.size());
                    for (const auto &[key, member]: members)
                    {
                        seed = mixHash(seed, std::hash<std::string_view>{}(key));
                        seed = mixHash(seed, hashOfValue(member));
                    }
                    return seed;
                }
                default:
                    return seed;
            }
        }

        /**
         * @brief 需要输出的锚点条目
         * @details 一条条目代表「一种结构」而不是「一个对象」：FormatValue 是值语义树，结构相同的
         *          重复子树通常各自持有独立的副本，若按对象地址去重就永远等不到第二次命中，
         *          结果变成给每份副本各发一个新锚点且从不写别名。因此等值判定一律按值进行：
         *          首次发射的那份写 `&aN`，其后与它等值的副本写 `*aN`（§3.2.2）。
         */
        struct AnchorEntry
        {
            const FormatValue *representative{nullptr}; ///< 该结构的首份出现，仅用于等值比较（生命周期由调用方保证）
            std::size_t        hash{0};                 ///< 该结构的递归哈希，用于等值比较前的快速剪枝
            std::size_t        index{0};                ///< 锚点编号（1 基，落地为 `a1`、`a2`……）
            bool               defined{false};          ///< 是否已写出 `&aN` 定义（未定义前不得使用别名）
        };

        /// 锚点表未命中时的槽位下标
        constexpr std::size_t kNoAnchorSlot = static_cast<std::size_t>(-1);

        /**
         * @brief YAML 发射器：持有输出缓冲与锚点表，按块/flow 两种上下文递归落地
         *
         * @details 输出永远以换行结尾：块容器的每一行、块标量的每一行都自带换行，
         *          单行标量则由调用方 endLine() 收尾，因此列号统计（m_lineStart）始终有效。
         */
        class YamlEmitter
        {
        public:
            /**
             * @brief 绑定序列化选项
             * @param options 序列化选项，生命周期必须覆盖整个发射过程
             */
            explicit YamlEmitter(const YamlWriteOptions &options) :
                m_options(options)
            {
            }

            /**
             * @brief 发射整份文档
             * @param root 文档根值
             * @return std::string YAML 文本
             * @throws FormatError 嵌套超限（kind 为 DepthExceeded）
             */
            std::string emit(const FormatValue &root)
            {
                m_output.reserve(estimateOutputSize(root) + 4);

                // §9.1.3：`---` 是文档起始标记；单文档输出可以省略，多文档拼接时必需
                if (m_options.emitDocumentStart)
                {
                    appendRaw("---");
                    appendLineBreak();
                }

                if (m_options.reuseAnchors)
                {
                    collectReusableAnchors(root);
                }

                appendRootValue(root);
                endLine();
                return std::move(m_output);
            }

        private:
            /**
             * @brief 追加文档根值
             * @param value 根值
             * @throws FormatError 嵌套超限
             */
            void appendRootValue(const FormatValue &value)
            {
                if (!isContainerValue(value))
                {
                    appendScalarValue(value, 0, false);
                    return;
                }

                // 深度按容器层数计：根容器是第 1 层，与解析侧 DepthGuard 的口径一致
                checkDepth(1);

                if (isEmptyContainer(value))
                {
                    // 空容器没有块风格写法（§8.2.2 / §8.2.3），只能退回 flow 或 null
                    if (m_options.useFlowForEmptyContainers)
                    {
                        appendEmptyContainer(value);
                    } else
                    {
                        appendRaw("null");
                    }
                    return;
                }

                if (isFlowEligible(value))
                {
                    appendInlineContainer(value, 1);
                    return;
                }

                appendBlockContainer(value, 0, 1);
            }

            /**
             * @brief 追加块风格容器（映射或序列）的全部行
             * @param value 容器值
             * @param indent 本容器各行（键或指示符）的缩进
             * @param containerDepth 本容器所在层数（根容器为 1）
             */
            void appendBlockContainer(const FormatValue &value, const std::size_t indent, const std::size_t containerDepth)
            {
                if (value.type() == FormatValueType::Object)
                {
                    appendBlockMapping(value.asObject(), indent, containerDepth);
                    return;
                }
                appendBlockSequence(value.asArray(), indent, containerDepth);
            }

            /**
             * @brief 追加块风格映射的各行
             * @param members 成员；FormatValueObject 是按键升序的映射，直接沿用其顺序，不重排
             * @param indent 键行缩进
             * @param containerDepth 本映射所在层数
             */
            void appendBlockMapping(const FormatValueObject &members, const std::size_t indent, const std::size_t containerDepth)
            {
                for (const auto &[key, member]: members)
                {
                    appendIndent(indent);
                    appendKey(key, false);
                    appendRaw(":");

                    // 空容器按「隐式 null」落地时冒号后必须留空（§8.2.2 的空值写法）
                    if (rendersAsImplicitNull(member))
                    {
                        endLine();
                        continue;
                    }

                    // 冒号后的分隔空白由 appendMappingValue 按值的落地形态决定
                    appendMappingValue(member, indent, containerDepth + 1);
                    endLine();
                }
            }

            /**
             * @brief 追加块风格序列的各行
             * @param elements 元素
             * @param indent 指示符 `-` 的缩进
             * @param containerDepth 本序列所在层数
             */
            void appendBlockSequence(const FormatValueArray &elements, const std::size_t indent, const std::size_t containerDepth)
            {
                for (const FormatValue &element: elements)
                {
                    appendIndent(indent);

                    // useFlowForEmptyContainers 关闭时，空容器只有一个孤立的 `-`
                    if (rendersAsImplicitNull(element))
                    {
                        appendRaw("-");
                        endLine();
                        continue;
                    }

                    if (!isContainerValue(element))
                    {
                        appendRaw("- ");
                        appendScalarValue(element, indent, false);
                        endLine();
                        continue;
                    }

                    checkDepth(containerDepth + 1);

                    // 重复子树：`- *a1`
                    if (isRepeatedAnchor(element))
                    {
                        appendRaw("- ");
                        appendAnchorAlias(element);
                        endLine();
                        continue;
                    }

                    if (isEmptyContainer(element))
                    {
                        appendRaw("- ");
                        appendEmptyContainer(element);
                        endLine();
                        continue;
                    }

                    if (isFlowEligible(element))
                    {
                        appendRaw("- ");
                        if (needsAnchorDefinition(element))
                        {
                            appendAnchorDefinition(element);
                            // flow 节点紧跟锚点，必须留一个分隔空白（§7.4.2 要求节点之间有分隔）
                            appendRaw(" ");
                        }
                        appendInlineContainer(element, containerDepth + 1);
                        endLine();
                        continue;
                    }

                    // 块容器条目：`-` 与节点属性同行，主体缩进到下一层。指示符后必须补分隔空白，
                    // 否则 `-&a1` 会被扫描器当成一个裸标量而不是序列条目（§8.2.1 的 s-separate-in-line）
                    appendRaw("-");
                    if (needsAnchorDefinition(element))
                    {
                        appendRaw(" ");
                        appendAnchorDefinition(element);
                    }
                    appendLineBreak();
                    appendBlockContainer(element, indent + indentStep(), containerDepth + 1);
                }
            }

            /**
             * @brief 追加映射成员的值（冒号已写出，分隔空白由本函数按落地形态决定）
             * @details 值与键同行时必须先补一个分隔空白（§8.2.2 的 `key: value`）；值换成块容器时
             *          键行以冒号收束，不留尾随空白，主体另起一层。
             * @param value 成员值
             * @param indent 键行缩进（续行与子块缩进都以此为基准）
             * @param containerDepth 本值作为容器时的层数
             * @throws FormatError 嵌套超限
             */
            void appendMappingValue(const FormatValue &value, const std::size_t indent, const std::size_t containerDepth)
            {
                if (!isContainerValue(value))
                {
                    appendRaw(" ");
                    appendScalarValue(value, indent, false);
                    return;
                }

                checkDepth(containerDepth);

                if (isRepeatedAnchor(value))
                {
                    appendRaw(" ");
                    appendAnchorAlias(value);
                    return;
                }

                if (isEmptyContainer(value))
                {
                    appendRaw(" ");
                    appendEmptyContainer(value);
                    return;
                }

                if (isFlowEligible(value))
                {
                    appendRaw(" ");
                    if (needsAnchorDefinition(value))
                    {
                        appendAnchorDefinition(value);
                        appendRaw(" ");
                    }
                    appendInlineContainer(value, containerDepth);
                    return;
                }

                // 块风格容器：键行以冒号收束（不留尾随空白），节点属性留在键行，主体另起一层（§8.2.2）
                if (needsAnchorDefinition(value))
                {
                    appendRaw(" ");
                    appendAnchorDefinition(value);
                }
                appendLineBreak();
                appendBlockContainer(value, indent + indentStep(), containerDepth);
            }

            /**
             * @brief 追加 flow 风格容器（保证所有子节点都是标量）
             * @param value 容器值
             * @param containerDepth 本容器所在层数
             */
            void appendInlineContainer(const FormatValue &value, const std::size_t containerDepth)
            {
                static_cast<void>(containerDepth);

                if (value.type() == FormatValueType::Array)
                {
                    const FormatValueArray &elements = value.asArray();
                    appendRaw("[");
                    for (std::size_t index = 0; index < elements.size(); ++index)
                    {
                        if (index > 0)
                        {
                            appendRaw(", ");
                        }
                        appendScalarValue(elements[index], 0, true);
                    }
                    appendRaw("]");
                    return;
                }

                const FormatValueObject &members = value.asObject();
                appendRaw("{");
                bool isFirstMember = true;
                for (const auto &[key, member]: members)
                {
                    if (!isFirstMember)
                    {
                        appendRaw(", ");
                    }
                    isFirstMember = false;
                    appendKey(key, true);
                    appendRaw(": ");
                    // flow 容器内不允许块节点（§7.4.2），多行文本一律走双引号转义
                    appendScalarValue(member, 0, true);
                }
                appendRaw("}");
            }

            /**
             * @brief 追加一个标量值
             * @param value 标量值
             * @param lineIndent 标量所在行的缩进（裸标量折行的续行基准）
             * @param flowContext 是否处于 flow 容器内部
             * @details 数组与对象不在本函数的职责内，位置决策（块还是 flow）由调用方完成。
             */
            void appendScalarValue(const FormatValue &value, const std::size_t lineIndent, const bool flowContext)
            {
                switch (value.type())
                {
                    case FormatValueType::Null:
                        appendRaw("null"); // §10.2.1.1：null 的规范写法
                        return;
                    case FormatValueType::Bool:
                        appendRaw(value.asBool() ? "true" : "false");
                        return;
                    case FormatValueType::Int:
                        appendInteger(value.asInt());
                        return;
                    case FormatValueType::UInt:
                        appendInteger(value.asUInt());
                        return;
                    case FormatValueType::Double:
                        appendFloatingPoint(value.asDouble());
                        return;
                    case FormatValueType::String:
                        appendStringValue(*value.getStringView(), lineIndent, flowContext);
                        return;
                    default:
                        return;
                }
            }

            /**
             * @brief 追加浮点值
             * @details 有限值走 std::to_chars 的最短往返表示；整数值的浮点补 `.0`，否则会被重新
             *          解析成整数类型。NaN 与无穷按核心 schema 写成 `.nan` / `.inf` / `-.inf`（§10.2.1.3）。
             * @param number 浮点值
             * @throws FormatError 数字无法转换（理论不可达：64 字节缓冲对 double 足够）
             */
            void appendFloatingPoint(const double number)
            {
                if (std::isnan(number))
                {
                    appendRaw(".nan");
                    return;
                }
                if (std::isinf(number))
                {
                    appendRaw(number > 0 ? ".inf" : "-.inf");
                    return;
                }

                char       buffer[64];
                const auto [pointer, errorCode] = std::to_chars(std::begin(buffer), std::end(buffer), number);
                if (errorCode != std::errc())
                {
                    throw FormatError(FormatErrorKind::InvalidNumber, "浮点数无法转换为最短文本", TextPosition{});
                }

                const std::size_t numberStart = m_output.size();
                appendRaw(std::string_view(buffer, static_cast<std::size_t>(pointer - buffer)));
                if (m_output.find_first_of(".eE", numberStart) == std::string::npos)
                {
                    appendRaw(".0");
                }
            }

            /**
             * @brief 以最短十进制文本追加整数
             * @tparam IntegerType 有符号或无符号整型
             * @param number 待追加的整数值
             * @throws FormatError 数字无法转换（理论不可达：24 字节缓冲对 64 位整数足够）
             */
            template<typename IntegerType>
            void appendInteger(const IntegerType number)
            {
                static_assert(std::is_integral_v<IntegerType>, "appendInteger 只接受整数类型");

                char       buffer[24];
                const auto [pointer, errorCode] = std::to_chars(std::begin(buffer), std::end(buffer), number);
                if (errorCode != std::errc())
                {
                    throw FormatError(FormatErrorKind::InvalidNumber, "整数无法转换为十进制文本", TextPosition{});
                }
                appendRaw(std::string_view(buffer, static_cast<std::size_t>(pointer - buffer)));
            }

            /**
             * @brief 追加字符串值
             * @details 选择顺序即「无损优先、其次最省」：含换行且可无损 → 块标量（§8.1）；
             *          裸标量安全且不歧义 → 裸标量（§7.3.3）；否则单引号（§7.3.1，只需 `''` 折叠）；
             *          含控制字符 → 双引号（§7.3.2 的完整转义表）。
             * @param text 字符串内容
             * @param lineIndent 所在行缩进（块标量内容缩进与裸标量折行都以此为基准）
             * @param flowContext 是否处于 flow 容器内部
             */
            void appendStringValue(const std::string_view text, const std::size_t lineIndent, const bool flowContext)
            {
                // flow 容器内不能出现块节点（§7.4.2），因此块标量只在块上下文启用
                if (!flowContext && m_options.multiLineStyle != ScalarStylePolicy::Quoted && hasLineBreak(text) &&
                    canUseBlockScalar(text))
                {
                    appendBlockScalarText(text, lineIndent);
                    return;
                }

                // 空串必定加引号：裸写的空标量是 null，不是空字符串（§10.2.1.1）
                const bool needQuotes = !isPlainScalarSafe(text, flowContext) ||
                                        (m_options.quoteAmbiguousStrings && looksLikeNonStringScalar(text));
                if (!needQuotes)
                {
                    appendPlainScalar(text, lineIndent, flowContext);
                    return;
                }

                if (isSingleQuotable(text))
                {
                    appendSingleQuoted(text);
                    return;
                }
                appendDoubleQuoted(text);
            }

            /**
             * @brief 追加裸标量，按 lineWidth 在安全的空格处折行
             * @details §7.3.3 规定裸标量的单个换行在折叠时还原为一个空格，因此折行点只能选
             *          「前后都不是空格的单个空格」：把该空格换成换行加缩进后，回读完全一致。
             * @param text 标量文本（调用方已确保裸写安全）
             * @param lineIndent 标量所在行缩进
             * @param flowContext 是否处于 flow 容器内部（flow 内不折行）
             */
            void appendPlainScalar(const std::string_view text, const std::size_t lineIndent, const bool flowContext)
            {
                const std::size_t width = m_options.lineWidth;
                if (flowContext || width == 0)
                {
                    appendRaw(text);
                    return;
                }

                const std::size_t continuationIndent = lineIndent + indentStep();
                std::size_t       cursor             = 0;

                while (cursor < text.size())
                {
                    const std::size_t column = currentColumn();
                    if (column + (text.size() - cursor) <= width)
                    {
                        appendRaw(text.substr(cursor));
                        return;
                    }

                    // 本行还能容纳到的最大下标（至少给续行留一个字符）
                    const std::size_t room  = width > column ? width - column : 0;
                    std::size_t       limit = cursor + room;
                    if (limit >= text.size())
                    {
                        limit = text.size() - 1;
                    }

                    std::size_t breakPoint = std::string_view::npos;
                    for (std::size_t index = limit; index > cursor; --index)
                    {
                        if (text[index] != ' ' || text[index - 1] == ' ')
                        {
                            continue;
                        }
                        if (index + 1 >= text.size() || text[index + 1] == ' ')
                        {
                            continue;
                        }
                        if (!isFoldSafeContinuation(text, index + 1))
                        {
                            continue;
                        }
                        breakPoint = index;
                        break;
                    }

                    if (breakPoint == std::string_view::npos)
                    {
                        appendRaw(text.substr(cursor)); // 本行找不到安全折点，整段写出
                        return;
                    }

                    appendRaw(text.substr(cursor, breakPoint - cursor));
                    appendLineBreak();
                    appendIndent(continuationIndent);
                    cursor = breakPoint + 1;
                }
            }

            /**
             * @brief 追加块标量（`|` 或 `>`）
             * @details 头部固定带显式缩进指示符：自动探测以「首个非空行」的缩进为准，含前导空格的
             *          行会被吃掉缩进（§8.1.1.1），只有显式写出位数才可靠。
             *          chomping 按尾随换行数 N 三态落地（§8.1.1.2）：N==0 写 `-`（strip）、N==1 默认
             *          clip（正文行尾的换行被保留）、N>=2 写 `+`（keep，补 N-1 个空行）。
             * @param text 多行文本（调用方已确保 canUseBlockScalar 成立）
             * @param blockIndent 块标量所在行（键行或 `-` 行）的缩进
             */
            void appendBlockScalarText(const std::string_view text, const std::size_t blockIndent)
            {
                const std::size_t trailingBreaks = trailingLineBreakCount(text);
                const std::size_t contentStep    = blockScalarIndentStep();
                const bool        useFolded      = m_options.multiLineStyle == ScalarStylePolicy::Folded &&
                                       foldedStyleIsLossless(text);

                appendRaw(useFolded ? ">" : "|");
                const char indentIndicator = kIndentIndicatorDigits[contentStep - 1];
                appendRaw(std::string_view(&indentIndicator, 1));

                if (trailingBreaks == 0)
                {
                    appendRaw("-"); // strip：正文行尾的换行也被丢弃
                } else if (trailingBreaks > 1)
                {
                    appendRaw("+"); // keep：尾随空行由下面的 N-1 个空行补齐
                }
                // trailingBreaks == 1 时保持默认的 clip：只保留正文末行的换行，正合 1 个
                appendLineBreak();

                const std::string_view body          = text.substr(0, text.size() - trailingBreaks);
                const std::size_t      contentIndent = blockIndent + contentStep;

                std::size_t lineStart = 0;
                while (true)
                {
                    const std::size_t      lineEnd = body.find(kLineBreak, lineStart);
                    const std::string_view line    = body.substr(lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);

                    // 空行不写缩进：写了也只是空白行，扫描器同样按空行处理，反而留下尾随空白
                    if (!line.empty())
                    {
                        appendIndent(contentIndent);
                        appendRaw(line);
                    }
                    appendLineBreak();

                    if (lineEnd == std::string_view::npos)
                    {
                        break;
                    }
                    lineStart = lineEnd + 1;
                }

                // keep：正文末行自带的换行之外，每个尾随空行再贡献一个换行（§8.1.1.2 的 keep 语义）
                for (std::size_t round = 1; round < trailingBreaks; ++round)
                {
                    appendLineBreak();
                }
            }

            /**
             * @brief 追加单引号标量
             * @details 成段拷贝普通字符，只在遇到 `'` 时切换成 `''`（§7.3.1），避免逐字符 push_back。
             * @param text 标量文本
             */
            void appendSingleQuoted(const std::string_view text)
            {
                appendRaw("'");

                std::size_t runStart = 0;
                std::size_t index    = 0;
                while (index < text.size())
                {
                    if (text[index] != '\'')
                    {
                        ++index;
                        continue;
                    }
                    appendRaw(text.substr(runStart, index - runStart));
                    appendRaw("''");
                    ++index;
                    runStart = index;
                }
                appendRaw(text.substr(runStart));

                appendRaw("'");
            }

            /**
             * @brief 追加双引号标量
             * @details 普通字符成段拷贝，只在遇到需转义字符时切换：短转义表（`\t`、`\n`、`\\`、`\"` 等）
             *          之外的 C0 控制符与 DEL 写成 `\xXX`（§7.3.2 的转义表）。
             *          另外，前置空白引导的 `#` 会被扫描器当成行尾注释砍掉整段内容（§6.2），
             *          必须写成 `\x23`，这也是引号标量唯一的兜底手段。
             * @param text 标量文本
             */
            void appendDoubleQuoted(const std::string_view text)
            {
                appendRaw("\"");

                std::size_t runStart = 0;
                for (std::size_t index = 0; index < text.size(); ++index)
                {
                    const char        character              = text[index];
                    const char *const shortEscape            = shortEscapeFor(character);
                    // 注释引导 `#` 必须先于「成段拷贝」被拦下，否则整行会被截断
                    const bool        needsCommentHashEscape = character == '#' && index > 0 && isSpaceOrTab(text[index - 1]);
                    if (shortEscape == nullptr && !isControlCharacter(character) && !needsCommentHashEscape)
                    {
                        continue;
                    }

                    appendRaw(text.substr(runStart, index - runStart));
                    if (shortEscape != nullptr)
                    {
                        appendRaw(shortEscape);
                    } else
                    {
                        appendHexadecimalEscape(character);
                    }
                    runStart = index + 1;
                }
                appendRaw(text.substr(runStart));

                appendRaw("\"");
            }

            /**
             * @brief 追加 `\xXX` 形式的转义
             * @details 只用于不可打印的 ASCII（U+0000..U+001F 与 U+007F），因此一枚 ASCII 字节
             *          对应一枚码位，回读后字节完全一致。
             * @param character 待转义字符
             */
            void appendHexadecimalEscape(const char character)
            {
                static constexpr char kHexadecimalDigits[] = "0123456789abcdef";

                const auto byte      = static_cast<unsigned char>(character);
                const char escape[4] = {'\\', 'x', kHexadecimalDigits[(byte >> 4) & 0x0FU], kHexadecimalDigits[byte & 0x0FU]};
                appendRaw(std::string_view(escape, sizeof(escape)));
            }

            /**
             * @brief 追加映射键
             * @details 键与值共用同一套引号规则；额外一条：裸 `<<` 会被解析器当作合并键
             *          （§8.2.1 的映射合并惯例），必须引号包裹才表示普通字符串键。
             * @param key 键文本
             * @param flowContext 是否处于 flow 容器内部
             */
            void appendKey(const std::string_view key, const bool flowContext)
            {
                const bool plainSafe = isPlainScalarSafe(key, flowContext) && key != "<<" &&
                                       !(m_options.quoteAmbiguousStrings && looksLikeNonStringScalar(key));
                if (plainSafe)
                {
                    appendRaw(key);
                    return;
                }
                if (isSingleQuotable(key))
                {
                    appendSingleQuoted(key);
                    return;
                }
                appendDoubleQuoted(key);
            }

            /**
             * @brief 追加空容器的 flow 写法 `[]` / `{}`
             * @param value 空容器
             */
            void appendEmptyContainer(const FormatValue &value)
            {
                appendRaw(value.type() == FormatValueType::Array ? "[]" : "{}");
            }

            /**
             * @brief 查值对应的锚点槽位
             * @details 先比递归哈希做剪枝，再用 FormatValue::operator== 精确确认，因此哈希碰撞
             *          只会带来一次多余的深比较，不会造成错误复用。
             * @param value 待查值
             * @return std::size_t 槽位下标；未登记时返回 kNoAnchorSlot
             */
            [[nodiscard]] std::size_t anchorSlotOf(const FormatValue &value) const
            {
                if (m_anchors.empty())
                {
                    return kNoAnchorSlot;
                }

                const std::size_t hash = hashOfValue(value);
                for (std::size_t slot = 0; slot < m_anchors.size(); ++slot)
                {
                    if (m_anchors[slot].hash == hash && *m_anchors[slot].representative == value)
                    {
                        return slot;
                    }
                }
                return kNoAnchorSlot;
            }

            /**
             * @brief 判断值是否为重复出现的子树（应写成别名）
             * @param value 待判定值
             * @return true 该结构已在本次发射中定义过锚点，应写成 `*aN`
             */
            [[nodiscard]] bool isRepeatedAnchor(const FormatValue &value) const
            {
                const std::size_t slot = anchorSlotOf(value);
                return slot != kNoAnchorSlot && m_anchors[slot].defined;
            }

            /**
             * @brief 判断值是否需要写出锚点定义
             * @param value 待判定值
             * @return true 该结构是重复子树的首次出现，需要写出 `&aN`
             */
            [[nodiscard]] bool needsAnchorDefinition(const FormatValue &value) const
            {
                const std::size_t slot = anchorSlotOf(value);
                return slot != kNoAnchorSlot && !m_anchors[slot].defined;
            }

            /**
             * @brief 写出锚点定义 `&aN`
             * @details 只在 needsAnchorDefinition 为真时有实际输出；分隔空白由调用方补齐。
             * @param value 待定义节点
             */
            void appendAnchorDefinition(const FormatValue &value)
            {
                const std::size_t slot = anchorSlotOf(value);
                if (slot == kNoAnchorSlot || m_anchors[slot].defined)
                {
                    return;
                }
                m_anchors[slot].defined = true;
                appendRaw("&");
                appendAnchorName(m_anchors[slot].index);
            }

            /**
             * @brief 写出别名 `*aN`
             * @details §3.2.2：别名指向此前最近的同名锚点，解析侧按深拷贝展开，而本表只登记
             *          等值的结构，因此 round-trip 得到的仍是与原值相等的树。
             * @param value 已定义过锚点的重复子树
             */
            void appendAnchorAlias(const FormatValue &value)
            {
                const std::size_t slot = anchorSlotOf(value);
                if (slot == kNoAnchorSlot)
                {
                    return;
                }
                appendRaw("*");
                appendAnchorName(m_anchors[slot].index);
            }

            /**
             * @brief 追加锚点名 `aN`
             * @param index 锚点编号（1 基）
             */
            void appendAnchorName(const std::size_t index)
            {
                appendRaw("a");
                appendInteger(index);
            }

            /**
             * @brief 判断容器是否可以写成 flow 风格
             * @details 条件：flowThreshold 非 0、元素数不超过该值、且元素全为标量。
             *          一旦含容器元素就退回块风格，避免出现跨行 flow 这种难读又难 diff 的形状。
             * @param value 待判定容器
             * @return true 可以写成 `[a, b]` / `{k: v}`
             */
            [[nodiscard]] bool isFlowEligible(const FormatValue &value) const
            {
                if (m_options.flowThreshold == 0)
                {
                    return false;
                }

                if (value.type() == FormatValueType::Array)
                {
                    const FormatValueArray &elements = value.asArray();
                    if (elements.empty() || elements.size() > m_options.flowThreshold)
                    {
                        return false;
                    }
                    for (const FormatValue &element: elements)
                    {
                        if (isContainerValue(element))
                        {
                            return false;
                        }
                    }
                    return true;
                }

                const FormatValueObject &members = value.asObject();
                if (members.empty() || members.size() > m_options.flowThreshold)
                {
                    return false;
                }
                for (const auto &[key, member]: members)
                {
                    static_cast<void>(key);
                    if (isContainerValue(member))
                    {
                        return false;
                    }
                }
                return true;
            }

            /**
             * @brief 判断空容器是否按「隐式 null」落地
             * @details 仅在 useFlowForEmptyContainers 关闭时成立：块风格没有空容器的写法，
             *          此时值位置留空，回读得到 null（该选项已声明的有损取舍）。
             * @param value 待判定值
             * @return true 该值应留下一个空的值位置
             */
            [[nodiscard]] bool rendersAsImplicitNull(const FormatValue &value) const noexcept
            {
                return !m_options.useFlowForEmptyContainers && isEmptyContainer(value);
            }

            /**
             * @brief 判断值是否为空容器
             * @param value 待判定值
             * @return true 空数组或空对象
             */
            [[nodiscard]] bool isEmptyContainer(const FormatValue &value) const noexcept
            {
                return isContainerValue(value) && childCountOf(value) == 0;
            }

            /**
             * @brief 检查容器层数是否超限
             * @param containerDepth 当前容器层数（根容器为 1）
             * @throws FormatError 超出 options.maximumDepth（kind 为 DepthExceeded）
             */
            void checkDepth(const std::size_t containerDepth) const
            {
                if (m_options.maximumDepth != 0 && containerDepth > m_options.maximumDepth)
                {
                    // 序列化没有输入文本可定位，位置取默认值（第 1 行第 1 列）
                    throw FormatError(FormatErrorKind::DepthExceeded,
                                      std::format("序列化嵌套深度超出上限 {}", m_options.maximumDepth),
                                      TextPosition{});
                }
            }

            /**
             * @brief 取一层块缩进的空格数
             * @details §6.1 规定缩进只能用空格且至少一个字符：indentWidth 为 0 时按 1 处理，
             *          否则无从表达块结构。
             * @return std::size_t 每层缩进宽度
             */
            [[nodiscard]] std::size_t indentStep() const noexcept
            {
                return m_options.indentWidth == 0 ? 1 : m_options.indentWidth;
            }

            /**
             * @brief 取块标量的内容缩进位数
             * @details §8.1.1.1 的缩进指示符只接受 1~9；缩进宽度更大时内容仍按指示符给出的
             *          位数缩进，多余的空格会成为正文，反而破坏内容。
             * @return std::size_t 内容缩进位数（1~9）
             */
            [[nodiscard]] std::size_t blockScalarIndentStep() const noexcept
            {
                const std::size_t step = indentStep();
                return step > 9 ? 9 : step;
            }

            /**
             * @brief 追加一段文本
             * @param text 待追加文本
             */
            void appendRaw(const std::string_view text)
            {
                m_output.append(text);
            }

            /**
             * @brief 追加一个换行并记录新行起点
             */
            void appendLineBreak()
            {
                m_output.push_back(kLineBreak);
                m_lineStart = m_output.size();
            }

            /**
             * @brief 追加缩进空白
             * @param indent 缩进空格数
             */
            void appendIndent(const std::size_t indent)
            {
                m_output.append(indent, ' ');
            }

            /**
             * @brief 收束当前行：输出不以换行结尾时补一个换行
             */
            void endLine()
            {
                if (m_output.empty() || m_output.back() != kLineBreak)
                {
                    appendLineBreak();
                }
            }

            /**
             * @brief 取当前行已写出的列数
             * @return std::size_t 从行首到当前写指针的字符数
             */
            [[nodiscard]] std::size_t currentColumn() const noexcept
            {
                return m_output.size() - m_lineStart;
            }

            /**
             * @brief 收集可复用的重复子树并编号锚点
             * @details 只在 reuseAnchors 打开时调用：先做一次迭代式深度校验，再按发射顺序收集
             *          候选容器，按结构哈希分桶，桶内用 FormatValue::operator== 精确归组，
             *          每种出现两次以上的结构登记一条锚点条目（首次发射写 `&aN`，其后写 `*aN`）。
             * @param root 文档根值
             * @throws FormatError 嵌套超限（kind 为 DepthExceeded）
             */
            void collectReusableAnchors(const FormatValue &root)
            {
                verifyDepthIteratively(root);

                std::vector<const FormatValue *> candidates;
                collectContainersInEmissionOrder(root, true, candidates);

                std::map<std::size_t, std::vector<const FormatValue *> > buckets;
                for (const FormatValue *candidate: candidates)
                {
                    buckets[hashOfValue(*candidate)].push_back(candidate);
                }

                std::size_t nextIndex = 0;
                for (const auto &[hash, bucket]: buckets)
                {
                    if (bucket.size() < 2)
                    {
                        continue; // 桶里只有一个成员，不可能有重复结构
                    }

                    // 哈希只负责归桶：真正是否相等由 operator== 裁决，碰撞不会带来错误复用
                    std::vector<bool> grouped(bucket.size(), false);
                    for (std::size_t position = 0; position < bucket.size(); ++position)
                    {
                        if (grouped[position])
                        {
                            continue;
                        }
                        grouped[position] = true;

                        bool isRepeated = false;
                        for (std::size_t other = position + 1; other < bucket.size(); ++other)
                        {
                            if (grouped[other] || !(*bucket[other] == *bucket[position]))
                            {
                                continue;
                            }
                            grouped[other] = true;
                            isRepeated     = true;
                        }

                        // 一种结构只登记一条：代表仅用于等值判定，锚点编号与结构一一对应
                        if (isRepeated)
                        {
                            m_anchors.push_back(AnchorEntry{bucket[position], hash, ++nextIndex, false});
                        }
                    }
                }
            }

            /**
             * @brief 按发射顺序收集候选容器
             * @details 跳过根节点（在文档开头写锚点属性要多占一行且没有复用价值）与空容器
             *          （`[]` / `{}` 本身只有 2 字节）。子节点顺序与发射顺序完全一致。
             * @param value 当前节点
             * @param isRoot 是否为文档根
             * @param candidates 输出参数，按发射顺序追加候选节点地址
             */
            void collectContainersInEmissionOrder(const FormatValue &value, const bool isRoot, std::vector<const FormatValue *> &candidates)
            {
                if (!isContainerValue(value))
                {
                    return;
                }
                if (!isRoot && childCountOf(value) > 0)
                {
                    candidates.push_back(&value);
                }

                if (value.type() == FormatValueType::Array)
                {
                    for (const FormatValue &element: value.asArray())
                    {
                        collectContainersInEmissionOrder(element, false, candidates);
                    }
                    return;
                }
                for (const auto &[key, member]: value.asObject())
                {
                    static_cast<void>(key);
                    collectContainersInEmissionOrder(member, false, candidates);
                }
            }

            /**
             * @brief 用显式栈做一次深度校验
             * @details 必须早于哈希与发射：哈希与发射都是递归实现，先在超深值上抛出
             *          FormatError 才能避免调用栈被写爆。
             * @param root 文档根值
             * @throws FormatError 嵌套超限（kind 为 DepthExceeded）
             */
            void verifyDepthIteratively(const FormatValue &root) const
            {
                struct Pending
                {
                    const FormatValue *value;          ///< 待访问节点
                    std::size_t        containerDepth; ///< 该节点作为容器时的层数
                };

                std::vector<Pending> pending;
                pending.push_back(Pending{&root, 1});

                while (!pending.empty())
                {
                    const Pending current = pending.back();
                    pending.pop_back();
                    if (!isContainerValue(*current.value))
                    {
                        continue;
                    }

                    checkDepth(current.containerDepth);
                    const std::size_t childDepth = current.containerDepth + 1;

                    if (current.value->type() == FormatValueType::Array)
                    {
                        for (const FormatValue &element: current.value->asArray())
                        {
                            pending.push_back(Pending{&element, childDepth});
                        }
                        continue;
                    }
                    for (const auto &[key, member]: current.value->asObject())
                    {
                        static_cast<void>(key);
                        pending.push_back(Pending{&member, childDepth});
                    }
                }
            }

            /**
             * @brief 粗估输出文本的字节规模，用于一次性 reserve
             * @details 用显式栈遍历，避免为预估再引入一通递归；估算只求量级正确（宁大勿小），
             *          不参与任何正确性判断。
             * @param root 文档根值
             * @return std::size_t 估算字节数
             */
            [[nodiscard]] std::size_t estimateOutputSize(const FormatValue &root) const
            {
                std::size_t                      total = 0;
                std::vector<const FormatValue *> pending;
                pending.push_back(&root);

                while (!pending.empty())
                {
                    const FormatValue *current = pending.back();
                    pending.pop_back();

                    switch (current->type())
                    {
                        case FormatValueType::Null:
                            total += 5;
                            break;
                        case FormatValueType::Bool:
                            total += 6;
                            break;
                        case FormatValueType::Int:
                        case FormatValueType::UInt:
                            total += 24;
                            break;
                        case FormatValueType::Double:
                            total += 32;
                            break;
                        case FormatValueType::String:
                            total += current->asString().size() + 4;
                            break;
                        case FormatValueType::Array:
                        {
                            const FormatValueArray &elements = current->asArray();
                            total                            += 2 + elements.size() * 4;
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
                                total += key.size() + 6;
                                pending.push_back(&member);
                            }
                            break;
                        }
                        default:
                            total += 5;
                            break;
                    }
                }
                return total;
            }

            const YamlWriteOptions & m_options;      ///< 序列化选项（引用外部对象，生命周期由调用方保证）
            std::string              m_output;       ///< 输出缓冲
            std::size_t              m_lineStart{0}; ///< 当前行在输出缓冲中的起始偏移，供折行列号判断
            std::vector<AnchorEntry> m_anchors;      ///< 重复子树的锚点条目，仅 reuseAnchors 时非空
        };
    } // namespace

    std::string YamlWriter::write(const FormatValue &value)
    {
        return write(value, YamlWriteOptions{});
    }

    std::string YamlWriter::write(const FormatValue &value, const YamlWriteOptions &options)
    {
        YamlEmitter emitter(options);
        return emitter.emit(value);
    }
} // namespace AsynGyanis::Base
