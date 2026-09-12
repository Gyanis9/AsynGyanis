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
 * - 分页：LIMIT / OFFSET 直接内联十进制整数（取值来自强类型 size_t，不经外部文本）；
 *   OFFSET 单独出现时补 "LIMIT -1"，因为 SQLite 要求 OFFSET 必须跟在 LIMIT 之后；
 * - 表达式字段（如 COUNT(*)、COALESCE(age, 0)）原样输出，不加引号，
 *   加了引号会被当成列名从而改变语义。
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

    private:
        /**
         * @brief 渲染字段引用：纯标识符加引号，表达式原样输出
         * @details 处理三种情形：单个标识符（加双引号）、限定名（users.id → "users"."id"）、
         *          含运算符/括号/空格的表达式（原样输出，例如 COUNT(*)）。
         * @param fieldText 字段引用文本（列名或表达式）
         * @return std::string 可直接写入 SQL 的字段片段
         */
        [[nodiscard]] std::string renderFieldReference(std::string_view fieldText) const;

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
