/**
 * @file Column.h
 * @brief 字段描述符 —— 编译期结合运行期的列元信息
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details ColumnDescriptor<T, MemberType> 描述数据库表的一个列，由 Column() 辅助函数在编译期
 *          结合成员指针与列名创建。该函数标记为 consteval、结构体是 C++20 聚合体（支持 designated
 *          initializers），因此全部元信息在编译期确定，成员指针也可在编译期传递并参与类型推导。
 */
#pragma once

#include <string_view>

namespace AsynGyanis::Database::Queryable
{

    /**
     * @brief 字段描述符模板
     *
     * @tparam T           所属表结构的类类型
     * @tparam MemberType_ 字段的 C++ 成员类型（内部使用 MemberType_ 避免与类型别名冲突）
     *
     * @details 将 C++ 结构体成员与数据库列名绑定。
     *          通过 Column() 辅助函数创建实例，所有信息在编译期确定。
     *
     * @code
     *   constexpr auto descriptor = Column(&User::name, "name");
     *   // descriptor.columnName == "name"
     *   // descriptor.memberPointer == &User::name
     * @endcode
     */
    template<typename T, typename MemberType_>
    struct ColumnDescriptor
    {
        using ClassType  = T;           ///< 所属结构体类型
        using MemberType = MemberType_; ///< 成员字段类型

        MemberType_ T::* memberPointer = nullptr; ///< 成员指针，编译期可传递
        std::string_view columnName    = {};      ///< 数据库列名
        std::string_view propertyName  = {};      ///< 属性名，默认与 columnName 相同
    };

    /**
     * @brief 创建字段描述符（只提供列名，属性名默认与列名相同）
     *
     * @tparam T           结构体类型（自动推导）
     * @tparam MemberType_ 成员类型（自动推导）
     * @param memberPointer 成员指针，如 &User::name
     * @param columnName    数据库列名
     * @return consteval ColumnDescriptor<T, MemberType_> 编译期确定的字段描述符
     *
     * @code
     *   constexpr auto col = Column(&User::name, "name");
     * @endcode
     */
    template<typename T, typename MemberType_>
    consteval auto Column(MemberType_ T::*memberPointer, std::string_view columnName) noexcept -> ColumnDescriptor<T, MemberType_>
    {
        return ColumnDescriptor<T, MemberType_>{
                .memberPointer = memberPointer,
                .columnName = columnName,
                .propertyName = columnName
        };
    }

    /**
     * @brief 创建字段描述符（同时指定列名与属性名）
     *
     * @tparam T           结构体类型（自动推导）
     * @tparam MemberType_ 成员类型（自动推导）
     * @param memberPointer 成员指针
     * @param columnName    数据库列名
     * @param propertyName  属性名（在 ORM 映射中可与列名不同）
     * @return consteval ColumnDescriptor<T, MemberType_> 编译期确定的字段描述符
     */
    template<typename T, typename MemberType_>
    consteval auto Column(MemberType_ T::*memberPointer, std::string_view columnName, std::string_view propertyName) noexcept -> ColumnDescriptor<T, MemberType_>
    {
        return ColumnDescriptor<T, MemberType_>{
                .memberPointer = memberPointer,
                .columnName = columnName,
                .propertyName = propertyName
        };
    }

} // namespace AsynGyanis::Database::Queryable
