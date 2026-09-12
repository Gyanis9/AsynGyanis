/**
 * @file SqlDialect.h
 * @brief SQL 方言抽象基类 —— 查询树 → 参数化 SQL 的翻译契约
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 方言层把与具体数据库无关的查询树（QueryNode）翻译成某一种数据库能执行的
 *          参数化 SQL（SqlStatement）。所有 SQL 文本差异都被收敛到本层：
 *          标识符引用字符、占位符写法、分页语法、布尔字面量等由各实现自行处理，
 *          上层的 ORM 执行器只依赖本抽象接口。
 *
 * ## 接口构成（本次演进新增写语句与事务语句）
 * - 读：translate() 生成 SELECT；
 * - 写：translateInsert() / translateUpdate() / translateDelete() / translateInsertBatch()
 *   生成 INSERT / UPDATE / DELETE / 多行 INSERT；
 * - 事务：beginTransactionStatement() / commitStatement() / rollbackStatement()
 *   给出事务控制语句文本（各引擎语法不同，不能写死在事务对象里）；
 * - 基础设施：quoteIdentifier() / placeholder() / supportsLimitOffset()。
 *
 * 写语句进入本层而不是留在 ORM 侧，是为了让「WHERE 条件怎么渲染、参数按什么顺序收集」
 * 只有一份实现：条件树可以递归嵌套、IN 集合要展开成多个占位符、IS NULL 不产生参数，
 * 这些规则一旦在 ORM 侧再写一遍，两边就会随着方言演进而产生行为差异。
 * 因此 ORM 只负责把「要写的列」和「要绑的值」整理好交给方言（列名放在 QueryNode
 * 的 selectColumns 里，值按同一顺序放在 values 里），条件仍然走 QueryNode 的
 * whereConditions，由方言统一渲染。
 *
 * ## 实现约定（后续 MySQL / PostgreSQL 方言照此实现）
 * - 任何 translate*() 都不得把字段值拼进 SQL 文本，只能产出占位符并在 parameters 里按序取值；
 * - 任何 translate*() 都不得修改传入的查询树（入参为 const 引用，实现必须是纯函数）；
 * - 任何 translate*() 产出的 parameters[i] 必须与 SQL 文本中第 i 个占位符一一对应，
 *   写语句里赋值参数在前、WHERE 条件参数在后，顺序与它们在文本中出现的先后一致；
 * - 入参个数与列数/条件不匹配时实现必须抛 std::invalid_argument，绝不能生成半截语句；
 * - quoteIdentifier() 必须转义标识符内部的引用字符（双引号翻倍 / 反引号翻倍），
 *   否则含引号的列名会破坏语句结构；
 * - 同一个 dialect 实例可能被多个线程并发调用，实现必须无状态（本层不持有可变成员）。
 */
#pragma once

