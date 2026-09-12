/**
 * @file RowMapper.h
 * @brief ORM 行映射 —— 编译期在 DatabaseResult 的一行与结构体 T 之间双向转换
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件承担 ORM 的「值映射」职责，两个方向各一组接口：
 *
 * ## 结果集 → 结构体（读方向）
 * mapResultRow<T>() 把结果集当前行映射成一个 T，mapResultRows<T>() 遍历全部行。
 * 列的对应关系来自 TableSchema<T>::kColumns：按 tuple 顺序取出每列的 columnName，
 * 在结果集里按**列名**查找下标再读值，因此结果集的列顺序与结构体声明顺序无关。
 * 成员类型到 DatabaseValue 备选的转换规则：
 * - 整型（含 char/short/int/long long）← std::int64_t，越界即报错而不是截断；
 * - bool ← bool 或 std::int64_t（SQLite 没有布尔存储类，0/1 的整数收窄成 bool）；
 * - 浮点 ← double 或 std::int64_t（只读语句可能把整数值回传成 INTEGER）；
 * - std::string ← std::string；
 * - std::optional<U> ← NULL（monostate）映射成空 optional，其余情况递归按 U 转换。
 * 类型不符、列缺失、NULL 落到非 optional 成员，都会抛出带中文说明的 std::runtime_error，
 * 而不是给出一个字段静默为 0 的半成品对象。
 *
 * ## 结构体 → 参数（写方向）
 * toDatabaseValue() 把单个成员值转成数据库统一值，供 INSERT / UPDATE 的绑定参数使用。
 * 规则与读方向对称：整型统一按 int64_t 绑定，无符号整型超出 int64_t 时降级为十进制文本
 * （DatabaseValue 已冻结、没有无符号备选，详见实现处注释），optional 空值绑定为 SQL NULL。
 *
 * ## 编译期约束
 * RowMappable<T> 要求 T 是可聚合初始化（默认构造）且已特化 TableSchema<T>（kColumns 非空）
 * 的结构体；成员类型不受支持时由函数体内的 static_assert 给出中文编译错误。
 */
#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/TableSchema.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Database::Queryable
{
    // ========================================================================
    // 编译期约束
    // ========================================================================

    namespace Detail
    {
        /**
         * @brief 判定类型是否为 std::optional
         * @tparam T 待判定类型
         */
        template<typename T>
        struct IsOptional : std::false_type
        {
        };

        /**
         * @brief std::optional 的特化
         * @tparam InnerType optional 包装的元素类型
         */
        template<typename InnerType>
        struct IsOptional<std::optional<InnerType>> : std::true_type
        {
        };

        /**
         * @brief 判定单个列类型是否被行映射支持
         * @details 支持整型、bool、浮点、std::string，以及它们的 std::optional 包装（递归判定）。
         * @tparam MemberType 结构体成员类型
         */
        template<typename MemberType>
        struct IsSupportedColumnType
            : std::bool_constant<std::is_integral_v<MemberType> ||
                                 std::is_floating_point_v<MemberType> ||
                                 std::is_same_v<MemberType, std::string>>
        {
        };

        /**
         * @brief std::optional 的特化：支持与否取决于内部类型
         * @tparam InnerType optional 包装的元素类型
         */
        template<typename InnerType>
        struct IsSupportedColumnType<std::optional<InnerType>> : IsSupportedColumnType<InnerType>
        {
        };

        /**
         * @brief 编译期永不成立的谓词，用于让 static_assert 在模板实例化后才求值
         * @tparam T 任意类型
         */
        template<typename T>
        inline constexpr bool kAlwaysFalse = false;

        /**
         * @brief 判定 TableSchema<T> 声明的全部列类型是否都被支持
         * @tparam T 结构体类型
         * @return true 所有列类型都受支持
         */
        template<typename T>
        [[nodiscard]] constexpr bool allColumnTypesSupported() noexcept
        {
            using ColumnTuple = std::remove_cvref_t<decltype(TableSchema<T>::kColumns)>;

            // 用下标序列展开 tuple，对每个 ColumnDescriptor 的 MemberType 做静态判定
            return []<std::size_t... Indices>(std::index_sequence<Indices...>)
            {
                return (IsSupportedColumnType<typename std::tuple_element_t<Indices, ColumnTuple>::MemberType>::value && ...);
            }(std::make_index_sequence<std::tuple_size_v<ColumnTuple>>{});
        }

        /**
         * @brief 抛出带中文说明的列类型错误
         * @param columnName 出错的列名
         * @param expectedTypeName 期望的 C++ 类型说明
         * @param cellValue 结果集实际给出的值
         */
        [[noreturn]] inline void throwColumnTypeError(const std::string_view columnName,
                                                     const std::string_view expectedTypeName,
                                                     const DatabaseValue &cellValue)
        {
            throw std::runtime_error("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 期望 " +
                                     std::string(expectedTypeName) + "，实际为 " +
                                     databaseValueTypeName(cellValue) +
                                     "。若该列可能为 NULL，请把成员声明为 std::optional");
        }

        /**
         * @brief 把一个单元格的值转换成目标成员类型
         * @tparam MemberType 目标成员类型（可为 std::optional 包装）
         * @param cellValue 结果集当前行的单元格值
         * @param columnName 列名，仅用于错误信息
         * @return MemberType 转换后的值
         * @throws std::runtime_error 类型不匹配、整型越界或 NULL 落到非 optional 成员
         */
        template<typename MemberType>
        [[nodiscard]] MemberType convertDatabaseValue(const DatabaseValue &cellValue, const std::string_view columnName)
        {
            using BareType = std::remove_cv_t<MemberType>;

            if constexpr (IsOptional<BareType>::value)
            {
                // SQL NULL 映射成空 optional；非 NULL 时递归按内部类型转换
                if (std::holds_alternative<std::monostate>(cellValue))
                {
                    return std::nullopt;
                }
                return convertDatabaseValue<typename BareType::value_type>(cellValue, columnName);
            }
            else if constexpr (std::is_same_v<BareType, bool>)
            {
                if (const auto *booleanValue = std::get_if<bool>(&cellValue))
                {
                    return *booleanValue;
                }
                // SQLite 没有布尔存储类，INTEGER 的 0/1 需要收窄成 bool
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    return *integerValue != 0;
                }
                throwColumnTypeError(columnName, "bool", cellValue);
            }
            else if constexpr (std::is_integral_v<BareType>)
            {
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    // 窄化必须校验范围：把 300 塞进 std::uint8_t 会静默变成 44，
                    // 这类错误在业务层极难定位，宁可在映射处直接失败
                    if (!std::in_range<BareType>(*integerValue))
                    {
                        throw std::runtime_error("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 的值 " +
                                                 std::to_string(*integerValue) + " 超出目标整型的取值范围");
                    }
                    return static_cast<BareType>(*integerValue);
                }
                // 浮点给出整型列（如 MySQL 的 DECIMAL）不在当前支持范围，明确报错而不是取整
                throwColumnTypeError(columnName, "整型（Int64）", cellValue);
            }
            else if constexpr (std::is_floating_point_v<BareType>)
            {
                if (const auto *realValue = std::get_if<double>(&cellValue))
                {
                    return static_cast<BareType>(*realValue);
                }
                // 整数值的 REAL 列在 SQLite 里可能回传 INTEGER，按数值语义接受
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    return static_cast<BareType>(*integerValue);
                }
                throwColumnTypeError(columnName, "浮点（Double）", cellValue);
            }
            else if constexpr (std::is_same_v<BareType, std::string>)
            {
                if (const auto *textValue = std::get_if<std::string>(&cellValue))
                {
                    return *textValue;
                }
                throwColumnTypeError(columnName, "std::string", cellValue);
            }
            else
            {
                // 不受支持的成员类型已被 allColumnTypesSupported() 的 static_assert 拦住，
                // 这里只是让 if constexpr 的所有分支都有返回值
                static_assert(kAlwaysFalse<MemberType>,
                              "RowMapper：不支持的成员类型。仅支持整型、bool、浮点、std::string，"
                              "以及它们的 std::optional 包装");
                return MemberType{};
            }
        }

    } // namespace Detail

    /**
     * @brief 约束：T 可以被行映射
     *
     * @details 三个条件缺一不可：
     *          - T 是聚合体且可默认构造（映射时先构造空对象再逐列赋值）；
     *          - TableSchema<T> 已特化且 kColumns 非空（未特化时主模板是空 tuple）。
     *          不满足时 mapResultRow() 不会被选中，编译器会直接报「约束不满足」。
     *
     * @tparam T 表数据结构类型
     */
    template<typename T>
    concept RowMappable = std::is_aggregate_v<T> &&
                          std::is_default_constructible_v<T> &&
                          (std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<T>::kColumns)>> > 0);

    // ========================================================================
    // 结果集 → 结构体
    // ========================================================================

    namespace Detail
    {
        /**
         * @brief 把当前行的一列赋值给结构体的对应成员
         * @tparam T 结构体类型
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param mappedRow 目标结构体，成员的赋值目标
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param result 结果集，游标须已停在有效行上
         * @throws std::runtime_error 列不存在或类型不匹配
         */
        template<typename T, typename ColumnDescriptorType>
        void assignColumn(T &mappedRow, const ColumnDescriptorType &columnDescriptor, const DatabaseResult &result)
        {
            // 按列名解析下标：结果集的列顺序由 SELECT 列表决定，与结构体声明顺序无关，
            // 因此不能按下标硬编码，否则一旦查询换列就会整体错位
            const std::optional<std::size_t> columnIndex = result.columnIndex(columnDescriptor.columnName);
            if (!columnIndex.has_value())
            {
                throw std::runtime_error("ORM 行映射失败：结果集中不存在列 \"" +
                                         std::string(columnDescriptor.columnName) + "\"（表 " +
                                         std::string(TableSchema<T>::kTableName) + "）");
            }

            const DatabaseValue cellValue = result.getValue(columnIndex.value());

            using MemberType = typename ColumnDescriptorType::MemberType;
            mappedRow.*(columnDescriptor.memberPointer) =
                convertDatabaseValue<MemberType>(cellValue, columnDescriptor.columnName);
        }

    } // namespace Detail

    /**
     * @brief 把结果集的当前行映射成结构体
     *
     * @details 调用方需保证游标已停在有效行上（即刚调用过 DatabaseResult::next() 且返回 true）。
     *          映射过程按 TableSchema<T>::kColumns 的顺序逐列取值并赋值。
     *
     * @tparam T 已特化 TableSchema 的聚合类型
     * @param result 结果集，只读访问
     * @return T 映射后的结构体
     * @throws std::runtime_error 列缺失、类型不匹配、整型越界或 NULL 落到非 optional 成员
     */
    template<RowMappable T>
    [[nodiscard]] T mapResultRow(const DatabaseResult &result)
    {
        // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯
        static_assert(Detail::allColumnTypesSupported<T>(),
                      "RowMapper：TableSchema<T>::kColumns 中存在不支持的列类型。"
                      "仅支持整型、bool、浮点、std::string，以及它们的 std::optional 包装");

        T mappedRow{};

        std::apply(
            [&mappedRow, &result](const auto &...columnDescriptors)
            {
                // 折叠表达式逐个赋值：逗号运算符保证从左到右按 kColumns 顺序执行
                (Detail::assignColumn<T>(mappedRow, columnDescriptors, result), ...);
            },
            TableSchema<T>::kColumns);

        return mappedRow;
    }

    /**
     * @brief 遍历结果集剩余的全部行并逐行映射
     *
     * @details 从当前游标位置开始推进（调用 next() 直到结束），因此调用方应传入尚未推进的
     *          结果集才能映射到全部行；已推进过的结果集只会映射剩余行。
     *
     * @tparam T 已特化 TableSchema 的聚合类型
     * @param result 结果集，会推进其游标
     * @return std::vector<T> 映射后的行列表，结果集为空时返回空向量
     * @throws std::runtime_error 任意一行映射失败
     */
    template<RowMappable T>
    [[nodiscard]] std::vector<T> mapResultRows(DatabaseResult &result)
    {
        std::vector<T> mappedRows;

        // rowCount() 只在驱动能预知总行数时才有意义（SQLite 对只读语句会预扫描），
        // 用它预留容量可以把逐行 push_back 的扩容次数降为 0；无法预知时返回 0，行为不变
        if (const std::size_t knownRowCount = result.rowCount(); knownRowCount > 0)
        {
            mappedRows.reserve(knownRowCount);
        }

        while (result.next())
        {
            mappedRows.push_back(mapResultRow<T>(result));
        }

        return mappedRows;
    }

    // ========================================================================
    // 结构体成员 → 绑定参数
    // ========================================================================

    namespace Detail
    {
        /**
         * @brief 把结构体成员值转换成数据库统一值
         * @tparam MemberType 成员类型（可为 std::optional 包装）
         * @param value 成员值
         * @return DatabaseValue 可直接作为绑定参数的统一值
         */
        template<typename MemberType>
        [[nodiscard]] DatabaseValue toDatabaseValue(const MemberType &value)
        {
            using BareType = std::remove_cvref_t<MemberType>;

            if constexpr (IsOptional<BareType>::value)
            {
                // 空 optional 绑定为 SQL NULL：与「空字符串」是不同的语义
                if (!value.has_value())
                {
                    return std::monostate{};
                }
                return toDatabaseValue(value.value());
            }
            else if constexpr (std::is_same_v<BareType, bool>)
            {
                // 用 in_place_type 显式指定备选：DatabaseValue 里 bool 可隐式转成 int64_t/double，
                // 交给 variant 的转换构造去选会把「意图」交给重载规则，写清楚更稳
                return DatabaseValue{std::in_place_type<bool>, value};
            }
            else if constexpr (std::is_integral_v<BareType>)
            {
                if constexpr (std::is_unsigned_v<BareType>)
                {
                    // DatabaseValue 没有无符号备选（该类型已冻结）：
                    // 放得进 int64_t 时按有符号整数绑定（保持整数比较与索引可用），
                    // 超出时降级为十进制文本，避免静默回绕成负数给出错误数值
                    if (static_cast<std::uint64_t>(value) >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return DatabaseValue{std::to_string(value)};
                    }
                }
                return DatabaseValue{static_cast<std::int64_t>(value)};
            }
            else if constexpr (std::is_floating_point_v<BareType>)
            {
                return DatabaseValue{static_cast<double>(value)};
            }
            else if constexpr (std::is_same_v<BareType, std::string>)
            {
                return DatabaseValue{std::in_place_type<std::string>, value};
            }
            else
            {
                static_assert(kAlwaysFalse<MemberType>,
                              "RowMapper：不支持的成员类型，无法转换为绑定参数。"
                              "仅支持整型、bool、浮点、std::string，以及它们的 std::optional 包装");
                return std::monostate{};
            }
        }

    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable
