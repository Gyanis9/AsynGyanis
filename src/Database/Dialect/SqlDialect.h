/**
 * @file SqlDialect.h
 * @brief SQL 方言抽象基类 —— 查询树 → 参数化 SQL 的翻译契约
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 方言层把与具体数据库无关的查询树（QueryNode）翻译成本引擎能执行的参数化 SQL
 *          （SqlStatement）：标识符引用、占位符写法、分页语法等文本差异全部收敛在本层，
 *          上层 ORM 只依赖本抽象接口。实现必须无状态（同一实例可能被多线程并发调用），
 *          且任何 translate*() 都不得修改传入的查询树、不得把取值拼进 SQL 文本。
 */
#pragma once

#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

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
         * @throws Base::InvalidArgumentException 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateInsert(const Queryable::QueryNode &query, std::span<const DatabaseValue> values) const = 0;

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
         * @throws Base::InvalidArgumentException 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateUpdate(const Queryable::QueryNode &query, std::span<const DatabaseValue> values) const = 0;

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
         * @throws Base::InvalidArgumentException 列数为空、rows 为空，或某行的取值个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateInsertBatch(const Queryable::QueryNode &query, std::span<const std::vector<DatabaseValue> > rows) const = 0;

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
         * @brief 把一个逻辑列类型翻译成本引擎的物理类型名
         *
         * @details 建表迁移（SchemaMigrator）只按成员类型给出逻辑类型（ColumnType），
         *          物理类型名由各引擎回答：SQLite 的存储类只有 5 个（INTEGER/REAL/TEXT/BLOB/NULL），
         *          MySQL 的整数按位宽分家且有真正的无符号类型，布尔在两者中都不是独立物理类型。
         *          这类「引擎知识」若写在 ORM 侧，每加一个方言就要改一次 ORM，因此放在本层。
         *
         * @param type 逻辑列类型
         * @return std::string_view 该引擎可直接写进列定义的物理类型名（不含 NOT NULL 等约束），
         *         例如 SQLite 的 "INTEGER"、MySQL 的 "BIGINT UNSIGNED"
         * @note 本方法只做「类型名」翻译，不涉及取值编解码；值一律以 DatabaseValue 绑定给驱动
         */
        [[nodiscard]] virtual std::string_view columnTypeName(ColumnType type) const noexcept = 0;

        /**
         * @brief 把一个逻辑列类型翻译成**可作主键**的物理类型名
         *
         * @details 主键（以及任何索引列）对物理类型有额外要求：MySQL 的 TEXT/LONGBLOB
         *          不能直接进索引，建表语句会以 1170 号错误被拒，必须换成带长度的同族类型
         *          （VARCHAR/VARBINARY）。默认实现直接沿用 columnTypeName()——SQLite 的
         *          TEXT 主键本就合法，无需区分；只有引擎存在这种限制时才重写。
         *
         * @param type 逻辑列类型
         * @return std::string_view 该引擎可直接写进**主键列定义**的物理类型名
         * @note 需要为键列换类型时，换出的类型可能比原类型短（例如 TEXT → VARCHAR(255)），
         *       超长取值会被引擎按自己的规则拒绝——这是引擎侧的硬限制，不是本映射的取舍
         */
        [[nodiscard]] virtual std::string_view keyColumnTypeName(const ColumnType type) const noexcept
        {
            return columnTypeName(type);
        }

        /**
         * @brief 生成「查询某张表是否存在」的元数据语句
         *
         * @details 各引擎的表清单来源完全不同：SQLite 查 sqlite_master，MySQL 查
         *          information_schema（并且要按当前库名过滤，否则同名表在别的库里会被误判存在）。
         *          这类元数据查询与事务语句同属「引擎知识」，因此同样由方言给出文本，
         *          迁移工具只负责执行与解读结果。
         *
         * @param tableName 待查询的表名（未加引用字符的原始名字）
         * @return SqlStatement 返回「一行一列」的计数语句：第一列是匹配该表名的行数
         *         （0 表示不存在，大于 0 表示存在）；表名以占位符 + 绑定参数送出，不拼进 SQL 文本
         */
        [[nodiscard]] virtual SqlStatement tableExistsStatement(std::string_view tableName) const = 0;

        /**
         * @brief 引用一个标识符
         *
         * @details SQLite 等标准方言使用双引号，MySQL 使用反引号。
         *          标识符内部与引用字符相同的字符必须翻转义（翻倍），
         *          例如 SQLite 下 quoteIdentifier("a\"b") 得到 "a""b"。
         *
         * @param identifier 待引用的标识符，不含外层的引用字符
         * @return std::string 已加引用字符并完成转义的文本
         */
        [[nodiscard]] virtual std::string quoteIdentifier(std::string_view identifier) const = 0;

        /**
         * @brief 生成一个参数占位符
         *
         * @details 现有两个方言的位置参数都写作 "?"，不含序号信息；参数与占位符的对应关系
         *          由「按出现顺序依次压入 parameters」保证（见本文件的参数顺序契约），
         *          因此本接口不需要知道自己是第几个参数。
         *
         * @return std::string 占位符文本，当前恒为 "?"
         */
        [[nodiscard]] virtual std::string placeholder() const = 0;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         * @return true 支持（SQLite / MySQL）；false 需要各实现自行改写分页
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
