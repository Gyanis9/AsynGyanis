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

    void MySqlDialect::appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const
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

    std::optional<std::string_view> MySqlDialect::rowLockClause() const noexcept
    {
        // InnoDB 只在事务里真正持有这把锁：autocommit 下语句一结束就释放，等于没锁住
        return "FOR UPDATE";
    }

    std::optional<std::string_view> MySqlDialect::readOnlyTransactionStatement() const noexcept
    {
        // MySQL 的正式写法就是 START TRANSACTION READ ONLY；不用 SET TRANSACTION READ ONLY，
        // 那条只管紧随其后的下一笔事务，与本对象「这一笔就是只读」的意图不是一件事
        return "START TRANSACTION READ ONLY";
    }

    std::optional<std::string> MySqlDialect::currentDatabaseNameStatement() const
    {
        // GET_LOCK 是服务器级的：把库名揉进锁键，同服务器上的两个库才不会互相挡住迁移
        return std::string{"SELECT DATABASE()"};
    }

    std::optional<std::string> MySqlDialect::tryAcquireMigrationLockStatement(const std::uint64_t key) const
    {
        // 第二个参数 0 = 立刻返回；键是十进制数字串，不含引号或反斜杠
        return "SELECT GET_LOCK('" + std::to_string(key) + "', 0)";
    }

    std::optional<std::string> MySqlDialect::releaseMigrationLockStatement(const std::uint64_t key) const
    {
        return "SELECT RELEASE_LOCK('" + std::to_string(key) + "')";
    }

    std::string_view MySqlDialect::isolationLevelStatement(const TransactionIsolationLevel level) const noexcept
    {
        // MySQL 的 SET TRANSACTION（不带 SESSION/GLOBAL）按文档只设置「下一笔事务」的隔离级别，
        // 恰好就是紧跟着的那条 START TRANSACTION 要用的；本级不支持时不返回空串的场合，
        // 由 Transaction 构造按「拒绝」处置（空串 = 该引擎给不了这条语句）
        switch (level)
        {
            case TransactionIsolationLevel::ReadUncommitted:
                return "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";
            case TransactionIsolationLevel::ReadCommitted:
                return "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
            case TransactionIsolationLevel::RepeatableRead:
                return "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
            case TransactionIsolationLevel::Serializable:
                return "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
            case TransactionIsolationLevel::Default:
                break;
        }
        return {};
    }

    std::string_view MySqlDialect::insertKeywordPhrase(const Queryable::InsertConflict conflict) const
    {
        // REPLACE INTO 与 SQLite 的 OR REPLACE 同族（先删后插），但它是独立语句而不是 INSERT 的修饰：
        // 表上的触发器会被多触发一次 DELETE，自增主键也会换新值——语义写进头文件的 @details
        switch (conflict)
        {
            case Queryable::InsertConflict::Fail:
                return "INSERT INTO ";
            case Queryable::InsertConflict::Ignore:
                return "INSERT IGNORE INTO ";
            case Queryable::InsertConflict::Replace:
                return "REPLACE INTO ";
            case Queryable::InsertConflict::Upsert:
                // 真 upsert：冲突时只改本次插入的列，不是 REPLACE 的先删后插
                return "INSERT INTO ";
        }
        return "INSERT INTO ";
    }

    void MySqlDialect::appendInsertConflictTail(std::string &sqlText, const Queryable::InsertConflict conflict, const Queryable::QueryNode &query) const
    {
        if (conflict != Queryable::InsertConflict::Upsert)
        {
            // Ignore/Replace 在前缀里已经表达完，Fail 是普通 INSERT：三者都不加尾部
            return;
        }

        // MySQL 的 ON DUPLICATE KEY UPDATE 不需要冲突目标：它按表上的任意唯一键判定。
        // VALUES(col) 取的是本次 INSERT 想写进去的值（8.0.20 起被标记弃用但仍是兼容写法，
        // 新别名语法要 8.0.19+，为了跨 5.7/MariaDB 仍用它）
        sqlText += " ON DUPLICATE KEY UPDATE ";
        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            appendFieldReference(sqlText, query.selectColumns[index]);
            sqlText += " = VALUES(";
            appendFieldReference(sqlText, query.selectColumns[index]);
            sqlText += ")";
        }
    }

    std::string_view MySqlDialect::columnTypeName(const ColumnType type) const noexcept
    {
        // 映射依据见头文件：MySQL 的整数按位宽/符号分家，因此这里选的是与 C++ 类型位宽一致的成员
        switch (type)
        {
            case ColumnType::Int64:
                return "BIGINT";
            case ColumnType::UInt64:
                return "BIGINT UNSIGNED";
            case ColumnType::Double:
                return "DOUBLE";
            case ColumnType::Bool:
                return "TINYINT(1)";
            case ColumnType::Text:
                return "TEXT";
            case ColumnType::Blob:
                return "LONGBLOB";
            case ColumnType::Decimal:
                // MySQL 的 DECIMAL 必须把精度与标度都定死，没有「不限量」那一档。这里取 (65,30)：
                // 精度是本引擎的上限，标度 30 够金额用。代价写在头文件——小数位多于 30 位会被服务端
                // 四舍五入，整数位多于 35 位直接报错，两者都不会静默丢数；要别的宽度由调用方自己写 DDL
                return "DECIMAL(65,30)";
            case ColumnType::Timestamp:
            case ColumnType::LocalDateTime:
                // 两个逻辑类型在这一家落到同一个物理类型，是有意的，理由两条（判据在真机用例里）：
                // ① 本引擎唯一带时区语义的 TIMESTAMP 列会**按会话 time_zone** 重新渲染取值，而线上交回
                //    的文本不带偏移标记——读的人无从知道它是哪个时区。换算的钥匙在会话参数里，而本驱动
                //    不拥有那个参数（建连只定字符集与只读语句时限），不该为了一个列型去替会话做主；
                //    ② TIMESTAMP 的取值范围只到 2038-01-19（32 位秒），而 DATETIME 到 9999 年。
                // 于是本映射把 UTC 瞬时存成 DATETIME(6)：**字面墙上时间，按约定是 UTC**（约定写在
                // Common/TemporalText.h 与 README 的能力清单里）。带 6 是小数秒精度，DATETIME 不带
                // 长度就只存整秒，写 "…:30.5" 进去读回来是 "…:30"，那才是静默丢位。
                return "DATETIME(6)";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    std::string_view MySqlDialect::keyColumnTypeName(const ColumnType type) const noexcept
    {
        // 只有「不带长度的变长类型」需要为索引换成带长度前缀的同族类型，其余按原映射走。
        // 长度 255 的依据见头文件：utf8mb4 下 1020 字节，远在 InnoDB 索引前缀上限之内
        switch (type)
        {
            case ColumnType::Text:
                return "VARCHAR(255)";
            case ColumnType::Blob:
                return "VARBINARY(255)";
            default:
                return columnTypeName(type);
        }
    }

    std::string MySqlDialect::autoIncrementPrimaryKeyDefinition(const std::string_view quotedColumnName, const ColumnType type) const
    {
        // InnoDB 只接受整数列做自增键；文本/浮点等给空串，由迁移工具在建表前报「不支持」
        if (type != ColumnType::Int64 && type != ColumnType::UInt64)
        {
            return {};
        }

        // AUTO_INCREMENT 必须写在 PRIMARY KEY 之前，且该列同时要求 NOT NULL——与 SQLite 的位置正好相反
        return std::string(quotedColumnName) + " " + std::string(keyColumnTypeName(type)) + " NOT NULL AUTO_INCREMENT PRIMARY KEY";
    }

    SqlStatement MySqlDialect::tableExistsStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // information_schema.tables 是全实例共享的元数据表，因此必须同时限定库名与表名：
        // 只用 table_name 过滤会把别的库里的同名表也算进来，导致「表不存在却报告存在」。
        // DATABASE() 取当前会话的默认库，正是本条连接操作的那个库
        statement.sql = "SELECT COUNT(*) FROM information_schema.tables "
                        "WHERE table_schema = DATABASE() AND table_type = 'BASE TABLE' AND table_name = ";
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    SqlStatement MySqlDialect::columnListingStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // 与 tableExistsStatement() 同一套限定：information_schema 是全实例共享的元数据，
        // 不限定库名就会把别的库里的同名表算进来，那份「本表已有列」的清单是假的——而它接下来
        // 决定哪些列要补，错了就是重复加列（引擎报错）或该补的列没补（读写随后报列不存在）
        statement.sql = "SELECT column_name FROM information_schema.COLUMNS "
                        "WHERE table_schema = DATABASE() AND table_name = ";
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.sql += " ORDER BY ordinal_position";
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
