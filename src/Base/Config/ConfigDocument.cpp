#include "Base/Config/ConfigDocument.h"

#include "Base/Config/ConfigValueType.h"
#include "Platform/FileSystem/FileSystem.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 读取文本文件全部内容
         * @param filePath 目标文件路径
         * @return std::optional<std::string> 成功返回内容，无法打开返回 std::nullopt
         */
        [[nodiscard]] std::optional<std::string> readTextFile(const std::filesystem::path &filePath)
        {
            std::ifstream file(filePath, std::ios::in | std::ios::binary);
            if (!file.is_open())
            {
                return std::nullopt;
            }

            // 先按文件大小预留容量：istreambuf_iterator 逐字符追加会触发多次重新分配与搬移。
            // 取不到大小（管道、特殊文件）时保持按需增长，不影响正确性
            std::string     content;
            std::error_code sizeError;
            if (const std::uintmax_t fileSize = std::filesystem::file_size(filePath, sizeError); !sizeError)
            {
                content.reserve(static_cast<std::string::size_type>(fileSize));
            }

            content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

            // BOM 在这里就剥掉，而不是留给 JSON 解析器：只带 BOM 的文件（Windows 记事本把空文件
            // 存成 UTF-8 就是这个字节串）会先过「空文件」判定——三个非空白字节判不出空，
            // 于是带着空正文去解析，被报成「JSON 语法错误」。同一个文件在 YAML 侧被当空文档接受、
            // 真·空文件也被接受，只有 JSON+BOM 这一格被拒，而且每次热重载都重复报这个幽灵错误
            constexpr std::string_view kUtf8ByteOrderMark = "\xEF\xBB\xBF";
            if (content.starts_with(kUtf8ByteOrderMark))
            {
                content.erase(0, kUtf8ByteOrderMark.size());
            }
            return content;
        }

        /**
         * @brief 判断文本是否只由空白字符组成
         * @param text 待判定文本
         * @return true 为空或全空白
         */
        [[nodiscard]] bool isBlankText(std::string_view text) noexcept
        {
            return std::ranges::all_of(text, [](const unsigned char character) { return std::isspace(character) != 0; });
        }

        /// 单个配置文件的体积上限（64 MiB）：配置文件是人工产物，超出这个规模基本可判定为误传或
        /// 攻击载荷，在解析前拦下，免得解析器为超大文本做巨量分配
        constexpr std::uintmax_t kMaximumConfigFileBytes = 64ULL * 1024ULL * 1024ULL;

        /// 文档的嵌套深度上限（JSON 与 YAML 同一口径）。YAML 侧：别名在 DOM 里是共享引用，
        /// 循环别名会让转换无限递归；JSON 侧：nlohmann 的解析器是状态机、不会先崩在解析上，
        /// 会崩的是 DOM 的递归析构与我们自己摊平嵌套结构的递归
        constexpr std::size_t kMaximumDocumentDepth = 128;

        /// YAML 文档展开后的节点总数上限：别名炸弹（引用同一锚点逐层放大）靠这道预算拦下
        constexpr std::size_t kMaximumYamlNodeCount = 200000;

        /// YAML 展开文本的字节预算（键与标量文本的展开总量，16 MiB）。节点数预算按「个数」算，
        /// 挡不住「一个大标量被别名引用很多次」：每次引用都深拷贝一份文本，节点数远没超而字节数
        /// 能乘出来；输入文件的体积上限（64 MiB）只约束文本本身，同样管不住这份乘出来的展开量
        constexpr std::size_t kMaximumYamlExpandedBytes = 16ULL * 1024ULL * 1024ULL;

        /**
         * @brief 读取或转换配置文档时的失败（嵌套超限、不支持的标签、数值越界等）
         * @details 只在本翻译单元内抛出并被 loadConfigFile 捕获后转成中文错误文案，
         *          模块对外从不暴露该类型，因此刻意不并入 Base 的异常层次。
         * @note 本文件里那两处裸 `throw std::runtime_error`（含点号的键、空键段）是同一条通道：
         *       它们同样只走到 loadConfigFile 的 `catch (const std::runtime_error &)` 就被折进
         *       ConfigLoadResult::errors，从不交给调用方。把它们换成 Base:: 家族反而会改坏文案——
         *       框架异常的 what() 自带「[异常] … [文件:行 in 函数]」包装，那层包装会被当作
         *       「解析原因」拼进 errors。对外抛的那批（ConfigValidationException 等）才走框架层次。
         */
        class DocumentConversionException : public std::runtime_error
        {
        public:
            /**
             * @brief 以中文失败原因构造异常
             * @param reason 不含文件名的失败原因
             */
            explicit DocumentConversionException(const std::string &reason) : std::runtime_error(reason)
            {
            }
        };

        /**
         * @brief 扣减 YAML 展开的字节预算，不够就抛出
         * @param byteCount 本次要计入的字节数（键或标量的文本长度）
         * @param remainingByteBudget 剩余预算，逐次扣减
         * @throws DocumentConversionException 预算不够
         */
        void consumeYamlExpandedBytes(const std::size_t byteCount, std::size_t &remainingByteBudget)
        {
            if (byteCount > remainingByteBudget)
            {
                throw DocumentConversionException(std::format("文档展开后的文本总量超过上限 {} MiB：引用同一锚点的别名会被逐份复制文本，请减少别名引用或拆分文件",
                                                              kMaximumYamlExpandedBytes / (1024ULL * 1024ULL)));
            }
            remainingByteBudget -= byteCount;
        }

        /**
         * @brief 把库的位置标记格式化为可读文本
         * @param mark yaml-cpp 的位置标记
         * @return std::string 形如「第 N 行，第 M 列」；标记无效时返回「位置未知」
         */
        [[nodiscard]] std::string describeYamlMark(const YAML::Mark &mark)
        {
            if (mark.is_null())
            {
                return "位置未知";
            }
            // 库的行列从 0 起算，报给用户时统一转成从 1 起算
            return std::format("第 {} 行，第 {} 列", mark.line + 1, mark.column + 1);
        }

        /**
         * @brief 按核心 schema 识别布尔文本
         * @param text 标量文本
         * @return std::optional<bool> 不是布尔文本时返回空；只认 true/false 的三种大小写组合
         */
        [[nodiscard]] std::optional<bool> parseCoreBoolean(const std::string &text) noexcept
        {
            if (text == "true" || text == "True" || text == "TRUE")
            {
                return true;
            }
            if (text == "false" || text == "False" || text == "FALSE")
            {
                return false;
            }
            return std::nullopt;
        }

        /**
         * @brief 把一位数字累加进无符号量，超出 uint64 可表示范围时返回 false
         * @details YAML 与 JSON 两条解析路共用这一条越界判据：同一个字面量在一侧被明确拒绝、
         *          在另一侧被静默折成 double，等于「同一份配置换个后缀就换行为」
         * @param magnitude 已累加的绝对值，成功时就地更新
         * @param digit 本次累加的数字（0~15）
         * @param base 进制（8、10、16）
         * @return bool 未溢出返回 true；返回 false 时 magnitude 保持原值
         */
        [[nodiscard]] bool accumulateDecimalDigit(std::uint64_t &magnitude, const int digit, const std::uint64_t base) noexcept
        {
            // 先判「乘 base 再加一位」是否会回绕：静默回绕会得到一个看似正常的错误数值
            if (magnitude > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(digit)) / base)
            {
                return false;
            }
            magnitude = magnitude * base + static_cast<std::uint64_t>(digit);
            return true;
        }

        /**
         * @brief 判断一个绝对值在带负号时是否还能落进 int64
         * @details uint64 能装下的负数只到 INT64_MIN 的量级（|-2^63| = 2^63），再大同样越界
         * @param magnitude 绝对值
         * @return bool 可表示为 int64 时返回 true
         */
        [[nodiscard]] constexpr bool fitsNegativeInt64(const std::uint64_t magnitude) noexcept
        {
            return magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL;
        }

        /**
         * @brief 按核心 schema 识别整数文本（十进制、0o 八进制、0x 十六进制，可带正负号）
         * @details 非负整数与 JSON 侧口径一致地落无符号数（原生解析把非负整数放进 number_unsigned）。
         *          YAML 1.2.2 §10.3.2 的十进制整数是 [-+]? [0-9]+，前导零与一个正负号都允许；
         *          八进制是 0o[0-7]+，十六进制是 0x[0-9a-fA-F]+。
         * @param text 标量文本
         * @param node 所在节点，用于错误位置
         * @return std::optional<ConfigValue> 不是整数文本时返回空
         * @throws DocumentConversionException 是整数文本但超出 64 位表示范围
         */
        [[nodiscard]] std::optional<ConfigValue> parseCoreInteger(const std::string &text, const YAML::Node &node)
        {
            std::size_t position = 0;
            bool        negative = false;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
            {
                negative = text[position] == '-';
                ++position;
            }

            int base = 10;
            if (text.compare(position, 2, "0o") == 0)
            {
                base = 8;
                position += 2;
            } else if (text.compare(position, 2, "0x") == 0)
            {
                base = 16;
                position += 2;
            }
            if (position >= text.size())
            {
                return std::nullopt;
            }

            std::uint64_t magnitude = 0;
            for (; position < text.size(); ++position)
            {
                const char character = text[position];
                int        digit     = -1;
                if (character >= '0' && character <= '9')
                {
                    digit = character - '0';
                } else if (base == 16 && character >= 'a' && character <= 'f')
                {
                    digit = character - 'a' + 10;
                } else if (base == 16 && character >= 'A' && character <= 'F')
                {
                    digit = character - 'A' + 10;
                }
                if (digit < 0 || digit >= base)
                {
                    // 出现非数字字符（如 1.2.3、0xZZ）：整体不是整数
                    return std::nullopt;
                }
                // 先按无符号累加，溢出即越界：静默回绕会得到看似正常的错误数值
                if (!accumulateDecimalDigit(magnitude, digit, static_cast<std::uint64_t>(base)))
                {
                    throw DocumentConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
                }
            }

            if (negative)
            {
                // 负方向 uint64 只到 INT64_MIN 的量级，再大同样越界
                if (!fitsNegativeInt64(magnitude))
                {
                    throw DocumentConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
                }
                if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL)
                {
                    return ConfigValue(std::numeric_limits<std::int64_t>::min());
                }
                return ConfigValue(-static_cast<std::int64_t>(magnitude));
            }
            return ConfigValue(magnitude);
        }

        /**
         * @brief 按核心 schema 识别浮点文本
         * @details YAML 1.2.2 §10.3.2 的浮点是
         *          [-+]? ( \. [0-9]+ | [0-9]+ ( \. [0-9]* )? ) ( [eE] [-+]? [0-9]+ )?：
         *          前导点（.5）、尾随点（1.）、整数部分的前导零（007.5）与带符号指数都合法，
         *          整个数也允许一个前导 '+'。另接受 .inf/.nan 系列。
         *          裸 inf/nan 按 schema 属字符串，而库的解析会接受它们，因此先按字符集挡掉再做严格的全串解析。
         * @param text 标量文本
         * @return std::optional<double> 不是浮点文本时返回空
         * @throws DocumentConversionException 是浮点文本但超出 double 表示范围
         */
        [[nodiscard]] std::optional<double> parseCoreFloat(const std::string &text)
        {
            std::string_view signlessText = text;
            bool             negative     = false;
            if (!signlessText.empty() && (signlessText.front() == '+' || signlessText.front() == '-'))
            {
                negative = signlessText.front() == '-';
                signlessText.remove_prefix(1);
            }

            // 核心 schema 的浮点只有一个可选符号。剥掉它之后仍以符号开头（如 "+-1"），
            // 剩下的 "-1" 会被 from_chars 当成负数接受，必须在数值解析之前判为非浮点
            if (!signlessText.empty() && (signlessText.front() == '+' || signlessText.front() == '-'))
            {
                return std::nullopt;
            }

            if (signlessText == ".inf" || signlessText == ".Inf" || signlessText == ".INF")
            {
                return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            }
            if (signlessText == ".nan" || signlessText == ".NaN" || signlessText == ".NAN")
            {
                return std::numeric_limits<double>::quiet_NaN();
            }

            // 字符集先挡一道：库也接受 "inf"/"nan"/"0x1p3" 这类写法，但它们不是核心 schema 的浮点
            bool hasDigit = false;
            for (const char character: signlessText)
            {
                if (character >= '0' && character <= '9')
                {
                    hasDigit = true;
                } else if (character != '.' && character != '-' && character != '+' && character != 'e' && character != 'E')
                {
                    return std::nullopt;
                }
            }
            if (!hasDigit)
            {
                return std::nullopt;
            }

            std::string_view numericText = text;
            // 核心 schema 允许的前导 '+' 不是标准库解析所认的写法，剥掉；'-' 保留给库处理
            if (!numericText.empty() && numericText.front() == '+')
            {
                numericText.remove_prefix(1);
            }

            double parsedValue                   = 0.0;
            const auto [stopPosition, errorCode] = std::from_chars(numericText.data(), numericText.data() + numericText.size(), parsedValue);
            if (errorCode == std::errc::result_out_of_range)
            {
                throw DocumentConversionException(std::format("浮点值 '{}' 超出 double 表示范围", text));
            }
            if (errorCode != std::errc() || stopPosition != numericText.data() + numericText.size())
            {
                // 形态不合（如 1.2.3、1e）：不是浮点，按字符串原样保留
                return std::nullopt;
            }
            return parsedValue;
        }

        /**
         * @brief 把 YAML 标量转换为 JSON 标量
         * @details 引号标量（标签 "!"）一律按字符串处理；普通标量（标签 "?"）按 YAML 1.2
         *          核心 schema 识别 true/false、整数与浮点，其余原样作字符串；显式标签只认
         *          核心 schema 的 str/int/float/bool/null，其余（如 !!binary、!!timestamp）
         *          明确报错而不是静默丢成字符串。
         * @param node 标量节点
         * @param remainingByteBudget 剩余展开字节预算；标量文本正是别名乘法所复制的东西，在这里扣
         * @return ConfigValue 转换结果
         * @throws DocumentConversionException 标签不受支持、整数或浮点越界、显式 bool/float 的取值不合法、展开字节超限
         */
        [[nodiscard]] ConfigValue yamlScalarToConfigValue(const YAML::Node &node, std::size_t &remainingByteBudget)
        {
            const std::string &tag  = node.Tag();
            const std::string &text = node.Scalar();
            consumeYamlExpandedBytes(text.size(), remainingByteBudget);

            if (tag == "!" || tag == "tag:yaml.org,2002:str")
            {
                return ConfigValue(text);
            }
            if (tag == "tag:yaml.org,2002:null")
            {
                return ConfigValue(nullptr);
            }
            if (tag == "tag:yaml.org,2002:bool")
            {
                if (const std::optional<bool> booleanValue = parseCoreBoolean(text))
                {
                    return ConfigValue(*booleanValue);
                }
                throw DocumentConversionException(std::format("!!bool 的取值 '{}' 不是合法布尔（{}）：只接受 true/false", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:int")
            {
                if (const std::optional<ConfigValue> integerValue = parseCoreInteger(text, node))
                {
                    return *integerValue;
                }
                throw DocumentConversionException(std::format("!!int 的取值 '{}' 不是合法整数（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:float")
            {
                if (const std::optional<double> floatingValue = parseCoreFloat(text))
                {
                    return ConfigValue(*floatingValue);
                }
                throw DocumentConversionException(std::format("!!float 的取值 '{}' 不是合法浮点（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag != "?")
            {
                throw DocumentConversionException(
                        std::format("不支持的 YAML 标签 '{}'（{}）：请改用 !!str/!!int/!!float/!!bool/!!null 或去掉标签", tag, describeYamlMark(node.Mark())));
            }

            // 普通标量：核心 schema 的类型判定，顺序为 bool → int → float → string
            if (const std::optional<bool> booleanValue = parseCoreBoolean(text))
            {
                return ConfigValue(*booleanValue);
            }
            if (const std::optional<ConfigValue> integerValue = parseCoreInteger(text, node))
            {
                return *integerValue;
            }
            if (const std::optional<double> floatingValue = parseCoreFloat(text))
            {
                return ConfigValue(*floatingValue);
            }
            return ConfigValue(text);
        }

        /**
         * @brief 递归把 yaml-cpp 节点转换为 JSON 值
         * @details 别名在 yaml-cpp 的 DOM 里是共享引用、在 JSON 里只能深拷贝，故用深度、节点总数
         *          与展开文本字节三道上限拦住循环别名与别名炸弹（后两道分工：节点数拦「逐层放大」、
         *          字节数拦「一个大标量被抄很多份」）；重复键在 DOM 里保留为两份，这里明确报错
         *          而不让后者静默覆盖前者。
         * @param node 当前节点
         * @param depth 当前嵌套深度（根为 0）
         * @param remainingNodeBudget 剩余可转换节点数，逐节点扣减
         * @param remainingByteBudget 剩余可展开的文本字节数（键与标量），逐份扣减
         * @return ConfigValue 转换结果
         * @throws DocumentConversionException 超过深度、节点数或展开字节上限、映射键不是标量、映射存在重复键
         */
        [[nodiscard]] ConfigValue yamlNodeToConfigValue(const YAML::Node &node, const std::size_t depth, std::size_t &remainingNodeBudget, std::size_t &remainingByteBudget)
        {
            if (depth > kMaximumDocumentDepth)
            {
                throw DocumentConversionException(std::format("文档嵌套或别名展开超过 {} 层：若文件里存在循环别名，请先解开锚点引用", kMaximumDocumentDepth));
            }
            if (remainingNodeBudget == 0)
            {
                throw DocumentConversionException(std::format("文档展开后的节点总数超过上限 {}：引用同一锚点的别名会被逐层复制，请减少别名引用", kMaximumYamlNodeCount));
            }
            --remainingNodeBudget;

            // 空值（null）与未定义节点都按 null 处理：「键存在但没有取值」是合法形态
            if (!node.IsDefined() || node.IsNull())
            {
                return ConfigValue(nullptr);
            }

            if (node.IsSequence())
            {
                ConfigValue elements = ConfigValue::array();
                for (const YAML::Node &element: node)
                {
                    elements.push_back(yamlNodeToConfigValue(element, depth + 1, remainingNodeBudget, remainingByteBudget));
                }
                return elements;
            }

            if (node.IsMap())
            {
                ConfigValue members = ConfigValue::object();
                for (const auto &entry: node)
                {
                    if (!entry.first.IsScalar())
                    {
                        throw DocumentConversionException(std::format("映射的键必须是标量（{}）：复杂键无法表示为 JSON 对象键", describeYamlMark(entry.first.Mark())));
                    }
                    const std::string key = entry.first.Scalar();
                    consumeYamlExpandedBytes(key.size(), remainingByteBudget);
                    if (members.contains(key))
                    {
                        throw DocumentConversionException(std::format("映射中存在重复键 '{}'（{}）：请删除重复定义", key, describeYamlMark(entry.first.Mark())));
                    }
                    members[key] = yamlNodeToConfigValue(entry.second, depth + 1, remainingNodeBudget, remainingByteBudget);
                }
                return members;
            }

            // 到这里只剩标量：未定义与 null 已在前面处理
            return yamlScalarToConfigValue(node, remainingByteBudget);
        }

        /**
         * @brief 解析 YAML 文本并转换为 JSON 值模型
         * @details 只取首份文档（配置文件不做多文档语义）；空输入与空文档返回 null，
         *          由根节点类型校验给出「根节点必须是映射」。
         * @param text 文档文本
         * @return ConfigValue 文档根值
         * @throws YAML::Exception YAML 语法非法
         * @throws DocumentConversionException 超过展开或深度上限、含不支持的标签
         */
        [[nodiscard]] ConfigValue yamlDocumentToConfigValue(const std::string &text)
        {
            const YAML::Node document = YAML::Load(text);
            if (!document.IsDefined() || document.IsNull())
            {
                return ConfigValue(nullptr);
            }

            std::size_t remainingNodeBudget = kMaximumYamlNodeCount;
            std::size_t remainingByteBudget = kMaximumYamlExpandedBytes;
            return yamlNodeToConfigValue(document, 0, remainingNodeBudget, remainingByteBudget);
        }

        /**
         * @brief 挡下 JSON 文本里超出 64 位整数范围或 double 表示范围的数值字面量
         * @details nlohmann 在词法阶段就把超出 [INT64_MIN, UINT64_MAX] 的整数字面量折成 double：
         *          精度当场丢失且不留任何标记，之后 `getInt` 只能按「类型不符」回落默认值；浮点侧同样
         *          静默——下溢的写法（1e-400）被折成 0.0。而同一个数值写成 YAML 会被明确拒绝
         *          （见 parseCoreInteger 与 parseCoreFloat）。同一份配置换个后缀就换行为，
         *          是这里最难查的一类现场，因此解析前先把这两种字面量扫出来拒掉。
         *          判范围只能在解析前做：事后 DOM 里已经分不出「超大整数」与「浮点数」。
         *          扫描只认字符串字面量之外的数值记数器，误拒比漏拒更糟（合法配置会直接加载失败）。
         * @param text 去掉 BOM 后的 JSON 文档文本
         * @throws DocumentConversionException 存在超出表示范围的数值字面量
         */
        void skipJsonStringLiteral(const std::string &text, std::size_t &index) noexcept
        {
            // 进入时 index 指向开引号，返回时指向闭引号之后；未闭合时停在文本末尾
            ++index;
            for (bool isEscaped = false; index < text.size(); ++index)
            {
                const char character = text[index];
                if (isEscaped)
                {
                    isEscaped = false;
                } else if (character == '\\')
                {
                    isEscaped = true;
                } else if (character == '"')
                {
                    ++index;
                    return;
                }
            }
        }

        /**
         * @brief 判定一段浮点字面量是否超出 double 表示范围
         * @param text 文档文本
         * @param tokenBegin 字面量起点
         * @param tokenEnd 字面量终点（不含）
         * @throws DocumentConversionException 上下溢
         */
        void rejectOutOfRangeFloatLiteral(const std::string &text, const std::size_t tokenBegin, const std::size_t tokenEnd)
        {
            double floatingValue                 = 0.0;
            const auto [stopPosition, errorCode] = std::from_chars(text.data() + tokenBegin, text.data() + tokenEnd, floatingValue);
            static_cast<void>(stopPosition);
            if (errorCode == std::errc::result_out_of_range)
            {
                throw DocumentConversionException(std::format("浮点值 '{}' 超出 double 表示范围（第 {} 字节处）", text.substr(tokenBegin, tokenEnd - tokenBegin), tokenBegin + 1U));
            }
        }

        /**
         * @brief 判定一段整数字面量是否超出 64 位表示范围
         * @param text 文档文本
         * @param tokenBegin 字面量起点
         * @param digitsBegin 第一位数字
         * @param tokenEnd 字面量终点（不含）
         * @param isNegative 是否带负号
         * @throws DocumentConversionException 超出有符号或无符号 64 位
         */
        void rejectOutOfRangeIntegerLiteral(const std::string &text, const std::size_t tokenBegin, const std::size_t digitsBegin, const std::size_t tokenEnd, const bool isNegative)
        {
            std::uint64_t magnitude  = 0;
            bool          overflowed = false;
            for (std::size_t digitPosition = digitsBegin; digitPosition < tokenEnd; ++digitPosition)
            {
                if (!accumulateDecimalDigit(magnitude, text[digitPosition] - '0', 10U))
                {
                    overflowed = true;
                    break;
                }
            }
            if (overflowed || (isNegative && !fitsNegativeInt64(magnitude)))
            {
                throw DocumentConversionException(std::format("整数 '{}' 超出 64 位表示范围（第 {} 字节处）："
                                                              "JSON 会把这样的字面量折成 double 并丢掉精度，"
                                                              "取用时会按类型不符回落默认值",
                                                              text.substr(tokenBegin, tokenEnd - tokenBegin), tokenBegin + 1U));
            }
        }

        void rejectOutOfRangeJsonNumbers(const std::string &text)
        {
            std::size_t index = 0;
            while (index < text.size())
            {
                const char currentCharacter = text[index];

                if (currentCharacter == '"')
                {
                    // 整个字符串字面量跳掉：键名与字符串值里的数字与数值记数器无关
                    skipJsonStringLiteral(text, index);
                    continue;
                }

                const bool startsInteger = (currentCharacter >= '0' && currentCharacter <= '9') ||
                                           (currentCharacter == '-' && index + 1 < text.size() && text[index + 1] >= '0' && text[index + 1] <= '9');
                if (!startsInteger)
                {
                    ++index;
                    continue;
                }

                const std::size_t tokenBegin  = index;
                const bool        isNegative  = currentCharacter == '-';
                const std::size_t digitsBegin = tokenBegin + (isNegative ? 1U : 0U);

                std::size_t cursor         = digitsBegin;
                bool        looksLikeFloat = false;
                while (cursor < text.size())
                {
                    const char digitCharacter = text[cursor];
                    if (digitCharacter >= '0' && digitCharacter <= '9')
                    {
                        ++cursor;
                        continue;
                    }
                    if (digitCharacter == '.' || digitCharacter == 'e' || digitCharacter == 'E')
                    {
                        // 带小数点或指数：本来就是浮点字面量，落到 double 不算变形（范围另判）
                        looksLikeFloat = true;
                        ++cursor;
                        // 指数符号只在 e/E 之后才算这一串的一部分：漏了它就把 "1e-400" 扫成 "1e"，
                        // 后面的 from_chars 只会报「形态不合」而看不见下溢
                        if (digitCharacter == 'e' || digitCharacter == 'E')
                        {
                            if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-'))
                            {
                                ++cursor;
                            }
                        }
                        continue;
                    }
                    break;
                }
                index = cursor;
                if (looksLikeFloat)
                {
                    // 浮点写法不在整数判据里，但同样要判范围：nlohmann 把下溢（1e-400）悄悄折成 0.0，
                    // 而 YAML 侧走 from_chars，上下溢都会报「超出 double 表示范围」——两侧口径必须一致
                    rejectOutOfRangeFloatLiteral(text, tokenBegin, cursor);
                    continue;
                }

                rejectOutOfRangeIntegerLiteral(text, tokenBegin, digitsBegin, cursor, isNegative);
            }
        }

        /**
         * @brief 解析 JSON 文本（先剥掉可选的 UTF-8 BOM）
         * @details 深度闸门挂在解析回调上：nlohmann 的解析器自己是状态机，超限的是随后
         *          那个几万层的 DOM 的递归析构，以及我们把嵌套结构摊平成点分键的那趟递归，
         *          所以在建 DOM 的途中就抛出，交回调用方的是「一轮失败的加载」而不是崩溃。
         *          越界数值字面量在建 DOM 之前先挡掉（见 rejectOutOfRangeJsonNumbers）——
         *          nlohmann 一旦把它们折成 double，事后从 DOM 里就分不出「写了个超大整数」还是
         *          「写了个浮点数」。
         * @param text 文档文本
         * @return ConfigValue 文档根值
         * @throws DocumentConversionException 嵌套超过 kMaximumDocumentDepth 层，或数值字面量超出可表示范围
         * @throws nlohmann::json::exception 语法错误（消息自带行列与出错记号）
         */
        [[nodiscard]] ConfigValue parseJsonDocument(const std::string &text)
        {
            const auto depthGuard = [](const int depth, const ConfigValue::parse_event_t, ConfigValue &)
            {
                if (static_cast<std::size_t>(depth) > kMaximumDocumentDepth)
                {
                    throw DocumentConversionException(std::format("文档嵌套超过 {} 层：正常配置不会这么深，多半是误传或机器生成的文件", kMaximumDocumentDepth));
                }
                return true;
            };

            // nlohmann 不认 UTF-8 BOM，而 Windows 编辑器常写 BOM：先剥掉再解析。
            // 判越界整数用的是同一份去 BOM 后的文本，字节位置才对得上
            constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
            if (text.starts_with(kUtf8Bom))
            {
                const std::string documentText = text.substr(kUtf8Bom.size());
                rejectOutOfRangeJsonNumbers(documentText);
                return ConfigValue::parse(documentText, depthGuard);
            }

            rejectOutOfRangeJsonNumbers(text);
            return ConfigValue::parse(text, depthGuard);
        }

        /**
         * @brief 按文件后缀选择原生解析器解析一份配置文档
         * @details .json 走 nlohmann_json；.yaml/.yml 走 yaml-cpp 再转换成 JSON 值模型。
         * @param text 文档文本
         * @param filePath 文件路径，仅用于挑选解析器
         * @return ConfigValue 文档根值
         * @throws nlohmann::json::exception JSON 语法非法
         * @throws YAML::Exception YAML 语法非法
         * @throws DocumentConversionException YAML 文档无法忠实转换为 JSON 值模型
         */
        [[nodiscard]] ConfigValue parseDocumentBySuffix(const std::string &text, const std::filesystem::path &filePath)
        {
            if (isJsonFile(Platform::FileSystem::utf8FromPath(filePath)))
            {
                return parseJsonDocument(text);
            }
            return yamlDocumentToConfigValue(text);
        }

    } // namespace

    bool ConfigDocument::load(const std::filesystem::path &filePath, ConfigValue &document, std::string &error)
    {
        // 先按文件大小拦下超大文件：解析器要为全文分配内存，读进来再判就晚了
        std::error_code      sizeError;
        const std::uintmax_t fileSize = std::filesystem::file_size(filePath, sizeError);
        if (!sizeError && fileSize > kMaximumConfigFileBytes)
        {
            error = std::format("文件 '{}' 超过配置文件的体积上限（{} MiB）：请拆分或精简该文件", Platform::FileSystem::utf8FromPath(filePath),
                                kMaximumConfigFileBytes / (1024ULL * 1024ULL));
            return false;
        }

        const std::optional<std::string> text = readTextFile(filePath);
        if (!text.has_value())
        {
            error = "无法打开文件 '" + Platform::FileSystem::utf8FromPath(filePath) + "'：文件不存在或不可读";
            return false;
        }

        std::string_view           textView           = *text;
        constexpr std::string_view kUtf8ByteOrderMark = "\xEF\xBB\xBF";
        if (textView.starts_with(kUtf8ByteOrderMark))
        {
            textView.remove_prefix(kUtf8ByteOrderMark.size());
        }
        if (isBlankText(textView))
        {
            // 空文件（含只有 BOM 的 JSON）：不报错、不产出配置项；空文档用 null 表示
            document = ConfigValue();
            return true;
        }

        try
        {
            // 文档必须可改写：叶子值会被移出以便省掉一次深拷贝（文档本身是本次解析的临时产物）
            document = parseDocumentBySuffix(*text, filePath);
        } catch (const ConfigValue::exception &exception)
        {
            // 原生库的消息自带行列与出错记号，是定位所需的全部信息，原样带出
            error = std::format("解析错误：'{}'：JSON 语法错误：{}", Platform::FileSystem::utf8FromPath(filePath), exception.what());
            return false;
        } catch (const YAML::Exception &exception)
        {
            error = std::format("解析错误：'{}'：YAML 语法错误：{}（{}）", Platform::FileSystem::utf8FromPath(filePath), exception.msg, describeYamlMark(exception.mark));
            return false;
        } catch (const std::runtime_error &exception)
        {
            // YAML → JSON 值模型转换失败：本文件内抛出的中文原因（含位置）
            error = std::format("解析错误：'{}'：{}", Platform::FileSystem::utf8FromPath(filePath), exception.what());
            return false;
        } catch (const std::exception &exception)
        {
            error = "加载 '" + Platform::FileSystem::utf8FromPath(filePath) + "' 时发生意外错误：" + exception.what();
            return false;
        }
        return true;
    }
} // namespace AsynGyanis::Base
