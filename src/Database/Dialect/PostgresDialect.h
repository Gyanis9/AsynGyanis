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
 * - 占位符：写作 "$1" "$2" 而不是 "?"。这是 PostgreSQL 自己的语法：
 *   扩展查询协议（PQexecParams / PQprepare）要求语句文本用 $n 标出参数位置，
 *   序号从 1 开始。因此本方言是唯一真正用到 placeholder() 序号参数的实现——
 *   基类保证「先取序号再压参数」，序号与 parameters 的下标严格对应；
 * - 标识符：双引号引用，内部双引号翻倍（"weird""name"），与 SQLite 相同、
 *   与 MySQL 的反引号不同。PostgreSQL 会把未引用的标识符折叠成小写，
 *   因此一律引用也顺带保证了大写列名的可用性；
 * - 分页：直接沿用基类的标准关键字形式，值以占位符送出。PostgreSQL 既允许
 *   "LIMIT $1" 单独出现，也允许 "OFFSET $2" 单独出现（SQLite 需要补 "LIMIT -1"，
 *   MySQL 需要补无符号上界常量），因此本类不需要覆写分页钩子；
 * - 事务：开启用 "BEGIN"（PostgreSQL 的等价写法 START TRANSACTION 同样可用，
 *   BEGIN 更短且是官方文档的首选），提交 COMMIT、回滚 ROLLBACK；
 * - 参数上限：65535，依据是扩展查询协议 Bind 报文里参数个数为 Int16 字段；
 * - 类型名：BIGINT / NUMERIC(20) / DOUBLE PRECISION / BOOLEAN / TEXT / BYTEA / NUMERIC（十进制）。
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
 * PostgreSQL 的文本类型（TEXT / VARCHAR / JSON / 字符类型）无法存储 NUL 字节：
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

