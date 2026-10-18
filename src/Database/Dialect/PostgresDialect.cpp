/**
 * @file PostgresDialect.cpp
 * @brief PostgreSQL 方言实现 —— 引擎知识部分（占位符 / 事务 / 类型名 / 元数据 / 参数上限）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/PostgresDialect.h"

#include "Base/Exception/InvalidArgumentException.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    DatabaseType PostgresDialect::type() const noexcept
    {
        return DatabaseType::PostgreSql;
    }

    void PostgresDialect::appendPlaceholder(std::string &sqlText, const std::size_t index) const
    {
        // PostgreSQL 的参数是显式带序号的 $n，且序号从 1 开始：基类传入的 index 就是
        // parameters 的下标，因此这里加一。这是三种引擎里唯一真正使用序号参数的实现
        // （SQLite / MySQL 都是 "?"），也是接口保留 index 形参的原因
        sqlText += '$';
        sqlText += std::to_string(index + 1);
    }

    std::string_view PostgresDialect::insertKeywordPhrase(const Queryable::InsertConflict conflict) const
    {
        // 三档的前缀与 Fail 逐字相同：PostgreSQL 的处置没有开头形式，全部走尾部子句——
        // 这里显式忽略 conflict，让「为什么三个档给同一份文本」在实现处可见
        static_cast<void>(conflict);
        return "INSERT INTO ";
    }

    void PostgresDialect::appendInsertConflictTail(std::string &sqlText, const Queryable::InsertConflict conflict, const Queryable::QueryNode &query) const
    {
        switch (conflict)
        {
            case Queryable::InsertConflict::Fail:
                // 冲突就报错是普通 INSERT 的语义，不加任何子句——既有调用的线上字节一分不变
                return;

            case Queryable::InsertConflict::Ignore:
                // DO NOTHING 不带冲突目标：对任意唯一约束冲突都跳过，不去猜哪一列是业务键
                sqlText += " ON CONFLICT DO NOTHING";
                return;

            case Queryable::InsertConflict::Replace:
            case Queryable::InsertConflict::Upsert:
                break;
        }

        // Replace → DO UPDATE：必须给出冲突目标（推断列或约束名）——真机实测：不带目标的
        // DO UPDATE 被服务端以 42601「requires inference specification or constraint name」拒绝
        // （只有 DO NOTHING 能省目标）。目标列由调用方经 conflictTargetColumns 提供（ORM 从
        // TableSchema 的主键自动带上）；缺了就当场拒绝，不生成一条服务端必拒的语句。
        // SET 列出本次插入的全部列，未列出的列保持旧值（见头文件里与 MySQL REPLACE 的语义差异）；
        // 列名的渲染与 INSERT 列清单同源（appendFieldReference），EXCLUDED 是伪表、其后只跟列名本身
        if (query.conflictTargetColumns.empty())
        {
            throw Base::InvalidArgumentException("PostgreSQL 方言：ON CONFLICT DO UPDATE 需要冲突目标（唯一键的列名），而查询树的 "
                                                 "conflictTargetColumns 为空。请填入唯一键列（ORM 会从 TableSchema 的主键自动带上），"
                                                 "或改用 Ignore 档（DO NOTHING 不需要目标）");
        }

        sqlText += " ON CONFLICT (";
        for (std::size_t index = 0; index < query.conflictTargetColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            appendFieldReference(sqlText, query.conflictTargetColumns[index]);
        }
        sqlText += ") DO UPDATE SET ";

        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            appendFieldReference(sqlText, query.selectColumns[index]);
            sqlText += " = EXCLUDED.";
            appendFieldReference(sqlText, query.selectColumns[index]);
        }
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

    std::optional<std::string> PostgresDialect::currentDatabaseNameStatement() const
    {
        // 咨询锁是服务器级的：把库名揉进锁键，同服务器上的两个库才不会互相挡住迁移
        return std::string{"SELECT current_database()"};
    }

    std::optional<std::string> PostgresDialect::tryAcquireMigrationLockStatement(const std::uint64_t key) const
    {
        // 单键形式收 bigint；调用方已把键压进 63 位，十进制原文可以直接内联
        return "SELECT pg_try_advisory_lock(" + std::to_string(key) + ')';
    }

    std::optional<std::string> PostgresDialect::releaseMigrationLockStatement(const std::uint64_t key) const
    {
        return "SELECT pg_advisory_unlock(" + std::to_string(key) + ')';
    }

    std::string_view PostgresDialect::isolationLevelStatement(const TransactionIsolationLevel level) const noexcept
    {
        // 四档语法 PostgreSQL 全接受（Read Uncommitted 内部按 Read Committed 处理，
        // 标准允许实现更严格，见头文件说明）；Default 无需设置语句。
        // 语句文本与 PostgreSQL 文档的写法逐字一致
        switch (level)
        {
            case TransactionIsolationLevel::Default:
                return {};
            case TransactionIsolationLevel::ReadUncommitted:
                return "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";
            case TransactionIsolationLevel::ReadCommitted:
                return "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
            case TransactionIsolationLevel::RepeatableRead:
                return "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
            case TransactionIsolationLevel::Serializable:
                return "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
        }
        return {};
    }

    std::optional<std::string_view> PostgresDialect::rowLockClause() const noexcept
    {
        // 与 MySQL 同一句文本：这一家的 FOR UPDATE 语义是「锁住读到的行直到本事务结束」
        return "FOR UPDATE";
    }

    std::optional<std::string_view> PostgresDialect::readOnlyTransactionStatement() const noexcept
    {
        // 这一家 START TRANSACTION 与 BEGIN 同义，READ ONLY 是它们的尾缀；
        // 不用 SET TRANSACTION READ ONLY——那条要在 BEGIN 之后发，本形态一步到位
        return "START TRANSACTION READ ONLY";
    }

    bool PostgresDialect::isolationLevelStatementFollowsBegin() const noexcept
    {
        // 事务块外执行 SET TRANSACTION 只会得到警告且不生效（见头文件说明），
        // 因此必须让事务对象在 BEGIN 之后补发
        return true;
    }

    std::string_view PostgresDialect::columnTypeName(const ColumnType type) const noexcept
    {
        switch (type)
        {
            case ColumnType::Int64:
                return "BIGINT";
            case ColumnType::UInt64:
                // PostgreSQL 没有无符号整数类型。NUMERIC(20) 的 20 位十进制正好覆盖
                // 0 .. 18446744073709551615（2^64-1），因此选用它而不是 BIGINT：
                // BIGINT 会让上半个取值域在写入时直接溢出报错，属于静默丢数据之外的硬失败，
                // 但也是「ORM 能写、数据库不能存」的能力缺口；NUMERIC 则两端都能往返
                return "NUMERIC(20)";
            case ColumnType::Double:
                return "DOUBLE PRECISION";
            case ColumnType::Bool:
                return "BOOLEAN";
            case ColumnType::Text:
                return "TEXT";
            case ColumnType::Blob:
                return "BYTEA";
            case ColumnType::Decimal:
                // 不带精度限定的 NUMERIC 是「任意精度、按写入的标度存」，与本库的十进制承载形态同构：
                // 写进去几位小数就读回几位，服务端不做四舍五入。要固定标度（金额常见的 NUMERIC(19,4)）
                // 由调用方自己写 DDL，本映射不替业务决定标度
                return "NUMERIC";
            case ColumnType::Timestamp:
                // 这一家是唯一把「带时区的瞬时」做成真类型的：timestamptz 内部存的是瞬间，按会话
                // TimeZone 渲染，交回的文本自带偏移。本库读回来时按那个偏移折回 UTC 瞬时，所以
                // 会话改时区只改显示不改瞬间——这条由真机用例钉着，不是注释里的说法
                return "TIMESTAMPTZ";
            case ColumnType::LocalDateTime:
                // 与之相对的是不带时区的 timestamp：存的正是字面历法读数。精度显式写 6 位小数秒
                // （默认也是 6，但写出来才不必解释「这一列到底留不留小数」，两家的默认值还不一样）
                return "TIMESTAMP(6)";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    std::string PostgresDialect::autoIncrementPrimaryKeyDefinition(const std::string_view quotedColumnName, const ColumnType type) const
    {
        // 标识列只接受整数类型（smallint / int / bigint）：其余类型（含承接无符号 64 位的
        // NUMERIC(20)）不能做 IDENTITY，给空串让迁移器在建表前拒绝并点名，而不是建出一张
        // 主键不会自增的表。BIGINT 的取值映射与 columnTypeName 同源，两处不会漂移
        if (type != ColumnType::Int64)
        {
            return {};
        }

        // BY DEFAULT 而不是 ALWAYS：允许显式插入指定值（数据迁移与修复需要），
        // 未显式给出时由服务端按序列发号
        return std::string(quotedColumnName) + " " + std::string(columnTypeName(ColumnType::Int64)) + " GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY";
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
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    SqlStatement PostgresDialect::columnListingStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // 与 tableExistsStatement() 同一套限定：information_schema 覆盖全库所有模式，
        // 不限定模式名就会把其它模式里的同名表算进来，那份「本表已有列」的清单是假的——
        // 而它接下来决定哪些列要补，错了就是重复加列（引擎报错）或该补的列没补
        statement.sql = "SELECT column_name FROM information_schema.columns "
                        "WHERE table_schema = current_schema() AND table_name = ";
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.sql += " ORDER BY ordinal_position";
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
