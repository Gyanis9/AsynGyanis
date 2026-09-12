/**
 * @file MySqlDialect.cpp
 * @brief MySQL 方言实现 —— 引擎知识部分（占位符 / 分页 / 事务 / 类型名 / 元数据 / 参数上限）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/MySqlDialect.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    DatabaseType MySqlDialect::type() const noexcept
    {
        return DatabaseType::MySql;
    }

    std::string MySqlDialect::placeholder() const
    {
        // MySQL 的位置参数不区分类型、不区分序号，一律写作 '?'（文本协议与 mysql_stmt_* 都是如此）；
        // 参数与占位符的对应关系由「按出现顺序压入」保证，因此这里不需要知道序号
        return "?";
    }

    void MySqlDialect::appendLimitOffsetClause(std::string &sqlText,
                                               std::vector<DatabaseValue> &parameters,
                                               const Queryable::QueryNode &query) const
    {
        // 只给了 offset 时，MySQL 不允许 OFFSET 单独出现，也没有 SQLite 可用的 "LIMIT -1"
        // （LIMIT 给负值会被判非法）。官方文档给出的「不限行数」写法就是无符号 64 位整数的上界；
        // 它是编译期常量，不引入注入面，也不占用绑定参数，因此占位符序号与参数下标的对应关系不变。
        // 补出这一句之后，余下部分（LIMIT 占位符 → OFFSET 占位符，先取序号再压参数）
        // 与标准 SQL 完全一致，交给基类默认实现复用，不在本方言里再写一份
        if (!query.limit.has_value() && query.offset.has_value())
        {
            sqlText += " LIMIT ";
            sqlText += kUnboundedRowLimitLiteral;
        }

        // limit 与 offset 都给：基类产出 " LIMIT ? OFFSET ?"；
        // 只给 limit：产出 " LIMIT ?"；两个都不给：什么都不输出
        StandardSqlDialect::appendLimitOffsetClause(sqlText, parameters, query);
    }

    std::string_view MySqlDialect::beginTransactionStatement() const noexcept
    {
        // START TRANSACTION 是 MySQL 文档的标准开启写法；不用 "BEGIN"（那更像过程式块的开头），
        // 也没有 SQLite 的 IMMEDIATE 模式可选：InnoDB 的行锁在第一条写语句执行时才真正取
        return "START TRANSACTION";
    }

    std::string_view MySqlDialect::commitStatement() const noexcept
    {
        return "COMMIT";
    }

    std::string_view MySqlDialect::rollbackStatement() const noexcept
    {
        return "ROLLBACK";
    }

    std::string_view MySqlDialect::columnTypeName(const ColumnType type) const noexcept
    {
        // 映射依据见头文件：MySQL 的整数按位宽/符号分家，因此这里选的是与 C++ 类型位宽一致的成员
        switch (type)
        {
            case ColumnType::Int64:  return "BIGINT";
            case ColumnType::UInt64: return "BIGINT UNSIGNED";
            case ColumnType::Double: return "DOUBLE";
            case ColumnType::Bool:   return "TINYINT(1)";
            case ColumnType::Text:   return "TEXT";
            case ColumnType::Blob:   return "LONGBLOB";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    SqlStatement MySqlDialect::tableExistsStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // information_schema.tables 是全实例共享的元数据表，因此必须同时限定库名与表名：
        // 只用 table_name 过滤会把别的库里的同名表也算进来，导致「表不存在却报告存在」。
        // DATABASE() 取当前会话的默认库，正是本条连接操作的那个库
        statement.sql = "SELECT COUNT(*) FROM information_schema.tables "
                        "WHERE table_schema = DATABASE() AND table_name = ";
        statement.sql += placeholder();
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    std::size_t MySqlDialect::maximumStatementParameters() const noexcept
    {
        // 常量取值的依据（预处理协议 2 字节的参数个数字段）见头文件说明。
        // 它不像 SQLite 的 999 那样可能被编译期宏调小，是协议层硬上限；真正的变量是
        // max_allowed_packet（整条语句文本的字节数），因此调用方还须控制单批的数据量
        return kMaximumStatementParameters;
    }

    char MySqlDialect::identifierQuoteCharacter() const noexcept
    {
        // MySQL 的官方引用符是反引号；双引号在默认 sql_mode 下是字符串字面量，不能用
        return '`';
    }

    std::string_view MySqlDialect::dialectName() const noexcept
    {
        return "MySQL";
    }

} // namespace AsynGyanis::Database
