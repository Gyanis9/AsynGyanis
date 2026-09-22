/**
 * @file MySqlDialect.h
 * @brief MySQL 方言 —— 只覆写引擎知识，其余继承 StandardSqlDialect
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details StandardSqlDialect 的 MySQL / MariaDB 实现，只回答引擎知识：反引号引用（双引号在默认
 *          sql_mode 下是字符串字面量，用它引用列名会静默改变语义）、'?' 占位符、分页的
 *          "LIMIT ? OFFSET ?"（只给 offset 时补无符号 64 位上界）、"START TRANSACTION"、
 *          类型名按位宽与符号分家、元数据按 DATABASE() 限定当前库、参数上限 65535。
 *
 * @note MySQL 规定 OFFSET 必须跟在 LIMIT 之后，"OFFSET ?" 单独出现是语法错误，
 *       它也不接受 SQLite 那种 "LIMIT -1"（负值在 LIMIT 里被判为非法参数）。
 */
#pragma once

#include "Database/Dialect/StandardSqlDialect.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief MySQL / MariaDB SQL 方言
     *
     * @details 只覆写引擎知识（引用字符、事务语句、分页、类型名、元数据与参数上限），
     *          查询树渲染与参数收集一律继承基类 StandardSqlDialect。
     *
     * @note 无状态实现，可被多线程并发调用；实例由 DialectRegistry 以共享指针提供，
     *       调用方一般不需要自己构造。
     */
    class MySqlDialect final : public StandardSqlDialect
    {
    public:
        /**
         * @brief 默认构造函数
         */
        MySqlDialect() = default;

        /**
         * @brief 析构函数
         */
        ~MySqlDialect() override = default;

        // 方言是纯翻译规则、不含状态，拷贝一份与共享同一实例等价（registry 用共享指针持有）
        MySqlDialect(const MySqlDialect &) = default;

        MySqlDialect &operator=(const MySqlDialect &) = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @details 重写 SqlDialect::type()：恒返回 DatabaseType::MySql，与连接状态无关。
         * @return DatabaseType DatabaseType::MySql
         */
        [[nodiscard]] DatabaseType type() const noexcept override;

        /**
         * @brief 获取 MySQL 的开启事务语句
         * @details 重写 SqlDialect::beginTransactionStatement()：返回 "START TRANSACTION"。
         *          这是 MySQL 文档中的标准写法，"BEGIN" 同样可用但语义上更像过程式语句块的开始；
         *          MySQL 也没有 SQLite 的 IMMEDIATE 模式可选（InnoDB 的行锁在第一条写语句时取）。
         * @return std::string_view 恒为 "START TRANSACTION"
         */
        [[nodiscard]] std::string_view beginTransactionStatement() const noexcept override;

        /**
         * @brief 把逻辑列类型翻译成 MySQL 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：整数按位宽与符号分家，选与 C++ 类型位宽
         *          对齐的成员；Bool→"TINYINT(1)" 是官方保留的「是否型」写法
         *          （8.0.19 起整数显示宽度被弃用，唯独它例外保留），客户端据此识别布尔列。
         * @param type 逻辑列类型
         * @return std::string_view 对应物理类型名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 把逻辑列类型翻译成可作主键的 MySQL 物理类型名
         * @details 重写 SqlDialect::keyColumnTypeName()：TEXT 与 LONGBLOB 不能直接进索引
         *          （1170 号错误），而主键就是索引，因此 Text→"VARCHAR(255)"、
         *          Blob→"VARBINARY(255)"；255 在 utf8mb4 下为 1020 字节，远在 InnoDB
         *          3072 字节索引前缀上限之内，超长取值则由引擎按硬限制拒绝。
         * @param type 逻辑列类型
         * @return std::string_view 对应物理类型名；非文本/二进制类型与 columnTypeName() 一致
         */
        [[nodiscard]] std::string_view keyColumnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 MySQL 的「表是否存在」查询
         * @details 重写 SqlDialect::tableExistsStatement()：表清单在 information_schema.tables 里，
         *          它是整个实例共享的，只用 table_name 过滤会把其它库里的同名表统计进来，
         *          因此必须用 DATABASE() 同时限定当前会话的默认库。视图与表共用同一个名字空间，
         *          所以要再用 table_type='BASE TABLE' 排除视图——与 SQLite 侧 type='table' 的过滤同口径；
         *          漏掉它时视图会被报成「表已存在」，建表被跳过，之后的写入落在视图上只得到一句报错。
         * @param tableName 待查询的表名
         * @return SqlStatement "SELECT COUNT(*) FROM information_schema.tables WHERE
         *         table_schema = DATABASE() AND table_type = 'BASE TABLE' AND table_name = ?"
         *         及其唯一绑定参数；结果为一行一列，0 表示不存在
         */
        [[nodiscard]] SqlStatement tableExistsStatement(std::string_view tableName) const override;

        /**
         * @brief 获取 MySQL 单条语句的参数个数上限
         * @details 重写 SqlDialect::maximumStatementParameters()：返回 65535——COM_STMT_PREPARE 应答
         *          报文里「参数个数」字段只有 2 字节，这是协议层能表达的上限（不同于 SQLite 那种
         *          可被编译期宏调小的语言级限制）。另有实际约束：每个占位符都会在语句文本里展开，
         *          整条报文必须装进 max_allowed_packet，因此调用方仍应控制单批的数据量。
         * @return std::size_t 恒为 kMaximumStatementParameters（65535）
         */
        [[nodiscard]] std::size_t maximumStatementParameters() const noexcept override;

        static constexpr std::size_t kMaximumStatementParameters = 65535; ///< MySQL 单条预处理语句的参数个数上限（= COM_STMT_PREPARE 报文中 2 字节的 num_params 字段上界）

        static constexpr std::string_view kUnboundedRowLimitLiteral = "18446744073709551615"; ///< MySQL 表达「不限行数」的常量：无符号 64 位整数的上界（官方文档给出的 LIMIT 上界写法）

    protected:
        /**
         * @brief 取得 MySQL 的标识符引用字符
         * @details 重写 StandardSqlDialect::identifierQuoteCharacter()：MySQL 的官方引用符是
         *          反引号，因此返回 '`'（SQLite 是双引号）。
         * @return char 恒为反引号 '`'
         */
        [[nodiscard]] char identifierQuoteCharacter() const noexcept override;

        /**
         * @brief 取得本方言的显示名
         * @details 重写 StandardSqlDialect::dialectName()：返回 "MySQL"，用于拼出
         *          「MySQL 方言：取值个数（1）与待写列数（2）不一致…」这类中文错误文本。
         * @return std::string_view 恒为 "MySQL"
         */
        [[nodiscard]] std::string_view dialectName() const noexcept override;

        /**
         * @brief 渲染 MySQL 的分页子句（值走占位符，只给 OFFSET 时补出「不限行数」常量）
         *
         * @details 重写 StandardSqlDialect::appendLimitOffsetClause()：基类的 " LIMIT " + 占位符
         *          与 " OFFSET " + 占位符 正是 MySQL 的关键字形式，本覆写只补一种情形——只给了
         *          offset 而没给 limit 时先输出 " LIMIT " + kUnboundedRowLimitLiteral
         *          （MySQL 不允许 OFFSET 单独出现，也不接受 SQLite 的 "LIMIT -1"）。
         *          该常量不占绑定参数，占位符序号与参数下标的对应关系不受影响。
         *
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，分页值由基类默认实现按占位符出现顺序追加
         * @param query 提供 limit / offset 的查询树
         */
        void appendLimitOffsetClause(std::string &sqlText, std::vector<DatabaseValue> &parameters, const Queryable::QueryNode &query) const override;
    };

} // namespace AsynGyanis::Database
