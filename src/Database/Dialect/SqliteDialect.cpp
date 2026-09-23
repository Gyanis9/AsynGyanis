#include "Database/Dialect/SqliteDialect.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    DatabaseType SqliteDialect::type() const noexcept
    {
        return DatabaseType::Sqlite;
    }

    void SqliteDialect::appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const
    {
        // 分页值一律内联、不占绑定参数；形参保留只为与基类钩子签名一致
        static_cast<void>(parameters);

        if (query.limit.has_value())
        {
            // 分页值内联而不占占位符：它来自 QueryNode 的 std::size_t（强类型、非外部文本），
            // 不存在注入面；内联还能让 SQLite 在编译期就知道行数上限，避免额外的绑定步骤。
            // 先 LIMIT 后 OFFSET 的顺序与基类默认实现一致
            sqlText += " LIMIT ";
            sqlText += std::to_string(query.limit.value());
        }

        if (query.offset.has_value())
        {
            if (!query.limit.has_value())
            {
                // SQLite 规定 OFFSET 必须紧跟在 LIMIT 之后，单独给 OFFSET 会被判语法错误；
                // "LIMIT -1" 是 SQLite 表达「不限行数」的官方写法
                sqlText += " LIMIT -1";
            }
            sqlText += " OFFSET ";
            sqlText += std::to_string(query.offset.value());
        }
    }

    std::string_view SqliteDialect::beginTransactionStatement() const noexcept
    {
        // IMMEDIATE 在 BEGIN 时就取写锁，避免 DEFERRED 事务在第一条写语句升级锁时撞上
        // SQLITE_BUSY（这类失败无法靠重试化解，因为两个连接会互相持有读锁）
        return "BEGIN IMMEDIATE";
    }

    std::string_view SqliteDialect::columnTypeName(const ColumnType type) const noexcept
    {
        // 映射依据见头文件：SQLite 只有 INTEGER / REAL / TEXT / BLOB 四个可用存储类，
        // 布尔与无符号整数都没有独立类型，只能落在 INTEGER 上
        switch (type)
        {
            case ColumnType::Int64:
                return "INTEGER";
            case ColumnType::UInt64:
                return "INTEGER";
            case ColumnType::Double:
                return "REAL";
            case ColumnType::Bool:
                return "INTEGER";
            case ColumnType::Text:
                return "TEXT";
            case ColumnType::Blob:
                return "BLOB";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    std::string SqliteDialect::autoIncrementPrimaryKeyDefinition(const std::string_view quotedColumnName,
                                                                const ColumnType type) const
    {
        // 只有整数主键能当自增列：其它类型 SQLite 会直接拒建，这里给空串让迁移工具在建表前就报明
        if (type != ColumnType::Int64 && type != ColumnType::UInt64)
        {
            return {};
        }

        // 类型名刻意取 INTEGER 而不是按成员映射：SQLite 只把「类型正好写成 INTEGER 的单列主键」
        // 当作 rowid 别名，写成 INT 或 BIGINT 都只会得到一个普通整数主键，AUTOINCREMENT 也就无从谈起
        return std::string(quotedColumnName) + " " + std::string(columnTypeName(ColumnType::Int64)) + " PRIMARY KEY AUTOINCREMENT";
    }

    SqlStatement SqliteDialect::tableExistsStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // sqlite_master 是当前库文件的只读元数据表：type='table' 过滤掉索引/视图/触发器，
        // 再按 name 精确匹配。表名走占位符，含引号或分号的表名也只是普通文本
        statement.sql = "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ";
        statement.sql += placeholder();
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    std::size_t SqliteDialect::maximumStatementParameters() const noexcept
    {
        // 常量 kMaximumStatementParameters 的取值来源见头文件说明：SQLITE_MAX_VARIABLE_NUMBER 的默认值。
        // 按保守值分块，即使目标 SQLite 用了更小的自定义上限，也只是多分几批而不会失败
        return kMaximumStatementParameters;
    }

    char SqliteDialect::identifierQuoteCharacter() const noexcept
    {
        // SQL 标准的双引号形式，因此返回 '"'；MySQL 用反引号
        return '"';
    }

    std::string_view SqliteDialect::dialectName() const noexcept
    {
        return "SQLite";
    }

} // namespace AsynGyanis::Database