#include "AsynGyanisExport.h"

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
    class ASYN_DATABASE_API PostgresDialect final : public StandardSqlDialect
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
         * @brief 把一个参数占位符追加到 SQL 文本末尾
         * @details 重写 SqlDialect::appendPlaceholder()：PostgreSQL 的参数写作 "$1" "$2"（从 1 开始），
         *          这是扩展查询协议（PQexecParams / PQprepare）的语法要求，不能沿用 "?"。
         *          基类保证传入的 index 就是参数在 parameters 中的下标，因此这里加一。
         *          这是三种引擎里唯一真正使用序号参数的实现，也是接口保留 index 形参的原因。
         * @param sqlText 目标 SQL 文本，占位符追加在其末尾
         * @param index 参数序号，从 0 开始（对应 parameters 的下标）
         */
        void appendPlaceholder(std::string &sqlText, std::size_t index) const override;

        /**
         * @brief 给定冲突处置时，INSERT 语句开头的关键词短语
         * @details 重写 SqlDialect::insertKeywordPhrase()：PostgreSQL 没有 INSERT IGNORE /
         *          REPLACE INTO 这类开头形式，四档的前缀都是普通的 "INSERT INTO "，
         *          处置本身由尾部子句表达（见 appendInsertConflictTail()）。
         * @param conflict 唯一键冲突的处置意图
         * @return std::string_view 恒为 "INSERT INTO "
         */
        [[nodiscard]] std::string_view insertKeywordPhrase(Queryable::InsertConflict conflict) const override;

        /**
         * @brief 追加 PostgreSQL 的冲突处置尾部子句（ON CONFLICT …）
         * @details 重写 SqlDialect::appendInsertConflictTail()：
         *          - Fail → 不追加（普通 INSERT，冲突就报错）；
         *          - Ignore → " ON CONFLICT DO NOTHING"（不需要冲突目标）；
         *          - Replace → `ON CONFLICT (目标列) DO UPDATE SET 列 = EXCLUDED.列, …`：
         *            目标列取自 query.conflictTargetColumns（ORM 从 TableSchema 的主键自动带上），
         *            缺失时当场拒绝——不带目标的 DO UPDATE 会被服务端以 42601 拒绝（真机实测，
         *            只有 DO NOTHING 能省目标），生成它是把必拒的语句送到服务端才失败。SET 列出
         *            本次插入的全部列，未列出的列保持旧值——与 MySQL REPLACE 的整行替换在未列出的
         *            列上语义不同，这正是「按业务键覆盖」的常规写法（覆盖一行里被显式给出的部分）。
         *          - Upsert → 与 Replace 同一份 DO UPDATE（PostgreSQL 的 Replace 本来就是真 upsert）
         * @param sqlText 已渲染完 VALUES 行的 INSERT 文本，子句追加在其末尾
         * @param conflict 唯一键冲突的处置意图
         * @param query 提供待写列与冲突目标列的查询树（Replace/Upsert 档两者都要）
         * @throws Base::InvalidArgumentException Replace/Upsert 档缺 conflictTargetColumns 时
         */
        void appendInsertConflictTail(std::string &sqlText, Queryable::InsertConflict conflict, const Queryable::QueryNode &query) const override;

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
         * @brief 取当前库名：PostgreSQL 的咨询锁是服务器级的，锁键要按库分开
         * @return std::optional<std::string> 恒为 `SELECT current_database()`
         */
        [[nodiscard]] std::optional<std::string> currentDatabaseNameStatement() const override;

        /**
         * @brief 非阻塞取 PostgreSQL 会话级咨询锁
         * @details 语句是 `SELECT pg_try_advisory_lock(key)`，按单键形式；
         *          键已由调用方压进 63 位，不会越过 bigint 的有符号上界
         * @param key 锁键
         * @return std::optional<std::string> 语句文本
         */
        [[nodiscard]] std::optional<std::string> tryAcquireMigrationLockStatement(std::uint64_t key) const override;

        /**
         * @brief 释放 PostgreSQL 会话级咨询锁
         * @param key 与取得锁时同一个键
         * @return std::optional<std::string> 语句文本
         */
        [[nodiscard]] std::optional<std::string> releaseMigrationLockStatement(std::uint64_t key) const override;

        /**
         * @brief 获取 PostgreSQL 的隔离级别设置语句
         * @details 重写 SqlDialect::isolationLevelStatement()：文本是 `SET TRANSACTION ISOLATION
         *          LEVEL …`。它在 PostgreSQL 里必须在 BEGIN 之后、本事务第一条语句之前执行，
         *          因此本类同时覆写 isolationLevelStatementFollowsBegin() 返回 true。
         *          ReadUncommitted 档照发不拒：PostgreSQL 文档明说四档语法都接受，
         *          只是内部把 Read Uncommitted 按 Read Committed 处理（标准允许实现更严格），
         *          照发得到的是比请求更严格的级别，不会出现用户担心的脏读。
         * @param level 请求的隔离级别；Default 返回空串（无需设置语句）
         * @return std::string_view 设置语句
         */
        [[nodiscard]] std::string_view isolationLevelStatement(TransactionIsolationLevel level) const noexcept override;

        /**
         * @brief 行锁子句返回 "FOR UPDATE"
         * @details 重写 StandardSqlDialect::rowLockClause()：该子句锁住本次结果集的行直到事务结束。
         *          PostgreSQL 另有 `FOR UPDATE OF 表名`、`NOWAIT`、`SKIP LOCKED` 等修饰，本映射不暴露——
         *          多出来的每一档都得三家各自有对应写法才有意义，SQLite 那一格是空的。
         * @return std::optional<std::string_view> 恒有值 "FOR UPDATE"
         */
        [[nodiscard]] std::optional<std::string_view> rowLockClause() const noexcept override;

        /**
         * @brief 隔离级别设置语句在 BEGIN 之后执行（PostgreSQL 的硬性要求）
         * @details 重写 SqlDialect::isolationLevelStatementFollowsBegin()：PostgreSQL 的
         *          `SET TRANSACTION` 只在事务块内有效，事务块外发出会得到
         *          「can only be used in transaction blocks」警告且不生效——若按默认在 BEGIN 前发，
         *          就是一次静默降级（请求的级别根本没生效，事务按会话默认级别跑）。
         * @return true 恒为 true
         */
        [[nodiscard]] bool isolationLevelStatementFollowsBegin() const noexcept override;

        /**
         * @brief 把逻辑列类型翻译成 PostgreSQL 的物理类型名
         * @details 重写 SqlDialect::columnTypeName()：
         *          - Int64 → "BIGINT"（8 字节有符号，对应 C++ 的 std::int64_t）；
         *          - UInt64 → "NUMERIC(20)"：PostgreSQL 没有无符号整数类型，
         *            选 NUMERIC(20) 而不是 BIGINT 是因为前者能无损容纳 0 .. 2^64-1
         *            （20 位十进制正好覆盖 18446744073709551615），代价是服务端按精确数值
         *            处理、比整数略慢；选 BIGINT 会让上半个取值域在写入时直接溢出报错；
         *          - Double → "DOUBLE PRECISION"（8 字节 IEEE 754，标准 SQL 写法）；
         *          - Bool → "BOOLEAN"：三个引擎里唯一有真布尔类型的，
         *            服务端会拒绝把 '0'/'1' 以外、也不是 true/false 的文本写进布尔列；
         *          - Text → "TEXT"（变长，上限约 1 GiB，与 varchar 无性能差异）；
         *          - Blob → "BYTEA"（变长二进制，官方推荐的二进制承载类型）；
         *          - Decimal → "NUMERIC"：不带精度限定就是「任意精度、按写入的标度存」，
         *            写几位小数读回几位，服务端不做四舍五入 —— 与 Database::Decimal 的文本承载同构。
         *            要固定标度（金额常见的 NUMERIC(19,4)）由调用方自己写 DDL，本映射不替业务定标度。
         * @param type 逻辑列类型
         * @return std::string_view 对应物理类型名；未知取值回落到 "TEXT"（见基类约定）
         */
        [[nodiscard]] std::string_view columnTypeName(ColumnType type) const noexcept override;

        /**
         * @brief 生成 PostgreSQL 的自增主键列定义（GENERATED … AS IDENTITY）
         * @details 重写 SqlDialect::autoIncrementPrimaryKeyDefinition()：PostgreSQL 10 起用
         *          SQL 标准的标识列（IDENTITY）而不是旧的 SERIAL 伪类型。
         *          `GENERATED BY DEFAULT AS IDENTITY` 允许显式插入指定值（数据迁移与修复常用），
         *          因此选它而不是 GENERATED ALWAYS；类型取 columnTypeName() 对 Int64 的映射。
         * @param quotedColumnName 已按本方言引用好的列名
         * @param type 主键成员的逻辑列类型；标识列只接受整数类型，其余取值给空串
         * @return std::string 形如 `"id" BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY`；
         *         type 不是 Int64 时为空串（调用方据此在建表前拒绝）
         */
        [[nodiscard]] std::string autoIncrementPrimaryKeyDefinition(std::string_view quotedColumnName, ColumnType type) const override;

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
         * @brief 生成 PostgreSQL 的「列出已有列」查询
         * @details 重写 SqlDialect::columnListingStatement()：与 tableExistsStatement() 同一套
         *          限定（information_schema + current_schema()），列序按 ordinal_position，
         *          与建表时的定义顺序一致；表不存在时结果集为空（不报错）。
         *          表名以 $1 绑定参数送出，表名里的双引号或分号都不会改变语句结构。
         * @param tableName 待查询的表名
         * @return SqlStatement "SELECT column_name FROM information_schema.columns WHERE
         *         table_schema = current_schema() AND table_name = $1 ORDER BY ordinal_position"
         *         及其唯一绑定参数；结果为一行一列，每行是列名文本
         */
        [[nodiscard]] SqlStatement columnListingStatement(std::string_view tableName) const override;

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
