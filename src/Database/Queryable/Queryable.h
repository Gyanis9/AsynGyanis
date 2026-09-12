/**
 * @file Queryable.h
 * @brief 查询构建器 —— 类型安全的 ORM 查询入口，构建 QueryNode 查询树
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Queryable<T> 是 ORM 查询的门面类，提供流式接口构建查询树，
 *          并通过 toSql() 生成 SQL 文本。执行器方法（toList, first, executeNonQuery）
 *          需要连接池支持，将在 Phase 2 中完善。
 *
 * ## 使用范例
 * @code
 *   // 离线 SQL 生成（测试用，无需连接池）
 *   Queryable<User> query;
 *   query.where(Column(&User::age, "age") >= 18)
 *        .orderBy(asc("name"))
 *        .limit(10);
 *   std::string sql = query.toSql();
 *   // => "SELECT * FROM users WHERE age >= ? ORDER BY name ASC LIMIT 10"
 *
 *   // 在线查询（需要连接池）
 *   ConnectionPool pool(...);
 *   Queryable<User> query(pool);
 *   auto users = query.where(...).toList();
 * @endcode
 */
#pragma once

#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/QueryNode.h"
#include "Database/Queryable/TableSchema.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    class ConnectionPool;
}

namespace AsynGyanis::Database::Queryable
{

    /**
     * @brief ORM 查询构建器模板
     *
     * @tparam T 表数据结构类型，需有对应的 TableSchema<T> 特化
     *
     * @details 提供流式接口构建类型安全的数据库查询。
     *          两种构造方式：
     *          - 默认构造：离线模式，仅用于 SQL 生成和测试
     *          - 带连接池构造：在线模式，支持执行查询
     *
     * @note 当前仅实现 SQL 生成部分（toSql），执行器方法将在 Phase 2 实现。
     */
    template<typename T>
    class Queryable
    {
    public:
        /**
         * @brief 默认构造（离线模式）
         *
         * @details 用于仅生成 SQL 或测试场景，不需要连接池。
         *          表名从 TableSchema<T>::kTableName 自动获取。
         *          若 TableSchema 未特化或 kTableName 为空，则表名需要在首次构建前通过 table() 设置。
         */
        Queryable()
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        /**
         * @brief 构造并绑定连接池（在线模式）
         *
         * @details 使用连接池获取数据库连接执行查询。
         *          toList()、first()、executeNonQuery() 将使用此池。
         *
         * @param pool 数据库连接池
         */
        explicit Queryable(ConnectionPool &pool)
            : m_pool(&pool)
        {
            m_queryNode.tableName = std::string(TableSchema<T>::kTableName);
        }

        // ========================================================================
        // 构建器方法
        // ========================================================================

        /**
         * @brief 添加 WHERE 条件
         *
         * @details 使用 Expression.h 中的运算符构建条件。
         *          支持链式多次调用，条件之间用 AND 连接。
         *
         * @param condition 通过列比较或逻辑组合构建的条件
         * @return Queryable& 自身引用，支持链式调用
         *
         * @code
         *   query.where(Column(&User::age, "age") >= 18)
         *        .where(Column(&User::name, "name") == "Alice");
         * @endcode
         */
        Queryable &where(WhereCondition condition)
        {
            // 如果条件是 AND/OR/NOT 复合节点或普通比较，直接压入列表
            m_queryNode.whereConditions.push_back(std::move(condition));
            return *this;
        }

        /**
         * @brief 添加 ORDER BY 子句
         *
         * @param order 使用 asc()/desc() 创建的排序子句
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &orderBy(OrderByClause order)
        {
            m_queryNode.orderBy.push_back(std::move(order));
            return *this;
        }

        /**
         * @brief 设置 LIMIT 子句
         *
         * @param count 返回行数上限
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &limit(std::size_t count)
        {
            m_queryNode.limit = count;
            return *this;
        }

        /**
         * @brief 设置 OFFSET 子句
         *
         * @param skip 跳过的行数
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &offset(std::size_t skip)
        {
            m_queryNode.offset = skip;
            return *this;
        }

        /**
         * @brief 设置 SELECT 列
         *
         * @details 默认（不调用此方法或传入空列表）使用 TableSchema 中 kColumns 的顺序。
         *
         * @param columns 列名列表
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &select(std::vector<std::string> columns)
        {
            m_queryNode.selectColumns = std::move(columns);
            return *this;
        }

        /**
         * @brief 添加 JOIN 子句
         *
         * @param join 连接子句
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &join(JoinClause join)
        {
            m_queryNode.joins.push_back(std::move(join));
            return *this;
        }

        /**
         * @brief 设置 GROUP BY 字段
         *
         * @param fields 分组列名列表
         * @return Queryable& 自身引用，支持链式调用
         */
        Queryable &groupBy(std::vector<std::string> fields)
        {
            std::vector<FieldReference> fieldRefs;
            fieldRefs.reserve(fields.size());
            for (auto &field : fields)
            {
                fieldRefs.push_back(FieldReference{.name = std::move(field)});
            }
            m_queryNode.groupBy = std::move(fieldRefs);
            return *this;
        }

