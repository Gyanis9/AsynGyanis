/**
 * @file SqliteDialect.h
 * @brief SQLite 方言 —— 只覆写引擎知识，其余继承 StandardSqlDialect
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details StandardSqlDialect 的 SQLite 实现。SQLite 采用 SQL-92 风格的标准语法，
 *          与通用 SQL 最接近，因此查询树渲染（列/FROM/JOIN/WHERE/GROUP BY/HAVING/
 *          ORDER BY 的拼装、条件递归、IN 展开、参数收集顺序）完全继承基类，
 *          本类只回答「SQLite 特有」的那部分：引用字符与转义、占位符写法、事务语句、
 *          分页写法、类型名映射、元数据查询、参数上限。
 *
 * ## SQLite 引擎知识（本类唯一负责的部分）
 * - 标识符：双引号引用，内部双引号翻倍转义（"weird""name"）——由基类的
 *   quoteIdentifier() 按 identifierQuoteCharacter() 给出的字符执行；
 * - 占位符：一律 '?'，不区分类型，序号在文本中不体现；
 * - 分页：LIMIT / OFFSET 直接内联十进制整数（取值来自强类型 std::size_t，不经外部文本）；
 *   OFFSET 单独出现时补 "LIMIT -1"，因为 SQLite 要求 OFFSET 必须跟在 LIMIT 之后，
 *   而 "LIMIT -1" 正是 SQLite 表达「不限行数」的官方写法；
 * - 事务语句：开启用 "BEGIN IMMEDIATE"（立刻取写锁，避免多个连接都先取读锁、
 *   升级为写锁时撞上 SQLITE_BUSY 的经典死锁），提交用 "COMMIT"，回滚用 "ROLLBACK"；
 * - DDL 支撑：columnTypeName() 把逻辑列类型映射到 SQLite 的存储类
 *   （INTEGER / REAL / TEXT / BLOB，布尔与无符号整数都用 INTEGER 表达），
 *   tableExistsStatement() 查当前库文件私有的 sqlite_master 统计同名表，
 *   因此不需要任何库名限定（这是与 MySQL / PostgreSQL 的主要差异）；
 * - 参数上限：999（SQLITE_MAX_VARIABLE_NUMBER 的默认值）。
 *
 * ## 参数收集顺序（继承基类，不在本类重复实现）
 * WHERE / HAVING / JOIN...ON 里的比较值、IN 列表按出现顺序收集，
 * IS NULL / IS NOT NULL 不产生参数，列-列比较不产生参数。
 * 写语句（INSERT / UPDATE / DELETE / 多行 INSERT）与 SELECT 共用基类同一套条件渲染，
 * 因此条件树的递归展开、IN 展开、参数顺序在四个方向上的行为必然一致。
 * 多行 VALUES 自 SQLite 3.7.11 起受支持，基类的 "VALUES (…), (…)" 生成方式可直接使用，
 * 行数由调用方按 maximumStatementParameters() 自行分块。
 *
 * ## 表达式字段的判定
 * 表达式（如 COUNT(*)、COALESCE(age, 0)）原样输出，不加引号，加了引号会被当成列名从而
 * 改变语义。判定依据是「文本里有没有运算符、括号、逗号这类结构字符」：只含标识符字节与
 * 空格的文本一律按标识符加双引号引用，因此 foo bar 这种含空格的列名能被正确引用。
 * 该判定同样由基类实现，三个引擎共用。
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

#include "Database/Dialect/StandardSqlDialect.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief SQLite SQL 方言
     *
     * @details 本类是 StandardSqlDialect 的实现，只覆写引擎知识，查询树渲染与参数收集一律继承。
     *          与姊妹方言的差异（逐项给出依据）：
     *          - identifierQuoteCharacter() 返回双引号（MySQL 是反引号）；
     *          - dialectName() 返回 "SQLite"，用于拼出「SQLite 方言：…」这类中文错误文本；
     *          - appendLimitOffsetClause() 把分页值内联为十进制文本、不占绑定参数
     *            （MySQL 与 PostgreSQL 都走占位符）；
     *          - placeholder() 返回 "?"（PostgreSQL 是 "$n"）；
     *          - beginTransactionStatement() 返回 "BEGIN IMMEDIATE"
     *            （MySQL 是 "START TRANSACTION"、PostgreSQL 是 "BEGIN"）；
     *          - maximumStatementParameters() 返回 999（另两者是 65535）；
     *          - columnTypeName() / tableExistsStatement() 反映 SQLite 的存储类与 sqlite_master。
     *          除上述方法外本类不再提供任何成员：基类已给出这些行为的唯一实现。
     *
     * @note 无状态实现，可被多线程并发调用；实例由 DialectRegistry 以共享指针提供，
     *       调用方一般不需要自己构造。
     */
    class SqliteDialect final : public StandardSqlDialect
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
         * @brief 生成第 index 个参数占位符
         * @details 重写 SqlDialect::placeholder()：SQLite 的位置参数不区分类型，
         *          统一写作 "?"，序号仅用于按顺序收集参数，不体现在文本里。
         * @param index 参数序号，从 0 开始
         * @return std::string 恒为 "?"
         */
        [[nodiscard]] std::string placeholder(std::size_t index) const override;

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
         * @brief 把逻辑列类型翻译成 SQLite 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：SQLite 只有 5 个存储类
         *          （NULL / INTEGER / REAL / TEXT / BLOB），因此映射表很小：
         *          - Int64 → INTEGER（SQLite 的 INTEGER 是变长整数，最多 8 字节有符号）；
         *          - UInt64 → INTEGER：SQLite 根本没有无符号类型，只能退化为有符号 64 位，
         *            即 0 .. 2^63-1 之外的无符号取值无法以此列型原样表达。
         *            取舍：仍然用 INTEGER 而不是 TEXT。用 TEXT 会让排序、比较、索引全部退化为
         *            字符串语义（"10" < "9"），代价比取值范围上限大得多；超出 int64 上限的值
         *            由绑定期降级为十进制文本（见基类 convertParameter），因此更靠上的处理不需要改；
         *          - Double → REAL（8 字节 IEEE 754，与 C++ 的 double 一一对应）；
         *          - Bool → INTEGER：SQLite 没有布尔存储类，官方建议用整数 0/1 表达真假；
         *          - Text → TEXT（按数据库编码存储，默认 UTF-8）；
         *          - Blob → BLOB（按输入字节原样存储，不做任何转换）。
         * @param type 逻辑列类型
         * @return std::string_view 对应存储类名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 SQLite 的「表是否存在」查询
         * @details 重写 SqlDialect::tableExistsStatement()：SQLite 的表清单存放在
         *          sqlite_master（只读系统表）里，用 type='table' 过滤掉索引、视图与触发器，
         *          再按 name 精确匹配目标表名。sqlite_master 只属于当前所连接的那个库文件，
         *          所以不需要任何库名限定——这是嵌入式引擎与 MySQL / PostgreSQL 的主要差异。
         *          name 列以参数绑定送入，表名里出现引号或分号都不会改变语句结构。
         * @param tableName 待查询的表名
         * @return SqlStatement "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?"
         *         及其唯一绑定参数；结果为一行一列，0 表示不存在
         */
        [[nodiscard]] SqlStatement tableExistsStatement(std::string_view tableName) const override;

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

    protected:
        /**
         * @brief 取得 SQLite 的标识符引用字符
         * @details 重写 StandardSqlDialect::identifierQuoteCharacter()：SQLite 接受 SQL 标准的
         *          双引号形式，因此返回 '"'（与 PostgreSQL 相同，与 MySQL 的反引号不同）。
         *          该字符同时驱动基类 quoteIdentifier() 的加引用与内部翻倍转义，
         *          以及 renderFieldReference() 的「是否是可引用标识符」判定，两处必须一致。
         * @return char 恒为双引号 '"'
         */
        [[nodiscard]] char identifierQuoteCharacter() const noexcept override;

        /**
         * @brief 取得本方言的显示名
         * @details 重写 StandardSqlDialect::dialectName()：返回 "SQLite"，用于拼出
         *          「SQLite 方言：待写列列表为空…」这类中文错误文本，
         *          保留引擎名是为了让多方言并存的调用方能立刻判断是哪一侧的输入有问题。
         * @return std::string_view 恒为 "SQLite"
         */
        [[nodiscard]] std::string_view dialectName() const noexcept override;

        /**
         * @brief 渲染 SQLite 的分页子句（分页值内联，不占绑定参数）
         *
         * @details 重写 StandardSqlDialect::appendLimitOffsetClause()：基类的默认实现把
         *          limit / offset 各写成一个占位符并压入绑定参数，SQLite 的差异在于分页值
         *          **一律内联成十进制文本**：
         *          - 只给 limit：输出 " LIMIT " + std::to_string(limit)；
         *          - 只给 offset：先补 " LIMIT -1"（SQLite 规定 OFFSET 必须紧跟 LIMIT 之后，
         *            "LIMIT -1" 是它表达「不限行数」的官方写法），再输出 " OFFSET " + std::to_string(offset)；
         *          - 两者都给：先 " LIMIT " + 行数，再 " OFFSET " + 偏移量，顺序与基类一致；
         *          - 两者都不给：什么都不输出（与基类默认实现相同）。
         *          内联在安全上成立：取值来自 QueryNode 的 std::size_t（强类型、非外部文本），
         *          不存在注入面；此外它让 SQLite 在编译期就能确定行数上限，
         *          不把 LIMIT 也做成运行时绑定参数。
         *          本覆写因此不向 parameters 追加任何内容。
         *
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，本实现不向其追加元素
         * @param query 提供 limit / offset 的查询树
         */
        void appendLimitOffsetClause(std::string &sqlText,
                                    std::vector<DatabaseValue> &parameters,
                                    const Queryable::QueryNode &query) const override;
    };

} // namespace AsynGyanis::Database
