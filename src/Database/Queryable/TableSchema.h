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

        /**
         * @brief 乐观锁版本列名，默认空串＝不启用
         *
         * @details 声明之后，`update()` 生成的 WHERE 除主键外多一个「版本 = ?」条件
         *          （问的是手里这份数据的版本），SET 里把这一列写成「自己加一」而不是取结构体的值——
         *          这样「忘了加版本号」不会让更新悄悄失去保护，加一由服务端在一次语句里完成，
         *          两个会话各改一次不可能都成功。更新到 0 行即抛
         *          `Database::OptimisticLockException`（消息点名表、版本列、期望版本与主键值）。
         *          这一列必须是 `kColumns` 里声明过的整数列、且不能与主键同列——两处都在生成语句时拒绝。
         * @note 走的是「读出-改-写回」的经典乐观锁：它防的是并发覆盖（后写的人把先写的人抹掉），
         *       不是「这一行还在不在」；要让「不在就报错」也成立，仍按 update() 的 @warning
         *       用 first()/count() 确认存在性。与 `InsertConflict` 三档正交：那一档管 INSERT 撞唯一键，
         *       本格管 UPDATE 条件匹配不上，一次写入只可能落在其中一处
         */
        static constexpr std::string_view kVersionColumn = "";

        /// 关系声明：`std::tuple` 里放若干 `Relation(&T::成员, "外键列")`（见 Relations.h）。
        /// 它是纯增量声明——不写这一格的行类型行为与之前逐字相同（读取走 requires 探测）
        static constexpr auto kRelations = std::tuple{};
    };

    namespace Detail
    {
        /**
         * @brief 读取 TableSchema<T> 的自增主键声明，未声明时按 false 处理
         *
         * @details 必须用 requires 探测而不是直接写 TableSchema<T>::kIsAutoIncrementPrimaryKey：
         *          用户的特化是完全特化，不继承主模板的默认成员，直接取会让所有既有声明在这个
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

        /**
         * @brief 读取 TableSchema<T> 的乐观锁版本列声明，未声明时按「不启用」处理
         *
         * @details 与 declaresAutoIncrementPrimaryKey() 同款、同一条理由：用户的特化是完全特化、
         *          不继承主模板的默认成员，直接取 `TableSchema<T>::kVersionColumn` 会让所有既有声明
         *          在这个字段加入之后一起编译失败。探测使它成为纯增量声明，老代码一行不改仍然成立。
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @return std::string_view 版本列名；未声明或声明为空串时返回空视图（＝不启用乐观锁）
         */
        template<typename T>
        [[nodiscard]] consteval std::string_view versionColumnName()
        {
            if constexpr (requires { TableSchema<T>::kVersionColumn; })
            {
                return TableSchema<T>::kVersionColumn;
            } else
            {
                return {};
            }
        }
    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable
