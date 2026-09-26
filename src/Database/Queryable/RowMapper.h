/**
 * @file RowMapper.h
 * @brief ORM 行映射 —— 编译期在 DatabaseResult 的一行与结构体 T 之间双向转换
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件承担 ORM 的「值映射」职责：读方向按列名在结果集中定位列并严格转换（拒绝取整、截断），
 *          写方向把成员值规范化成 DatabaseValue；类型不符、列缺失、NULL 落到非 optional 成员一律抛
 *          带中文说明的 RowMappingException，而不是给出字段静默为 0 的半成品对象。
 */
#pragma once

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/RowMappingException.h"
#include "Database/Queryable/TableSchema.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
        struct IsOptional<std::optional<InnerType> > : std::true_type
        {
        };

        /**
         * @brief 判定单个列类型是否被行映射支持
         * @details 支持整型、bool、浮点、std::string、二进制载荷（std::vector<std::uint8_t>
         *          或 std::vector<std::byte>），以及它们的 std::optional 包装（递归判定）。
         *          二进制的两种等价拼法由共享 trait 判定，规则只有一份实现。
         * @tparam MemberType 结构体成员类型
         */
        template<typename MemberType>
        struct IsSupportedColumnType : std::bool_constant<std::is_integral_v<MemberType> ||
                                                          std::is_floating_point_v<MemberType> ||
                                                          std::is_same_v<MemberType, std::string> ||
                                                          AsynGyanis::Database::Detail::IsBinaryBytes<MemberType>::value>
        {
        };

        /**
         * @brief std::optional 的特化：支持与否取决于内部类型
         * @tparam InnerType optional 包装的元素类型
         */
        template<typename InnerType>
        struct IsSupportedColumnType<std::optional<InnerType> > : IsSupportedColumnType<InnerType>
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
            }(std::make_index_sequence<std::tuple_size_v<ColumnTuple> >{});
        }

        /**
         * @brief 抛出带中文说明的列类型错误
         * @param columnName 出错的列名
         * @param expectedTypeName 期望的 C++ 类型说明
         * @param cellValue 结果集实际给出的值
         */
        [[noreturn]] inline void throwColumnTypeError(const std::string_view columnName, const std::string_view expectedTypeName, const DatabaseValue &cellValue)
        {
            throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 期望 " +
                                      std::string(expectedTypeName) + "，实际为 " +
                                      databaseValueTypeName(cellValue) +
                                      "。若该列可能为 NULL，请把成员声明为 std::optional");
        }

        /**
         * @brief 目标浮点成员的类型名，只用于错误文本里指明「收窄进哪一个类型」
         * @tparam FloatingType 浮点成员类型
         * @return std::string_view 该浮点类型的可读名字
         */
        template<typename FloatingType>
        [[nodiscard]] constexpr std::string_view floatingTypeName() noexcept
        {
            if constexpr (std::is_same_v<FloatingType, float>)
            {
                return "float";
            }
            else if constexpr (std::is_same_v<FloatingType, double>)
            {
                return "double";
            }
            else
            {
                return "long double";
            }
        }

        /**
         * @brief 抛出带中文说明的整型→浮点取值错误（无损表示被破坏时使用）
         * @param columnName 出错的列名
         * @param expectedTypeName 目标浮点成员的类型名
         * @param reasonText 说明这一取值为何不能被无损收窄
         */
        [[noreturn]] inline void throwFloatNarrowingError(const std::string_view columnName, const std::string_view expectedTypeName,
                                                          const std::string &reasonText)
        {
            throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 不能映射为 " +
                                      std::string(expectedTypeName) + "：" + reasonText +
                                      "。收窄会静默改变数值，请先把成员声明成能容纳该取值的类型（整型或 double）");
        }

        /**
         * @brief 把一段十进制整型文本严格解析成目标整型
         *
         * @details 引擎存得下、却给不出 int64 的整数只能以文本返回（MySQL 的 BIGINT UNSIGNED
         *          上界 2^64-1），因此整型成员必须能读文本。解析必须**严格**：用 std::from_chars，
         *          不跳前导空白、不接受余文、按 C locale 解析且不抛异常（std::stoll 三者都会放宽）。
         *          解析宽度按目标类型的符号性选：无符号成员要吃下 int64 之外的上界。
         *
         * @tparam FundamentalType 目标整型（已剥掉 optional / cv 限定）
         * @param textValue 列值文本
         * @param columnName 列名，仅用于错误信息
         * @return FundamentalType 解析结果
         * @throws RowMappingException 文本不是纯十进制整数（含小数点、科学计数法、空白、多余字符，
         *         或无符号成员收到负号），或取值超出目标整型的范围
         * @warning SQLite 的列亲和性会把「装不下 int64 的十进制文本」转成 REAL，因此同一个 UInt64
         *          成员在 SQLite 上写进去、读回来会损失精度并因类型不符报错——这是引擎的存储能力边界
         *          （SQLite 只有 64 位有符号整数），需要精确承载 2^63 以上取值时应改用 MySQL 的
         *          BIGINT UNSIGNED（见 MySqlDialect::columnTypeName()）。
         */
        template<typename FundamentalType>
        [[nodiscard]] FundamentalType parseIntegerText(const std::string &textValue, const std::string_view columnName)
        {
            // 无符号目标按 uint64 解析，才能容纳 2^63 .. 2^64-1 这一段
            using ParseType = std::conditional_t<std::is_unsigned_v<FundamentalType>, std::uint64_t, std::int64_t>;

            ParseType                    parsedValue{};
            const char *const            textBegin   = textValue.data();
            const char *const            textEnd     = textBegin + textValue.size();
            const std::from_chars_result parseResult = std::from_chars(textBegin, textEnd, parsedValue);

            // out_of_range 单独给文案：此时文本本身是合法整数，只是超出目标位宽
            if (parseResult.ec == std::errc::result_out_of_range)
            {
                throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 的值 " + textValue +
                                          " 超出目标整型的取值范围");
            }

            // ptr != textEnd 表示尾部仍有余文（如 "12abc"）；无符号目标遇到负号也走这里
            if (parseResult.ec != std::errc{} || parseResult.ptr != textEnd)
            {
                throw RowMappingException(std::string("ORM 行映射失败：列 \"") + std::string(columnName) +
                                          "\" 的文本 \"" + textValue +
                                          (std::is_unsigned_v<FundamentalType>
                                               ? "\" 无法映射到无符号整型（只接受十进制数字，不接受负号、小数点或空格）"
                                               : "\" 无法映射到整型（只接受可选的负号与十进制数字）"));
            }

            // 「放得下才有意义」：无符号成员收到负号会在上一步被拒，这里再兜一次位宽
            if (!std::in_range<FundamentalType>(parsedValue))
            {
                throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 的值 " + textValue +
                                          " 超出目标整型的取值范围");
            }

            return static_cast<FundamentalType>(parsedValue);
        }

        /**
         * @brief 把一个单元格的值转换成目标成员类型
         * @details 整型接受 std::int64_t 或严格十进制文本（引擎存得下却给不出 int64 的整数只能以文本
         *          返回），越界即报错而不是取整、截断。bool 只认 0 与 1；浮点只接受能无损表示的取值
         *          （整数的连续精确区间是 ±2^digits，收窄到 float 时不得跨出其上下界）。
         * @tparam MemberType 目标成员类型（可为 std::optional 包装）
         * @param cellValue 结果集当前行的单元格值
         * @param columnName 列名，仅用于错误信息
         * @return MemberType 转换后的值
         * @throws RowMappingException 类型不匹配、整型或浮点收窄会改变数值、NULL 落到非 optional 成员
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
            } else if constexpr (std::is_same_v<BareType, bool>)
            {
                if (const auto *booleanValue = std::get_if<bool>(&cellValue))
                {
                    return *booleanValue;
                }
                // SQLite 没有布尔存储类，INTEGER 的 0/1 需要收窄成 bool
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    // 只认 0 与 1：把 7 收成 true 等于替调用方认定「非零即真」，而整数列里出现 2
                    // 通常意味着这一列压根不是布尔列（成员声明与列声明已经不符），必须在此暴露
                    if (*integerValue != 0 && *integerValue != 1)
                    {
                        throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 的整数值 " +
                                                  std::to_string(*integerValue) +
                                                  " 不是 0 或 1，无法映射为 bool。若该列确实存放多个取值，请把成员改成整型");
                    }
                    return *integerValue != 0;
                }
                throwColumnTypeError(columnName, "bool", cellValue);
            } else if constexpr (std::is_integral_v<BareType>)
            {
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    // 窄化必须校验范围：把 300 塞进 std::uint8_t 会静默变成 44，
                    // 这类错误在业务层极难定位，宁可在映射处直接失败
                    if (!std::in_range<BareType>(*integerValue))
                    {
                        throw RowMappingException("ORM 行映射失败：列 \"" + std::string(columnName) + "\" 的值 " +
                                                  std::to_string(*integerValue) + " 超出目标整型的取值范围");
                    }
                    return static_cast<BareType>(*integerValue);
                }

                // 十进制文本支路：引擎存得下、却给不出 int64 的整数只能以文本返回
                // （MySQL 的 BIGINT UNSIGNED 上界）。
                // 少了这一支，写到库里的 2^63 以上取值就再也读不回来
                if (const auto *textValue = std::get_if<std::string>(&cellValue))
                {
                    return parseIntegerText<BareType>(*textValue, columnName);
                }

                // 浮点给出整型列（如 MySQL 的 DECIMAL、或 SQLite 把超大整数降级成 REAL）
                // 不在当前支持范围，明确报错而不是取整：取整等于静默改变数值
                throwColumnTypeError(columnName,
                                     std::is_unsigned_v<BareType>
                                         ? "无符号整型（Int64 或十进制文本）"
                                         : "整型（Int64 或十进制文本）",
                                     cellValue);
            } else if constexpr (std::is_floating_point_v<BareType>)
            {
                if (const auto *realValue = std::get_if<double>(&cellValue))
                {
                    // 目标类型装不下时会静默变成无穷大——那是另一个数，不是「精度差一点」，因此拒绝；
                    // 原值本就是无穷大则逐值保真，予以接受。尾数精度损失属于「成员声明为 float」的既有取舍
                    if constexpr (std::numeric_limits<BareType>::max() < std::numeric_limits<double>::max())
                    {
                        if (std::isfinite(*realValue) && std::abs(*realValue) > std::numeric_limits<BareType>::max())
                        {
                            throwFloatNarrowingError(columnName, floatingTypeName<BareType>(),
                                                     "取值 " + std::to_string(*realValue) + " 超出其上下界");
                        }
                    }
                    return static_cast<BareType>(*realValue);
                }
                // 整数值的 REAL 列在 SQLite 里可能回传 INTEGER，按数值语义接受
                if (const auto *integerValue = std::get_if<std::int64_t>(&cellValue))
                {
                    // 只接受能逐位精确表示的整数：连续精确区间是 ±2^digits（float 为 2^24、double 为 2^53），
                    // 越界的整数转成浮点会取整成邻近的可表示值，即静默改值。
                    // 有效位数达到 int64 宽度的类型（x86 的 long double 是 64 位）整个区间都精确，
                    // 此时「1 左移 digits」本身就是非法移位——用 if constexpr 把那条分支整个丢掉，
                    // 未选中的分支不会被实例化，也就不会在编译期留下移位告警
                    constexpr std::int64_t exactIntegerLimit = []()
                    {
                        if constexpr (std::numeric_limits<BareType>::digits >= std::numeric_limits<std::int64_t>::digits)
                        {
                            return std::numeric_limits<std::int64_t>::max();
                        }
                        else
                        {
                            return std::int64_t{1} << std::numeric_limits<BareType>::digits;
                        }
                    }();
                    if (*integerValue > exactIntegerLimit || *integerValue < -exactIntegerLimit)
                    {
                        throwFloatNarrowingError(columnName, floatingTypeName<BareType>(),
                                                 "整数值 " + std::to_string(*integerValue) + " 超出其能精确表示的整数范围 ±2^" +
                                                 std::to_string(std::numeric_limits<BareType>::digits));
                    }
                    return static_cast<BareType>(*integerValue);
                }
                throwColumnTypeError(columnName, "浮点（Double）", cellValue);
            } else if constexpr (std::is_same_v<BareType, std::string>)
            {
                if (const auto *textValue = std::get_if<std::string>(&cellValue))
                {
                    return *textValue;
                }
                throwColumnTypeError(columnName, "std::string", cellValue);
            } else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
            {
                if (const auto *byteValue = std::get_if<BinaryBytes>(&cellValue))
                {
                    // 两种成员拼法由共享转换还原，保证「写进去什么、读回来什么」
                    return AsynGyanis::Database::Detail::fromBinaryBytes<BareType>(*byteValue);
                }

                // 刻意不接受 std::string：列产出文本而成员声明为二进制，说明列的声明与成员的
                // 声明已经不一致。把文本当字节收下能「跑通」，却让 schema 漂移一路静默传播，
                // 直到某天按二进制语义解读一段其实是文本的数据才暴露
                throwColumnTypeError(columnName, "二进制（Bytes）", cellValue);
            } else
            {
                // 不受支持的成员类型已被 allColumnTypesSupported() 的 static_assert 拦住，
                // 这里只是让 if constexpr 的所有分支都有返回值
                static_assert(kAlwaysFalse<MemberType>,
                              "RowMapper：不支持的成员类型。仅支持整型、bool、浮点、std::string、"
                              "二进制载荷（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                              "以及它们的 std::optional 包装");
                return MemberType{};
            }
        }

        /**
         * @brief 右值入口：成员是 std::string / 规范二进制类型、或它们的 std::optional 包装，且单元格正是对应
         *        载荷时，把变体里的堆缓冲直接搬走
         * @details assignColumn 手里的 cellValue 是即将析构的局部量；文本/二进制列走 const& 版本会把整段
         *          载荷再拷一份进成员（叠加驱动读值那次 = 每单元两次分配）。本重载对这两种大载荷 std::move
         *          搬走缓冲区（二进制仅规范拼法 `std::vector<std::uint8_t>` 可无损搬走，`std::vector<std::byte>`
         *          仍逐字节转、回落 const& 版）；可空成员（`optional<string>`/`optional<二进制>`）同形——非 NULL
         *          时把载荷搬进 optional 内部，NULL（monostate）与非匹配载荷都回落 const& 版走原有语义。
         *          其余标量类型没有可无损搬走的大缓冲区，统一转交 const& 版本按原语义取值，保持单一派发真相。
         * @tparam MemberType 目标成员类型
         * @param cellValue 即将被搬空的单元格值（右值引用）
         * @param columnName 列名，仅用于错误信息
         * @return MemberType 转换后的值
         * @throws RowMappingException 类型不匹配等，交由 const& 版本判定
         */
        template<typename MemberType>
        [[nodiscard]] MemberType convertDatabaseValue(DatabaseValue &&cellValue, const std::string_view columnName)
        {
            using BareType = std::remove_cv_t<MemberType>;

            if constexpr (std::is_same_v<BareType, std::string>)
            {
                if (auto *const textValue = std::get_if<std::string>(&cellValue))
                {
                    return std::move(*textValue);
                }
            }
            else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
            {
                if (auto *const byteValue = std::get_if<BinaryBytes>(&cellValue))
                {
                    return AsynGyanis::Database::Detail::fromBinaryBytes<BareType>(std::move(*byteValue));
                }
            }
            else if constexpr (IsOptional<BareType>::value)
            {
                using InnerType = typename BareType::value_type;
                if constexpr (std::is_same_v<InnerType, std::string>)
                {
                    // 可空文本列：非 NULL 时把堆缓冲搬进 optional，省掉「变体→成员」那次整串拷贝
                    if (auto *const textValue = std::get_if<std::string>(&cellValue))
                    {
                        return MemberType{std::move(*textValue)};
                    }
                }
                else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<InnerType>)
                {
                    if (auto *const byteValue = std::get_if<BinaryBytes>(&cellValue))
                    {
                        return MemberType{AsynGyanis::Database::Detail::fromBinaryBytes<InnerType>(std::move(*byteValue))};
                    }
                }
                // monostate（NULL→nullopt）与非匹配载荷：落到下面的 const& 版按原语义取值/报错
            }
            return convertDatabaseValue<MemberType>(static_cast<const DatabaseValue &>(cellValue), columnName);
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
                          (std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<T>::kColumns)> > > 0);

    // ========================================================================
    // 结果集 → 结构体
    // ========================================================================

    namespace Detail
    {
        /**
         * @brief 把结构体一列在结果集中的下标解析出来（找不到列即抛，不改动成员）
         * @tparam T 结构体类型
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param result 结果集
         * @param columnDescriptor 列的元信息（列名用于定位）
         * @return std::size_t 该列在结果集中的下标
         * @throws RowMappingException 列不存在
         */
        template<typename T, typename ColumnDescriptorType>
        [[nodiscard]] std::size_t resolveColumnIndex(const DatabaseResult &result, const ColumnDescriptorType &columnDescriptor)
        {
            // 按列名解析下标：结果集的列顺序由 SELECT 列表决定，与结构体声明顺序无关，
            // 因此不能按下标硬编码，否则一旦查询换列就会整体错位
            const std::optional<std::size_t> columnIndex = result.columnIndex(columnDescriptor.columnName);
            if (!columnIndex.has_value())
            {
                throw RowMappingException("ORM 行映射失败：结果集中不存在列 \"" +
                                          std::string(columnDescriptor.columnName) + "\"（表 " +
                                          std::string(TableSchema<T>::kTableName) + "）");
            }
            return columnIndex.value();
        }

        /**
         * @brief 用已解析好的下标把当前行的一列赋值给结构体的对应成员
         * @tparam T 结构体类型
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param mappedRow 目标结构体，成员的赋值目标
         * @param columnDescriptor 列的元信息（成员指针 + 列名）
         * @param result 结果集，游标须已停在有效行上
         * @param columnIndex 该列在结果集中的下标（由 resolveColumnIndex 一次性解析）
         * @throws RowMappingException 类型不匹配
         */
        template<typename T, typename ColumnDescriptorType>
        void assignColumn(T &mappedRow, const ColumnDescriptorType &columnDescriptor, DatabaseResult &result,
                          const std::size_t columnIndex)
        {
            // 按「交出所有权」读值：物化快照里的文本/二进制缓冲直接搬走，省掉一次整串拷贝与它的堆分配。
            // 一格只读这一次是本路径的前提，而「两格撞同一列」已在 resolveColumnIndices 处被拒绝
            DatabaseValue cellValue = result.takeValue(columnIndex);

            using MemberType                            = typename ColumnDescriptorType::MemberType;
            // 交出局部 cellValue 的所有权：文本列命中右值重载，把变体里的堆缓冲搬进成员，省一次整串拷贝
            mappedRow.*(columnDescriptor.memberPointer) = convertDatabaseValue<MemberType>(std::move(cellValue), columnDescriptor.columnName);
        }

        /**
         * @brief 一次性解析结构体全部列在结果集中的下标（列结构对整个结果集不变，供逐行复用）
         * @details 同时校验各列解析到**不同**的下标：两个成员写同一个列名（或结果集里有两个同名列）
         *          时，二者都会拿到同一列的值而无人报错，属于静默错值，必须在建表时就暴露。
         * @tparam T 结构体类型
         * @tparam IndexPositions kColumns 的下标序列
         * @param result 结果集
         * @return std::array<std::size_t, sizeof...(IndexPositions)> 与 kColumns 同序的下标表
         * @throws RowMappingException 任一列不存在（按 kColumns 顺序报告第一个缺失列），或两列解析到同一下标
         */
        template<typename T, std::size_t... IndexPositions>
        [[nodiscard]] std::array<std::size_t, sizeof...(IndexPositions)> resolveColumnIndices(const DatabaseResult &result,
                                                                                              std::index_sequence<IndexPositions...>)
        {
            std::array<std::size_t, sizeof...(IndexPositions)> columnIndices = {
                resolveColumnIndex<T>(result, std::get<IndexPositions>(TableSchema<T>::kColumns))...
            };

            // 列名表按 kColumns 同序取出，用于在撞名下标时报告是哪两个成员
            const std::array<std::string_view, sizeof...(IndexPositions)> columnNames = {
                std::get<IndexPositions>(TableSchema<T>::kColumns).columnName...
            };

            // 下标表至多几列，逐个两两比对即可，不值得为它建一张哈希表
            for (std::size_t currentIndex = 1; currentIndex < columnIndices.size(); ++currentIndex)
            {
                for (std::size_t previousIndex = 0; previousIndex < currentIndex; ++previousIndex)
                {
                    if (columnIndices[currentIndex] != columnIndices[previousIndex])
                    {
                        continue;
                    }
                    throw RowMappingException("ORM 行映射失败：结构体的第 " + std::to_string(previousIndex + 1U) + " 个与第 " +
                                              std::to_string(currentIndex + 1U) + " 个成员（列名 \"" +
                                              std::string(columnNames[previousIndex]) + "\" 与 \"" +
                                              std::string(columnNames[currentIndex]) + "\"，表 " +
                                              std::string(TableSchema<T>::kTableName) + "）都解析到结果集的第 " +
                                              std::to_string(columnIndices[currentIndex]) +
                                              " 列，两个成员会静默拿到同一个值。请给其中一列改用不同的列名，" +
                                              "或在 SELECT 列表里为该列起不同的别名");
                }
            }

            return columnIndices;
        }

        /**
         * @brief 用已解析的下标表把当前行逐列映射进结构体
         * @tparam T 结构体类型
         * @tparam IndexPositions kColumns 的下标序列
         * @param mappedRow 目标结构体
         * @param result 结果集，游标须已停在有效行上
         * @param columnIndices 与 kColumns 同序的下标表
         * @throws RowMappingException 类型不匹配
         */
        template<typename T, std::size_t... IndexPositions>
        void assignRowFromIndices(T &mappedRow, DatabaseResult &result,
                                  const std::array<std::size_t, sizeof...(IndexPositions)> &columnIndices,
                                  std::index_sequence<IndexPositions...>)
        {
            // 折叠表达式逐个赋值：逗号运算符保证从左到右按 kColumns 顺序执行
            (assignColumn<T>(mappedRow, std::get<IndexPositions>(TableSchema<T>::kColumns), result, columnIndices[IndexPositions]), ...);
        }
    } // namespace Detail

    /**
     * @brief 把结果集的当前行映射成结构体
     *
     * @details 调用方需保证游标已停在有效行上（即刚调用过 DatabaseResult::next() 且返回 true）。
     *          按列名在结果集中定位每一列（因此与结果集的列顺序无关），列缺失或类型不符抛
     *          RowMappingException 而不是给出字段静默为 0 的半成品对象。
     *
     * @tparam T 已特化 TableSchema 的聚合类型
     * @param result 结果集，游标须已停在有效行上；被映射的那一行各格载荷会被搬进返回值
     * @return T 映射后的结构体
     * @throws RowMappingException 列缺失、类型不匹配、整型或浮点收窄会改变数值、NULL 落到非 optional 成员
     */
    template<RowMappable T>
    [[nodiscard]] T mapResultRow(DatabaseResult &result)
    {
        // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯
        static_assert(Detail::allColumnTypesSupported<T>(),
                      "RowMapper：TableSchema<T>::kColumns 中存在不支持的列类型。"
                      "仅支持整型、bool、浮点、std::string、二进制载荷"
                      "（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                      "以及它们的 std::optional 包装");

        T mappedRow{};

        // 列下标只解析一次，逐列赋值复用同一张表（columnIndex 在某些驱动里是线性扫列名）
        constexpr std::size_t columnCount = std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<T>::kColumns)> >;
        const std::array<std::size_t, columnCount> columnIndices =
                Detail::resolveColumnIndices<T>(result, std::make_index_sequence<columnCount>{});
        Detail::assignRowFromIndices<T>(mappedRow, result, columnIndices, std::make_index_sequence<columnCount>{});

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
     * @throws RowMappingException 任意一行映射失败
     */
    template<RowMappable T>
    [[nodiscard]] std::vector<T> mapResultRows(DatabaseResult &result)
    {
        // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯
        static_assert(Detail::allColumnTypesSupported<T>(),
                      "RowMapper：TableSchema<T>::kColumns 中存在不支持的列类型。"
                      "仅支持整型、bool、浮点、std::string、二进制载荷"
                      "（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                      "以及它们的 std::optional 包装");

        std::vector<T> mappedRows;

        // rowCount() 只在驱动能预知总行数时才有意义（SQLite 对只读语句会预扫描），
        // 用它预留容量可以把逐行 push_back 的扩容次数降为 0；无法预知时返回 0，行为不变
        if (const std::size_t knownRowCount = result.rowCount(); knownRowCount > 0)
        {
            mappedRows.reserve(knownRowCount);
        }

        // 结果集的列结构对全部行固定不变，故列下标只在第一行解析一次并复用：
        // 部分驱动的 columnIndex 是线性扫列名，逐行重解会让整表映射退化成 O(行数 × 列数²)。
        // 留到第一行才解析（而非进循环前），是为了保住「空结果集即使列缺失也不抛、只返回空向量」的既有语义
        constexpr std::size_t columnCount = std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<T>::kColumns)> >;
        std::array<std::size_t, columnCount> columnIndices{};
        bool isColumnIndicesResolved = false;
        while (result.next())
        {
            if (!isColumnIndicesResolved)
            {
                columnIndices = Detail::resolveColumnIndices<T>(result, std::make_index_sequence<columnCount>{});
                isColumnIndicesResolved = true;
            }
            T mappedRow{};
            Detail::assignRowFromIndices<T>(mappedRow, result, columnIndices, std::make_index_sequence<columnCount>{});
            mappedRows.push_back(std::move(mappedRow));
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
         * @details 空 optional 绑定为 SQL NULL；整型统一按 int64_t 绑定，无符号值超出 int64_t 上限时降级为
         *          十进制文本（DatabaseValue 已冻结、没有无符号备选）；二进制成员转成二进制备选，驱动据此走
         *          sqlite3_bind_blob / MYSQL_TYPE_BLOB——按文本绑定会被 MySQL 按连接字符集重新解释载荷。
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
            } else if constexpr (std::is_same_v<BareType, bool>)
            {
                // 用 in_place_type 显式指定备选：DatabaseValue 里 bool 可隐式转成 int64_t/double，
                // 交给 variant 的转换构造去选会把「意图」交给重载规则，写清楚更稳
                return DatabaseValue{std::in_place_type<bool>, value};
            } else if constexpr (std::is_integral_v<BareType>)
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
            } else if constexpr (std::is_floating_point_v<BareType>)
            {
                return DatabaseValue{static_cast<double>(value)};
            } else if constexpr (std::is_same_v<BareType, std::string>)
            {
                return DatabaseValue{std::in_place_type<std::string>, value};
            } else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
            {
                // 用 in_place_type 显式指定二进制备选：它必须由「类型」表达出来，
                // 驱动据此走 sqlite3_bind_blob / MYSQL_TYPE_BLOB；std::byte 成员在这里
                // 被规范化成 uint8_t 序列，两种拼法落库后的字节完全一致
                return DatabaseValue{std::in_place_type<BinaryBytes>,
                                     AsynGyanis::Database::Detail::toBinaryBytes<BareType>(value)};
            } else
            {
                static_assert(kAlwaysFalse<MemberType>,
                              "RowMapper：不支持的成员类型，无法转换为绑定参数。"
                              "仅支持整型、bool、浮点、std::string、二进制载荷"
                              "（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                              "以及它们的 std::optional 包装");
                return std::monostate{};
            }
        }

    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable
