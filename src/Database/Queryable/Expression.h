/**
 * @file Expression.h
 * @brief 类型安全的表达式构建器 —— 提供流畅的链式语法构建 WhereCondition
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 用运算符重载与成员函数构建 WhereCondition：比较运算符（==、!=、<、<=、>、>=）、
 *          逻辑组合（&&、||、!）、LIKE / IN 与 IS NULL / IS NOT NULL；与 nullptr 比较会自动
 *          转为 IS NULL / IS NOT NULL 语义。
 */
#pragma once

#include "Database/Common/BinaryBytes.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/QueryNode.h"

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

namespace AsynGyanis::Database::Queryable
{

    // ========================================================================
    // 内部辅助：C++ 值 → ParameterValue 转换
    // ========================================================================

    namespace Detail
    {

        // nullptr → nullptr_t
        inline ParameterValue toParameterValue(std::nullptr_t) noexcept
        {
            return nullptr;
        }

        // bool → bool
        inline ParameterValue toParameterValue(bool value) noexcept
        {
            return value;
        }

        // int64_t 直接存储
        inline ParameterValue toParameterValue(int64_t value) noexcept
        {
            return value;
        }

        // uint64_t 直接存储
        inline ParameterValue toParameterValue(uint64_t value) noexcept
        {
            return value;
        }

        // double 直接存储
        inline ParameterValue toParameterValue(double value) noexcept
        {
            return value;
        }

        // std::string 移动存储
        inline ParameterValue toParameterValue(std::string value) noexcept
        {
            return std::move(value);
        }

        // const char* → std::string
        inline ParameterValue toParameterValue(const char *value)
        {
            return std::string(value);
        }

        // std::string_view → std::string
        inline ParameterValue toParameterValue(const std::string_view value)
        {
            return std::string(value);
        }

        // BinaryBytes → 二进制备选（规范拼法，直接移动）
        inline ParameterValue toParameterValue(BinaryBytes value) noexcept
        {
            return std::move(value);
        }

        // std::vector<std::byte> → 二进制备选（等价拼法，经共享转换规范化成 BinaryBytes）
        inline ParameterValue toParameterValue(const std::vector<std::byte> &value)
        {
            return AsynGyanis::Database::Detail::toBinaryBytes<std::vector<std::byte> >(value);
        }

        /**
         * @brief 整数类型（非 bool、非 int64_t/uint64_t）→ int64_t 或 uint64_t
         *
         * @details 使用 if constexpr 在单模板中处理有符号与无符号，
         *          避免 MSVC 上两个模板默认参数 SFINAE 的歧义问题。
         */
        template<typename T>
        auto toParameterValue(T value) noexcept -> std::enable_if_t<std::is_integral_v<T> &&
                                                                    !std::is_same_v<T, bool> &&
                                                                    !std::is_same_v<T, int64_t> &&
                                                                    !std::is_same_v<T, uint64_t>, ParameterValue>
        {
            if constexpr (std::is_signed_v<T>)
            {
                return static_cast<int64_t>(value);
            } else
            {
                return static_cast<uint64_t>(value);
            }
        }

        /**
         * @brief 枚举 → int64_t（不使用 std::to_underlying，C++23 不可用）
         */
        template<typename T>
        auto toParameterValue(T value) noexcept -> std::enable_if_t<std::is_enum_v<T>, ParameterValue>
        {
            return static_cast<int64_t>(value);
        }

        /**
         * @brief 浮点数（非 double）→ double
         */
        template<typename T>
        auto toParameterValue(T value) noexcept -> std::enable_if_t<std::is_floating_point_v<T> && !std::is_same_v<T, double>, ParameterValue>
        {
            return static_cast<double>(value);
        }

    } // namespace Detail

    // ========================================================================
    // 内部辅助：构建 FieldReference 值对象
    // ========================================================================

