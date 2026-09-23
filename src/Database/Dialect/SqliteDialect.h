/**
 * @file SqliteDialect.h
 * @brief SQLite 方言 —— 只覆写引擎知识，其余继承 StandardSqlDialect
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details StandardSqlDialect 的 SQLite 实现，只回答引擎知识：双引号引用、'?' 占位符、
 *          "BEGIN IMMEDIATE" 开事务（立刻取写锁，避免读锁升级为写锁时撞上 SQLITE_BUSY 的死锁）、
 *          分页值内联十进制且 OFFSET 单独出现时补 "LIMIT -1"（SQLite 要求 OFFSET 跟在 LIMIT 之后）、
 *          类型名映射到存储类、元数据查 sqlite_master、参数上限 999；条件渲染与参数顺序继承基类。
 */
#pragma once

#include "Database/Dialect/StandardSqlDialect.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief SQLite SQL 方言
     *
     * @details 只覆写引擎知识（引用字符、事务语句、分页、类型名、元数据与参数上限），
     *          查询树渲染与参数收集一律继承基类 StandardSqlDialect。
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
        SqliteDialect(const SqliteDialect &) = default;

        SqliteDialect &operator=(const SqliteDialect &) = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @details 重写 SqlDialect::type()：恒返回 DatabaseType::Sqlite，与连接状态无关。
         * @return DatabaseType DatabaseType::Sqlite
         */
        [[nodiscard]] DatabaseType type() const noexcept override;

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
         * @brief 把逻辑列类型翻译成 SQLite 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：SQLite 只有 5 个存储类，UInt64 只能退化为
         *          有符号 INTEGER（超过 int64 上限的取值由绑定期降级为十进制文本）；不用 TEXT
         *          承载整数，是因为字符串语义会让排序、比较、索引全部退化（"10" < "9"）。
         * @param type 逻辑列类型
         * @return std::string_view 对应存储类名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 SQLite 的自增主键列定义
         * @details 重写 SqlDialect::autoIncrementPrimaryKeyDefinition()：写成 "col" INTEGER PRIMARY KEY
         *          AUTOINCREMENT。类型必须是 INTEGER 且主键是单列——这是 SQLite 把该列认作 rowid 别名
         *          的语法条件（写成 INT/BIGINT 只会得到一个普通整数主键）；非整数主键给空串，由
         *          迁移工具在建表前报「不支持」。AUTOINCREMENT 一词只能跟在 PRIMARY KEY 之后，
         *          作用是「不复用已删除行的 rowid」，不写时 SQLite 仍会自增但会回收空洞。
         * @param quotedColumnName 已按本方言引用好的列名
         * @param type 主键成员的逻辑列类型
         * @return std::string 该列的完整定义；主键不是整数类型时为空串
         */
        [[nodiscard]] std::string autoIncrementPrimaryKeyDefinition(std::string_view quotedColumnName,
                                                                   ColumnType type) const override;

        /**
         * @brief 生成 SQLite 的「表是否存在」查询
         * @details 重写 SqlDialect::tableExistsStatement()：表清单在 sqlite_master（只读系统表）里，
         *          用 type='table' 过滤掉索引、视图与触发器。它是当前库文件私有的，因此不需要
         *          MySQL 那样的库名限定；name 以参数绑定送入，表名里的引号或分号不改变语句结构。
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

        static constexpr std::size_t kMaximumStatementParameters = 999; ///< SQLite 单条语句的参数个数保守上限（= SQLITE_MAX_VARIABLE_NUMBER 的默认值 999）

    protected:
        /**
         * @brief 取得 SQLite 的标识符引用字符
         * @details 重写 StandardSqlDialect::identifierQuoteCharacter()：SQLite 接受 SQL 标准的
         *          双引号形式，因此返回 '"'（与 MySQL 的反引号不同）。
         * @return char 恒为双引号 '"'
         */
        [[nodiscard]] char identifierQuoteCharacter() const noexcept override;

        /**
         * @brief 取得本方言的显示名
         * @details 重写 StandardSqlDialect::dialectName()：返回 "SQLite"，用于拼出
         *          「SQLite 方言：待写列列表为空…」这类中文错误文本。
         * @return std::string_view 恒为 "SQLite"
         */
        [[nodiscard]] std::string_view dialectName() const noexcept override;

        /**
         * @brief 渲染 SQLite 的分页子句（分页值内联，不占绑定参数）
         *
         * @details 重写 StandardSqlDialect::appendLimitOffsetClause()：基类把 limit / offset 各写成
         *          一个占位符压进 parameters，SQLite 则把分页值一律内联成十进制文本（只给 offset 时
         *          先补 " LIMIT -1"——SQLite 规定 OFFSET 必须紧跟 LIMIT）。取值来自 QueryNode 的
         *          std::size_t，强类型且非外部文本，内联不存在注入面。
         *
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，本实现不向其追加元素
         * @param query 提供 limit / offset 的查询树
         */
        void appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const override;
    };

} // namespace AsynGyanis::Database
