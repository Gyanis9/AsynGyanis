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
 * ## 实现约定（后续 MySQL / PostgreSQL 方言照此实现）
 * - translate() 不得把字段值拼进 SQL 文本，只能产出占位符并在 parameters 里按序取值；
 * - translate() 不得修改传入的查询树（入参为 const 引用，实现必须是纯函数）；
 * - quoteIdentifier() 必须转义标识符内部的引用字符（双引号翻倍 / 反引号翻倍），
 *   否则含引号的列名会破坏语句结构；
 * - 同一个 dialect 实例可能被多个线程并发调用，实现必须无状态（本层不持有可变成员）。
 */
#pragma once

#include "Database/Common/DatabaseType.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

#include <cstddef>
#include <string>
#include <string_view>

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
    };

} // namespace AsynGyanis::Database