        // ========================================================================
        // 执行器方法（Phase 2 实现）
        // ========================================================================

        /**
         * @brief 执行查询并返回所有结果
         *
         * @return std::vector<T> 查询结果列表（Phase 2 实现）
         *
         * @throw std::logic_error 当前为离线模式（无连接池）时抛出
         */
        std::vector<T> toList()
        {
            if (!m_pool)
            {
                throw std::logic_error("Queryable: toList() 需要连接池，当前为离线模式");
            }
            // Phase 2: 获取连接 → 执行 toSql() → 解析结果 → 返回
            return {};
        }

        /**
         * @brief 执行查询并返回第一条结果
         *
         * @return std::optional<T> 第一条结果，无结果时返回空（Phase 2 实现）
         *
         * @throw std::logic_error 当前为离线模式（无连接池）时抛出
         */
        std::optional<T> first()
        {
            if (!m_pool)
            {
                throw std::logic_error("Queryable: first() 需要连接池，当前为离线模式");
            }
            return std::nullopt;
        }

        /**
         * @brief 执行非查询操作（UPDATE/DELETE/INSERT）
         *
         * @details 根据 QueryNode 的状态生成 UPDATE/DELETE/INSERT 语句并执行。
         *
         * @return int 受影响的行数（Phase 2 实现）
         *
         * @throw std::logic_error 当前为离线模式（无连接池）时抛出
         */
        int executeNonQuery()
        {
            if (!m_pool)
            {
                throw std::logic_error("Queryable: executeNonQuery() 需要连接池，当前为离线模式");
            }
            return 0;
        }

        // ========================================================================
        // SQL 生成
        // ========================================================================

        /**
         * @brief 生成 SQL 文本
         *
         * @details 将当前 QueryNode 查询树转换为近似 SQL 字符串。
         *          使用 ? 作为参数占位符，具体方言适配由 Phase 2 的 Dialect 层完成。
         *
         * @return std::string 生成的 SQL 文本
         *
         * @code
         *   Queryable<User> query;
         *   query.where(Column(&User::age, "age") >= 18)
         *        .orderBy(asc("name"));
         *   std::string sql = query.toSql();
         *   // => "SELECT * FROM users WHERE age >= ? ORDER BY name ASC"
         * @endcode
         */
        [[nodiscard]] std::string toSql() const
        {
            return buildSelectSql();
        }

    private:
        // ========================================================================
        // SQL 生成实现
        // ========================================================================

        /**
         * @brief 构建 SELECT SQL 语句
         * @return std::string 完整的 SELECT 语句
         */
        [[nodiscard]] std::string buildSelectSql() const
        {
            std::string sql;

            // SELECT 子句
            sql += "SELECT ";
            if (m_queryNode.selectColumns.empty())
            {
                sql += "*";
            }
            else
            {
                for (std::size_t i = 0; i < m_queryNode.selectColumns.size(); ++i)
                {
                    if (i > 0) sql += ", ";
                    sql += m_queryNode.selectColumns[i];
                }
            }

            // FROM 子句
            sql += " FROM ";
            sql += m_queryNode.tableName;
            if (!m_queryNode.tableAlias.empty())
            {
                sql += " AS ";
                sql += m_queryNode.tableAlias;
            }

            // JOIN 子句
            for (const auto &joinClause : m_queryNode.joins)
            {
                sql += " ";
                sql += joinTypeToString(joinClause.type);
                sql += " JOIN ";
                sql += joinClause.tableName;
                if (!joinClause.tableAlias.empty())
                {
                    sql += " AS ";
                    sql += joinClause.tableAlias;
                }
                if (!joinClause.conditions.empty())
                {
                    sql += " ON ";
                    for (std::size_t i = 0; i < joinClause.conditions.size(); ++i)
                    {
                        if (i > 0) sql += " AND ";
                        sql += buildConditionString(joinClause.conditions[i]);
                    }
                }
            }

            // WHERE 子句
            if (!m_queryNode.whereConditions.empty())
            {
                sql += " WHERE ";
                for (std::size_t i = 0; i < m_queryNode.whereConditions.size(); ++i)
                {
                    if (i > 0) sql += " AND ";
                    sql += buildConditionString(m_queryNode.whereConditions[i]);
                }
            }

            // GROUP BY 子句
            if (!m_queryNode.groupBy.empty())
            {
                sql += " GROUP BY ";
                for (std::size_t i = 0; i < m_queryNode.groupBy.size(); ++i)
                {
                    if (i > 0) sql += ", ";
                    sql += fieldReferenceToString(m_queryNode.groupBy[i]);
                }
            }

            // HAVING 子句
            if (m_queryNode.having.has_value())
            {
                sql += " HAVING ";
                sql += buildConditionString(m_queryNode.having.value());
            }

            // ORDER BY 子句
            if (!m_queryNode.orderBy.empty())
            {
                sql += " ORDER BY ";
                for (std::size_t i = 0; i < m_queryNode.orderBy.size(); ++i)
                {
                    if (i > 0) sql += ", ";
                    sql += orderByToString(m_queryNode.orderBy[i]);
                }
            }

            // LIMIT 子句
            if (m_queryNode.limit.has_value())
            {
                sql += " LIMIT ";
                sql += std::to_string(m_queryNode.limit.value());
            }

            // OFFSET 子句
            if (m_queryNode.offset.has_value())
            {
                sql += " OFFSET ";
                sql += std::to_string(m_queryNode.offset.value());
            }

            return sql;
        }

