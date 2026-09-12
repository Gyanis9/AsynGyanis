/**
 * @file MySqlDialect.cpp
 * @brief MySQL 方言实现 —— 查询树到参数化 SQL 的翻译
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/MySqlDialect.h"

#include <cstdint>
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
         * @details 允许出现反引号：那正是「列名里带反引号、必须翻倍转义」的情形，
         *          交给 quoteIdentifier() 处理比原样输出安全得多。
         *          含运算符、括号、空格的文本会在上层被判为表达式，不会走到这里。
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
                // MySQL 的引用符是反引号（'`'），因此带反引号的名字仍走「引用并翻倍」这条路径
                if (!isIdentifierByte(character) && character != '`')
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

            // 每遇到一个点号就切出一段（空段保留，交给可引用判定为非法 → 走表达式分支）
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

        /**
         * @brief 把分页取值（size_t）转换成驱动可绑定的统一值
         * @details DatabaseValue 已冻结、没有无符号备选，这里沿用与 convertParameter 相同的两步降级：
         *          放得进 int64_t 就按有符号整数绑定（占位符在 LIMIT/OFFSET 上要求整数，
         *          MySQL 会拒绝字符串类型的参数），超出范围的极端页码转成十进制文本。
         *          现实中的页码远小于 INT64_MAX（9.2e18 行），该分支只为不静默回绕成负数而存在。
         * @param pageNumber 行数上限或偏移量
         * @return DatabaseValue 驱动可直接绑定的统一值
         */
        DatabaseValue pageNumberToDatabaseValue(const std::size_t pageNumber)
        {
            if (pageNumber <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return static_cast<std::int64_t>(pageNumber);
            }

            return std::to_string(pageNumber);
        }

    } // namespace

    DatabaseType MySqlDialect::type() const noexcept
    {
        return DatabaseType::MySql;
    }

    std::string MySqlDialect::quoteIdentifier(const std::string_view identifier) const
    {
        std::string quotedText;
        // 预分配：外层两个反引号，加上最坏情况下每个字节都要翻倍
        quotedText.reserve(identifier.size() * 2 + 2);

        quotedText.push_back('`');
        for (const char character: identifier)
        {
            // MySQL 用「引用符翻倍」表示标识符内部的引用符（`weird``name`），
            // 反斜杠在这里只是普通字符：用它转义既无效又会引入字面反斜杠
            if (character == '`')
            {
                quotedText.push_back('`');
            }
            quotedText.push_back(character);
        }
        quotedText.push_back('`');

        return quotedText;
    }

    std::string MySqlDialect::placeholder(const std::size_t index) const
    {
        // MySQL 的位置参数不区分类型、不区分序号，一律写作 '?'（文本协议与 mysql_stmt_* 都是如此）：
        // 序号参数在这里被忽略，它由调用方（appendParameter 等）用来确定参数在数组中的位置。
        // 保留 index 形参是为了让将来的 $1 风格方言能在不改动调用方代码的前提下用上它
        static_cast<void>(index);
        return "?";
    }

    bool MySqlDialect::supportsLimitOffset() const noexcept
    {
        // MySQL 原生支持 "LIMIT n OFFSET m"，无需任何改写
        return true;
    }

    std::size_t MySqlDialect::maximumStatementParameters() const noexcept
    {
        // 常量取值的依据（预处理协议 2 字节的参数个数字段）见头文件说明。
        // 它不像 SQLite 的 999 那样可能被编译期宏调小，是协议层硬上限；真正的变量是
        // max_allowed_packet（整条语句文本的字节数），因此调用方还须控制单批的数据量
        return kMaximumStatementParameters;
    }

    std::string MySqlDialect::renderFieldReference(const std::string_view fieldText) const
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
            // 含运算符、括号、空格等非标识符字节的文本按表达式原样输出：例如 COUNT(*)，
            // COALESCE(age, 0)，age + 1。对表达式整体加反引号会把它降级成一个列名，
            // 直接改变语义；这里不做任何加工在安全上也是成立的——字段引用全部来自编译期常量
            // （Column() 的 columnName 参数或 asc()/desc() 的字符串字面量），不是外部输入，
            // 而数据值一律走参数绑定，因此不存在注入面。
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
                // 标识符一律加反引号：既能容纳 order、group 这类保留字列名，也能容纳含空格的列名
                renderedText += quoteIdentifier(segments[index]);
            }
        }

        return renderedText;
    }

    std::string MySqlDialect::renderTableReference(const Queryable::QueryNode &query) const
    {
        std::string tableReference = quoteIdentifier(query.tableName);
        if (!query.tableAlias.empty())
        {
            // 别名同样加反引号：不加引号的别名遇到保留字（order、group）会被当成关键字
            tableReference += " AS ";
            tableReference += quoteIdentifier(query.tableAlias);
        }
        return tableReference;
    }

    void MySqlDialect::appendWhereClause(std::string &sqlText,
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

    void MySqlDialect::appendLimitOffsetClause(std::string &sqlText,
                                               std::vector<DatabaseValue> &parameters,
                                               const Queryable::QueryNode &query) const
    {
        // 分页值一个都没给时不输出任何内容（与 SQLite 方言不同：那边只用 OFFSET 才需要补 LIMIT，
        // 这里两个都不给同样什么都不输出）
        if (!query.limit.has_value() && !query.offset.has_value())
        {
            return;
        }

        sqlText += " LIMIT ";
        if (query.limit.has_value())
        {
            // LIMIT 的占位符排在前，与 QueryNode 的 limit / offset 字段顺序一致；
            // 先取序号再压参数，保证 parameters[i] 与第 i 个占位符严格对应
            sqlText += placeholder(parameters.size());
            parameters.push_back(pageNumberToDatabaseValue(query.limit.value()));
        }
        else
        {
            // MySQL 不允许 OFFSET 单独出现，也没有 SQLite 可用的 "LIMIT -1"（LIMIT 给负值会被判非法）。
            // 官方文档给出的「不限行数」写法就是无符号 64 位整数的上界；它是编译期常量，
            // 不引入注入面，也不占用绑定参数（占位符个数仍与参数个数一一对应）
            sqlText += kUnboundedRowLimitLiteral;
        }

        if (query.offset.has_value())
        {
            sqlText += " OFFSET ";
            sqlText += placeholder(parameters.size());
            parameters.push_back(pageNumberToDatabaseValue(query.offset.value()));
        }
    }

    void MySqlDialect::appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const
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

    void MySqlDialect::appendValueRow(std::string &sqlText,
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

    void MySqlDialect::requireMatchingColumnCount(const Queryable::QueryNode &query, const std::size_t valueCount)
    {
        if (query.selectColumns.empty())
        {
            throw std::invalid_argument("MySQL 方言：待写列列表为空，无法生成写语句（表 " +
                                        query.tableName + "）");
        }

        // 列与值对错位时生成的语句可能仍能执行，却会把值写进错误的列，
        // 这种错误在业务层极难定位，因此必须在翻译阶段就拦住
        if (valueCount != query.selectColumns.size())
        {
            throw std::invalid_argument("MySQL 方言：取值个数（" + std::to_string(valueCount) +
                                        "）与待写列数（" + std::to_string(query.selectColumns.size()) +
                                        "）不一致，无法生成写语句（表 " + query.tableName + "）");
        }
    }

    SqlStatement MySqlDialect::translate(const Queryable::QueryNode &query) const
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
        // 分页是 MySQL 与 SQLite 差异最大的子句（语法形式、OFFSET 的前置约束都不同），
        // 因此集中在 appendLimitOffsetClause() 里，并在那里收集分页参数
        appendLimitOffsetClause(sqlText, parameters, query);

        return statement;
    }

    SqlStatement MySqlDialect::translateInsert(const Queryable::QueryNode &query,
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

    SqlStatement MySqlDialect::translateUpdate(const Queryable::QueryNode &query,
                                               const std::span<const DatabaseValue> values) const
    {
        requireMatchingColumnCount(query, values.size());

        SqlStatement                statement;
        std::string                &sqlText    = statement.sql;
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

    SqlStatement MySqlDialect::translateDelete(const Queryable::QueryNode &query) const
    {
        SqlStatement statement;
        std::string &sqlText = statement.sql;

        sqlText += "DELETE FROM ";
        // 别名一并带上：WHERE 里以别名限定的列名（`u`.`id`）只有别名在场才能被解析
        sqlText += renderTableReference(query);

        // 无条件时 appendWhereClause() 不输出任何内容，SQL 退化为整表删除，与 SQL 语义一致
        appendWhereClause(sqlText, statement.parameters, query);

        return statement;
    }

    SqlStatement MySqlDialect::translateInsertBatch(const Queryable::QueryNode &query,
                                                    const std::span<const std::vector<DatabaseValue>> rows) const
    {
        if (rows.empty())
        {
            // 零行插入没有合法写法（"VALUES" 后面必须有至少一组括号），
            // 静默返回一句只能插 0 行的语句会让调用方以为写入了数据，因此直接失败
            throw std::invalid_argument("MySQL 方言：批量插入的行集合为空，无法生成 INSERT 语句");
        }

        requireMatchingColumnCount(query, rows.front().size());

        // 逐行校验：长度不一致的 VALUES 行会被引擎拒绝（"Column count doesn't match value count at row N"），
        // 与其把错误留给数据库，不如在翻译阶段就指出调用方给的行与列数对不齐。
        // 校验成本只是一次整数比较，相对拼接 SQL 文本可以忽略
        for (const std::vector<DatabaseValue> &rowValues: rows)
        {
            requireMatchingColumnCount(query, rowValues.size());
        }

        SqlStatement                statement;
        std::string                &sqlText    = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        // 参数总数 = 行数 × 列数，调用方可能只算了一遍列数，这里按最坏情况预留容量避免反复扩容
        parameters.reserve(query.selectColumns.size() * rows.size());

        sqlText += "INSERT INTO ";
        sqlText += quoteIdentifier(query.tableName);
        sqlText += " (";
        appendColumnList(sqlText, query);
        sqlText += ") VALUES ";

        // 逐行展开 "(?, ?), (?, ?)"：MySQL / MariaDB 原生支持多行 VALUES，
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

    void MySqlDialect::appendCondition(std::string &sqlText,
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
                    // 没有子条件的 NOT 视为恒假（NOT 恒真）
                    sqlText += "NOT (1 = 1)";
                    return;
                }

                // NOT 后面必须带括号：否则 "NOT a = ?" 在 MySQL 里会被解析成 "(NOT a) = ?"
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

    void MySqlDialect::appendParameter(std::string &sqlText,
                                       std::vector<DatabaseValue> &parameters,
                                       const Queryable::ParameterValue &parameter) const
    {
        // 占位符的序号就是「当前已收集的参数个数」，所以必须先取序号再压参数：
        // 顺序颠倒会让序号比实际位置大 1，$1 风格方言上就会错位
        sqlText += placeholder(parameters.size());
        parameters.push_back(convertParameter(parameter));
    }

    DatabaseValue MySqlDialect::convertParameter(const Queryable::ParameterValue &parameter)
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

    std::string_view MySqlDialect::comparisonOperatorText(const Queryable::SqlOperator sqlOperator) noexcept
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

    std::string_view MySqlDialect::joinTypeText(const Queryable::JoinType joinType) noexcept
    {
        using Queryable::JoinType;

        switch (joinType)
        {
            case JoinType::Inner: return "INNER";
            case JoinType::Left:  return "LEFT";
            case JoinType::Right: return "RIGHT";
            // CROSS JOIN 在 MySQL 里与 "INNER JOIN ... "（无 ON）等价，关键字照写
            case JoinType::Cross: return "CROSS";
            default:              return "INNER";
        }
    }

} // namespace AsynGyanis::Database
