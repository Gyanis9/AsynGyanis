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

        /**
         * @brief 主键是否由数据库自增生成，默认 false
         * @details 置为 true 时三处一起变：INSERT 的列清单里不出现主键（值交给引擎生成，
         *          写进去反而会占号段或与已用值冲突）；SchemaMigrator 建表时给主键加上本引擎的
         *          自增约束；生成的标识由 Queryable::insertAndGetGeneratedId() 从写回执上取回。
         *          只作用于主键——两个引擎都要求自增列是键，因此不提供「非主键的自增列」这种写法。
         */
        static constexpr bool kIsAutoIncrementPrimaryKey = false;
    };

    namespace Detail
    {
        /**
         * @brief 读取 TableSchema<T> 的自增主键声明，未声明时按 false 处理
         *
         * @details 必须用 requires 探测而不是直接写 TableSchema<T>::kIsAutoIncrementPrimaryKey：
         *          用户的特化是**完全特化**，不继承主模板的默认成员，直接取会让所有既有声明在这个
         *          字段加入后一起编译失败。探测使它成为纯粹的增量声明，老代码一行不改仍然成立。
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @return bool 主键是否由数据库自增生成
         */
        template<typename T>
        [[nodiscard]] consteval bool declaresAutoIncrementPrimaryKey()
        {
            if constexpr (requires { TableSchema<T>::kIsAutoIncrementPrimaryKey; })
            {
                return TableSchema<T>::kIsAutoIncrementPrimaryKey;
            } else
            {
                return false;
            }
        }
    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable
