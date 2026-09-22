/**
 * @file StandardSqlDialect.h
 * @brief 标准 SQL 方言基类 —— 三个引擎共用的查询树渲染与参数收集
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 「三种引擎写法完全一致的那部分」的唯一实现：查询树递归渲染（AND/OR/NOT、IN 展开、
 *          IS NULL、列-列比较）、子句书写顺序、写语句 SET/WHERE 拼装、ORM 参数值的降级转换；
 *          引擎知识（引用字符与转义、占位符、事务语句、分页、类型名、元数据、参数上限）全交子类。
 *          参数顺序契约：产出占位符即压入参数、序号取自已收集个数，故 parameters[i] 对应第 i 个占位符。
 */
#pragma once

#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 标准 SQL 方言基类（SQLite / MySQL 的共用实现）
     *
     * @details 无状态实现，可被多线程并发调用。子类只需覆写引擎知识，
     *          查询树渲染与参数收集行为因此在校验层面天然一致。
     */
    class StandardSqlDialect : public SqlDialect
    {
    public:
        /**
         * @brief 默认构造函数
         */
        StandardSqlDialect() = default;

        /**
         * @brief 析构函数
         */
        ~StandardSqlDialect() override = default;

        StandardSqlDialect(const StandardSqlDialect &) = default;

        StandardSqlDialect &operator=(const StandardSqlDialect &) = default;

        /**
         * @brief 把查询树翻译成带占位符的 SQL 与绑定参数
         *
         * @details 重写 SqlDialect::translate()：按 SELECT → FROM → JOIN → WHERE → GROUP BY →
         *          HAVING → ORDER BY → 分页 的书写顺序拼接文本；分页子句交给 appendLimitOffsetClause()，
         *          因此各引擎的分页差异不会影响其余子句。
         *
         * @param query 待翻译的查询树，本方法不修改它
         * @return SqlStatement SQL 文本与按序排列的绑定参数
         * @note 纯文本变换，不访问数据库，可在任意线程并发调用
         */
        [[nodiscard]] SqlStatement translate(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把单行插入翻译成带占位符的 INSERT
         *
         * @details 重写 SqlDialect::translateInsert()：列名取自 query.selectColumns 并逐个引用，
         *          取值由 values 按同一顺序绑定。
         *
         * @param query 提供表名与待写列的查询树
         * @param values 待绑定的字段值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement INSERT 文本与按列序排列的绑定参数
         * @throws Base::InvalidArgumentException 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsert(const Queryable::QueryNode &query, std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件更新翻译成带占位符的 UPDATE
         *
         * @details 重写 SqlDialect::translateUpdate()：SET 参数在前、条件参数在后，与文本中占位符的
         *          先后严格一致；WHERE 与 SELECT / DELETE 共用同一份渲染实现。
         *
         * @param query 提供表名、SET 列与 WHERE 条件的查询树
         * @param values 赋给各 SET 列的取值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement UPDATE 文本与按序排列的绑定参数
         * @throws Base::InvalidArgumentException 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateUpdate(const Queryable::QueryNode &query, std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件删除翻译成带占位符的 DELETE
         *
         * @details 重写 SqlDialect::translateDelete()：无条件时整段 WHERE 被省略（整表删除，与 SQL 语义一致）。
         *          表别名一并带上，因为 WHERE 里以别名限定的列名只有别名在场才能被解析。
         *
         * @param query 提供表名与 WHERE 条件的查询树
         * @return SqlStatement DELETE 文本与按序排列的绑定参数
         */
        [[nodiscard]] SqlStatement translateDelete(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把多行插入翻译成一次多行 VALUES 的 INSERT
         *
         * @details 重写 SqlDialect::translateInsertBatch()：参数按「行优先、行内按列序」展开；
         *          参数总数是否超过引擎上限由调用方按 maximumStatementParameters() 分块。
         *
         * @param query 提供表名与待写列的查询树
         * @param rows 待插入的行，每行的取值个数必须等于 query.selectColumns 的列数
         * @return SqlStatement 多行 INSERT 文本与按序排列的绑定参数
         * @throws Base::InvalidArgumentException 列数为空、rows 为空，或某行取值个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsertBatch(const Queryable::QueryNode &query, std::span<const std::vector<DatabaseValue> > rows) const override;

        /**
         * @brief 用本引擎的引用字符引用标识符，并翻倍转义内部引用字符
         *
         * @details 重写 SqlDialect::quoteIdentifier()：引用字符由 identifierQuoteCharacter() 给出，
         *          内部同字符按 SQL 规则翻倍表示（"a""b" / `a``b`）。反斜杠不能作为转义字符：
         *          它在三种引擎里都是普通字符，用它既无效又会引入字面反斜杠。
         *
         * @param identifier 待引用的标识符，不含外层引用字符
         * @return std::string 已加引用字符并完成转义的文本
         */
        [[nodiscard]] std::string quoteIdentifier(std::string_view identifier) const override;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         *
         * @details 重写 SqlDialect::supportsLimitOffset()：SQLite / MySQL 都原生支持关键字形式的分页，
         *          因此恒为 true；「OFFSET 能否单独出现」这类细节由 appendLimitOffsetClause() 各自处理。
         *
         * @return true 恒为 true
         */
        [[nodiscard]] bool supportsLimitOffset() const noexcept override;

        /**
         * @brief 生成一个参数占位符
         *
         * @details 重写 SqlDialect::placeholder()：SQLite 与 MySQL 的位置参数都写作 "?"，
         *          参数与占位符的对应关系由「按出现顺序依次压入 parameters」保证（见参数顺序契约）。
         *          实现内联在头文件：一条 150 行 × 6 列的批量语句要取用它 900 次，而两个派生方言
         *          各自的建表/存在性渲染也用它，放在实现文件里会让这些循环各吃一次跨编译单元调用。
         *
         * @return std::string_view 占位符文本，恒为 "?"
         */
        [[nodiscard]] std::string_view placeholder() const override
        {
            return std::string_view{"?"};
        }

        /**
         * @brief 取得提交事务的语句文本
         *
         * @details 重写 SqlDialect::commitStatement()：两个引擎都用标准 SQL 的 "COMMIT"，
         *          实现收在此处；差异只在开启事务一侧（见各子类）。
         *
         * @return std::string_view 恒为 "COMMIT"
         */
        [[nodiscard]] std::string_view commitStatement() const noexcept override;

        /**
         * @brief 取得回滚事务的语句文本
         *
         * @details 重写 SqlDialect::rollbackStatement()：两个引擎都用标准 SQL 的 "ROLLBACK"。
         *
         * @return std::string_view 恒为 "ROLLBACK"
         */
        [[nodiscard]] std::string_view rollbackStatement() const noexcept override;

    protected:
        /**
         * @brief 取得本引擎的标识符引用字符
         * @return char 双引号（SQLite）或反引号（MySQL）
         * @note 本字符同时用于 quoteIdentifier() 的加引用与 renderFieldReference() 的
         *       「是否是可引用标识符」判定，两处必须一致，否则含空格或引用符的列名会被误判
         */
        [[nodiscard]] virtual char identifierQuoteCharacter() const noexcept = 0;

        /**
         * @brief 取得本方言的显示名，用于组成中文错误文本
         * @return std::string_view 例如 "SQLite" / "MySQL"
         * @note 错误文本形如「SQLite 方言：待写列列表为空…」，保留引擎名是为了让
         *       多方言并存的调用方能立刻判断是哪一侧的输入有问题
         */
        [[nodiscard]] virtual std::string_view dialectName() const noexcept = 0;

        /**
         * @brief 渲染分页子句并收集分页参数
         *
         * @details 默认实现为 SQL 标准的关键字形式：" LIMIT <占位符>" 与 " OFFSET <占位符>" 各自
         *          独立输出（先 LIMIT 后 OFFSET），两者都不存在时什么都不输出。MySQL 覆写只为补出
         *          「只给 offset」时要写的不限行数常量；SQLite 的分页值走内联且需要 "LIMIT -1" 补位。
         *
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，分页值按占位符出现顺序追加
         * @param query 提供 limit / offset 的查询树
         * @note 覆写方必须保持「先取占位符序号、再压参数」的顺序，否则 $n 方言的序号会整体偏移
         */
        virtual void appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const;

        /**
         * @brief 把字段引用追加进输出缓冲：纯标识符加引用字符，表达式原样输出
         *
         * @details 处理三种情形：单个标识符（加引用）、限定名（users.id → "users"."id"）、含结构
         *          字符的表达式（原样输出，例如 COUNT(*)）。判定依据是「有无结构字符」而非「有无空格」，
         *          因此含空格的列名仍是标识符。原样输出在安全上成立：字段引用全部来自编译期常量
         *          （Column() 的列名参数或 asc()/desc() 的字面量），不是外部输入，数据值一律走参数绑定。
         *
         * @param sqlText 输出缓冲区，字段片段追加到末尾
         * @param fieldText 字段引用文本（列名或表达式）
         * @note 片段直写 sqlText 而不是先返回一个临时 std::string：一条查询要渲染七八个字段引用，
         *       临时串里长度超过短字符串缓冲的那个（"balance" 加引号已超 15 字节）每次都要建一份堆缓冲
         */
        void appendFieldReference(std::string &sqlText, std::string_view fieldText) const;

        /**
         * @brief 把「已引用的表名 + 可选别名」追加进输出缓冲
         * @details SELECT / UPDATE / DELETE 共用，保证表名与别名的引用方式在任何语句里都一致。
         * @param sqlText 输出缓冲区
         * @param query 提供 tableName 与 tableAlias 的查询树
         */
        void appendTableReference(std::string &sqlText, const Queryable::QueryNode &query) const;

        /**
         * @brief 把加好引用字符并完成转义的标识符追加进输出缓冲
         * @details 这是引用规则的唯一实现：quoteIdentifier() 只负责建一个空串再调本方法，
         *          两条通道不可能出现转义分歧。
         * @param sqlText 输出缓冲区
         * @param identifier 待引用的标识符，不含外层引用字符
         */
        void appendQuotedIdentifier(std::string &sqlText, std::string_view identifier) const;

        /**
         * @brief 渲染 " WHERE 条件..." 子句（无条件时什么都不输出）
         * @details 本层唯一的条件渲染入口，translate() / translateUpdate() / translateDelete() 全走它：
         *          AND/OR/NOT 递归展开、IN 展开多个占位符、IS NULL 与列-列比较不占参数。
         *          一份实现意味着三个方向的参数顺序不可能出现分歧。
         * @param sqlText 输出缓冲区，" WHERE ..." 追加到末尾
         * @param parameters 输出参数列表，条件产生的取值按占位符出现顺序追加
         * @param query 提供 whereConditions 的查询树
         */
        void appendWhereClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染 INSERT 的列名列表
         * @param sqlText 输出缓冲区，形如 "列1", "列2" 的片段追加到末尾
         * @param query 提供列名列表（selectColumns）的查询树
         */
        void appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染一行 VALUES 的占位符并收集其参数
         * @param sqlText 输出缓冲区，形如 "(?, ?)" 的片段追加到末尾
         * @param parameters 输出参数列表，本行取值按列序追加
         * @param rowValues 本行各列的取值
         */
        void appendValueRow(std::string &sqlText, std::vector<DatabaseValue> &parameters, std::span<const DatabaseValue> rowValues) const;

        /**
         * @brief 校验取值个数与待写列数一致
         * @details 个数不符说明调用方把列与值对错了位，生成的语句即使能执行也会写错列，
         *          这种错误在业务层极难定位，因此在翻译阶段就失败并给出中文原因。
         * @param query 提供 selectColumns 的查询树
         * @param valueCount 本次提供的取值个数
         * @throws Base::InvalidArgumentException 列数为空，或 valueCount 与列数不一致
         */
        void requireMatchingColumnCount(const Queryable::QueryNode &query, std::size_t valueCount) const;

        /**
         * @brief 校验绑定参数个数不超过本引擎的单条语句上限
         * @details 上限由 maximumStatementParameters() 给出（SQLite 999、MySQL 65535）。不先拦的话，
         *          超限要到执行阶段才由驱动回一句引擎原文，指不出是哪一段条件撑爆的。
         * @param parameterCount 本条语句将产出的绑定参数个数（须与渲染分支的产出精确一致）
         * @throws Base::InvalidArgumentException 参数个数超过本引擎单条语句的上限
         */
        void requireWithinParameterBudget(std::size_t parameterCount) const;

        /**
         * @brief 递归渲染一个 WHERE / HAVING / ON 条件
         * @details AND/OR/NOT 递归展开 children，IN/NOT IN 展开 inValues，
         *          IS NULL / IS NOT NULL 与列-列比较不产生参数。
         * @param sqlText 输出缓冲区，条件文本追加到末尾
         * @param parameters 输出参数列表，占位符对应的取值按序追加
         * @param condition 待渲染的条件节点
         */
        void appendCondition(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::WhereCondition &condition) const;

        /**
         * @brief 追加一个右操作数参数的占位符并收集其取值
         * @param sqlText 输出缓冲区，占位符文本追加到末尾
         * @param parameters 输出参数列表，转换后的取值追加到末尾
         * @param parameter 待绑定的参数值
         */
        void appendParameter(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::ParameterValue &parameter) const;

        /**
         * @brief 把 ORM 参数值转换成驱动层的统一值
         * @details uint64_t 在 DatabaseValue 中没有对应备选（该类型已冻结）：
         *          放得进 int64_t 时转成有符号整数，超出范围时转成十进制文本——
         *          宁可让比较按文本进行，也不引入第二套无符号类型或静默回绕成负数。
         * @param parameter ORM 参数值
         * @return DatabaseValue 驱动可直接绑定的统一值
         */
        [[nodiscard]] static DatabaseValue convertParameter(const Queryable::ParameterValue &parameter);

        /**
         * @brief 把分页取值（size_t）转换成驱动可绑定的统一值
         * @details 与 convertParameter 同样的两步降级：放得进 int64_t 就按整数绑定
         *          （分页位置在多数引擎上都要求整数参数），超出范围时转成十进制文本。
         *          现实中的页码远小于 INT64_MAX，该分支只为不静默回绕成负数而存在。
         * @param pageNumber 行数上限或偏移量
         * @return DatabaseValue 驱动可直接绑定的统一值
         */
        [[nodiscard]] static DatabaseValue pageNumberToDatabaseValue(std::size_t pageNumber);

        /**
         * @brief 将比较操作符转成 SQL 文本
         * @param sqlOperator 操作符枚举
         * @return std::string_view 对应的 SQL 操作符文本
         * @throws Base::LogicException 传入的不是比较操作符（调用方漏了分支）
         */
        [[nodiscard]] static std::string_view comparisonOperatorText(Queryable::SqlOperator sqlOperator);

        /**
         * @brief 将连接类型转成 SQL 关键字
         * @param joinType 连接类型枚举
         * @return std::string_view INNER / LEFT / RIGHT / CROSS
         * @throws Base::LogicException 传入的不是已知连接类型（调用方漏了分支）
         */
        [[nodiscard]] static std::string_view joinTypeText(Queryable::JoinType joinType);
    };

} // namespace AsynGyanis::Database
