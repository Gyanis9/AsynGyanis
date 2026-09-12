/**
 * @file PostgresDialect.h
 * @brief PostgreSQL 方言 —— 把查询树翻译成 PostgreSQL 可执行的参数化 SQL
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details StandardSqlDialect 的 PostgreSQL 实现。查询树渲染（条件递归、IN 展开、
 *          三方向共用的 WHERE、参数收集顺序）全部继承自基类，本类只给出引擎知识。
 *
 * ## 与 SQLite / MySQL 方言的差异（逐项给出依据）
 * - 占位符：**写作 "$1" "$2" 而不是 "?"**。这是 PostgreSQL 自己的语法：
 *   扩展查询协议（PQexecParams / PQprepare）要求语句文本用 $n 标出参数位置，
 *   序号从 1 开始。因此本方言是唯一真正用到 placeholder() 序号参数的实现——
 *   基类保证「先取序号再压参数」，序号与 parameters 的下标严格对应；
 * - 标识符：双引号引用，内部双引号翻倍（"weird""name"），与 SQLite 相同、
 *   与 MySQL 的反引号不同。PostgreSQL 会把未引用的标识符折叠成小写，
 *   因此一律引用也顺带保证了大写列名的可用性；
 * - 分页：直接沿用基类的标准关键字形式，值以占位符送出。PostgreSQL 既允许
 *   "LIMIT $1" 单独出现，也允许 "OFFSET $2" 单独出现（SQLite 需要补 "LIMIT -1"，
 *   MySQL 需要补无符号上界常量），因此本类**不需要覆写分页钩子**；
 * - 事务：开启用 "BEGIN"（PostgreSQL 的等价写法 START TRANSACTION 同样可用，
 *   BEGIN 更短且是官方文档的首选），提交 COMMIT、回滚 ROLLBACK；
 * - 参数上限：65535，依据是扩展查询协议 Bind 报文里参数个数为 Int16 字段；
 * - 类型名：BIGINT / NUMERIC(20) / DOUBLE PRECISION / BOOLEAN / TEXT / BYTEA。
 *   PostgreSQL 是三个引擎里唯一有真布尔类型的，也是唯一需要为无符号 64 位
 *   另选类型的（它没有无符号整数，见 columnTypeName() 的说明）；
 * - 表清单：查 information_schema.tables 并按 current_schema() 过滤
 *   （MySQL 用 DATABASE()，SQLite 查库内私有的 sqlite_master）。
 *
 * ## 大小写与 search_path
 * 本方言一律给标识符加双引号，因此表名/列名的大小写被原样保留；
 * 表存在性查询用 current_schema() 限定，与不带模式名执行 DML 时的解析规则一致。
 *
 * ## 一个必须在送参数之前知道的引擎限制
 * PostgreSQL 的文本类型（TEXT / VARCHAR / JSON / 字符类型）**无法存储 NUL 字节**：
 * 服务端会以「invalid byte sequence for encoding "UTF8": 0x00」之类报文拒绝整条语句。
 * 因此含 '\0' 的 std::string 参数不能直接用于文本列（本驱动会在送出前本地拒绝并给出中文原因，
 * 见 PostgresConnection::execute()）。需要承载任意二进制时应使用 BYTEA 列，
 * 并把取值按十六进制文本（形如 \x48656c6c6f）作为普通参数送出——那是纯 ASCII、不含 NUL 的文本。
 * 注意这是引擎的存储限制，不是本方言或 ORM 的取舍；同一段带 '\0' 的文本在 MySQL / SQLite 上是合法的。
 *
 * ## 安全考量
 * 所有来自 C++ 侧的数据（比较值、IN 列表、分页值）都只以 $n 占位符出现，
 * 值本身通过 parameters 交给 PQexecParams 绑定，因此含单引号、"--"、分号的字符串
 * 只会被当成普通文本，不会改变语句结构。
 *
 * @code
 *   PostgresDialect dialect;
 *   Queryable::QueryNode node;
 *   node.tableName = "users";
 *   node.whereConditions.push_back(WhereCondition{
 *       .left  = Queryable::FieldReference{.name = "age"},
 *       .op    = Queryable::SqlOperator::Ge,
 *       .right = Queryable::ParameterValue{std::int64_t{18}}
 *   });
 *   SqlStatement statement = dialect.translate(node);
 *   // => sql:        SELECT * FROM "users" WHERE "age" >= $1
 *   // => parameters: { std::int64_t{18} }
 * @endcode
 */
#pragma once

