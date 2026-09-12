/**
 * @file PostgresDialect.cpp
 * @brief PostgreSQL 方言实现 —— 引擎知识部分（占位符 / 事务 / 类型名 / 元数据 / 参数上限）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/PostgresDialect.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    DatabaseType PostgresDialect::type() const noexcept
    {
        return DatabaseType::PostgreSql;
    }

    std::string PostgresDialect::placeholder(const std::size_t index) const
    {
        // PostgreSQL 的参数是显式带序号的 $n，且序号从 1 开始：
        // 基类传入的 index 就是 parameters 的下标，因此这里加一。
        // 这是三种引擎里唯一真正使用序号参数的实现（SQLite / MySQL 都是 "?"），
        // 也是 SqlDialect::placeholder() 保留 index 形参的唯一原因
        return "$" + std::to_string(index + 1);
    }

    std::string_view PostgresDialect::beginTransactionStatement() const noexcept
    {
        // BEGIN 与 START TRANSACTION 在 PostgreSQL 里完全等价；选前者是因为它是
        // 官方文档事务章节的首选写法，且与 psql 的默认行为一致
        return "BEGIN";
    }

    std::string_view PostgresDialect::commitStatement() const noexcept
    {
        // END 是等价别名；COMMIT 更明确，也是 SQL 标准写法
        return "COMMIT";
    }

    std::string_view PostgresDialect::rollbackStatement() const noexcept
    {
        // ABORT 是等价别名；ROLLBACK 更明确，也是 SQL 标准写法
        return "ROLLBACK";
    }

    std::string_view PostgresDialect::columnTypeName(const ColumnType type) const noexcept
    {
        switch (type)
        {
            case ColumnType::Int64:  return "BIGINT";
            case ColumnType::UInt64:
                // PostgreSQL 没有无符号整数类型。NUMERIC(20) 的 20 位十进制正好覆盖
                // 0 .. 18446744073709551615（2^64-1），因此选用它而不是 BIGINT：
                // BIGINT 会让上半个取值域在写入时直接溢出报错，属于静默丢数据之外的硬失败，
                // 但也是「ORM 能写、数据库不能存」的能力缺口；NUMERIC 则两端都能往返
                return "NUMERIC(20)";
            case ColumnType::Double: return "DOUBLE PRECISION";
            case ColumnType::Bool:   return "BOOLEAN";
            case ColumnType::Text:   return "TEXT";
            case ColumnType::Blob:   return "BYTEA";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    SqlStatement PostgresDialect::tableExistsStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // information_schema.tables 覆盖当前数据库内的所有模式，因此必须用 current_schema()
        // 限定：不加这一条会把其它模式里的同名表也统计进来，得到「不存在却报告存在」的错判。
        // current_schema() 是 search_path 里第一个可用模式，正是不带模式名的 DML 解析表的位置。
        // 表名走 $1 绑定参数，含双引号或分号的表名只是普通文本
        statement.sql = "SELECT COUNT(*) FROM information_schema.tables "
                        "WHERE table_schema = current_schema() AND table_name = ";
        statement.sql += placeholder(statement.parameters.size());
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    std::size_t PostgresDialect::maximumStatementParameters() const noexcept
    {
        // 扩展查询协议的 Bind 报文用 2 字节有符号整数表达参数个数，65535 即其上界；
        // 按这个上限分块，即使服务端另有更严的资源限制，也只是多分几批而不会失败
        return kMaximumStatementParameters;
    }

    char PostgresDialect::identifierQuoteCharacter() const noexcept
    {
        return '"';
    }

    std::string_view PostgresDialect::dialectName() const noexcept
    {
        return "PostgreSQL";
    }

} // namespace AsynGyanis::Database