        /**
         * @brief 构建单个条件的 SQL 字符串
         * @param condition 待转换的条件
         * @return std::string 条件对应的 SQL 片段
         */
        [[nodiscard]] static std::string buildConditionString(const WhereCondition &condition)
        {
            // 复合节点（AND/OR/NOT）：递归展开 children
            if (condition.op == SqlOperator::And)
            {
                if (condition.children.empty())
                {
                    return "(1=1)";
                }
                std::string result = "(";
                for (std::size_t i = 0; i < condition.children.size(); ++i)
                {
                    if (i > 0) result += " AND ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ")";
                return result;
            }

            if (condition.op == SqlOperator::Or)
            {
                if (condition.children.empty())
                {
                    return "(1=0)";
                }
                std::string result = "(";
                for (std::size_t i = 0; i < condition.children.size(); ++i)
                {
                    if (i > 0) result += " OR ";
                    result += buildConditionString(condition.children[i]);
                }
                result += ")";
                return result;
            }

            if (condition.op == SqlOperator::Not)
            {
                if (condition.children.empty())
                {
                    return "NOT (1=1)";
                }
                return "NOT " + buildConditionString(condition.children[0]);
            }

            // 叶子节点：left op right
            std::string result;
            result += fieldReferenceToString(condition.left);
            result += " ";
            result += operatorToString(condition.op);
            result += " ";

            if (condition.op == SqlOperator::IsNull || condition.op == SqlOperator::IsNotNull)
            {
                // IS NULL / IS NOT NULL 不需要右操作数
                return result;
            }

            if (condition.op == SqlOperator::In || condition.op == SqlOperator::NotIn)
            {
                if (condition.inValues.empty())
                {
                    result += "(?)";
                }
                else
                {
                    result += "(";
                    for (std::size_t i = 0; i < condition.inValues.size(); ++i)
                    {
                        if (i > 0) result += ", ";
                        result += "?";
                    }
                    result += ")";
                }
                return result;
            }

            result += "?";
            return result;
        }

        /**
         * @brief 将 SqlOperator 转为 SQL 操作符字符串
         */
        [[nodiscard]] static std::string_view operatorToString(SqlOperator op)
        {
            using namespace std::string_view_literals;
            switch (op)
            {
                case SqlOperator::Eq:        return "="sv;
                case SqlOperator::Neq:       return "!="sv;
                case SqlOperator::Gt:        return ">"sv;
                case SqlOperator::Ge:        return ">="sv;
                case SqlOperator::Lt:        return "<"sv;
                case SqlOperator::Le:        return "<="sv;
                case SqlOperator::Like:      return "LIKE"sv;
                case SqlOperator::In:        return "IN"sv;
                case SqlOperator::NotIn:     return "NOT IN"sv;
                case SqlOperator::IsNull:    return "IS NULL"sv;
                case SqlOperator::IsNotNull: return "IS NOT NULL"sv;
                default:                     return "="sv;
            }
        }

        /**
         * @brief 将 JoinType 转为 SQL 连接类型字符串
         */
        [[nodiscard]] static std::string_view joinTypeToString(JoinType type)
        {
            using namespace std::string_view_literals;
            switch (type)
            {
                case JoinType::Inner: return "INNER"sv;
                case JoinType::Left:  return "LEFT"sv;
                case JoinType::Right: return "RIGHT"sv;
                case JoinType::Cross: return "CROSS"sv;
                default:              return "INNER"sv;
            }
        }

        /**
         * @brief 将 FieldReference 转为字符串
         */
        [[nodiscard]] static std::string fieldReferenceToString(const FieldReference &field)
        {
            return field.name;
        }

        /**
         * @brief 将 OrderByClause 转为 SQL 排序字符串
         */
        [[nodiscard]] static std::string orderByToString(const OrderByClause &order)
        {
            std::string result = fieldReferenceToString(order.field);
            if (order.descending)
            {
                result += " DESC";
            }
            else
            {
                result += " ASC";
            }
            return result;
        }

        // ========================================================================
        // 数据成员
        // ========================================================================

        QueryNode        m_queryNode; ///< 查询树节点，存储所有查询构建信息
        ConnectionPool  *m_pool = nullptr; ///< 数据库连接池指针，离线模式为 nullptr
    };

} // namespace AsynGyanis::Database::Queryable