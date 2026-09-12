/**
 * @file SqliteDialect.cpp
 * @brief SQLite 方言实现 —— 查询树到参数化 SQL 的翻译
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/SqliteDialect.h"

#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /// UTF-8 多字节序列的首字节与后续字节都 >= 0x80，用于把中文列名识别为合法标识符
        constexpr unsigned char kUtf8ContinuationLowerBound = 0x80;

        /**
         * @brief 判断单个字节能否出现在不带引号的标识符里
         * @param character 待判断的字节
         * @return true 字母、数字、下划线，或 UTF-8 多字节序列的一部分
         */
        bool isIdentifierByte(const char character) noexcept
        {
            const unsigned char byte = static_cast<unsigned char>(character);

            // 手写字符区间而不用 std::isalnum：后者受当前 locale 影响，
            // 且要求参数可表示为 unsigned char（负数直接传给它是未定义行为）
            const bool isAsciiLetter = (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
                                       (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'));
            const bool isAsciiDigit = byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9');

            return isAsciiLetter || isAsciiDigit || byte == static_cast<unsigned char>('_') ||
                   byte >= kUtf8ContinuationLowerBound;
        }

        /**
         * @brief 判断文本是否是一个可以安全加引号引用的标识符
         * @details 在裸标识符字节之外还放行两类字符：
         *          - 双引号：那正是「列名里带引号、必须翻倍转义」的情形，
         *            交给 quoteIdentifier() 处理比原样输出安全得多；
         *          - 空格：`weird name` 这类列名在真实库里确实存在（被引用的标识符允许含空格），
         *            而空格既不能出现在裸标识符里，也不会改变表达式结构——真正的表达式必然带
         *            运算符、括号或逗号，那些字节会让本判定为假，从而走「表达式原样输出」的分支。
         *          含运算符、括号、逗号等结构字符的文本因此仍被判为表达式，不会走到这里。
         * @param text 待判断的文本
         * @return true 可以直接加引号引用（空文本返回 false）
         */
        bool isQuotableIdentifier(const std::string_view text) noexcept
        {
            if (text.empty())
            {
                return false;
            }

            for (const char character: text)
            {
                // 双引号走「引用并翻倍」这条路径；空格同理，加上引号后它只是一个普通字符，
                // 不会破坏语句结构
                if (!isIdentifierByte(character) && character != '"' && character != ' ')
                {
                    return false;
                }
            }

            return true;
        }

        /**
         * @brief 把限定名按 '.' 切成若干段
         * @details 不处理引号内的点号：调用方传入的是列名或表达式文本，
         *          已经加好引号的文本不需要再切分（会走表达式分支原样输出）。
         * @param text 待切分的文本
         * @return std::vector<std::string_view> 各段视图，无点号时只含整体一段
         */
        std::vector<std::string_view> splitQualifiedName(const std::string_view text)
        {
            std::vector<std::string_view> segments;
            std::size_t                   segmentStart = 0;

            // 每遇到一个点号就切出一段（空段保留，交给 isSimpleIdentifier 判定为非法 → 走表达式分支）
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                if (text[index] == '.')
                {
                    segments.push_back(text.substr(segmentStart, index - segmentStart));
                    segmentStart = index + 1;
                }
            }
            segments.push_back(text.substr(segmentStart));

            return segments;
        }

    } // namespace

    DatabaseType SqliteDialect::type() const noexcept
    {
        return DatabaseType::Sqlite;
    }

    std::string SqliteDialect::quoteIdentifier(const std::string_view identifier) const
    {
        std::string quotedText;
        // 预分配：外层两个引号，加上最坏情况下每个字节都要翻倍
        quotedText.reserve(identifier.size() * 2 + 2);

        quotedText.push_back('"');
        for (const char character: identifier)
        {
            // SQL 标准（SQLite 同样遵循）用「引号翻倍」表示标识符内部的引号：
            // 反斜杠在 SQLite 里只是普通字符，用它转义既无效又会引入字面反斜杠
            if (character == '"')
            {
                quotedText.push_back('"');
            }
            quotedText.push_back(character);
        }
        quotedText.push_back('"');

        return quotedText;
    }

    std::string SqliteDialect::placeholder(const std::size_t index) const
    {
        // SQLite 的位置参数不区分类型、不区分序号，一律写作 '?'：
        // 序号参数在这里被忽略，它由调用方（appendParameter）用来确定参数在数组中的位置。
        // 保留 index 形参是为了让将来的 $1 风格方言能在不改动调用方代码的前提下用上它。
        static_cast<void>(index);
        return "?";
    }

    bool SqliteDialect::supportsLimitOffset() const noexcept
    {
        // SQLite 原生支持 "LIMIT n OFFSET m"，无需任何改写
        return true;
    }

    std::size_t SqliteDialect::maximumStatementParameters() const noexcept
    {
        // 常量 kMaximumStatementParameters 的取值来源见头文件说明：SQLITE_MAX_VARIABLE_NUMBER 的默认值。
        // 按保守值分块，即使目标 SQLite 用了更小的自定义上限，也只是多分几批而不会失败
        return kMaximumStatementParameters;
    }

    std::string SqliteDialect::renderFieldReference(const std::string_view fieldText) const
    {
        // 单个通配符不是标识符：加引号会得到一个名为 "*" 的列，语义完全不同
        if (fieldText == "*")
        {
            return "*";
        }

        const std::vector<std::string_view> segments = splitQualifiedName(fieldText);

        // 只有每一段都是可引用的标识符（或通配符）时才按标识符渲染，否则整体视为表达式
        bool isIdentifierChain = !segments.empty();
        for (const std::string_view segment: segments)
        {
            if (segment == "*")
            {
                continue;
            }
            if (!isQuotableIdentifier(segment))
            {
                isIdentifierChain = false;
                break;
            }
        }

        if (!isIdentifierChain)
        {
            // 含运算符、括号、逗号等结构字符的文本按表达式原样输出：例如 COUNT(*)，
            // COALESCE(age, 0)，age + 1。对表达式整体加引号会把它降级成一个列名，
            // 直接改变语义；这里不做任何加工在安全上也是成立的——字段引用全部来自编译期常量
            // （Column() 的 columnName 参数或 asc()/desc() 的字符串字面量），不是外部输入，
            // 而数据值一律走参数绑定，因此不存在注入面。
            // 只含标识符字节与空格的文本（如含空格的列名）不走这里，会被引用成 "weird name"
            return std::string(fieldText);
        }

        std::string renderedText;
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            if (index > 0)
            {
                renderedText.push_back('.');
            }

            if (segments[index] == "*")
            {
                // users.* 里的通配符同样不加引号
                renderedText.push_back('*');
            }
            else
            {
                // 标识符一律加引号：既能容纳 order、group 这类保留字列名，也避免大小写折叠带来的歧义
                renderedText += quoteIdentifier(segments[index]);
            }
        }

        return renderedText;
    }

    std::string SqliteDialect::renderTableReference(const Queryable::QueryNode &query) const
    {
        std::string tableReference = quoteIdentifier(query.tableName);
        if (!query.tableAlias.empty())
        {
            // 别名同样加引号：不加引号的别名遇到保留字（order、group）会被当成关键字
            tableReference += " AS ";
            tableReference += quoteIdentifier(query.tableAlias);
        }
        return tableReference;
    }

    void SqliteDialect::appendWhereClause(std::string &sqlText,
                                          std::vector<DatabaseValue> &parameters,
                                          const Queryable::QueryNode &query) const
    {
        // 没有条件就整段不输出：SELECT 得到全表查询、DELETE 得到整表删除，
        // 都是 SQL 本身的语义，不在这一层额外补 "WHERE 1 = 1" 之类的伪条件
        if (query.whereConditions.empty())
        {
            return;
        }

        sqlText += " WHERE ";
        for (std::size_t index = 0; index < query.whereConditions.size(); ++index)
        {
            // 顶层多个条件按 ORM 的约定以 AND 连接（Queryable::where() 多次调用即「同时满足」）
            if (index > 0)
            {
                sqlText += " AND ";
            }
            appendCondition(sqlText, parameters, query.whereConditions[index]);
        }
    }

    void SqliteDialect::appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const
    {
        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            // 列名一律走 renderFieldReference()：与 SELECT 列表用同一套引用/表达式判定规则
            sqlText += renderFieldReference(query.selectColumns[index]);
        }
    }

    void SqliteDialect::appendValueRow(std::string &sqlText,
                                       std::vector<DatabaseValue> &parameters,
                                       const std::span<const DatabaseValue> rowValues) const
    {
        sqlText += '(';
        for (std::size_t index = 0; index < rowValues.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            // 先取占位符序号再压参数：序号就是「当前已收集的参数个数」，
            // 顺序颠倒会让 $1 风格方言上的序号整体偏移（见 appendParameter 的同类说明）
            sqlText += placeholder(parameters.size());
            parameters.push_back(rowValues[index]);
        }
        sqlText += ')';
    }

    void SqliteDialect::requireMatchingColumnCount(const Queryable::QueryNode &query, const std::size_t valueCount)
    {
        if (query.selectColumns.empty())
        {
            throw std::invalid_argument("SQLite 方言：待写列列表为空，无法生成写语句（表 " +
                                        query.tableName + "）");
        }

        // 列与值对错位时生成的语句可能仍能执行，却会把值写进错误的列，
        // 这种错误在业务层极难定位，因此必须在翻译阶段就拦住
        if (valueCount != query.selectColumns.size())
        {
            throw std::invalid_argument("SQLite 方言：取值个数（" + std::to_string(valueCount) +
                                        "）与待写列数（" + std::to_string(query.selectColumns.size()) +
                                        "）不一致，无法生成写语句（表 " + query.tableName + "）");
        }
    }

    SqlStatement SqliteDialect::translate(const Queryable::QueryNode &query) const
    {
        SqlStatement                statement;
        std::string                &sqlText    = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        // ---------- SELECT 列 ----------
        sqlText += "SELECT ";
        if (query.selectColumns.empty())
        {
            // 查询树没有指定列时退化为通配符。ORM 层（Queryable<T>）在需要按 TableSchema
            // 的列序对齐结果时才显式展开列名，方言层不依赖任何模板参数，因此只能给出
            // 语法上最通用、顺序由数据库决定的 '*'
            sqlText += '*';
        }
        else
        {
            for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
            {
                if (index > 0)
                {
                    sqlText += ", ";
                }
                sqlText += renderFieldReference(query.selectColumns[index]);
            }
        }

        // ---------- FROM ----------
        sqlText += " FROM ";
        // 表名与别名的引用方式在四个方向上必须一致，因此统一走 renderTableReference()
        sqlText += renderTableReference(query);

        // ---------- JOIN ----------
        for (const Queryable::JoinClause &joinClause: query.joins)
        {
            sqlText += ' ';
            sqlText += joinTypeText(joinClause.type);
            sqlText += " JOIN ";
            sqlText += renderFieldReference(joinClause.tableName);
            if (!joinClause.tableAlias.empty())
            {
                sqlText += " AS ";
                sqlText += quoteIdentifier(joinClause.tableAlias);
            }

            // CROSS JOIN 按语义不接受 ON 子句，但查询树若显式填了条件就照写，
            // 由数据库报错而不是在这里静默丢弃调用方的意图
            if (!joinClause.conditions.empty())
            {
                sqlText += " ON ";
                for (std::size_t index = 0; index < joinClause.conditions.size(); ++index)
                {
                    if (index > 0)
                    {
                        sqlText += " AND ";
                    }
                    appendCondition(sqlText, parameters, joinClause.conditions[index]);
                }
            }
        }

        // ---------- WHERE ----------
        // 条件渲染与参数收集只有 appendWhereClause() 一份实现，UPDATE / DELETE 同样调用它
        appendWhereClause(sqlText, parameters, query);

        // ---------- GROUP BY ----------
        if (!query.groupBy.empty())
        {
            sqlText += " GROUP BY ";
            for (std::size_t index = 0; index < query.groupBy.size(); ++index)
            {
                if (index > 0)
                {
                    sqlText += ", ";
                }
                sqlText += renderFieldReference(query.groupBy[index].name);
            }
        }

        // ---------- HAVING ----------
        if (query.having.has_value())
        {
            sqlText += " HAVING ";
            // HAVING 的条件参数跟在 WHERE 参数之后，顺序与它在 SQL 中的出现位置一致
            appendCondition(sqlText, parameters, query.having.value());
        }

        // ---------- ORDER BY ----------
        if (!query.orderBy.empty())
        {
            sqlText += " ORDER BY ";
            for (std::size_t index = 0; index < query.orderBy.size(); ++index)
            {
                if (index > 0)
                {
                    sqlText += ", ";
                }
                sqlText += renderFieldReference(query.orderBy[index].field.name);
                // 方向必须显式写出：默认升序虽然与 SQL 一致，但显式 "ASC" 让生成的 SQL 可读且稳定
                sqlText += query.orderBy[index].descending ? " DESC" : " ASC";
            }
        }

        // ---------- LIMIT / OFFSET ----------
        if (query.limit.has_value())
        {
            // 分页值内联而不占占位符：它来自 QueryNode 的 std::size_t（强类型、非外部文本），
            // 不存在注入面；内联还能让 SQLite 在编译期就知道行数上限，避免额外的绑定步骤
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

        return statement;
    }

    SqlStatement SqliteDialect::translateInsert(const Queryable::QueryNode &query,
                                                const std::span<const DatabaseValue> values) const
    {
        requireMatchingColumnCount(query, values.size());

        SqlStatement statement;
        std::string &sqlText = statement.sql;

        // INSERT 不接受表别名（"INSERT INTO 表 AS 别名" 是语法错误），因此这里只引用表名。
        // 查询树在插入方向由 ORM 现造，本来就不带别名，此处显式忽略是防止误用
        sqlText += "INSERT INTO ";
        sqlText += quoteIdentifier(query.tableName);
        sqlText += " (";
        appendColumnList(sqlText, query);
        sqlText += ") VALUES ";
        // 单行插入就是「只有一行 VALUES」的批量插入，占位符与参数的收集规则完全一致
        appendValueRow(sqlText, statement.parameters, values);

        return statement;
    }

    SqlStatement SqliteDialect::translateUpdate(const Queryable::QueryNode &query,
                                                const std::span<const DatabaseValue> values) const
    {
        requireMatchingColumnCount(query, values.size());

        SqlStatement statement;
        std::string &sqlText = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        sqlText += "UPDATE ";
        sqlText += renderTableReference(query);
        sqlText += " SET ";

        // SET 子句先于 WHERE 输出，赋值参数因此排在条件参数之前，
        // 与文本里占位符的先后顺序严格一致（见 SqlStatement.h 的参数顺序契约）
        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            sqlText += renderFieldReference(query.selectColumns[index]);
            sqlText += " = ";
            sqlText += placeholder(parameters.size());
            // 赋值取值由 ORM 以 DatabaseValue 形式给出，已经是驱动可直接绑定的形态，无需再转换
            parameters.push_back(values[index]);
        }

        // WHERE 与 SELECT / DELETE 共用同一份渲染与参数收集实现
        appendWhereClause(sqlText, parameters, query);

        return statement;
    }

    SqlStatement SqliteDialect::translateDelete(const Queryable::QueryNode &query) const
    {
        SqlStatement statement;
        std::string &sqlText = statement.sql;

        sqlText += "DELETE FROM ";
        // 别名一并带上：WHERE 里以别名限定的列名（"u"."id"）只有别名在场才能被解析
        sqlText += renderTableReference(query);

        // 无条件时 appendWhereClause() 不输出任何内容，SQL 退化为整表删除，与 SQL 语义一致
        appendWhereClause(sqlText, statement.parameters, query);

        return statement;
    }

    SqlStatement SqliteDialect::translateInsertBatch(const Queryable::QueryNode &query,
                                                     const std::span<const std::vector<DatabaseValue>> rows) const
    {
        if (rows.empty())
        {
            // 零行插入没有合法写法（"VALUES" 后面必须有至少一组括号），
            // 静默返回一句只能插 0 行的语句会让调用方以为写入了数据，因此直接失败
            throw std::invalid_argument("SQLite 方言：批量插入的行集合为空，无法生成 INSERT 语句");
        }

        requireMatchingColumnCount(query, rows.front().size());

        // 逐行校验：长度不一致的 VALUES 行会被引擎拒绝（"all VALUES must have the same number of terms"），
        // 与其把错误留给数据库，不如在翻译阶段就指出调用方给的行与列数对不齐。
        // 校验成本只是一次整数比较，相对拼接 SQL 文本可以忽略
        for (const std::vector<DatabaseValue> &rowValues: rows)
        {
            requireMatchingColumnCount(query, rowValues.size());
        }

        SqlStatement statement;
        std::string &sqlText = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        // 参数总数 = 行数 × 列数，调用方可能只算了一遍列数，这里按最坏情况预留容量避免反复扩容
        parameters.reserve(query.selectColumns.size() * rows.size());

        sqlText += "INSERT INTO ";
        sqlText += quoteIdentifier(query.tableName);
        sqlText += " (";
        appendColumnList(sqlText, query);
        sqlText += ") VALUES ";

        // 逐行展开 "(?, ?), (?, ?)"：SQLite 自 3.7.11 起支持多行 VALUES，
        // 参数按「行优先、行内按列序」压入，与文本中占位符的先后完全对齐
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            if (rowIndex > 0)
            {
                sqlText += ", ";
            }
            appendValueRow(sqlText, parameters, rows[rowIndex]);
        }

        return statement;
    }

    std::string_view SqliteDialect::beginTransactionStatement() const noexcept
    {
        // IMMEDIATE 在 BEGIN 时就取写锁，避免 DEFERRED 事务在第一条写语句升级锁时撞上
        // SQLITE_BUSY（这类失败无法靠重试化解，因为两个连接会互相持有读锁）
        return "BEGIN IMMEDIATE";
    }

    std::string_view SqliteDialect::commitStatement() const noexcept
    {
        return "COMMIT";
    }

    std::string_view SqliteDialect::rollbackStatement() const noexcept
    {
        return "ROLLBACK";
    }

    std::string_view SqliteDialect::columnTypeName(const ColumnType type) const noexcept
    {
        // 映射依据见头文件：SQLite 只有 INTEGER / REAL / TEXT / BLOB 四个可用存储类，
        // 布尔与无符号整数都没有独立类型，只能落在 INTEGER 上
        switch (type)
        {
            case ColumnType::Int64:  return "INTEGER";
            case ColumnType::UInt64: return "INTEGER";
            case ColumnType::Double: return "REAL";
            case ColumnType::Bool:   return "INTEGER";
            case ColumnType::Text:   return "TEXT";
            case ColumnType::Blob:   return "BLOB";
            default:
                // 本方法是 noexcept 且被建表语句生成直接调用，没有可回退的分支：
                // 遇到将来新增而本实现尚未认得的枚举取值，返回最宽容的 TEXT，
                // 让表能被建出来（值仍可通过绑定写出）而不是让整个过程崩掉
                return "TEXT";
        }
    }

    SqlStatement SqliteDialect::tableExistsStatement(const std::string_view tableName) const
    {
        SqlStatement statement;

        // sqlite_master 是当前库文件的只读元数据表：type='table' 过滤掉索引/视图/触发器，
        // 再按 name 精确匹配。表名走占位符，含引号或分号的表名也只是普通文本
        statement.sql = "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ";
        statement.sql += placeholder(statement.parameters.size());
        statement.parameters.emplace_back(std::string(tableName));

        return statement;
    }

    void SqliteDialect::appendCondition(std::string &sqlText,
                                        std::vector<DatabaseValue> &parameters,
                                        const Queryable::WhereCondition &condition) const
    {
        using Queryable::SqlOperator;

        switch (condition.op)
        {
            case SqlOperator::And:
            {
                // 空 children 的 AND 按恒真处理：既不产出非法 SQL，也保持「空条件不限制结果」的语义
                if (condition.children.empty())
                {
                    sqlText += "(1 = 1)";
                    return;
                }

                // 括号不能省：父节点可能是 OR，去掉括号会改变结合性
                sqlText += '(';
                for (std::size_t index = 0; index < condition.children.size(); ++index)
                {
                    if (index > 0)
                    {
                        sqlText += " AND ";
                    }
                    appendCondition(sqlText, parameters, condition.children[index]);
                }
                sqlText += ')';
                return;
            }

            case SqlOperator::Or:
            {
                // 空 children 的 OR 按恒假处理，与 AND 的恒真形成对偶
                if (condition.children.empty())
                {
                    sqlText += "(1 = 0)";
                    return;
                }

                sqlText += '(';
                for (std::size_t index = 0; index < condition.children.size(); ++index)
                {
                    if (index > 0)
                    {
                        sqlText += " OR ";
                    }
                    appendCondition(sqlText, parameters, condition.children[index]);
                }
                sqlText += ')';
                return;
            }

            case SqlOperator::Not:
            {
                if (condition.children.empty())
                {
                    // 没有子条件的 NOT 视为恒假（NOT 恒真），与旧实现的 (1 = 1) 语义一致
                    sqlText += "NOT (1 = 1)";
                    return;
                }

                // NOT 后面必须带括号：否则 "NOT a = ?" 在多数数据库里会被解析成 "(NOT a) = ?"
                sqlText += "NOT (";
                appendCondition(sqlText, parameters, condition.children[0]);
                sqlText += ')';
                return;
            }

            case SqlOperator::IsNull:
            {
                sqlText += renderFieldReference(condition.left.name);
                // IS NULL 不接受右操作数，也绝不绑定参数：NULL 的比较必须用 IS 而不是 "= NULL"
                sqlText += " IS NULL";
                return;
            }

            case SqlOperator::IsNotNull:
            {
                sqlText += renderFieldReference(condition.left.name);
                sqlText += " IS NOT NULL";
                return;
            }

            case SqlOperator::In:
            case SqlOperator::NotIn:
            {
                sqlText += renderFieldReference(condition.left.name);
                sqlText += (condition.op == SqlOperator::In) ? " IN " : " NOT IN ";

                if (condition.inValues.empty())
                {
                    // "IN ()" 是非法 SQL，不能生成；空集合在集合语义下「一个都不匹配」，
                    // 因此 IN 换成恒假、NOT IN 换成恒真，等价、确定且不占用参数
                    sqlText += (condition.op == SqlOperator::In) ? "(1 = 0)" : "(1 = 1)";
                    return;
                }

                sqlText += '(';
                for (std::size_t index = 0; index < condition.inValues.size(); ++index)
                {
                    if (index > 0)
                    {
                        sqlText += ", ";
                    }
                    // 集合元素逐个产出占位符并同步压参数，参数顺序与占位符位置严格一致
                    appendParameter(sqlText, parameters, condition.inValues[index]);
                }
                sqlText += ')';
                return;
            }

            default:
                break;
        }

        // ---------- 叶子比较 ----------
        sqlText += renderFieldReference(condition.left.name);
        sqlText += ' ';
        sqlText += comparisonOperatorText(condition.op);
        sqlText += ' ';

        // 右操作数既可能是参数值，也可能是另一个字段引用（列-列比较，如 "age" > "min_age"）
        if (std::holds_alternative<Queryable::FieldReference>(condition.right))
        {
            // 列-列比较两侧都是标识符，不需要也不能绑定参数
            sqlText += renderFieldReference(std::get<Queryable::FieldReference>(condition.right).name);
        }
        else
        {
            appendParameter(sqlText, parameters, std::get<Queryable::ParameterValue>(condition.right));
        }
    }

    void SqliteDialect::appendParameter(std::string &sqlText,
                                        std::vector<DatabaseValue> &parameters,
                                        const Queryable::ParameterValue &parameter) const
    {
        // 占位符的序号就是「当前已收集的参数个数」，所以必须先取序号再压参数：
        // 顺序颠倒会让序号比实际位置大 1，$1 风格方言上就会错位
        sqlText += placeholder(parameters.size());
        parameters.push_back(convertParameter(parameter));
    }

    DatabaseValue SqliteDialect::convertParameter(const Queryable::ParameterValue &parameter)
    {
        return std::visit(
            [](const auto &value) -> DatabaseValue
            {
                using ValueType = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<ValueType, std::nullptr_t>)
                {
                    // ORM 用 nullptr 表达 SQL NULL，驱动层用 std::monostate 表达，二者语义相同
                    return std::monostate{};
                }
                else if constexpr (std::is_same_v<ValueType, std::uint64_t>)
                {
                    // DatabaseValue 已冻结，没有无符号备选，这里做两步降级：
                    // - 放得进 int64_t：转成有符号整数，保持整数比较语义，也让索引与算术可用；
                    // - 超出 int64_t：转成十进制文本。宁可让比较按文本进行（而不是静默回绕成负数
                    //   给出错误数值），也不引入第二套无符号类型；这类取值现实中只出现在
                    //   哈希 ID、位掩码等场景，文本比较通常仍能得到正确结果
                    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    {
                        return static_cast<std::int64_t>(value);
                    }
                    return std::to_string(value);
                }
                else
                {
                    // bool / int64_t / double / std::string 在两种 variant 中同名同类型，直接构造即可
                    return DatabaseValue{value};
                }
            },
            parameter);
    }

    std::string_view SqliteDialect::comparisonOperatorText(const Queryable::SqlOperator sqlOperator) noexcept
    {
        using Queryable::SqlOperator;

        switch (sqlOperator)
        {
            case SqlOperator::Eq:    return "=";
            case SqlOperator::Neq:   return "!=";
            case SqlOperator::Gt:    return ">";
            case SqlOperator::Ge:    return ">=";
            case SqlOperator::Lt:    return "<";
            case SqlOperator::Le:    return "<=";
            case SqlOperator::Like:  return "LIKE";
            case SqlOperator::In:    return "IN";
            case SqlOperator::NotIn: return "NOT IN";
            default:                 return "=";
        }
    }

    std::string_view SqliteDialect::joinTypeText(const Queryable::JoinType joinType) noexcept
    {
        using Queryable::JoinType;

        switch (joinType)
        {
            case JoinType::Inner: return "INNER";
            case JoinType::Left:  return "LEFT";
            // SQLite 3.39.0 起支持 RIGHT JOIN，更早的版本会在编译期直接报语法错误，
            // 这里照写意图，让数据库给出明确错误而不是被上层悄悄改成 LEFT
            case JoinType::Right: return "RIGHT";
            case JoinType::Cross: return "CROSS";
            default:              return "INNER";
        }
    }

} // namespace AsynGyanis::Database
