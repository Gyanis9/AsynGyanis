/**
 * @file QueryNode.h
 * @brief 查询树节点 —— 纯数据结构，表达关系型查询的抽象语法树
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 定义查询树的全部节点类型：纯数据聚合体，不含行为逻辑，由方言层翻译成具体 SQL。
 *          全部使用值语义 + std::unique_ptr 管理，支持移动；WhereCondition 的 right 变体同时支持
 *          列-值比较与列-列比较，FieldReference 既可承载列名也可承载表达式文本。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace AsynGyanis::Database::Queryable
{

    // ========================================================================
    // SqlOperator
    // ========================================================================

    /**
     * @brief SQL 操作符枚举
     *
     * @details 涵盖比较、逻辑、模式匹配与空值判断四类操作符，各枚举值的语义见行尾注释。
     */
    enum class SqlOperator
    {
        Eq,        ///< 等于（=）
        Neq,       ///< 不等于（!= 或 <>）
        Gt,        ///< 大于（>）
        Ge,        ///< 大于等于（>=）
        Lt,        ///< 小于（<）
        Le,        ///< 小于等于（<=）
        Like,      ///< 模式匹配（LIKE）
        In,        ///< 在集合中（IN）
        NotIn,     ///< 不在集合中（NOT IN）
        IsNull,    ///< 为空（IS NULL）
        IsNotNull, ///< 不为空（IS NOT NULL）
        And,       ///< 逻辑与（AND）
        Or,        ///< 逻辑或（OR）
        Not        ///< 逻辑非（NOT）
    };

    // ========================================================================
    // FieldReference
    // ========================================================================

    /**
     * @brief 字段引用 —— 列名或表达式字符串
     *
     * @details 包装一个列名（如 "age"）或表达式（如 "COALESCE(age, 0)"），
     *          用于 WHERE、ORDER BY、GROUP BY 等子句。作为 distinct type
     *          避免与 ParameterValue 中的 std::string 在 variant 中产生歧义。
     */
    struct FieldReference
    {
        std::string name; ///< 列名或表达式文本
    };

    // ========================================================================
    // ParameterValue
    // ========================================================================

    /**
     * @brief 参数值变体 —— 数据库查询参数的 C++ 表示
     *
     * @details 各备选依次为 SQL NULL、布尔、有符号/无符号 64 位整数、浮点、字符串与二进制载荷
     *          （后者用于按 BLOB / BINARY 列做条件查询）。
     *
     * @note 新增备选一律**追加在末尾**：既有备选的下标是已发布的契约，调整顺序会让按固定
     *       下标取值的调用点静默取错类型。
     * @note 不使用 std::to_underlying（C++23），枚举值通过 static_cast 转换。
     */
    using ParameterValue = std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string, std::vector<std::uint8_t> >;

    // ========================================================================
    // WhereCondition
    // ========================================================================

    /**
     * @brief WHERE 子句条件节点
     *
     * @details 表示一个比较条件 left op right；right 为 FieldReference 时即列-列比较，
     *          children 承载 AND/OR/NOT 的子条件，inValues 为 IN/NOT IN 提供值列表。
     */
    struct WhereCondition
    {
        FieldReference                               left;     ///< 左操作数：字段列名或表达式
        SqlOperator                                  op;       ///< 操作符
        std::variant<ParameterValue, FieldReference> right;    ///< 右操作数：参数值或另一字段引用
        std::vector<ParameterValue>                  inValues; ///< IN/NOT IN 的值列表（操作符为 In/NotIn 时有效）
        std::vector<WhereCondition>                  children; ///< 子条件列表（AND/OR/NOT 复合节点时使用，存储子表达式树）
    };

    // ========================================================================
    // JoinType
    // ========================================================================

    /**
     * @brief 连接类型枚举
     */
    enum class JoinType
    {
        Inner, ///< 内连接（INNER JOIN）
        Left,  ///< 左外连接（LEFT JOIN）
        Right, ///< 右外连接（RIGHT JOIN）
        Cross  ///< 交叉连接（CROSS JOIN）
    };

    // ========================================================================
    // JoinClause
    // ========================================================================

    /**
     * @brief JOIN 子句节点
     *
     * @details 描述一个表连接操作，包含连接类型、目标表、别名和 ON 条件。
     */
    struct JoinClause
    {
        JoinType                    type;       ///< 连接类型
        std::string                 tableName;  ///< 被连接的表名
        std::string                 tableAlias; ///< 被连接表的别名（可为空）
        std::vector<WhereCondition> conditions; ///< ON 子句的连接条件
    };

    // ========================================================================
    // OrderByClause
    // ========================================================================

    /**
     * @brief ORDER BY 子句节点
     *
     * @details 指定排序字段与方向。
     */
    struct OrderByClause
    {
        FieldReference field;              ///< 排序字段
        bool           descending = false; ///< 是否降序排列，默认为升序
    };

    // ========================================================================
    // QueryNode
    // ========================================================================

    /**
     * @brief 完整查询树节点
     *
     * @details 聚合了一个 SELECT 查询的全部组成部分，包括表名、列、条件、
     *          连接、分组、排序和分页信息。所有字段均为值语义，支持移动。
     *          方言层接收此节点并翻译为具体 SQL 方言。
     */
    struct QueryNode
    {
        std::string                   tableName;       ///< 主表名
        std::string                   tableAlias;      ///< 主表别名（可为空）
        std::vector<std::string>      selectColumns;   ///< SELECT 列名列表；为空时自动推断为所有列
        std::vector<WhereCondition>   whereConditions; ///< WHERE 条件列表（逻辑与连接）
        std::vector<JoinClause>       joins;           ///< JOIN 子句列表
        std::vector<OrderByClause>    orderBy;         ///< ORDER BY 子句列表
        std::vector<FieldReference>   groupBy;         ///< GROUP BY 字段列表
        std::optional<WhereCondition> having;          ///< HAVING 条件
        std::optional<std::size_t>    limit;           ///< LIMIT 行数上限
        std::optional<std::size_t>    offset;          ///< OFFSET 偏移量
    };

} // namespace AsynGyanis::Database::Queryable
