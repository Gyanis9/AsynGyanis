/**
 * @file SqliteDialect.h
 * @brief SQLite 方言 —— 把查询树翻译成 SQLite 可执行的参数化 SQL
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details SqlDialect 的 SQLite 实现。SQLite 采用 SQL-92 风格的标准语法，
 *          与通用 SQL 最接近，因此本实现同时充当其它方言的参考实现。
 *
 * ## 生成的 SQL 规则
 * - 标识符：双引号引用，内部双引号翻倍转义（"weird""name"）；
 * - 占位符：一律 '?'，不区分类型；
 * - 参数：WHERE / HAVING / JOIN...ON 里的比较值、IN 列表按出现顺序收集，
 *   IS NULL / IS NOT NULL 不产生参数，列-列比较不产生参数；
 * - 写语句：INSERT / UPDATE / DELETE / 多行 INSERT 与 SELECT 共用同一套 WHERE 渲染
 *   与参数收集规则（本实现把它们收敛到 appendWhereClause 等私有辅助函数里），
 *   因此条件树的递归展开、IN 展开、参数顺序在四个方向上的行为必然一致；
 * - 多行 VALUES：SQLite 自 3.7.11 起支持 "VALUES (…), (…)"，本实现直接使用该语法，
 *   行数由调用方按参数上限自行分块；
 * - 事务语句：开启用 "BEGIN IMMEDIATE"（立刻取写锁，避免多个连接都先取读锁、
 *   升级为写锁时撞上 SQLITE_BUSY 的经典死锁），提交用 "COMMIT"，回滚用 "ROLLBACK"；
 * - 分页：LIMIT / OFFSET 直接内联十进制整数（取值来自强类型 size_t，不经外部文本）；
 *   OFFSET 单独出现时补 "LIMIT -1"，因为 SQLite 要求 OFFSET 必须跟在 LIMIT 之后；
 * - 表达式字段（如 COUNT(*)、COALESCE(age, 0)）原样输出，不加引号，
 *   加了引号会被当成列名从而改变语义。
 *   判定依据是「文本里有没有运算符、括号、逗号这类结构字符」：只含标识符字节与空格的文本
 *   一律按标识符加双引号引用，因此 foo bar 这种含空格的列名能被正确引用而不是当成表达式。
 *
 * ## 安全考量
 * 所有来自 C++ 侧的数据（比较值、IN 列表）都只以占位符形式出现，
 * 值本身通过 parameters 交给驱动的绑定接口，因此含单引号、"--"、分号的字符串
 * 只会被当成普通文本，不会改变语句结构。详见 SqlStatement.h 的说明。
 *
 * @code
 *   SqliteDialect dialect;
 *   Queryable::QueryNode node;
 *   node.tableName = "users";
 *   node.whereConditions.push_back(WhereCondition{
 *       .left  = Queryable::FieldReference{.name = "age"},
 *       .op    = Queryable::SqlOperator::Ge,
 *       .right = Queryable::ParameterValue{std::int64_t{18}}
 *   });
 *   SqlStatement statement = dialect.translate(node);
 *   // => sql:        SELECT * FROM "users" WHERE "age" >= ?
 *   // => parameters: { std::int64_t{18} }
 * @endcode
 */
#pragma once

