/**
 * @file MySqlDialect.h
 * @brief MySQL 方言 —— 只覆写引擎知识，其余继承 StandardSqlDialect
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details StandardSqlDialect 的 MySQL / MariaDB 实现，与 SqliteDialect 是姊妹实现：
 *          两者共用基类同一套查询树渲染、同一套「一边拼文本一边压参数」的顺序契约，
 *          差异全部收敛在本类覆写的几个方法里。
 *
 * ## 与 SQLite 方言的差异（逐项给出依据）
 * - 标识符：反引号引用，内部反引号翻倍（`` `weird``name` ``）。反引号是 MySQL 的官方引用符，
 *   而双引号在默认的 sql_mode（不含 ANSI_QUOTES）下是**字符串字面量**——
 *   用双引号引用列名会把 `"order"` 变成常量 'order'，语句能跑但语义完全不同；
 * - 占位符：同为 '?'（MySQL 的位置参数只认问号，也不是 $1 风格）；
 * - 分页：用标准关键字形式 "LIMIT ? OFFSET ?"，值以绑定参数送出，理由见下；
 * - 事务：开启用 "START TRANSACTION"（不是 SQLite 的 "BEGIN IMMEDIATE"——
 *   MySQL 没有 IMMEDIATE 关键字，且 InnoDB 的行锁在第一条写语句时取，不存在
 *   SQLite 那种「读锁升级为写锁」的死锁模型）；
 * - 参数上限：65535（协议层硬上限，见 maximumStatementParameters()），不是 SQLite 的 999；
 * - DDL 支撑：类型名按位宽与符号分家（BIGINT / BIGINT UNSIGNED / DOUBLE / TINYINT(1) / TEXT / LONGBLOB），
 *   布尔没有独立类型，用官方惯例的 TINYINT(1)；表清单来自全实例共享的 information_schema.tables，
 *   因此必须用 DATABASE() 限定当前库（SQLite 查的是每个库文件私有的 sqlite_master）。
 *
 * ## 分页为何选 "LIMIT ? OFFSET ?"
 * - 关键字形式是标准 SQL 的写法，MySQL / MariaDB / PostgreSQL / SQLite 都接受；
 *   而 "LIMIT 偏移量, 行数" 是 MySQL 专有语法，且两个操作数的顺序与关键字形式**相反**
 *   （先给偏移量再给行数），是分页翻页时的经典错误来源，因此不选它；
 * - 取值走占位符而不是内联十进制文本，使「SQL 文本里绝不出现数据」这条契约在本方言中
 *   没有例外：分页值与其它取值一样由驱动的绑定接口送入，调用方不必区分哪类值被内联了；
 * - 参数顺序为「先 LIMIT 后 OFFSET」，与 QueryNode 的 limit / offset 字段顺序一致，
 *   也与文本中占位符的出现顺序一致。
 *
 * @note MySQL 规定 OFFSET 必须跟在 LIMIT 之后，"OFFSET ?" 单独出现是语法错误。
 *       只给了 offset 时，本实现补出官方文档给出的「不限行数」写法
 *       "LIMIT 18446744073709551615 OFFSET ?"（2^64-1，即无符号 64 位整数的上界）。
 *       MySQL 不接受 SQLite 那种 "LIMIT -1"，负值在 LIMIT 里会被判为非法参数。
 *
 * ## 继承自基类的部分（本类不重复实现）
 * - 全部 translate*()：SELECT / INSERT / UPDATE / DELETE / 多行 INSERT 的文本拼装；
 * - quoteIdentifier()：按 identifierQuoteCharacter() 给出的反引号加引用并翻倍转义；
 * - 全部私有渲染辅助与参数转换：条件树的递归展开、IN 展开、参数顺序、uint64 降级等。
 *
 * ## 安全考量
 * 所有来自 C++ 侧的数据（比较值、IN 列表、分页值）都只以占位符形式出现，
 * 值本身通过 parameters 交给驱动的绑定接口，因此含单引号、"--"、分号的字符串
 * 只会被当成普通文本，不会改变语句结构。详见 SqlStatement.h 的说明。
 *
 * @code
 *   MySqlDialect dialect;
 *   Queryable::QueryNode node;
 *   node.tableName = "users";
 *   node.whereConditions.push_back(WhereCondition{
 *       .left  = Queryable::FieldReference{.name = "age"},
 *       .op    = Queryable::SqlOperator::Ge,
 *       .right = Queryable::ParameterValue{std::int64_t{18}}
 *   });
 *   SqlStatement statement = dialect.translate(node);
 *   // => sql:        SELECT * FROM `users` WHERE `age` >= ?
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
     * @brief MySQL / MariaDB SQL 方言
     *
     * @details 本类是 StandardSqlDialect 的实现，只覆写引擎知识，查询树渲染与参数收集一律继承。
     *          与姊妹方言的差异（逐项给出依据）：
     *          - identifierQuoteCharacter() 返回反引号（SQLite 与 PostgreSQL 是双引号）；
     *          - dialectName() 返回 "MySQL"，用于拼出「MySQL 方言：…」这类中文错误文本；
     *          - appendLimitOffsetClause() 让 limit / offset 都占绑定参数（SQLite 内联十进制文本），
     *            且只给 offset 时补出无符号 64 位上界常量作为「不限行数」（SQLite 用 "LIMIT -1"，
     *            PostgreSQL 允许 OFFSET 单独出现、无需补任何常量）；
     *          - placeholder() 返回 "?"（PostgreSQL 是 "$n"）；
     *          - beginTransactionStatement() 返回 "START TRANSACTION"
     *            （SQLite 是 "BEGIN IMMEDIATE"、PostgreSQL 是 "BEGIN"）；
     *          - maximumStatementParameters() 返回 65535（SQLite 是 999）；
     *          - columnTypeName() / tableExistsStatement() 反映 MySQL 的位宽/符号分家与
     *            information_schema + DATABASE() 限定。
     *          除上述方法外本类不再提供任何成员：基类已给出这些行为的唯一实现。
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
        MySqlDialect(const MySqlDialect &)            = default;
        MySqlDialect &operator=(const MySqlDialect &) = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @details 重写 SqlDialect::type()：恒返回 DatabaseType::MySql，与连接状态无关。
         * @return DatabaseType DatabaseType::MySql
         */
        [[nodiscard]] DatabaseType type() const noexcept override;

        /**
         * @brief 生成第 index 个参数占位符
         * @details 重写 SqlDialect::placeholder()：MySQL 的位置参数（文本协议与
         *          mysql_stmt_* 预处理接口）都写作 "?"，序号只用于按顺序收集参数。
         * @param index 参数序号，从 0 开始
         * @return std::string 恒为 "?"
         */
        [[nodiscard]] std::string placeholder(std::size_t index) const override;

        /**
         * @brief 获取 MySQL 的开启事务语句
         * @details 重写 SqlDialect::beginTransactionStatement()：返回 "START TRANSACTION"。
         *          这是 MySQL 文档中的标准写法，"BEGIN" 同样可用但语义上更像过程式语句块的开始；
         *          MySQL 也没有 SQLite 的 IMMEDIATE 模式可选（InnoDB 的行锁在第一条写语句时取）。
         * @return std::string_view 恒为 "START TRANSACTION"
         */
        [[nodiscard]] std::string_view beginTransactionStatement() const noexcept override;

        /**
         * @brief 获取 MySQL 的提交事务语句
         * @details 重写 SqlDialect::commitStatement()：返回 "COMMIT"。
         * @return std::string_view 恒为 "COMMIT"
         */
        [[nodiscard]] std::string_view commitStatement() const noexcept override;

        /**
         * @brief 获取 MySQL 的回滚事务语句
         * @details 重写 SqlDialect::rollbackStatement()：返回 "ROLLBACK"。
         * @return std::string_view 恒为 "ROLLBACK"
         */
        [[nodiscard]] std::string_view rollbackStatement() const noexcept override;

        /**
         * @brief 把逻辑列类型翻译成 MySQL 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：MySQL 的整数按位宽与符号分家，
         *          这里一律选与 C++ 类型位宽对齐的成员：
         *          - Int64 → "BIGINT"（8 字节有符号，对应 C++ 的 std::int64_t）；
         *          - UInt64 → "BIGINT UNSIGNED"（8 字节无符号，0 .. 2^64-1）；
         *          - Double → "DOUBLE"（8 字节 IEEE 754；不用 FLOAT，它只有 4 字节且精度不足）；
         *          - Bool → "TINYINT(1)"：MySQL 没有布尔类型，BOOL/BOOLEAN 只是 TINYINT(1) 的同义词，
         *            而 TINYINT(1) 是官方保留的「是否型」惯例写法（8.0.19 起整数显示宽度被弃用，
         *            唯独 TINYINT(1) 例外保留），客户端也据此识别布尔列；
         *          - Text → "TEXT"（上限 65535 字节，按列字符集编码；utf8mb4 下约可放 16000 个字符）；
         *          - Blob → "LONGBLOB"（上限 4 GiB，足以直接承接任意二进制载荷）。
         * @param type 逻辑列类型
         * @return std::string_view 对应物理类型名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 MySQL 的「表是否存在」查询
         * @details 重写 SqlDialect::tableExistsStatement()：MySQL 没有 SQLite 那样的库内元数据表，
         *          表清单在 information_schema.tables 里，而它是整个实例共享的：
         *          只用 table_name 过滤会把其它库里的同名表也统计进来，导致「表其实不存在却报告存在」，
         *          因此必须用 DATABASE() 同时限定当前会话的默认库。
         *          table_name 以参数绑定送入，表名里的反引号或分号都不会改变语句结构。
         * @param tableName 待查询的表名
         * @return SqlStatement "SELECT COUNT(*) FROM information_schema.tables WHERE
         *         table_schema = DATABASE() AND table_name = ?" 及其唯一绑定参数；
         *         结果为一行一列，0 表示不存在
         */
        [[nodiscard]] SqlStatement tableExistsStatement(std::string_view tableName) const override;

        /**
         * @brief 获取 MySQL 单条语句的参数个数上限
         * @details 重写 SqlDialect::maximumStatementParameters()：返回常量 kMaximumStatementParameters。
         *          取值依据是客户端/服务端协议本身：预处理语句的 COM_STMT_PREPARE 应答报文里，
         *          「参数个数」字段只有 2 字节，因此 65535 是协议层能表达的上限，与 SQLite 那种
         *          可被编译期宏调小的语言级限制不是一回事。协议之外还有一层实际约束：
         *          每个占位符都会在语句文本里展开，整条 COM_STMT_PREPARE 报文必须能装进
         *          max_allowed_packet（MySQL 8.0 默认 64 MiB），因此调用方在按本上限分块之外，
         *          仍应控制单批的数据量，避免「参数个数没超、报文却超了」。
         * @return std::size_t 恒为 kMaximumStatementParameters（65535）
         */
        [[nodiscard]] std::size_t maximumStatementParameters() const noexcept override;

        /// MySQL 单条预处理语句的参数个数上限（= COM_STMT_PREPARE 报文中 2 字节的 num_params 字段上界）
        static constexpr std::size_t kMaximumStatementParameters = 65535;

        /// MySQL 表达「不限行数」的常量：无符号 64 位整数的上界（官方文档给出的 LIMIT 上界写法）
        static constexpr std::string_view kUnboundedRowLimitLiteral = "18446744073709551615";

    protected:
        /**
         * @brief 取得 MySQL 的标识符引用字符
         * @details 重写 StandardSqlDialect::identifierQuoteCharacter()：MySQL 的官方引用符是
         *          反引号，因此返回 '`'（SQLite 与 PostgreSQL 都是双引号）。
         *          该字符同时驱动基类 quoteIdentifier() 的加引用与内部翻倍转义，
         *          以及 renderFieldReference() 的「是否是可引用标识符」判定，两处必须一致。
         * @return char 恒为反引号 '`'
         */
        [[nodiscard]] char identifierQuoteCharacter() const noexcept override;

        /**
         * @brief 取得本方言的显示名
         * @details 重写 StandardSqlDialect::dialectName()：返回 "MySQL"，用于拼出
         *          「MySQL 方言：取值个数（1）与待写列数（2）不一致…」这类中文错误文本，
         *          保留引擎名是为了让多方言并存的调用方能立刻判断是哪一侧的输入有问题。
         * @return std::string_view 恒为 "MySQL"
         */
        [[nodiscard]] std::string_view dialectName() const noexcept override;

        /**
         * @brief 渲染 MySQL 的分页子句（值走占位符，只给 OFFSET 时补出「不限行数」常量）
         *
         * @details 重写 StandardSqlDialect::appendLimitOffsetClause()：基类的默认实现已经给出
         *          " LIMIT " + 占位符 与 " OFFSET " + 占位符 的标准写法（先 LIMIT 后 OFFSET，
         *          两者都没有则什么都不输出），与 MySQL 的关键字形式 "LIMIT ? OFFSET ?" 完全一致，
         *          因此本覆写只在一种情形下先行补料：**只给了 offset 而没给 limit 时**，
         *          先输出 " LIMIT " + kUnboundedRowLimitLiteral（无符号 64 位整数的上界，
         *          官方文档给出的「不限行数」写法），随后才把余下部分交给基类默认实现。
         *          MySQL 不允许 OFFSET 单独出现，也不能像 SQLite 那样写 "LIMIT -1"（负值判非法）。
         *          补出的常量是编译期字面量，不引入注入面，也不占绑定参数，
         *          因此占位符序号与参数下标的对应关系不受影响。
         *          limit 与 offset 的占位符、以及「先取占位符序号、再压参数」的顺序
         *          全部由基类默认实现保证，本覆写不重复实现，也不自行向 parameters 追加元素。
         *
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，分页值由基类默认实现按占位符出现顺序追加
         * @param query 提供 limit / offset 的查询树
         */
        void appendLimitOffsetClause(std::string &sqlText,
                                    std::vector<DatabaseValue> &parameters,
                                    const Queryable::QueryNode &query) const override;
    };

} // namespace AsynGyanis::Database