#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
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
     * @brief SQL 方言抽象基类
     *
     * @details 每个受支持的数据库对应一个实现（当前只有 SqliteDialect）。
     *          实例由 DialectRegistry 按 DatabaseType 提供，调用方通过共享指针持有。
     *
     * @note 本类刻意不含任何数据成员：方言是"翻译规则"而不是"连接状态"，
     *       无状态才能被安全共享与并发调用。
     */
    class SqlDialect
    {
    public:
        /**
         * @brief 析构函数
         */
        virtual ~SqlDialect() = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @return DatabaseType 方言所属的数据库类型
         */
        [[nodiscard]] virtual DatabaseType type() const noexcept = 0;

        /**
         * @brief 把查询树翻译成带占位符的 SQL 与绑定参数
         *
         * @details 覆盖 SELECT 的全部子句：列、FROM、JOIN、WHERE、GROUP BY、HAVING、
         *          ORDER BY、LIMIT/OFFSET。所有字段值一律产出占位符，
         *          参数按占位符在 SQL 文本中出现的顺序压入 parameters。
         *
         * @param query 待翻译的查询树，本方法不修改它
         * @return SqlStatement SQL 文本与按序排列的绑定参数
         * @note 本方法不访问数据库，纯文本变换，可在任何线程上并发调用
         */
        [[nodiscard]] virtual SqlStatement translate(const Queryable::QueryNode &query) const = 0;

        /**
         * @brief 把「单行插入」翻译成带占位符的 INSERT
         *
         * @details 生成 "INSERT INTO 表 (列…) VALUES (?, …)"：要写的列取自
         *          query.selectColumns（按给定顺序），取值由 values 按同一顺序提供。
         *          表名与列名一律走本方言的标识符引用，取值一律占位符 + 绑定参数，
         *          parameters 与 values 顺序完全一致。
         *
         * @param query 提供表名与待写列的查询树，本方法不修改它
         * @param values 待绑定的字段值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement INSERT 文本与按列序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateInsert(const Queryable::QueryNode &query,
                                                           std::span<const DatabaseValue> values) const = 0;

        /**
         * @brief 把「按条件更新」翻译成带占位符的 UPDATE
         *
         * @details 生成 "UPDATE 表 SET 列 = ?, … WHERE 条件"：SET 的列取自
         *          query.selectColumns，取值由 values 按同一顺序提供；WHERE 由
         *          query.whereConditions 渲染，使用的正是与 SELECT 完全相同的条件逻辑
         *          （递归 children、IN 展开、IS NULL 不占参数）。
         *          参数顺序：先全部赋值参数，再全部条件参数，与文本中的出现顺序一致。
         *
         * @param query 提供表名、SET 列与 WHERE 条件的查询树，本方法不修改它
         * @param values 赋给各 SET 列的取值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement UPDATE 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateUpdate(const Queryable::QueryNode &query,
                                                           std::span<const DatabaseValue> values) const = 0;

        /**
         * @brief 把「按条件删除」翻译成带占位符的 DELETE
         *
         * @details 生成 "DELETE FROM 表 [WHERE 条件]"：条件部分与 SELECT / UPDATE 共用
         *          同一套渲染规则；whereConditions 为空时省略整个 WHERE 子句（即整表删除，
         *          调用方需自行确认这一语义）。
         *
         * @param query 提供表名与 WHERE 条件的查询树，本方法不修改它
         * @return SqlStatement DELETE 文本与按序排列的绑定参数
         */
        [[nodiscard]] virtual SqlStatement translateDelete(const Queryable::QueryNode &query) const = 0;

        /**
         * @brief 把「多行插入」翻译成一次多行 VALUES 的 INSERT
         *
         * @details 生成 "INSERT INTO 表 (列…) VALUES (?, …), (?, …), …"：行数由 rows 决定，
         *          每行的取值个数必须等于 query.selectColumns 的列数；参数按「行优先、行内按列序」
         *          展开。是否支持多行 VALUES 语法由各实现决定，不支持的方言可以改为
         *          生成多条语句或直接抛异常，但绝不能静默丢掉任何一行。
         *          调用方需自行控制行数使参数总数不超过引擎上限（SQLite 为 999）。
         *
         * @param query 提供表名与待写列的查询树，本方法不修改它
         * @param rows 待插入的行，每行是该行各列的取值
         * @return SqlStatement 多行 INSERT 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空、rows 为空，或某行的取值个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateInsertBatch(const Queryable::QueryNode &query,
                                                                std::span<const std::vector<DatabaseValue>> rows) const = 0;

        /**
         * @brief 获取开启事务的语句文本
         *
         * @details 各引擎语法不同（SQLite 用 "BEGIN IMMEDIATE" 立刻取写锁，
         *          MySQL 用 "START TRANSACTION"），因此语句文本由方言给出而不是写死在
         *          事务对象里；事务对象只负责在正确的连接上执行它。
         *
         * @return std::string_view 本方言的开启事务语句
         */
        [[nodiscard]] virtual std::string_view beginTransactionStatement() const noexcept = 0;

        /**
         * @brief 获取提交事务的语句文本
         * @return std::string_view 本方言的提交语句，保证语句本身不含分号
         */
        [[nodiscard]] virtual std::string_view commitStatement() const noexcept = 0;

        /**
         * @brief 获取回滚事务的语句文本
         * @return std::string_view 本方言的回滚语句，保证语句本身不含分号
         */
        [[nodiscard]] virtual std::string_view rollbackStatement() const noexcept = 0;

        /**
         * @brief 引用一个标识符
         *
         * @details SQLite / PostgreSQL 等标准方言使用双引号，MySQL 使用反引号。
         *          标识符内部与引用字符相同的字符必须翻转义（翻倍），
         *          例如 SQLite 下 quoteIdentifier("a\"b") 得到 "a""b"。
         *
         * @param identifier 待引用的标识符，不含外层的引用字符
         * @return std::string 已加引用字符并完成转义的文本
         */
        [[nodiscard]] virtual std::string quoteIdentifier(std::string_view identifier) const = 0;

        /**
         * @brief 生成第 index 个参数占位符
         *
         * @details SQLite / MySQL / PostgreSQL 的位置参数都写作 "?"，
         *          保留本接口是为了将来支持 $1 这类显式带序号的风格
         *          （PostgreSQL 的 libpq 原生协议、Oracle 的 :1 等）。
         *
         * @param index 参数序号，从 0 开始，按占位符出现顺序递增
         * @return std::string 该位置的占位符文本
         */
        [[nodiscard]] virtual std::string placeholder(std::size_t index) const = 0;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         * @return true 支持（SQLite / MySQL / PostgreSQL）；false 需要各实现自行改写分页
         */
        [[nodiscard]] virtual bool supportsLimitOffset() const noexcept = 0;

        /**
         * @brief 获取单条语句允许的最大绑定参数个数
         *
         * @details 各引擎的上限不同（SQLite 的 SQLITE_MAX_VARIABLE_NUMBER 默认 999，
         *          MySQL 的占位符上限则是 65535），属于典型的引擎知识，因此由方言回答。
         *          批量写入的调用方据此把行数与列数换算成每批行数，避免一次绑定过多参数而被引擎拒绝。
         *
         * @return std::size_t 单条语句的参数个数上限，恒大于 0
         */
        [[nodiscard]] virtual std::size_t maximumStatementParameters() const noexcept = 0;
    };

} // namespace AsynGyanis::Database