#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief SQLite SQL 方言
     *
     * @details 无状态实现，可被多线程并发调用；实例由 DialectRegistry 以共享指针提供，
     *          调用方一般不需要自己构造。
     */
    class SqliteDialect final : public SqlDialect
    {
    public:
        /**
         * @brief 默认构造函数
         */
        SqliteDialect() = default;

        /**
         * @brief 析构函数
         */
        ~SqliteDialect() override = default;

        // 方言是纯翻译规则、不含状态，拷贝一份与共享同一实例等价（registry 用共享指针持有）
        SqliteDialect(const SqliteDialect &)            = default;
        SqliteDialect &operator=(const SqliteDialect &) = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @details 重写 SqlDialect::type()：恒返回 DatabaseType::Sqlite，与连接状态无关。
         * @return DatabaseType DatabaseType::Sqlite
         */
        [[nodiscard]] DatabaseType type() const noexcept override;

        /**
         * @brief 把查询树翻译成 SQLite 的参数化 SQL
         * @details 重写 SqlDialect::translate()：按 SELECT → FROM → JOIN → WHERE →
         *          GROUP BY → HAVING → ORDER BY → LIMIT/OFFSET 的书写顺序拼接文本，
         *          每写入一个占位符就同步压入一个参数，保证参数顺序与占位符出现顺序严格一致。
         *          与基类契约的差异：本实现不使用布尔字面量（SQLite 用 1/0 的整数表达真假），
         *          空 IN 集合翻译成恒假常量而不产出非法的 "IN ()"。
         * @param query 待翻译的查询树，本方法不修改它
         * @return SqlStatement SQL 文本与按序排列的绑定参数
         * @note 纯文本变换，不访问数据库，可在任意线程并发调用
         */
        [[nodiscard]] SqlStatement translate(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把单行插入翻译成 SQLite 的 INSERT
         * @details 重写 SqlDialect::translateInsert()：生成
         *          "INSERT INTO 表 (列…) VALUES (?, …)"。列名取自 query.selectColumns
         *          并逐个加双引号引用，取值由 values 按同一顺序绑定；本实现不修改入参。
         *          与基类契约一致，个数不符时抛 std::invalid_argument 而不是生成半截语句。
         * @param query 提供表名与待写列的查询树
         * @param values 待绑定的字段值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement INSERT 文本与按列序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsert(const Queryable::QueryNode &query,
                                                   std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件更新翻译成 SQLite 的 UPDATE
         * @details 重写 SqlDialect::translateUpdate()：生成
         *          "UPDATE 表 SET 列 = ?, … [WHERE 条件]"。SET 的列与占位符按 selectColumns
         *          顺序输出，随后整段 WHERE 交给与 SELECT 共用的 appendWhereClause()，
         *          因此条件树的递归、IN 展开、参数收集顺序与本方言的 SELECT 完全一致；
         *          SET 参数在前、条件参数在后，与文本中占位符的先后严格对应。
         * @param query 提供表名、SET 列与 WHERE 条件的查询树
         * @param values 赋给各 SET 列的取值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement UPDATE 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateUpdate(const Queryable::QueryNode &query,
                                                   std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件删除翻译成 SQLite 的 DELETE
         * @details 重写 SqlDialect::translateDelete()：生成 "DELETE FROM 表 [WHERE 条件]"，
         *          条件渲染复用 appendWhereClause()；无条件时整段 WHERE 被省略（整表删除）。
         *          表别名存在时一并写出，因为 WHERE 里以别名限定的列名只有别名在场才能解析。
         * @param query 提供表名与 WHERE 条件的查询树
         * @return SqlStatement DELETE 文本与按序排列的绑定参数
         */
        [[nodiscard]] SqlStatement translateDelete(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把多行插入翻译成 SQLite 的多行 VALUES 语句
         * @details 重写 SqlDialect::translateInsertBatch()：生成
         *          "INSERT INTO 表 (列…) VALUES (?, …), (?, …), …"，参数按「行优先、行内按列序」
         *          展开。SQLite 从 3.7.11 起原生支持多行 VALUES，本实现直接使用；
         *          参数总数是否超过 SQLITE_MAX_VARIABLE_NUMBER 由调用方负责分块。
         * @param query 提供表名与待写列的查询树
         * @param rows 待插入的行，每行的取值个数必须等于 query.selectColumns 的列数
         * @return SqlStatement 多行 INSERT 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空、rows 为空，或某行取值个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsertBatch(const Queryable::QueryNode &query,
                                                        std::span<const std::vector<DatabaseValue>> rows) const override;

        /**
         * @brief 获取 SQLite 的开启事务语句
         * @details 重写 SqlDialect::beginTransactionStatement()：返回 "BEGIN IMMEDIATE"。
         *          不用裸 "BEGIN"（DEFERRED）：DEFERRED 事务在第一条写语句才尝试升级为写锁，
         *          两个连接同时如此操作时必然有一方拿到 SQLITE_BUSY，且无法靠重试自动化解；
         *          IMMEDIATE 在开启时就取写锁，失败立刻暴露在 BEGIN 这一步。
         * @return std::string_view 恒为 "BEGIN IMMEDIATE"
         */
        [[nodiscard]] std::string_view beginTransactionStatement() const noexcept override;

        /**
         * @brief 获取 SQLite 的提交事务语句
         * @details 重写 SqlDialect::commitStatement()：返回 "COMMIT"。
         * @return std::string_view 恒为 "COMMIT"
         */
        [[nodiscard]] std::string_view commitStatement() const noexcept override;

        /**
         * @brief 获取 SQLite 的回滚事务语句
         * @details 重写 SqlDialect::rollbackStatement()：返回 "ROLLBACK"。
         * @return std::string_view 恒为 "ROLLBACK"
         */
        [[nodiscard]] std::string_view rollbackStatement() const noexcept override;

        /**
         * @brief 用双引号引用标识符并翻转义内部双引号
         * @details 重写 SqlDialect::quoteIdentifier()：SQLite 接受 SQL 标准的双引号形式，
         *          内部双引号按标准翻倍（"weird""name"）而不是用反斜杠转义
         *          （SQLite 的字符串字面量不认反斜杠转义，反斜杠会被当成普通字符）。
         * @param identifier 待引用的标识符，不含外层双引号
         * @return std::string 形如 "identifier" 的文本，内部双引号已翻倍
         */
        [[nodiscard]] std::string quoteIdentifier(std::string_view identifier) const override;

        /**
         * @brief 生成第 index 个参数占位符
         * @details 重写 SqlDialect::placeholder()：SQLite 的位置参数不区分类型，
         *          统一写作 "?"，序号仅用于按顺序收集参数，不体现在文本里。
         * @param index 参数序号，从 0 开始
         * @return std::string 恒为 "?"
         */
        [[nodiscard]] std::string placeholder(std::size_t index) const override;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         * @details 重写 SqlDialect::supportsLimitOffset()：SQLite 原生支持
         *          "LIMIT n" 与 "LIMIT n OFFSET m"（OFFSET 不能单独出现）。
         * @return true 恒为 true
         */
        [[nodiscard]] bool supportsLimitOffset() const noexcept override;

        /**
         * @brief 获取 SQLite 单条语句的参数个数上限
         * @details 重写 SqlDialect::maximumStatementParameters()：返回常量 kMaximumStatementParameters。
         *          该值来自 SQLite 的编译期宏 SQLITE_MAX_VARIABLE_NUMBER，官方默认值是 999
         *          （3.32 起该宏的默认上限被提高到 32766，但绝大多数发行版仍按 999 编译，
         *          且 sqlite3_limit(SQLITE_LIMIT_VARIABLE_NUMBER) 可通过第三方构建调小，
         *          因此按保守值 999 分块，宁可多分几批也不会被引擎拒绝）。
         * @return std::size_t 恒为 kMaximumStatementParameters（999）
         */
        [[nodiscard]] std::size_t maximumStatementParameters() const noexcept override;

        /// SQLite 单条语句的参数个数保守上限（= SQLITE_MAX_VARIABLE_NUMBER 的默认值 999）
        static constexpr std::size_t kMaximumStatementParameters = 999;

    private:
        /**
         * @brief 渲染字段引用：纯标识符加引号，表达式原样输出
         * @details 处理三种情形：单个标识符（加双引号）、限定名（users.id → "users"."id"）、
         *          含运算符/括号/逗号等结构字符的表达式（原样输出，例如 COUNT(*)）。
         *          判定依据是「文本里有没有结构字符」而不是「有没有空格」：含空格的标识符
         *          （如 foo bar 这种列名）仍是标识符，会被引用成 "foo bar"，不被误判为表达式。
         * @param fieldText 字段引用文本（列名或表达式）
         * @return std::string 可直接写入 SQL 的字段片段
         */
        [[nodiscard]] std::string renderFieldReference(std::string_view fieldText) const;

        /**
         * @brief 渲染表引用：加引号的表名，附带可选的 "AS 别名"
         * @details SELECT / INSERT / UPDATE / DELETE 四个方向共用，保证表名与别名的
         *          引用方式在任何语句里都一致。
         * @param query 提供 tableName 与 tableAlias 的查询树
         * @return std::string "表名" 或 "表名 AS "别名""
         */
        [[nodiscard]] std::string renderTableReference(const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染 " WHERE 条件..." 子句（无条件时什么都不输出）
         * @details 这是本方言唯一的条件渲染入口，translate() / translateUpdate() /
         *          translateDelete() 全部走它：AND/OR/NOT 递归展开 children、IN 展开多个
         *          占位符、IS NULL 与列-列比较不占参数、每写一个占位符就同步压一个参数。
         *          一份实现意味着三个方向的参数顺序与文本顺序不可能出现分歧。
         * @param sqlText 输出缓冲区，" WHERE ..." 追加到末尾
         * @param parameters 输出参数列表，条件产生的取值按占位符出现顺序追加
         * @param query 提供 whereConditions 的查询树
         */
        void appendWhereClause(std::string &sqlText,
                               std::vector<DatabaseValue> &parameters,
                               const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染 INSERT 的列名列表
         * @details 形如 "\"id\", \"name\""，逐个走 quoteIdentifier()；
         *          四个写方向共用同一份列名引用逻辑。
         * @param sqlText 输出缓冲区，追加到末尾
         * @param query 提供列名列表（selectColumns）的查询树
         */
        void appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染一行 VALUES 的占位符并收集其参数
         * @param sqlText 输出缓冲区，形如 "(?, ?)" 的片段追加到末尾
         * @param parameters 输出参数列表，本行取值按列序追加
         * @param rowValues 本行各列的取值
         */
        void appendValueRow(std::string &sqlText,
                            std::vector<DatabaseValue> &parameters,
                            std::span<const DatabaseValue> rowValues) const;

        /**
         * @brief 校验取值个数与待写列数一致
         * @details 个数不符说明调用方把列与值对错了位，生成出来的语句即使能执行也会写错列，
         *          因此在这里直接失败并给出中文原因，绝不生成半截语句。
         * @param query 提供 selectColumns 的查询树
         * @param valueCount 本次提供的取值个数
         * @throws std::invalid_argument 列数为空，或 valueCount 与列数不一致
         */
        static void requireMatchingColumnCount(const Queryable::QueryNode &query, std::size_t valueCount);

        /**
         * @brief 递归渲染一个 WHERE / HAVING / ON 条件
         * @details AND/OR/NOT 递归展开 children，IN/NOT IN 展开 inValues，
         *          IS NULL / IS NOT NULL 与列-列比较不产生参数；每产出一个占位符
         *          就压入一个参数，保证顺序一致。
         * @param sql 输出缓冲区，条件文本追加到末尾
         * @param parameters 输出参数列表，占位符对应的取值按序追加
         * @param condition 待渲染的条件节点
         */
        void appendCondition(std::string &sql,
                             std::vector<DatabaseValue> &parameters,
                             const Queryable::WhereCondition &condition) const;

        /**
         * @brief 渲染一个右操作数参数并改写 SQL 占位符
         * @param sql 输出缓冲区，占位符文本追加到末尾
         * @param parameters 输出参数列表，转换后的取值追加到末尾
         * @param parameter 待绑定的参数值
         */
        void appendParameter(std::string &sql,
                             std::vector<DatabaseValue> &parameters,
                             const Queryable::ParameterValue &parameter) const;

        /**
         * @brief 把 ORM 参数值转换成驱动层的统一值
         * @details uint64_t 在 DatabaseValue 中没有对应备选（该类型已冻结）：
         *          放得进 int64_t 时转成有符号整数，超出范围时转成十进制文本，取舍见实现处注释。
         * @param parameter ORM 参数值
         * @return DatabaseValue 驱动可直接绑定的统一值
         */
        [[nodiscard]] static DatabaseValue convertParameter(const Queryable::ParameterValue &parameter);

        /**
         * @brief 将比较操作符转成 SQL 文本
         * @param sqlOperator 操作符枚举
         * @return std::string_view 对应的 SQL 操作符文本
         */
        [[nodiscard]] static std::string_view comparisonOperatorText(Queryable::SqlOperator sqlOperator) noexcept;

        /**
         * @brief 将连接类型转成 SQL 关键字
         * @param joinType 连接类型枚举
         * @return std::string_view INNER / LEFT / RIGHT / CROSS
         */
        [[nodiscard]] static std::string_view joinTypeText(Queryable::JoinType joinType) noexcept;
    };

} // namespace AsynGyanis::Database
