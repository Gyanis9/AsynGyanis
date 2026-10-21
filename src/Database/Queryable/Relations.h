/**
 * @file Relations.h
 * @brief 显式关系装载：关系声明与「分批补取」的取值转换
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这一层的取向是显式装载：关系写在 `TableSchema<T>` 里（`kRelations`），要在查询上
 *          点名装载（`Queryable<T>::then()`）。本仓刻意不做懒加载代理——在协程运行时里「访问属性
 *          会挂起」是最容易咬人的形状（Dapper 的 `Split`、EF 的 `Include`、SqlSugar 的 `Includes`
 *          同样都是显式装载）。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/LogicException.h"

#include "Database/Common/DatabaseValue.h"
#include "Database/Queryable/QueryNode.h"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Database::Queryable
{
    /**
     * @brief 一条关系的声明：成员指针 + 外键列名
     * @tparam OwnerType 声明它的行类型（`TableSchema<OwnerType>` 里写 `kRelations`）
     * @tparam MemberType 承载被装载对象的那一个成员的类型，方向由它决定：
     * `std::vector<U>` → 一对多：外键列在 U 的表里（指向本行的主键）；
     * `U` / `std::optional<U>` → 多对一：外键列在 本行所在的表里（指向 U 的主键）。
     */
    template<typename OwnerType, typename MemberType>
    struct RelationDescriptor
    {
        MemberType OwnerType::*memberPointer    = nullptr; ///< 装载结果落在这个成员上
        std::string_view       foreignKeyColumn = {};      ///< 外键列名，归属哪张表见上面 @tparam 的说明
    };

    /**
     * @brief 创建一条关系声明（编译期）
     * @tparam OwnerType 行类型（自动推导）
     * @tparam MemberType 成员类型（自动推导，决定方向）
     * @param memberPointer 装载结果落在哪个成员上
     * @param foreignKeyColumn 外键列名（多对一时是本表列，一对多时是对方表的列）
     * @return consteval RelationDescriptor<OwnerType, MemberType> 关系声明
     */
    template<typename OwnerType, typename MemberType>
    consteval auto Relation(MemberType OwnerType::*memberPointer, const std::string_view foreignKeyColumn) noexcept -> RelationDescriptor<OwnerType, MemberType>
    {
        return RelationDescriptor<OwnerType, MemberType>{.memberPointer = memberPointer, .foreignKeyColumn = foreignKeyColumn};
    }

    namespace Detail
    {
        /**
         * @brief 关系成员的形状：被装载的行类型 + 方向
         * @tparam MemberType 成员类型（`std::vector<U>` / `std::optional<U>` / `U`）
         */
        template<typename MemberType>
        struct RelationTarget
        {
            using RowType                      = MemberType; ///< 被装载的行类型
            static constexpr bool isCollection = false;      ///< false＝多对一（本条指向一行）
            static constexpr bool isOptional   = false;      ///< 是否可空（决定缺失时留空还是保持原值）
        };

        template<typename RelatedRowType>
        struct RelationTarget<std::vector<RelatedRowType>>
        {
            using RowType                      = RelatedRowType;
            static constexpr bool isCollection = true;
            static constexpr bool isOptional   = false;
        };

        template<typename RelatedRowType>
        struct RelationTarget<std::optional<RelatedRowType>>
        {
            using RowType                      = RelatedRowType;
            static constexpr bool isCollection = false;
            static constexpr bool isOptional   = true;
        };

        /**
         * @brief 把行键（DatabaseValue）转成查询树能绑定的参数
         * @details 键只可能是标量：整型、文本、浮点、布尔、二进制与空值。复合类型（列表/哈希）
         *          不可能是键——拿它当键是调用方写错了（`kPrimaryKey` 指的列类型不对），
         *          当场抛 `LogicException` 而不是生成一条服务端必拒的语句
         * @param key 键值
         * @param columnName 键所在的列名（报错文案里点名）
         * @return ParameterValue 绑定值
         * @throws Base::LogicException 键是复合类型
         */
        [[nodiscard]] inline ParameterValue toParameterValueFromKey(const DatabaseValue &key, const std::string_view columnName)
        {
            return std::visit(
                    [&columnName](const auto &concrete) -> ParameterValue
                    {
                        using ValueType = std::decay_t<decltype(concrete)>;
                        if constexpr (std::is_same_v<ValueType, std::monostate>)
                        {
                            return nullptr;
                        } else if constexpr (std::is_same_v<ValueType, bool> || std::is_same_v<ValueType, std::int64_t> || std::is_same_v<ValueType, double> ||
                                             std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, std::vector<std::uint8_t>>)
                        {
                            return concrete;
                        } else if constexpr (std::is_same_v<ValueType, Decimal>)
                        {
                            // 键落在十进制列上时按文本交给参数层：ParameterValue 没有 Decimal 这一档
                            // （它只服务查询条件的字面量），而十进制文本正是各家引擎都能精确收下的形态
                            return concrete.text();
                        } else if constexpr (std::is_same_v<ValueType, Timestamp> || std::is_same_v<ValueType, LocalDateTime>)
                        {
                            // 日期时间当外键时不折成文本再交给参数层：文本形态是按引擎分家的
                            // （MySQL 不带偏移、PostgreSQL 的 timestamptz 必须带 +00、SQLite 自带 Z），
                            // 而这一层拿不到「我是哪家」的信息。猜一种就是让关联条件静默失效，
                            // 所以点明原因：真需要按时间关联的模型请用整型或文本键
                            throw Base::LogicException("关系装载：列 \"" + std::string(columnName) +
                                                       "\" 的值是日期时间，不能当键用——"
                                                       "日期时间的文本形态按引擎不同（本层不知道是哪一家），请改用整型或文本类型的键");
                        } else
                        {
                            throw Base::LogicException("关系装载：列 \"" + std::string(columnName) + "\" 的值是复合类型，不能当键用——请检查 TableSchema 里那一格指向的列");
                        }
                    },
                    key);
        }
    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable
