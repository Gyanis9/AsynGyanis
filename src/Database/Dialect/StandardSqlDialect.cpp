#include "Database/Dialect/StandardSqlDialect.h"

#include <algorithm>

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        constexpr unsigned char kUtf8ContinuationLowerBound = 0x80; ///< UTF-8 多字节序列的首字节与后续字节都 >= 0x80，用于把中文列名识别为合法标识符

        /**
         * @brief 判断单个字节能否出现在不带引号的标识符里
         * @param character 待判断的字节
         * @return true 字母、数字、下划线，或 UTF-8 多字节序列的一部分
         */
        bool isIdentifierByte(const char character) noexcept
        {
            const auto byte = static_cast<unsigned char>(character);

            // 手写字符区间而不用 std::isalnum：后者受当前 locale 影响，
            // 且要求参数可表示为 unsigned char（负数直接传给它是未定义行为）
            const bool isAsciiLetter = (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
                                       (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'));
            const bool isAsciiDigit = byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9');

            return isAsciiLetter || isAsciiDigit || byte == static_cast<unsigned char>('_') ||
                   byte >= kUtf8ContinuationLowerBound;
        }

        /**
         * @brief 判断文本是否是一个可以安全加引用字符的标识符
         * @details 在裸标识符字节之外还放行本引擎的引用字符（列名里带引用符的情形，
         *          交给 quoteIdentifier() 翻倍转义比原样输出安全）与空格（"full name" 这类
         *          列名真实存在，且空格不会改变表达式结构——真正的表达式必然带运算符、
         *          括号或逗号，那些字节会让本判定为假，从而走「表达式原样输出」分支）。
         * @param text 待判断的文本
         * @param quoteCharacter 本引擎的标识符引用字符
         * @return true 可以直接加引用（空文本返回 false）
         */
        bool isQuotableIdentifier(const std::string_view text, const char quoteCharacter) noexcept
        {
            if (text.empty())
            {
                return false;
            }

            if (!std::ranges::all_of(text, [quoteCharacter](const char character)
            {
                return isIdentifierByte(character)
                       || character == quoteCharacter
                       || character == ' ';
            }))
            {
                return false;
            }

            return true;
        }

        /**
         * @brief 判断整串文本是否是「可引用标识符以点号相连」的限定名
         * @details 与逐段渲染共用同一套扫描规则：空段（"a..b"、".a"、"a."）判否，交给表达式分支
         *          原样输出；"*" 段算合法（users.* 是合法写法），但渲染时不加引用。
         *          判定与渲染都就地扫描，不先切出一个分段容器——一条查询要判七八次，
         *          每次那份 std::vector<std::string_view> 就是一次堆分配。
         * @param text 待判断的文本
         * @param quoteCharacter 本引擎的标识符引用字符
         * @return true 整串可按限定名渲染
         */
        bool isQuotableQualifiedName(const std::string_view text, const char quoteCharacter) noexcept
        {
            std::size_t segmentStart = 0;
            for (;;)
            {
                const std::size_t dotPosition = text.find('.', segmentStart);
                // dotPosition 为 npos 时长度自动覆盖到串尾，不必为末尾那段单开分支
                const std::string_view segment = text.substr(segmentStart, dotPosition - segmentStart);
                if (segment != "*" && !isQuotableIdentifier(segment, quoteCharacter))
                {
                    return false;
                }

                if (dotPosition == std::string_view::npos)
                {
                    return true;
                }
                segmentStart = dotPosition + 1;
            }
        }

        /**
         * @brief 估算一条查询的 SQL 文本产出长度，供拼接缓冲一次性预留容量
         * @details std::string 从空串起按 15→31→63→127 逐次翻倍，一条十几个标识符的 SELECT
         *          中途要换三次堆缓冲。这里按「标识符长度 × 2（外层引用符与最坏情况的翻倍转义）
         *          + 每个片段一组关键字与标点的余量」估一个上界；条件树只在顶层按片段计一次，
         *          嵌套条件因此可能估少——估少只是退回原来的翻倍路径，估多则白占几十字节，
         *          两种偏差都不影响产出文本。
         * @param query 待翻译的查询树
         * @return std::size_t 预留字节数
         */
        [[nodiscard]] std::size_t estimateSqlTextCapacity(const Queryable::QueryNode &query) noexcept
        {
            // 单个片段的固定文本余量：", "、" = "、括号、占位符与关键字
            constexpr std::size_t kPerFragmentOverheadBytes = 16;
            // "SELECT  FROM WHERE GROUP BY HAVING ORDER BY LIMIT OFFSET" 这些骨架关键字的余量
            constexpr std::size_t kClauseSkeletonBytes = 64;

            std::size_t identifierBytes = query.tableName.size() + query.tableAlias.size();
            for (const std::string &column: query.selectColumns)
            {
                identifierBytes += column.size();
            }
            for (const Queryable::JoinClause &joinClause: query.joins)
            {
                identifierBytes += joinClause.tableName.size() + joinClause.tableAlias.size();
            }
            for (const Queryable::FieldReference &field: query.groupBy)
            {
                identifierBytes += field.name.size();
            }
            for (const Queryable::OrderByClause &order: query.orderBy)
            {
                identifierBytes += order.field.name.size();
            }

            const std::size_t fragmentCount = query.selectColumns.size() + query.joins.size() + query.groupBy.size()
                                            + query.orderBy.size() + query.whereConditions.size() + 1;

            return kClauseSkeletonBytes + identifierBytes * 2 + fragmentCount * kPerFragmentOverheadBytes;
        }

        /**
         * @brief 统计一棵条件树会产出多少个绑定参数
         * @details 递归形状与 appendCondition 的分支一一对应：IN 按元素个数计，IS NULL 系与
         *          列-列比较不占参数。两处规则必须同步，否则预留容量偏小又会走扩容。
         * @param condition 条件节点
         * @return std::size_t 本节点及其子树产生的参数个数
         */
        [[nodiscard]] std::size_t countConditionParameters(const Queryable::WhereCondition &condition) noexcept
        {
            using Queryable::SqlOperator;

            switch (condition.op)
            {
                case SqlOperator::And:
                case SqlOperator::Or:
                case SqlOperator::Not:
                {
                    // Not 与 And/Or 同形：参数全部来自子条件。多于一个子条件时渲染阶段会先因
                    // 取非语义不确定而拒绝，这里按全部子条件累加只为守住
                    // 「递归形状与 appendCondition 的分支一一对应」那条约定
                    std::size_t parameterCount = 0;
                    for (const Queryable::WhereCondition &child: condition.children)
                    {
                        parameterCount += countConditionParameters(child);
                    }
                    return parameterCount;
                }

                case SqlOperator::IsNull:
                case SqlOperator::IsNotNull:
                    return 0;

                case SqlOperator::In:
                case SqlOperator::NotIn:
                    // 空集合换成恒假/恒真写法，一个参数都不占
                    return condition.inValues.size();

                default:
                    return std::holds_alternative<Queryable::FieldReference>(condition.right) ? 0 : 1;
            }
        }

    } // namespace

    std::string StandardSqlDialect::quoteIdentifier(const std::string_view identifier) const
    {
        std::string quotedText;
        // 预分配：外层两个引用字符，加上最坏情况下每个字节都要翻倍
        quotedText.reserve(identifier.size() * 2 + 2);
        appendQuotedIdentifier(quotedText, identifier);
        return quotedText;
    }

    void StandardSqlDialect::appendQuotedIdentifier(std::string &sqlText, const std::string_view identifier) const
    {
        const char quoteCharacter = identifierQuoteCharacter();

        sqlText.push_back(quoteCharacter);
        for (const char character: identifier)
        {
            // 内部引用字符按 SQL 规则翻倍表示（"a""b" / `a``b`）：
            // 反斜杠在三种引擎里都只是普通字符，用它转义既无效又会引入字面反斜杠
            if (character == quoteCharacter)
            {
                sqlText.push_back(quoteCharacter);
            }
            sqlText.push_back(character);
        }
        sqlText.push_back(quoteCharacter);
    }

    bool StandardSqlDialect::supportsLimitOffset() const noexcept
    {
        // SQLite / MySQL 都原生支持关键字形式的分页，差异只在
        // 「OFFSET 能否单独出现」这类细节上，由 appendLimitOffsetClause() 各自处理
        return true;
    }

    void StandardSqlDialect::appendFieldReference(std::string &sqlText, const std::string_view fieldText) const
    {
        // 空文本既不是列名也不是表达式：交给下面的表达式分支会「什么都不输出」，
        // 产出 SELECT  FROM "t" 这种要到服务端才报语法错误的语句，指不出是哪一项空了
        if (fieldText.empty())
        {
            throw Base::InvalidArgumentException(std::string(dialectName())
                                                + " 方言：字段名为空，无法生成引用。SELECT 列表、GROUP BY、ORDER BY 与条件左值"
                                                  "都必须是非空的列名或表达式（如 \"id\"、\"t.id\"、\"COUNT(*)\"）");
        }

        // 单个通配符不是标识符：加引用会得到一个名为 "*" 的列，语义完全不同
        if (fieldText == "*")
        {
            sqlText.push_back('*');
            return;
        }

        const char quoteCharacter = identifierQuoteCharacter();

        if (!isQuotableQualifiedName(fieldText, quoteCharacter))
        {
            // 含运算符、括号、逗号等结构字符的文本按表达式原样输出：例如 COUNT(*)，
            // COALESCE(age, 0)，age + 1。对表达式整体加引用会把它降级成一个列名，直接改变语义。
            // **信任边界**：这一段按原样拼进 SQL，因此字段引用只允许来自编译期常量
            // （Column() 的 columnName 参数或 asc()/desc() 的字符串字面量）等可信文本；
            // 数据值一律走参数绑定。调用方若把用户输入喂进 select()/groupBy()，那就是注入面——
            // 公共 API 的文档已就这条边界给出 @warning
            // 只含标识符字节与空格的文本（如含空格的列名）不走这里，会被引用成 "full name"
            sqlText.append(fieldText);
            return;
        }

        // 判定与渲染各扫一遍串（几十字节），比为了分段再建一个容器划算
        std::size_t segmentStart = 0;
        for (;;)
        {
            const std::size_t dotPosition = fieldText.find('.', segmentStart);
            const std::string_view segment = fieldText.substr(segmentStart, dotPosition - segmentStart);

            if (segment == "*")
            {
                // users.* 里的通配符同样不加引用
                sqlText.push_back('*');
            } else
            {
                // 标识符一律加引用：既能容纳 order、group 这类保留字列名，也避免大小写折叠带来的歧义
                appendQuotedIdentifier(sqlText, segment);
            }

            if (dotPosition == std::string_view::npos)
            {
                return;
            }
            sqlText.push_back('.');
            segmentStart = dotPosition + 1;
        }
    }

    void StandardSqlDialect::appendTableReference(std::string &sqlText, const std::string_view tableName,
                                                  const std::string_view tableAlias) const
    {
        // 空表名加引用会得到 `""` / "" 这样一个合法但必定不存在的标识符，
        // 报出来的错在服务器侧（"no such table"），指不到「查询树根本没填表名」这个真因
        if (tableName.empty())
        {
            throw Base::InvalidArgumentException(std::string(dialectName())
                                                + " 方言：查询树的表名为空，无法生成语句。请填写 QueryNode::tableName"
                                                  "（Queryable 走 TableSchema<T>::kTableName，特化时别留空）");
        }

        // 点号在 SQL 里是「库.表」的层级分隔，因此逐段引用：整块包起来会得到一张名叫 shop.orders
        // 的表，而不是 shop 库下的 orders。段与段之间只判空（"a..b" 生成的是不合法的引用），
        // **不做字符校验**——加了引用的标识符能容纳任何字节（含空格、连字符的名字都合法），
        // 校验只会把合法名字判死，而「原样拼进语句」这条路本来就由引用堵住了
        std::size_t segmentStart = 0;
        for (;;)
        {
            const std::size_t dotPosition = tableName.find('.', segmentStart);
            const std::string_view segment = tableName.substr(segmentStart, dotPosition - segmentStart);
            if (segment.empty())
            {
                throw Base::InvalidArgumentException(std::string(dialectName())
                                                    + " 方言：表名「" + std::string(tableName)
                                                    + "」有点号相邻的空段，无法逐段引用。"
                                                      "要指定库/模式前缀就写成 \"schema.table\"，两侧都要有名字");
            }
            appendQuotedIdentifier(sqlText, segment);

            if (dotPosition == std::string_view::npos)
            {
                break;
            }
            sqlText.push_back('.');
            segmentStart = dotPosition + 1;
        }

        if (!tableAlias.empty())
        {
            // 别名同样加引用：不加引用的别名遇到保留字（order、group）会被当成关键字
            sqlText += " AS ";
            appendQuotedIdentifier(sqlText, tableAlias);
        }
    }

    void StandardSqlDialect::appendTableReference(std::string &sqlText, const Queryable::QueryNode &query) const
    {
        appendTableReference(sqlText, query.tableName, query.tableAlias);
    }

    void StandardSqlDialect::appendWhereClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const
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

    void StandardSqlDialect::appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const
    {
        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            // 列名一律走 appendFieldReference()：与 SELECT 列表用同一套引用/表达式判定规则
            appendFieldReference(sqlText, query.selectColumns[index]);
        }
    }

    void StandardSqlDialect::appendValueRow(std::string &sqlText, std::vector<DatabaseValue> &parameters, const std::span<const DatabaseValue> rowValues) const
    {
        sqlText += '(';
        for (std::size_t index = 0; index < rowValues.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            // 占位符与参数按同一顺序成对产出，两处顺序必须一致（见 appendParameter 的说明）
            sqlText += placeholder();
            parameters.push_back(rowValues[index]);
        }
        sqlText += ')';
    }

    void StandardSqlDialect::requireMatchingColumnCount(const Queryable::QueryNode &query,
                                                        const std::size_t           valueCount) const
    {
        if (query.selectColumns.empty())
        {
            throw Base::InvalidArgumentException(std::string(dialectName()) + " 方言：待写列列表为空，无法生成写语句（表 " + query.tableName + "）");
        }

        // 列与值对错位时生成的语句可能仍能执行，却会把值写进错误的列，
        // 这种错误在业务层极难定位，因此必须在翻译阶段就拦住
        if (valueCount != query.selectColumns.size())
        {
            throw Base::InvalidArgumentException(std::string(dialectName()) + " 方言：取值个数（" + std::to_string(valueCount) +
                                                 "）与待写列数（" + std::to_string(query.selectColumns.size()) +
                                                 "）不一致，无法生成写语句（表 " + query.tableName + "）");
        }
    }

    void StandardSqlDialect::requireWithinParameterBudget(const std::size_t parameterCount) const
    {
        // 超过引擎单条语句的参数上限时，两侧驱动都只在执行阶段回一句引擎原文
        // （SQLite 是 "too many SQL variables"、MySQL 是占位符数超限），既指不出是哪一段条件撑爆的、
        // 也不说该怎么办。传入的是**渲染完成后实际产出的参数个数**：占位符只在 push 参数时写出，
        // 两者一一对应，而方言自己决定要不要把某个取值内联进文本（SQLite 内联分页），
        // 所以渲染前的估算数只能用于预留缓冲，拿它当判据会误拒贴着上限的查询
        if (const std::size_t parameterBudget = maximumStatementParameters(); parameterCount > parameterBudget)
        {
            throw Base::InvalidArgumentException(std::string(dialectName()) + " 方言：本条语句需要 " +
                                                 std::to_string(parameterCount) + " 个绑定参数，超过该引擎单条语句的上限 " +
                                                 std::to_string(parameterBudget) + " 个。请缩小 IN 列表或一次写入的列数，" +
                                                 "也可以把这次操作按上限拆成多条语句分批执行");
        }
    }

    void StandardSqlDialect::appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const
    {
        if (query.limit.has_value())
        {
            sqlText += " LIMIT ";
            // 先取序号再压参数，保证 parameters[i] 与第 i 个占位符严格对应
            sqlText += placeholder();
            parameters.push_back(pageNumberToDatabaseValue(query.limit.value()));
        }

        if (query.offset.has_value())
        {
            // OFFSET 单独出现是本实现的合法输入；引擎不允许这种写法的方言
            // （MySQL / SQLite）自行覆写本方法补出 LIMIT
            sqlText += " OFFSET ";
            sqlText += placeholder();
            parameters.push_back(pageNumberToDatabaseValue(query.offset.value()));
        }
    }

    SqlStatement StandardSqlDialect::translate(const Queryable::QueryNode &query) const
    {
        SqlStatement                statement;
        std::string &               sqlText    = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        // 一次把两处缓冲定够：文本按内容估上界，参数个数由条件树精确算出
        // （分页最多各占一个参数，方言若把取值内联进文本就是留宽一点，不影响产出）。
        // 这里必须把 JOIN 的 ON 条件与 HAVING 一并算进来——它们同样产出占位符，
        // 漏算的那一类查询会在渲染过程中把 parameters 撑大，退化成逐次扩容
        sqlText.reserve(estimateSqlTextCapacity(query));
        std::size_t expectedParameterCount = (query.limit.has_value() ? 1 : 0) + (query.offset.has_value() ? 1 : 0);
        for (const Queryable::WhereCondition &condition: query.whereConditions)
        {
            expectedParameterCount += countConditionParameters(condition);
        }
        for (const Queryable::JoinClause &join: query.joins)
        {
            for (const Queryable::WhereCondition &condition: join.conditions)
            {
                expectedParameterCount += countConditionParameters(condition);
            }
        }
        if (query.having.has_value())
        {
            expectedParameterCount += countConditionParameters(query.having.value());
        }
        parameters.reserve(expectedParameterCount);

        // ---------- SELECT 列 ----------
        sqlText += "SELECT ";
        if (query.selectColumns.empty())
        {
            // 查询树没有指定列时退化为通配符。ORM 层（Queryable<T>）在需要按 TableSchema
            // 的列序对齐结果时才显式展开列名，方言层不依赖任何模板参数，因此只能给出
            // 语法上最通用、顺序由数据库决定的 '*'
            sqlText += '*';
        } else
        {
            for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
            {
                if (index > 0)
                {
                    sqlText += ", ";
                }
                appendFieldReference(sqlText, query.selectColumns[index]);
            }
        }

        // ---------- FROM ----------
        sqlText += " FROM ";
        // 表名与别名的引用方式在四个方向上必须一致，因此统一走 appendTableReference()
        appendTableReference(sqlText, query);

        // ---------- JOIN ----------
        for (const Queryable::JoinClause &joinClause: query.joins)
        {
            sqlText += ' ';
            sqlText += joinTypeText(joinClause.type);
            sqlText += " JOIN ";
            // 被连接的表与主表是同一种东西，必须走同一条引用规则：此前它走的是字段引用，
            // 而字段引用允许「表达式原样输出」——表名位置没有任何合法表达式，那条让路等于
            // 把含运算符的文本直接拼进语句
            appendTableReference(sqlText, joinClause.tableName, joinClause.tableAlias);

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
                appendFieldReference(sqlText, query.groupBy[index].name);
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
                appendFieldReference(sqlText, query.orderBy[index].field.name);
                // 方向必须显式写出：默认升序虽然与 SQL 一致，但显式 "ASC" 让生成的 SQL 可读且稳定
                sqlText += query.orderBy[index].descending ? " DESC" : " ASC";
            }
        }

        // ---------- 分页 ----------
        // 三个引擎在这条子句上的差异最大，交给子类覆写的钩子处理
        appendLimitOffsetClause(sqlText, parameters, query);

        // 参数上限在这里按**实际产出的参数数**判定：渲染前那个估算值只是给 reserve 用的宽度
        // （分页按「各占一个」估，而 SQLite 把分页取值内联进文本、一个占位符都不产生），
        // 拿估计数当判据会把刚好贴着上限的查询误拒
        requireWithinParameterBudget(parameters.size());

        return statement;
    }

    SqlStatement StandardSqlDialect::translateInsert(const Queryable::QueryNode &query, const std::span<const DatabaseValue> values) const
    {
        requireMatchingColumnCount(query, values.size());

        SqlStatement statement;
        std::string &sqlText = statement.sql;

        sqlText.reserve(estimateSqlTextCapacity(query));
        // INSERT 的参数就是逐列取值，个数已被 requireMatchingColumnCount 校验过，可以直接定容
        statement.parameters.reserve(values.size());

        // INSERT 不接受表别名（"INSERT INTO 表 AS 别名" 是语法错误），因此这里只引用表名。
        // 查询树在插入方向由 ORM 现造，本来就不带别名，此处显式忽略是防止误用
        sqlText += "INSERT INTO ";
        appendQuotedIdentifier(sqlText, query.tableName);
        sqlText += " (";
        appendColumnList(sqlText, query);
        sqlText += ") VALUES ";
        // 单行插入就是「只有一行 VALUES」的批量插入，占位符与参数的收集规则完全一致
        appendValueRow(sqlText, statement.parameters, values);

        requireWithinParameterBudget(statement.parameters.size());

        return statement;
    }

    SqlStatement StandardSqlDialect::translateUpdate(const Queryable::QueryNode &query, const std::span<const DatabaseValue> values) const
    {
        requireMatchingColumnCount(query, values.size());

        SqlStatement                statement;
        std::string &               sqlText    = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        sqlText.reserve(estimateSqlTextCapacity(query));
        // SET 参数在前、条件参数在后：总数 = 列数 + 各条件树的参数数，一次定够
        std::size_t expectedParameterCount = values.size();
        for (const Queryable::WhereCondition &condition: query.whereConditions)
        {
            expectedParameterCount += countConditionParameters(condition);
        }
        parameters.reserve(expectedParameterCount);

        sqlText += "UPDATE ";
        appendTableReference(sqlText, query);
        sqlText += " SET ";

        // SET 子句先于 WHERE 输出，赋值参数因此排在条件参数之前，
        // 与文本里占位符的先后顺序严格一致（见 SqlStatement.h 的参数顺序契约）
        for (std::size_t index = 0; index < query.selectColumns.size(); ++index)
        {
            if (index > 0)
            {
                sqlText += ", ";
            }
            appendFieldReference(sqlText, query.selectColumns[index]);
            sqlText += " = ";
            sqlText += placeholder();
            // 赋值取值由 ORM 以 DatabaseValue 形式给出，已经是驱动可直接绑定的形态，无需再转换
            parameters.push_back(values[index]);
        }

        // WHERE 与 SELECT / DELETE 共用同一份渲染与参数收集实现
        appendWhereClause(sqlText, parameters, query);

        requireWithinParameterBudget(parameters.size());

        return statement;
    }

    SqlStatement StandardSqlDialect::translateDelete(const Queryable::QueryNode &query) const
    {
        SqlStatement statement;
        std::string &sqlText = statement.sql;

        sqlText.reserve(estimateSqlTextCapacity(query));
        std::size_t expectedParameterCount = 0;
        for (const Queryable::WhereCondition &condition: query.whereConditions)
        {
            expectedParameterCount += countConditionParameters(condition);
        }
        statement.parameters.reserve(expectedParameterCount);

        sqlText += "DELETE FROM ";
        // 别名一并带上：WHERE 里以别名限定的列名（"u"."id"）只有别名在场才能被解析
        appendTableReference(sqlText, query);

        // 无条件时 appendWhereClause() 不输出任何内容，SQL 退化为整表删除，与 SQL 语义一致
        appendWhereClause(sqlText, statement.parameters, query);

        requireWithinParameterBudget(statement.parameters.size());

        return statement;
    }

    SqlStatement StandardSqlDialect::translateInsertBatch(const Queryable::QueryNode &query, const std::span<const std::vector<DatabaseValue> > rows) const
    {
        if (rows.empty())
        {
            // 零行插入没有合法写法（"VALUES" 后面必须有至少一组括号），
            // 静默返回一句只能插 0 行的语句会让调用方以为写入了数据，因此直接失败
            throw Base::InvalidArgumentException(std::string(dialectName()) + " 方言：批量插入的行集合为空，无法生成 INSERT 语句");
        }

        requireMatchingColumnCount(query, rows.front().size());

        // 逐行校验：长度不一致的 VALUES 行会被引擎拒绝（"all VALUES must have the same number of terms"），
        // 与其把错误留给数据库，不如在翻译阶段就指出调用方给的行与列数对不齐。
        // 校验成本只是一次整数比较，相对拼接 SQL 文本可以忽略
        for (const std::vector<DatabaseValue> &rowValues: rows)
        {
            requireMatchingColumnCount(query, rowValues.size());
        }

        SqlStatement                statement;
        std::string &               sqlText    = statement.sql;
        std::vector<DatabaseValue> &parameters = statement.parameters;

        // 参数总数 = 行数 × 列数，调用方可能只算了一遍列数，这里按最坏情况预留容量避免反复扩容
        parameters.reserve(query.selectColumns.size() * rows.size());
        // 文本容量 = 查询树骨架 + 每行一组占位符（", " 与括号），批量方向行数可能远大于其它子句
        sqlText.reserve(estimateSqlTextCapacity(query) + rows.size() * (query.selectColumns.size() * 3 + 2));

        sqlText += "INSERT INTO ";
        appendQuotedIdentifier(sqlText, query.tableName);
        sqlText += " (";
        appendColumnList(sqlText, query);
        sqlText += ") VALUES ";

        // 逐行展开 "(?, ?), (?, ?)"：本层覆盖的三个引擎都支持多行 VALUES，
        // 参数按「行优先、行内按列序」压入，与文本中占位符的先后完全对齐
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            if (rowIndex > 0)
            {
                sqlText += ", ";
            }
            appendValueRow(sqlText, parameters, rows[rowIndex]);
        }

        // 写方向同样判上限：分块是 ORM 的职责，直接调方言的调用方也要拿到中文原因，
        // 而不是驱动在 execute 阶段回一句引擎原文
        requireWithinParameterBudget(parameters.size());

        return statement;
    }

    void StandardSqlDialect::appendCondition(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::WhereCondition &condition) const
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
                    // 空子条件按恒真处理（与 AND/OR 的空子条件同一条规则），取非得假
                    sqlText += "NOT (1 = 1)";
                    return;
                }

                if (condition.children.size() > 1U)
                {
                    // 多个子条件取非在语义上是不确定的：NOT (a AND b) 与 (NOT a) AND (NOT b) 结果不同，
                    // 替调用方挑一个就等于静默改谓词。公开的构造入口只会放一个子条件，
                    // 走到这里说明是手搓的树，宁可报错也不要给出一条「看起来对」的语句
                    throw Base::InvalidArgumentException(std::string(dialectName()) + " 方言：NOT 条件带了 " +
                                                         std::to_string(condition.children.size()) +
                                                         " 个子条件，取非的含义不确定（NOT (a AND b) 与 NOT a AND NOT b 结果不同）。"
                                                         "请先用 && 或 || 把这批子条件合成一个节点，再用 ! 取非");
                }

                // NOT 后面必须带括号：否则 "NOT a = ?" 在多数数据库里会被解析成 "(NOT a) = ?"
                sqlText += "NOT (";
                appendCondition(sqlText, parameters, condition.children[0]);
                sqlText += ')';
                return;
            }

            case SqlOperator::IsNull:
            {
                appendFieldReference(sqlText, condition.left.name);
                // IS NULL 不接受右操作数，也绝不绑定参数：NULL 的比较必须用 IS 而不是 "= NULL"
                sqlText += " IS NULL";
                return;
            }

            case SqlOperator::IsNotNull:
            {
                appendFieldReference(sqlText, condition.left.name);
                sqlText += " IS NOT NULL";
                return;
            }

            case SqlOperator::In:
            case SqlOperator::NotIn:
            {
                appendFieldReference(sqlText, condition.left.name);
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
        appendFieldReference(sqlText, condition.left.name);
        sqlText += ' ';
        sqlText += comparisonOperatorText(condition.op);
        sqlText += ' ';

        // 右操作数既可能是参数值，也可能是另一个字段引用（列-列比较，如 "age" > "min_age"）
        if (std::holds_alternative<Queryable::FieldReference>(condition.right))
        {
            // 列-列比较两侧都是标识符，不需要也不能绑定参数
            appendFieldReference(sqlText, std::get<Queryable::FieldReference>(condition.right).name);
        } else
        {
            appendParameter(sqlText, parameters, std::get<Queryable::ParameterValue>(condition.right));
        }

        // 字面量匹配必须补出 ESCAPE 子句：SQLite 的 LIKE 没有默认转义符，缺这一句时模式里的 '!'
        // 会被当成普通字符、'%' 与 '_' 仍按通配符解释，转义就成了摆设。子句文本与转义符同源
        // （见 kLikeEscapeCharacter），MySQL 与 SQLite 共用这一份
        if (condition.op == SqlOperator::LikeLiteral)
        {
            sqlText += Queryable::kLikeEscapeClauseText;
        }
    }

    void StandardSqlDialect::appendParameter(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::ParameterValue &parameter) const
    {
        // 先写占位符再压参数：两处顺序一致，SQL 文本里的第 i 个占位符就对应 parameters[i]
        sqlText += placeholder();
        parameters.push_back(convertParameter(parameter));
    }

    DatabaseValue StandardSqlDialect::convertParameter(const Queryable::ParameterValue &parameter)
    {
        return std::visit(
                []<typename T0>(const T0 &value) -> DatabaseValue
                {
                    using ValueType = std::decay_t<T0>;

                    if constexpr (std::is_same_v<ValueType, std::nullptr_t>)
                    {
                        // ORM 用 nullptr 表达 SQL NULL，驱动层用 std::monostate 表达，二者语义相同
                        return std::monostate{};
                    } else if constexpr (std::is_same_v<ValueType, std::uint64_t>)
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
                    } else if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t> >)
                    {
                        // 二进制在两种 variant 里同名同类型，无需翻译。这里是唯一一个「类型本身就是
                        // 绑定线索」的备选：驱动靠它决定走 sqlite3_bind_blob / MYSQL_TYPE_BLOB，
                        // 而不是按文本绑定后由服务端按连接字符集重新解释载荷
                        return DatabaseValue{value};
                    } else
                    {
                        // bool / int64_t / double / std::string 在两种 variant 中同名同类型，直接构造即可
                        return DatabaseValue{value};
                    }
                },
                parameter);
    }

    DatabaseValue StandardSqlDialect::pageNumberToDatabaseValue(const std::size_t pageNumber)
    {
        // 分页位置在多数引擎上都要求整数参数，因此优先按有符号整数绑定；
        // 超出 int64_t 的极端页码退化成十进制文本，只为不静默回绕成负数
        if (pageNumber <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return static_cast<std::int64_t>(pageNumber);
        }

        return std::to_string(pageNumber);
    }

    std::string_view StandardSqlDialect::comparisonOperatorText(const Queryable::SqlOperator sqlOperator)
    {
        using Queryable::SqlOperator;

        switch (sqlOperator)
        {
            case SqlOperator::Eq:
                return "=";
            case SqlOperator::Neq:
                return "!=";
            case SqlOperator::Gt:
                return ">";
            case SqlOperator::Ge:
                return ">=";
            case SqlOperator::Lt:
                return "<";
            case SqlOperator::Le:
                return "<=";
            case SqlOperator::Like:
                // 字面量匹配在文本上与 LIKE 同形，区别由 appendCondition 补出的 ESCAPE 子句承担
                // （两个 case 刻意合并：操作符文本只有一份，不给转义规则留出第二处真相）
            case SqlOperator::LikeLiteral:
                return "LIKE";
            case SqlOperator::In:
                return "IN";
            case SqlOperator::NotIn:
                return "NOT IN";
            default:
                // 复合节点与 IS NULL 系由渲染分支提前分流，走到这里说明漏了分支；
                // 静默给出 "=" 会生成语义错误的 SQL，宁可当场失败
                throw Base::LogicException("标准 SQL 方言：该操作符没有比较文本（枚举值 " +
                                           std::to_string(std::to_underlying(sqlOperator)) + "），请检查条件渲染是否漏了分支");
        }
    }

    std::string_view StandardSqlDialect::commitStatement() const noexcept
    {
        return "COMMIT";
    }

    std::string_view StandardSqlDialect::rollbackStatement() const noexcept
    {
        return "ROLLBACK";
    }

    std::string_view StandardSqlDialect::joinTypeText(const Queryable::JoinType joinType)
    {
        using Queryable::JoinType;

        switch (joinType)
        {
            case JoinType::Inner:
                return "INNER";
            case JoinType::Left:
                return "LEFT";
            // RIGHT JOIN 在 SQLite 上是 3.39.0 起才支持，更早的版本会在编译期直接报语法错误，
            // 这里照写意图，让数据库给出明确错误而不是被上层悄悄改成 LEFT
            case JoinType::Right:
                return "RIGHT";
            case JoinType::Cross:
                return "CROSS";
            default:
                // 未知取值静默当成 INNER 会把「连错表」变成「少连了一张表」，必须当场失败
                throw Base::LogicException("标准 SQL 方言：未知的连接类型（枚举值 " +
                                           std::to_string(std::to_underlying(joinType)) + "），请检查连接渲染分支");
        }
    }

} // namespace AsynGyanis::Database