#include "Database/Dialect/StandardSqlDialect.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief PostgreSQL SQL 方言
     *
     * @details 无状态实现，可被多线程并发调用；实例由 DialectRegistry 以共享指针提供，
     *          调用方一般不需要自己构造。
     */
    class PostgresDialect final : public StandardSqlDialect
    {
    public:
        /**
         * @brief 默认构造函数
         */
        PostgresDialect() = default;

        /**
         * @brief 析构函数
         */
        ~PostgresDialect() override = default;

        // 方言是纯翻译规则、不含状态，拷贝一份与共享同一实例等价（registry 用共享指针持有）
        PostgresDialect(const PostgresDialect &)            = default;
        PostgresDialect &operator=(const PostgresDialect &) = default;

        /**
         * @brief 获取本方言对应的数据库类型
         * @details 重写 SqlDialect::type()：恒返回 DatabaseType::PostgreSql，与连接状态无关。
         * @return DatabaseType DatabaseType::PostgreSql
         */
        [[nodiscard]] DatabaseType type() const noexcept override;

        /**
         * @brief 生成第 index 个参数占位符
         * @details 重写 SqlDialect::placeholder()：PostgreSQL 的参数写作 "$1" "$2"（从 1 开始），
         *          这是 PQexecParams / PQprepare 的语法要求，不能沿用 "?"。
         *          基类保证传入的 index 就是参数在 parameters 中的下标，因此这里加一即可。
         * @param index 参数序号，从 0 开始（对应 parameters 的下标）
         * @return std::string 形如 "$2" 的占位符文本
         */
        [[nodiscard]] std::string placeholder(std::size_t index) const override;

        /**
         * @brief 获取 PostgreSQL 的开启事务语句
         * @details 重写 SqlDialect::beginTransactionStatement()：返回 "BEGIN"。
         *          PostgreSQL 的 START TRANSACTION 与 BEGIN 是等价的两种写法，
         *          本实现选 BEGIN：它是官方文档在事务章节的首选写法，也与 psql 的行为一致。
         * @return std::string_view 恒为 "BEGIN"
         */
        [[nodiscard]] std::string_view beginTransactionStatement() const noexcept override;

        /**
         * @brief 获取 PostgreSQL 的提交事务语句
         * @details 重写 SqlDialect::commitStatement()：返回 "COMMIT"（END 是等价别名）。
         * @return std::string_view 恒为 "COMMIT"
         */
        [[nodiscard]] std::string_view commitStatement() const noexcept override;

        /**
         * @brief 获取 PostgreSQL 的回滚事务语句
         * @details 重写 SqlDialect::rollbackStatement()：返回 "ROLLBACK"（ABORT 是等价别名）。
         * @return std::string_view 恒为 "ROLLBACK"
         */
        [[nodiscard]] std::string_view rollbackStatement() const noexcept override;

        /**
         * @brief 把逻辑列类型翻译成 PostgreSQL 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：
         *          - Int64 → "BIGINT"（8 字节有符号，对应 C++ 的 std::int64_t）；
         *          - UInt64 → "NUMERIC(20)"：PostgreSQL **没有无符号整数类型**，
         *            选 NUMERIC(20) 而不是 BIGINT 是因为前者能无损容纳 0 .. 2^64-1
         *            （20 位十进制正好覆盖 18446744073709551615），代价是服务端按精确数值
         *            处理、比整数略慢；选 BIGINT 会让上半个取值域在写入时直接溢出报错；
         *          - Double → "DOUBLE PRECISION"（8 字节 IEEE 754，标准 SQL 写法）；
         *          - Bool → "BOOLEAN"：三个引擎里唯一有真布尔类型的，
         *            服务端会拒绝把 '0'/'1' 以外、也不是 true/false 的文本写进布尔列；
         *          - Text → "TEXT"（变长，上限约 1 GiB，与 varchar 无性能差异）；
         *          - Blob → "BYTEA"（变长二进制，官方推荐的二进制承载类型）。
         * @param type 逻辑列类型
         * @return std::string_view 对应物理类型名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 PostgreSQL 的「表是否存在」查询
         * @details 重写 SqlDialect::tableExistsStatement()：PostgreSQL 的表清单在
         *          information_schema.tables 里，它是当前数据库内所有模式共享的：
         *          只用 table_name 过滤会把其它模式（如其它用户的同名表）也统计进来，
         *          导致「表其实不存在却报告存在」，因此必须用 current_schema() 限定
         *          （对应 MySQL 用 DATABASE()、SQLite 查库内私有的 sqlite_master）。
         *          current_schema() 返回 search_path 里第一个可用模式，正是不带模式名
         *          执行 DML 时表被解析到的位置，因此两边口径一致。
         *          表名以 $1 绑定参数送出，表名里的双引号或分号都不会改变语句结构。
         * @param tableName 待查询的表名
         * @return SqlStatement "SELECT COUNT(*) FROM information_schema.tables WHERE
         *         table_schema = current_schema() AND table_name = $1" 及其唯一绑定参数；
         *         结果为一行一列，0 表示不存在
         */
        [[nodiscard]] SqlStatement tableExistsStatement(std::string_view tableName) const override;

        /**
         * @brief 获取 PostgreSQL 单条语句的参数个数上限
         * @details 重写 SqlDialect::maximumStatementParameters()：PostgreSQL 的扩展查询协议
         *          在 Bind 报文里用 2 字节的有符号整数表达参数个数，因此 65535 是协议层
         *          能表达的上限（与 MySQL 的 COM_STMT_PREPARE 同源，与 SQLite 那种可被
         *          编译期宏调小的语言级限制不是一回事）。
         *          协议之外还有一层实际约束：服务端对单条语句的参数总量、
         *          以及 max_stack_depth 之类的资源限制也会生效，因此调用方在按本上限分块之外，
         *          仍应控制单批的数据量。
         * @return std::size_t 恒为 kMaximumStatementParameters（65535）
         */
        [[nodiscard]] std::size_t maximumStatementParameters() const noexcept override;

        /// PostgreSQL 单条语句的参数个数上限（= Bind 报文中 Int16 的参数个数上界）
        static constexpr std::size_t kMaximumStatementParameters = 65535;

    protected:
        /**
         * @brief 取得 PostgreSQL 的标识符引用字符
         * @details 重写 StandardSqlDialect::identifierQuoteCharacter()：PostgreSQL 与 SQLite
         *          同用双引号（SQL 标准），而不是 MySQL 的反引号。
         * @return char 恒为双引号 '"'
         */
        [[nodiscard]] char identifierQuoteCharacter() const noexcept override;

        /**
         * @brief 取得本方言的显示名
         * @details 重写 StandardSqlDialect::dialectName()：用于拼出「PostgreSQL 方言：…」
         *          这类中文错误文本，便于多方言并存时定位是哪一侧的输入有问题。
         * @return std::string_view 恒为 "PostgreSQL"
         */
        [[nodiscard]] std::string_view dialectName() const noexcept override;
    };

} // namespace AsynGyanis::Database