    namespace Detail
    {
        /// 从 std::string_view 快速创建 FieldReference
        inline FieldReference makeFieldRef(const std::string_view name)
        {
            return FieldReference{.name = std::string(name)};
        }
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    /**
     * @brief 相等比较（column == value）
     *
     * @details 当 value 为 nullptr 时自动转为 IS NULL 语义。
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator==(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        if constexpr (std::is_same_v<ValueType, std::nullptr_t>)
        {
            return WhereCondition{
                    .left = Detail::makeFieldRef(column.columnName),
                    .op = SqlOperator::IsNull,
                    .right = ParameterValue{nullptr}
            };
        } else
        {
            return WhereCondition{
                    .left = Detail::makeFieldRef(column.columnName),
                    .op = SqlOperator::Eq,
                    .right = Detail::toParameterValue(value)
            };
        }
    }

    /**
     * @brief 字符串字面量的相等比较
     */
    template<typename T>
    WhereCondition operator==(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Eq,
                .right = ParameterValue{std::string(value)}
        };
    }

    /**
     * @brief 不等比较（column != value）
     *
     * @details 当 value 为 nullptr 时自动转为 IS NOT NULL 语义。
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator!=(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        if constexpr (std::is_same_v<ValueType, std::nullptr_t>)
        {
            return WhereCondition{
                    .left = Detail::makeFieldRef(column.columnName),
                    .op = SqlOperator::IsNotNull,
                    .right = ParameterValue{nullptr}
            };
        } else
        {
            return WhereCondition{
                    .left = Detail::makeFieldRef(column.columnName),
                    .op = SqlOperator::Neq,
                    .right = Detail::toParameterValue(value)
            };
        }
    }

    /**
     * @brief 字符串字面量的不等比较
     */
    template<typename T>
    WhereCondition operator!=(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Neq,
                .right = ParameterValue{std::string(value)}
        };
    }

    /**
     * @brief 小于比较（column < value）
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator<(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Lt,
                .right = Detail::toParameterValue(value)
        };
    }

    /**
     * @brief 字符串字面量的小于比较
     */
    template<typename T>
    WhereCondition operator<(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Lt,
                .right = ParameterValue{std::string(value)}
        };
    }

    /**
     * @brief 小于等于比较（column <= value）
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator<=(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Le,
                .right = Detail::toParameterValue(value)
        };
    }

    /**
     * @brief 字符串字面量的小于等于比较
     */
    template<typename T>
    WhereCondition operator<=(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Le,
                .right = ParameterValue{std::string(value)}
        };
    }

    /**
     * @brief 大于比较（column > value）
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator>(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Gt,
                .right = Detail::toParameterValue(value)
        };
    }

    /**
     * @brief 字符串字面量的大于比较
     */
    template<typename T>
    WhereCondition operator>(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Gt,
                .right = ParameterValue{std::string(value)}
        };
    }

    /**
     * @brief 大于等于比较（column >= value）
     */
    template<typename T, typename MemberType, typename ValueType>
    WhereCondition operator>=(const ColumnDescriptor<T, MemberType> &column, const ValueType &value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Ge,
                .right = Detail::toParameterValue(value)
        };
    }

    /**
     * @brief 字符串字面量的大于等于比较
     */
    template<typename T>
    WhereCondition operator>=(const ColumnDescriptor<T, std::string> &column, const char *value)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Ge,
                .right = ParameterValue{std::string(value)}
        };
    }

    // ========================================================================
    // 逻辑组合运算符
    // ========================================================================

    /**
     * @brief 逻辑与组合（&& → AND），右值版本
     *
     * @details 将两个条件用 AND 组合成复合条件节点。
     *          子条件存储在 children 中，toSql() 递归展开。
     */
    inline WhereCondition operator&&(WhereCondition &&leftCondition, WhereCondition &&rightCondition)
    {
        WhereCondition result;
        result.op    = SqlOperator::And;
        result.right = ParameterValue{nullptr};
        result.children.reserve(2);
        result.children.push_back(std::move(leftCondition));
        result.children.push_back(std::move(rightCondition));
        return result;
    }

    /**
     * @brief 逻辑与组合（&& → AND），左值版本（拷贝子条件）
     */
    inline WhereCondition operator&&(const WhereCondition &leftCondition, const WhereCondition &rightCondition)
    {
        WhereCondition result;
        result.op    = SqlOperator::And;
        result.right = ParameterValue{nullptr};
        result.children.reserve(2);
        result.children.push_back(leftCondition);
        result.children.push_back(rightCondition);
        return result;
    }

    /**
     * @brief 逻辑或组合（|| → OR），右值版本
     */
    inline WhereCondition operator||(WhereCondition &&leftCondition, WhereCondition &&rightCondition)
    {
        WhereCondition result;
        result.op    = SqlOperator::Or;
        result.right = ParameterValue{nullptr};
        result.children.reserve(2);
        result.children.push_back(std::move(leftCondition));
        result.children.push_back(std::move(rightCondition));
        return result;
    }

    /**
     * @brief 逻辑或组合（|| → OR），左值版本（拷贝子条件）
     */
    inline WhereCondition operator||(const WhereCondition &leftCondition, const WhereCondition &rightCondition)
    {
        WhereCondition result;
        result.op    = SqlOperator::Or;
        result.right = ParameterValue{nullptr};
        result.children.reserve(2);
        result.children.push_back(leftCondition);
        result.children.push_back(rightCondition);
        return result;
    }

    /**
     * @brief 逻辑非（! → NOT），右值版本
     */
    inline WhereCondition operator!(WhereCondition &&condition)
    {
        WhereCondition result;
        result.op    = SqlOperator::Not;
        result.right = ParameterValue{nullptr};
        result.children.reserve(1);
        result.children.push_back(std::move(condition));
        return result;
    }

    /**
     * @brief 逻辑非（! → NOT），左值版本（拷贝子条件）
     */
    inline WhereCondition operator!(const WhereCondition &condition)
    {
        WhereCondition result;
        result.op    = SqlOperator::Not;
        result.right = ParameterValue{nullptr};
        result.children.reserve(1);
        result.children.push_back(condition);
        return result;
    }

    // ========================================================================
    // LIKE / IN 表达式
    // ========================================================================

    /**
     * @brief LIKE 模式匹配
     *
     * @tparam T          结构体类型
     * @tparam MemberType 成员类型
     * @param column  字段描述符
     * @param pattern LIKE 模式字符串，如 "%张%"
     * @return WhereCondition 表示 column LIKE pattern 的条件
     */
    template<typename T, typename MemberType>
    WhereCondition like(const ColumnDescriptor<T, MemberType> &column, const std::string &pattern)
    {
        return WhereCondition{
                .left = Detail::makeFieldRef(column.columnName),
                .op = SqlOperator::Like,
                .right = ParameterValue{pattern}
        };
    }

    /**
     * @brief IN 集合判断
     *
     * @tparam T          结构体类型
     * @tparam MemberType 成员类型
     * @tparam Container  容器类型，需支持迭代和 value_type
     * @param column  字段描述符
     * @param values  值集合
     * @return WhereCondition 表示 column IN (values) 的条件
     */
    template<typename T, typename MemberType, typename Container>
    WhereCondition in(const ColumnDescriptor<T, MemberType> &column, const Container &values)
    {
        std::vector<ParameterValue> convertedValues;
        convertedValues.reserve(std::size(values));

        for (const auto &value: values)
        {
            convertedValues.push_back(Detail::toParameterValue(value));
        }

        WhereCondition result;
        result.left     = Detail::makeFieldRef(column.columnName);
        result.op       = SqlOperator::In;
        result.right    = ParameterValue{static_cast<int64_t>(0)};
        result.inValues = std::move(convertedValues);
        return result;
    }

    // ========================================================================
    // OrderByClause 构建辅助
    // ========================================================================

    /**
     * @brief 创建升序排序子句
     *
     * @param columnName 列名
     * @return OrderByClause 升序排序子句
     */
    inline OrderByClause asc(const std::string_view columnName)
    {
        return OrderByClause{
                .field = FieldReference{.name = std::string(columnName)},
                .descending = false
        };
    }

    /**
     * @brief 创建降序排序子句
     *
     * @param columnName 列名
     * @return OrderByClause 降序排序子句
     */
    inline OrderByClause desc(const std::string_view columnName)
    {
        return OrderByClause{
                .field = FieldReference{.name = std::string(columnName)},
                .descending = true
        };
    }

} // namespace AsynGyanis::Database::Queryable
