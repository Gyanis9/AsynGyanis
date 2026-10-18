#include "Database/Dialect/SqliteDialect.h"

#include "Base/Exception/InvalidArgumentException.h"

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

    std::string_view SqliteDialect::isolationLevelStatement(const TransactionIsolationLevel /*level*/) const noexcept
    {
        // SQLite 没有「按事务请求隔离级别」这条路：它全库一把写锁，隔离级别由日志模式决定
        // （回滚日志下是串行化、WAL 下是可重复读级快照），且 PRAGMA read_uncommitted 改的是
        // 连接的持续状态而不是单笔事务——在借出期间改它会留给下一个借用者。
        // 因此一律给空串（= 拒绝），由 Transaction 构造带级别名抛出；Default 同样走这条零语句的路
        return {};
    }

    std::string_view SqliteDialect::insertKeywordPhrase(const Queryable::InsertConflict conflict) const
    {
        // 短语含结尾空格：共用渲染层拼完它就接表名，两处的拼接形状只在这一处约定
        switch (conflict)
        {
            case Queryable::InsertConflict::Fail:
                return "INSERT INTO ";
            case Queryable::InsertConflict::Ignore:
                return "INSERT OR IGNORE INTO ";
            case Queryable::InsertConflict::Replace:
                return "INSERT OR REPLACE INTO ";
            case Queryable::InsertConflict::Upsert:
                // 真 upsert 走 VALUES 之后的 ON CONFLICT 子句，前缀与普通 INSERT 相同
                return "INSERT INTO ";
        }
        // 枚举将来扩到更多档时回到最保守的那一个：宁可报错，也不静默改写别人那一行
        return "INSERT INTO ";
    }

    void SqliteDialect::appendInsertConflictTail(std::string &sqlText, const Queryable::InsertConflict conflict, const Queryable::QueryNode &query) const
    {
        if (conflict != Queryable::InsertConflict::Upsert)
        {
            return;
        }
        if (query.conflictTargetColumns.empty())
        {
            throw Base::InvalidArgumentException("SQLite 方言：ON CONFLICT DO UPDATE 需要冲突目标（唯一键的列名），而查询树的 "
                                                 "conflictTargetColumns 为空。请填入唯一键列，或改用 Ignore 档");
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
            sqlText += " = excluded.";
            appendFieldReference(sqlText, query.selectColumns[index]);
        }
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
            case ColumnType::Decimal:
                // SQLite 没有十进制类型，只有五种存储亲和，两条路各丢一样东西，这里选「保真优先」：
                // 写成 TEXT 亲和（类型名里带 CHAR/TEXT/CLOB 才是 TEXT 亲和，NUMERIC/DECIMAL 都会被
                // 引擎判成 NUMERIC 亲和）。选 NUMERIC 的代价实测两条：小数尾零被抹掉（"0.1000" 交回
                // "0.1"），并且值被折成 REAL 存 —— 读回来时驱动看到的是 SQLITE_FLOAT 而不是文本，
                // 要把它还原成 Decimal 就得在本库里拿 double 自己印一遍文本，那是造数，不是取数。
                // 选 TEXT 的代价一条，且必须让用的人知道：这列上的比较与排序按字典序进行
                // （"9" 大于 "10"），而 SUM()/AVG() 这类聚合会自己把文本折成数值，不受影响。
                // 需要数值序的场合请存整数分（Int64 列）或在查询里显式 CAST。
                // 类型名写成 TEXT 而不是保留 DECIMAL：保留后者会静默落到 NUMERIC 亲和，
                // 于是「声明看着是十进制、存进去是二进制浮点」，正是本类型要避免的事
                return "TEXT";
            case ColumnType::Timestamp:
            case ColumnType::LocalDateTime:
                // 两条都落 TEXT，道理与上面那格相同：本引擎没有日期时间类型，只有亲和规则。
                // 类型名刻意不写成 DATE / TIME / TIMESTAMP —— 亲和规则看的是子串
                // （INT / CHAR / CLOB / TEXT / BLOB / REAL / FLOA / DOUB），这几个词一个都不沾，
                // 会被判成 NUMERIC 亲和，于是「2026-10-18 09:20:30」要先被引擎试着折成数值，
                // 折不动才留下文本：等于把列的存储类交给运气。
                // 两个逻辑类型在物理上同型，区分它们的是**交出去的文本**：UTC 瞬时那一路带 'Z'、
                // 墙上时间那一路不带（渲染口径见 Common/TemporalText.h）。定宽 ISO 文本还有一层用处：
                // TEXT 亲和下比较按字典序，而定宽 ISO 的字典序与时间序一致，不像十进制会「9 大于 10」
                return "TEXT";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    std::string SqliteDialect::autoIncrementPrimaryKeyDefinition(const std::string_view quotedColumnName, const ColumnType type) const
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
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    SqlStatement SqliteDialect::columnListingStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // pragma_table_info 是内核提供的表值函数：cid 就是列的定义顺序，name 是列名。
        // 表名以绑定参数送入，因此带引号或分号的表名只被当成一次查询目标，不改变语句结构；
        // 表不存在时结果集为空（SQLite 不会为不存在的表报错），调用方据此先判表在不在
        statement.sql = "SELECT name FROM pragma_table_info(";
        appendPlaceholder(statement.sql, statement.parameters.size());
        statement.sql += ") ORDER BY cid";
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
