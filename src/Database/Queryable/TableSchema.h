/**
 * @file TableSchema.h
 * @brief 表结构注册 —— 用户通过特化此模板注册数据库表元信息
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 用户通过特化 TableSchema<T> 模板来注册数据结构的表名、列信息和主键。
 *          编译器通过模板参数自动推导字段类型信息。
 *
 * ## 使用范例
 * @code
 *   struct User {
 *       int id;
 *       std::string name;
 *       int age;
 *   };
 *
 *   template<>
 *   struct TableSchema<User> {
 *       static constexpr std::string_view kTableName = "users";
 *       static constexpr auto kColumns = std::tuple{
 *           Column(&User::id,   "id"),
 *           Column(&User::name, "name"),
 *           Column(&User::age,  "age"),
 *       };
 *       static constexpr std::string_view kPrimaryKey = "id";
 *   };
 * @endcode
 */
#pragma once

#include "Database/Queryable/Column.h"

#include <string_view>
#include <tuple>

namespace AsynGyanis::Database::Queryable
{

    /**
     * @brief 表结构注册模板（主模板）
     *
     * @tparam T 数据结构体类型
     *
     * @details 主模板提供空默认值。用户需通过模板特化来填写实际的表名、列信息和主键。
     *          编译器通过特化中的 kColumns 自动推导列名与字段类型。
     *
     * @note 使用 Column() 函数在 constexpr 上下文中创建 ColumnDescriptor 元组，
     *       编译器可在编译期展开元组并推导字段类型。
     */
    template<typename T>
    struct TableSchema
    {
        /// 数据库表名，默认为空字符串
        static constexpr std::string_view kTableName;

        /// 列信息元组，使用 Column() 函数列出的 ColumnDescriptor 元组
        static constexpr auto kColumns = std::tuple{};

        /// 主键列名，默认值为 "id"
        static constexpr std::string_view kPrimaryKey = "id";
    };

} // namespace AsynGyanis::Database::Queryable
