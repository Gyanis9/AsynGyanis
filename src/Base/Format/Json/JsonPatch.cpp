/**
 * @file JsonPatch.cpp
 * @brief JSON Patch（RFC 6902）：六种操作的解析、校验与原子应用
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonPatch.h"

#include "Base/Format/Json/JsonPointer.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Value/FormatValueType.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 带符号整数的「符号 + 幅值」表示，用于跨 Int/UInt 的精确比较
         */
        struct IntegralMagnitude
        {
            bool          isNegative{false}; ///< 是否为负数
            std::uint64_t magnitude{0};      ///< 绝对值
        };

        /**
         * @brief 把 FormatValue 归一化为「符号 + 幅值」
         * @details 用无符号取负实现取绝对值，避免对 INT64_MIN 取负造成有符号溢出（UB）。
         * @param value 待归一化的值
         * @return std::optional<IntegralMagnitude> 整数族返回幅值；非整数族返回空
         */
        std::optional<IntegralMagnitude> toIntegralMagnitude(const FormatValue &value) noexcept
        {
            if (const std::optional<std::int64_t> signedInteger = value.get<std::int64_t>())
            {
                const bool isNegative = *signedInteger < 0;
                const std::uint64_t magnitude = isNegative
                                                    ? ~static_cast<std::uint64_t>(*signedInteger) + 1ULL
                                                    : static_cast<std::uint64_t>(*signedInteger);
                return IntegralMagnitude{.isNegative = isNegative, .magnitude = magnitude};
            }

            // get<int64_t>() 只覆盖 int64 范围内的 UInt，超出部分由 get<uint64_t>() 接住
            if (const std::optional<std::uint64_t> unsignedInteger = value.get<std::uint64_t>())
            {
                return IntegralMagnitude{.isNegative = false, .magnitude = *unsignedInteger};
            }

            return std::nullopt;
        }

        /**
         * @brief 把数值统一取为 double
         * @param value 数值
         * @return double 对应的浮点值（整数族为近似值，仅用于整数与浮点混比）
         */
        double numberAsDouble(const FormatValue &value) noexcept
        {
            if (const std::optional<double> floatingPoint = value.getDouble())
            {
                return *floatingPoint;
            }
            if (const std::optional<std::int64_t> signedInteger = value.getInt())
            {
                return static_cast<double>(*signedInteger);
            }
            if (const std::optional<std::uint64_t> unsignedInteger = value.getUInt())
            {
                return static_cast<double>(*unsignedInteger);
            }
            return 0.0;
        }

        /**
         * @brief 数值相等判定
         * @details RFC 6902 §4.6：数值只要「数值上相等」即相等。整数族之间按整数精确比较，
         *          整数与浮点混比时退化为 double 比较（>2^53 时有固有精度损失，已在类注释中说明）；
         *          含 NaN 恒不相等。
         * @param left 左值（必须是数值）
         * @param right 右值（必须是数值）
         * @return bool 数值相等时返回 true
         */
        bool numbersEqual(const FormatValue &left, const FormatValue &right) noexcept
        {
            const std::optional<IntegralMagnitude> leftIntegral  = toIntegralMagnitude(left);
            const std::optional<IntegralMagnitude> rightIntegral = toIntegralMagnitude(right);

            if (leftIntegral.has_value() && rightIntegral.has_value())
            {
                return leftIntegral->isNegative == rightIntegral->isNegative &&
                       leftIntegral->magnitude == rightIntegral->magnitude;
            }

            const double leftValue  = numberAsDouble(left);
            const double rightValue = numberAsDouble(right);
            if (std::isnan(leftValue) || std::isnan(rightValue))
            {
                return false;
            }
            return leftValue == rightValue;
        }

        /**
         * @brief 补丁路径的拆分结果：父级指针 + 末段 token
         */
        struct PatchLocation
        {
            JsonPointer parent;    ///< 指向父容器的指针
            std::string lastToken; ///< 定位目标元素/成员的末段 token（已反转义）
        };

        /**
         * @brief 把指针拆成「父级指针 + 末段 token」
         * @details add/remove/replace 需要拿到父容器才能插入或删除，因此必须拆开末段。
         * @param pointer 已解析的指针
         * @return PatchLocation 拆分结果；空指针的调用方需自行处理根路径
         */
        PatchLocation splitPointer(const JsonPointer &pointer)
        {
            const std::vector<std::string> &tokens = pointer.tokens();
            std::vector<std::string>        parentTokens(tokens.begin(), tokens.end() - 1);
            return PatchLocation{.parent    = JsonPointer(std::move(parentTokens)),
                                 .lastToken = tokens.back()};
        }

        /**
         * @brief 取出操作对象中的字符串字段
         * @param operation 操作对象
         * @param memberName 字段名（op/path/from）
         * @param operationIndex 操作在补丁数组中的下标（从 0 起），用于报错定位
         * @return const std::string& 字段文本
         * @throws FormatError 字段缺失或不是字符串
         */
        const std::string &requireStringMember(const FormatValue &operation, const std::string_view memberName, const std::size_t operationIndex)
        {
            const FormatValue *member = operation.find(memberName);
            if (member == nullptr)
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  std::format("补丁第 {} 项缺少 '{}' 字段", operationIndex, memberName),
                                  TextPosition{});
            }

            const std::string *text = member->getStringView();
            if (text == nullptr)
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  std::format("补丁第 {} 项的 '{}' 字段必须是字符串", operationIndex, memberName),
                                  TextPosition{});
            }
            return *text;
        }

        /**
         * @brief 取出 add/replace/test 必需的 value 字段
         * @param operation 操作对象
         * @param operationIndex 操作在补丁数组中的下标
         * @return const FormatValue& value 字段的值（任意 JSON 类型）
         * @throws FormatError 字段缺失
         */
        const FormatValue &requireValueMember(const FormatValue &operation, const std::size_t operationIndex)
        {
            const FormatValue *value = operation.find("value");
            if (value == nullptr)
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  std::format("补丁第 {} 项缺少 'value' 字段", operationIndex),
                                  TextPosition{});
            }
            return *value;
        }

        /**
         * @brief 定位父容器并确认其可承载成员或元素
         * @param document 目标文档
         * @param location 拆分后的路径
         * @param operationName 操作名，用于组织中文错误文本
         * @return FormatValue& 父容器引用
         * @throws FormatError 父级不存在或父级不是对象/数组
         */
        FormatValue &requireParentContainer(FormatValue &document, const PatchLocation &location, const std::string_view operationName)
        {
            FormatValue *parent = location.parent.evaluateForWrite(document);
            if (parent == nullptr)
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("{} 操作的父级路径不存在：{}", operationName, location.parent.toString()),
                                  TextPosition{});
            }
            if (!parent->isObject() && !parent->isArray())
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("{} 操作的父级必须是对象或数组：{}", operationName, location.parent.toString()),
                                  TextPosition{});
            }
            return *parent;
        }

        /**
         * @brief 从数组 token 解析下标
         * @details 需要「具体下标」的操作（remove/replace）与 add 共用本函数，
         *          与 JsonPointer 的数组下标校验保持一致：前导零、`-`、非数字都非法。
         * @param location 拆分后的路径
         * @param operationName 操作名，用于组织错误文本
         * @return std::size_t 解析得到的下标（不校验是否越界，由调用方按操作语义判定）
         * @throws FormatError token 为空、`-`、非数字、前导零或超出 size_t 范围
         */
        std::size_t requireArrayIndex(const PatchLocation &location, const std::string_view operationName)
        {
            const std::string &token       = location.lastToken;
            const bool         leadingZero = token.size() > 1 && token.front() == '0';

            std::size_t index = 0;
            const auto [pointer, errorCode] = std::from_chars(token.data(), token.data() + token.size(), index);

            if (token.empty() || token == "-" || leadingZero ||
                errorCode != std::errc() || pointer != token.data() + token.size())
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  std::format("{} 操作的数组下标非法：{}", operationName, token),
                                  TextPosition{});
            }

            return index;
        }

        /**
         * @brief 应用 add 操作（RFC 6902 §4.1）
         * @details 对象成员为「新增或替换」；数组下标必须满足 `0 <= index <= size`，
         *          等于 size 等价追加，`-` 表示追加到末尾；根路径表示整体替换文档。
         * @param document 目标文档（就地修改）
         * @param path 目标路径
         * @param value 待写入的值
         * @throws FormatError 父级不存在/类型不符、下标非法或大于元素个数
         */
        void applyAdd(FormatValue &document, const JsonPointer &path, FormatValue value)
        {
            // RFC 6902 §4.1：目标为文档根时，等价于用 value 替换整份文档
            if (path.empty())
            {
                document = std::move(value);
                return;
            }

            const PatchLocation location = splitPointer(path);
            FormatValue        &parent   = requireParentContainer(document, location, "add");

            if (parent.isObject())
            {
                // RFC 6902 §4.1：成员不存在则新增、已存在则替换，两者是同一操作
                parent.set(location.lastToken, std::move(value));
                return;
            }

            const FormatValueArray &elements = parent.asArray();
            // RFC 6902 §4.1：数组 token '-' 表示「末尾指示」，直接在尾部追加
            if (location.lastToken == "-")
            {
                parent.pushBack(std::move(value));
                return;
            }

            const std::size_t index = requireArrayIndex(location, "add");
            // RFC 6902 §4.1：指定的下标不得大于数组元素个数；等于 size 即追加
            if (index > elements.size())
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("add 操作的数组下标 {} 超出元素个数 {}", index, elements.size()),
                                  TextPosition{});
            }
            static_cast<void>(parent.insert(index, std::move(value)));
        }

        /**
         * @brief 应用 remove 操作（RFC 6902 §4.2）
         * @details 目标必须存在；数组下标必须严格小于元素个数；根路径不允许删除。
         * @param document 目标文档（就地修改）
         * @param path 目标路径
         * @throws FormatError 目标不存在、父级非法、试图删除文档根
         */
        void applyRemove(FormatValue &document, const JsonPointer &path)
        {
            // 删除根会让文档失去根值，RFC 6902 未定义该情形；显式报错而不是静默置空
            if (path.empty())
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  "remove 操作不能删除文档根（如需清空请用 replace 指定 null）",
                                  TextPosition{});
            }

            const PatchLocation location = splitPointer(path);
            FormatValue        &parent   = requireParentContainer(document, location, "remove");

            if (parent.isObject())
            {
                if (!parent.erase(location.lastToken))
                {
                    throw FormatError(FormatErrorKind::PatchTargetMissing,
                                      std::format("remove 操作的目标不存在：{}", path.toString()),
                                      TextPosition{});
                }
                return;
            }

            const std::size_t index = requireArrayIndex(location, "remove");
            // RFC 6902 §4.2：目标必须存在，因此下标必须严格小于元素个数
            if (index >= parent.size())
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("remove 操作的数组下标 {} 越界（元素个数 {}）", index, parent.size()),
                                  TextPosition{});
            }
            static_cast<void>(parent.erase(index));
        }

        /**
         * @brief 应用 replace 操作（RFC 6902 §4.3）
         * @details 目标必须存在，语义等价于「先 remove 再 add 同一位置」；根路径替换整份文档。
         * @param document 目标文档（就地修改）
         * @param path 目标路径
         * @param value 替换后的值
         * @throws FormatError 目标不存在、父级非法、下标非法或越界
         */
        void applyReplace(FormatValue &document, const JsonPointer &path, FormatValue value)
        {
            // RFC 6902 §4.3：目标为文档根时整体替换
            if (path.empty())
            {
                document = std::move(value);
                return;
            }

            const PatchLocation location = splitPointer(path);
            FormatValue        &parent   = requireParentContainer(document, location, "replace");

            if (parent.isObject())
            {
                if (parent.find(location.lastToken) == nullptr)
                {
                    throw FormatError(FormatErrorKind::PatchTargetMissing,
                                      std::format("replace 操作的目标不存在：{}", path.toString()),
                                      TextPosition{});
                }
                parent.set(location.lastToken, std::move(value));
                return;
            }

            const std::size_t index = requireArrayIndex(location, "replace");
            if (index >= parent.size())
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("replace 操作的数组下标 {} 越界（元素个数 {}）", index, parent.size()),
                                  TextPosition{});
            }
            parent[index] = std::move(value);
        }

        /**
         * @brief 应用 test 操作（RFC 6902 §4.6）
         * @param document 目标文档（只读）
         * @param path 目标路径
         * @param expected 期望值
         * @throws FormatError 目标不存在或比较不相等
         */
        void applyTest(const FormatValue &document, const JsonPointer &path, const FormatValue &expected)
        {
            const FormatValue *target = path.evaluate(document);
            if (target == nullptr)
            {
                throw FormatError(FormatErrorKind::PatchTargetMissing,
                                  std::format("test 操作的目标不存在：{}", path.toString()),
                                  TextPosition{});
            }
            if (!JsonPatch::valuesEqual(*target, expected))
            {
                throw FormatError(FormatErrorKind::PatchTestFailed,
                                  std::format("test 操作失败：{} 处的值与给定值不相等", path.toString()),
                                  TextPosition{});
            }
        }

        /**
         * @brief 应用单条补丁操作
         * @details 负责操作对象的结构校验（op/path/from/value 的存在性与类型）与分发。
         * @param document 目标文档（就地修改）
         * @param operation 操作对象
         * @param operationIndex 操作下标（从 0 起），用于报错定位
         * @throws FormatError 结构非法、操作名未知或操作语义失败
         */
        void applyOperation(FormatValue &document, const FormatValue &operation, const std::size_t operationIndex)
        {
            // RFC 6902 §3：补丁数组的每一项都必须是操作对象
            if (!operation.isObject())
            {
                throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                  std::format("补丁第 {} 项必须是对象", operationIndex),
                                  TextPosition{});
            }

            const std::string &operationName = requireStringMember(operation, "op", operationIndex);
            const std::string &pathText      = requireStringMember(operation, "path", operationIndex);
            // path 与 from 都必须是合法 JSON Pointer，解析失败即抛错
            const JsonPointer  path          = JsonPointer::parse(pathText);

            if (operationName == "add")
            {
                applyAdd(document, path, requireValueMember(operation, operationIndex));
                return;
            }
            if (operationName == "remove")
            {
                applyRemove(document, path);
                return;
            }
            if (operationName == "replace")
            {
                applyReplace(document, path, requireValueMember(operation, operationIndex));
                return;
            }
            if (operationName == "test")
            {
                applyTest(document, path, requireValueMember(operation, operationIndex));
                return;
            }
            if (operationName == "move" || operationName == "copy")
            {
                const std::string &fromText = requireStringMember(operation, "from", operationIndex);
                const JsonPointer  from     = JsonPointer::parse(fromText);

                // RFC 6902 §4.4：from 不得是 path 的真前缀（即不得把节点移入自己的后代）
                if (operationName == "move" && from.isProperPrefixOf(path))
                {
                    throw FormatError(FormatErrorKind::InvalidPatchOperation,
                                      std::format("move 操作的 from（{}）不能是 path（{}）的祖先", fromText, pathText),
                                      TextPosition{});
                }

                const FormatValue *source = from.evaluate(document);
                if (source == nullptr)
                {
                    throw FormatError(FormatErrorKind::PatchTargetMissing,
                                      std::format("{} 操作的 from 不存在：{}", operationName, fromText),
                                      TextPosition{});
                }

                // 先复制源值再落地：copy 与 move 在「值被取走后源位置消失」这一点上语义不同
                FormatValue carried = *source;
                if (operationName == "move")
                {
                    // RFC 6902 §4.4：等价于先在 from 上 remove，再在 path 上 add 刚才取出的值
                    applyRemove(document, from);
                }
                applyAdd(document, path, std::move(carried));
                return;
            }

            throw FormatError(FormatErrorKind::InvalidPatchOperation,
                              std::format("补丁第 {} 项出现未知操作：{}", operationIndex, operationName),
                              TextPosition{});
        }
    } // namespace

    FormatValue JsonPatch::apply(const FormatValue &document, const FormatValue &patch)
    {
        // RFC 6902 §3：补丁文档必须是一个数组
        if (!patch.isArray())
        {
            throw FormatError(FormatErrorKind::InvalidPatchOperation,
                              "JSON Patch 必须是操作对象组成的数组",
                              TextPosition{});
        }

        // 原子性：全部操作落在文档副本上，只有全部成功才返回；
        // 任一步抛异常时入参 document 与 patch 都不会被改动（RFC 6902 §5）
        FormatValue                   working    = document;
        const FormatValueArray       &operations = patch.asArray();
        for (std::size_t index = 0; index < operations.size(); ++index)
        {
            applyOperation(working, operations[index], index);
        }

        return working;
    }

    void JsonPatch::applyInPlace(FormatValue &document, const FormatValue &patch)
    {
        // 先算后赋值：赋值语句只有在 apply() 成功返回后才执行，因此失败时 document 保持原样
        document = apply(document, patch);
    }

    bool JsonPatch::valuesEqual(const FormatValue &left, const FormatValue &right) noexcept
    {
        // RFC 6902 §4.6：数值按数值比较，因此 Int/UInt/Double 三者的相互比较先行处理
        const bool isLeftNumber  = left.isNumber();
        const bool isRightNumber = right.isNumber();
        if (isLeftNumber || isRightNumber)
        {
            // 数值与非数值（如 1 与 "1"）永不相等
            if (!isLeftNumber || !isRightNumber)
            {
                return false;
            }
            return numbersEqual(left, right);
        }

        // 非数值类型先比类型再比内容
        if (left.type() != right.type())
        {
            return false;
        }

        switch (left.type())
        {
            case FormatValueType::Null:
                return true;
            case FormatValueType::Bool:
                return left.asBool() == right.asBool();
            case FormatValueType::String:
                return left.asString() == right.asString();
            case FormatValueType::Array:
            {
                const FormatValueArray &leftElements  = left.asArray();
                const FormatValueArray &rightElements = right.asArray();
                if (leftElements.size() != rightElements.size())
                {
                    return false;
                }
                for (std::size_t index = 0; index < leftElements.size(); ++index)
                {
                    // 元素内部可能含数值，必须递归走本函数而不是 FormatValue::operator==
                    if (!valuesEqual(leftElements[index], rightElements[index]))
                    {
                        return false;
                    }
                }
                return true;
            }
            case FormatValueType::Object:
            {
                const FormatValueObject &leftMembers  = left.asObject();
                const FormatValueObject &rightMembers = right.asObject();
                if (leftMembers.size() != rightMembers.size())
                {
                    return false;
                }

                // 两侧都是按键升序的 std::map，可同步步进比较键与值
                auto leftIterator  = leftMembers.begin();
                auto rightIterator = rightMembers.begin();
                for (; leftIterator != leftMembers.end(); ++leftIterator, ++rightIterator)
                {
                    if (leftIterator->first != rightIterator->first ||
                        !valuesEqual(leftIterator->second, rightIterator->second))
                    {
                        return false;
                    }
                }
                return true;
            }
            default:
                // Int/UInt/Double 已在上面的数值分支返回
                return false;
        }
    }
} // namespace AsynGyanis::Base
