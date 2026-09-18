/**
 * @file TableSchema.h
 * @brief 表结构注册 —— 用户通过特化此模板注册数据库表元信息
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
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
     * @details 主模板给的是空默认值，用户特化后填入真实表名、列与主键；列用 Column() 在
     *          constexpr 上下文列出，编译器据此推导各字段的类型名与列名。
     */
    template<typename T>
    struct TableSchema
    {
        static constexpr std::string_view kTableName = ""; ///< 表名默认为空串：未特化的类型由此在 ORM 侧报错（见 SchemaMigrator 校验），而非编译期拦截

        static constexpr auto kColumns = std::tuple{}; ///< 列信息元组，使用 Column() 函数列出的 ColumnDescriptor 元组

        static constexpr std::string_view kPrimaryKey = "id"; ///< 主键列名，默认值为 "id"
    };

} // namespace AsynGyanis::Database::Queryable
