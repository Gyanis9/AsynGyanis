/**
 * @file MySqlDialect.h
 * @brief MySQL 方言 —— 把查询树翻译成 MySQL / MariaDB 可执行的参数化 SQL
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details SqlDialect 的 MySQL / MariaDB 实现，与 SqliteDialect 是姊妹实现：
 *          两者共用同一套查询树、同一套「一边拼文本一边压参数」的顺序契约，
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
 * ## 生成的 SQL 规则
 * - 参数：WHERE / HAVING / JOIN...ON 里的比较值、IN 列表、以及分页值按出现顺序收集，
 *   IS NULL / IS NOT NULL 不产生参数，列-列比较不产生参数；
 * - 写语句：INSERT / UPDATE / DELETE / 多行 INSERT 与 SELECT 共用同一套 WHERE 渲染
 *   与参数收集规则（本实现把它们收敛到 appendWhereClause 等私有辅助函数里），
 *   因此条件树的递归展开、IN 展开、参数顺序在四个方向上的行为必然一致；
 * - 多行 VALUES：MySQL 与 MariaDB 都原生支持 "VALUES (…), (…)"，本实现直接使用该语法，
 *   行数由调用方按参数上限自行分块；
 * - 表达式字段（如 COUNT(*)、COALESCE(age, 0)）原样输出，不加引号，
 *   加了引号会被当成列名从而改变语义（双引号还会被当成字符串字面量）。
 *   判定依据是「文本里有没有运算符、括号、逗号这类结构字符」：只含标识符字节与空格的文本
 *   一律按标识符加反引号引用，因此 foo bar 这种含空格的列名能被正确引用而不是当成表达式。
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

#include "Database/Dialect/SqlDialect.h"
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
     * @brief MySQL / MariaDB SQL 方言
     *
     * @details 无状态实现，可被多线程并发调用；实例由 DialectRegistry 以共享指针提供，
     *          调用方一般不需要自己构造。
     */
    class MySqlDialect final : public SqlDialect
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
         * @brief 把查询树翻译成 MySQL 的参数化 SQL
         * @details 重写 SqlDialect::translate()：按 SELECT → FROM → JOIN → WHERE →
         *          GROUP BY → HAVING → ORDER BY → LIMIT/OFFSET 的书写顺序拼接文本，
         *          每写入一个占位符就同步压入一个参数，保证参数顺序与占位符出现顺序严格一致。
         *          与基类契约的差异：分页用 "LIMIT ? OFFSET ?" 并占用两个绑定参数（先 LIMIT 后 OFFSET）；
         *          只给 OFFSET 时补出官方「不限行数」常量 18446744073709551615；
         *          空 IN 集合翻译成恒假常量而不产出非法的 "IN ()"。
         * @param query 待翻译的查询树，本方法不修改它
         * @return SqlStatement SQL 文本与按序排列的绑定参数
         * @note 纯文本变换，不访问数据库，可在任意线程并发调用
         */
        [[nodiscard]] SqlStatement translate(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把单行插入翻译成 MySQL 的 INSERT
         * @details 重写 SqlDialect::translateInsert()：生成
         *          "INSERT INTO 表 (`列`…) VALUES (?, …)"。列名取自 query.selectColumns
         *          并逐个加反引号引用，取值由 values 按同一顺序绑定；本实现不修改入参。
         *          与基类契约一致，个数不符时抛 std::invalid_argument 而不是生成半截语句。
         * @param query 提供表名与待写列的查询树
         * @param values 待绑定的字段值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement INSERT 文本与按列序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsert(const Queryable::QueryNode &query,
                                                   std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件更新翻译成 MySQL 的 UPDATE
         * @details 重写 SqlDialect::translateUpdate()：生成
         *          "UPDATE 表 SET `列` = ?, … [WHERE 条件]"。SET 的列与占位符按 selectColumns
         *          顺序输出，随后整段 WHERE 交给与 SELECT 共用的 appendWhereClause()，
         *          因此条件树的递归、IN 展开、参数收集顺序与本方言的 SELECT 完全一致；
         *          SET 参数在前、条件参数在后，与文本中占位符的先后严格对应。
         * @param query 提供表名、SET 列与 WHERE 条件的查询树
         * @param values 赋给各 SET 列的取值，个数必须等于 query.selectColumns 的列数
         * @return SqlStatement UPDATE 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空，或 values 个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateUpdate(const Queryable::QueryNode &query,
                                                   std::span<const DatabaseValue> values) const override;

        /**
         * @brief 把按条件删除翻译成 MySQL 的 DELETE
         * @details 重写 SqlDialect::translateDelete()：生成 "DELETE FROM 表 [WHERE 条件]"，
         *          条件渲染复用 appendWhereClause()；无条件时整段 WHERE 被省略（整表删除）。
         *          表别名存在时一并写出，因为 WHERE 里以别名限定的列名只有别名在场才能解析。
         *          单表别名形式的 DELETE 是 MySQL 支持的写法（多表删除才需要 "DELETE 别名 FROM …"，
         *          查询树不表达多表删除，因此不做该分支）。
         * @param query 提供表名与 WHERE 条件的查询树
         * @return SqlStatement DELETE 文本与按序排列的绑定参数
         */
        [[nodiscard]] SqlStatement translateDelete(const Queryable::QueryNode &query) const override;

        /**
         * @brief 把多行插入翻译成 MySQL 的多行 VALUES 语句
         * @details 重写 SqlDialect::translateInsertBatch()：生成
         *          "INSERT INTO 表 (`列`…) VALUES (?, …), (?, …), …"，参数按「行优先、行内按列序」
         *          展开。MySQL / MariaDB 原生支持多行 VALUES，本实现直接使用；
         *          参数总数是否超过 maximumStatementParameters() 由调用方负责分块。
         * @param query 提供表名与待写列的查询树
         * @param rows 待插入的行，每行的取值个数必须等于 query.selectColumns 的列数
         * @return SqlStatement 多行 INSERT 文本与按序排列的绑定参数
         * @throws std::invalid_argument 列数为空、rows 为空，或某行取值个数与列数不一致
         */
        [[nodiscard]] SqlStatement translateInsertBatch(const Queryable::QueryNode &query,
                                                        std::span<const std::vector<DatabaseValue>> rows) const override;

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
         * @brief 用反引号引用标识符并翻转义内部反引号
         * @details 重写 SqlDialect::quoteIdentifier()：MySQL 的官方引用符是反引号，
         *          内部反引号按同一规则翻倍表示（`` `weird``name` ``）。
         *          不用双引号：默认 sql_mode 下双引号是字符串字面量而不是标识符引用符，
         *          加了双引号的列名会被当成常量字符串，语句能执行但语义完全变了。
         * @param identifier 待引用的标识符，不含外层反引号
         * @return std::string 形如 `identifier` 的文本，内部反引号已翻倍
         */
        [[nodiscard]] std::string quoteIdentifier(std::string_view identifier) const override;

        /**
         * @brief 生成第 index 个参数占位符
         * @details 重写 SqlDialect::placeholder()：MySQL 的位置参数（文本协议与
         *          mysql_stmt_* 预处理接口）都写作 "?"，序号只用于按顺序收集参数。
         * @param index 参数序号，从 0 开始
         * @return std::string 恒为 "?"
         */
        [[nodiscard]] std::string placeholder(std::size_t index) const override;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         * @details 重写 SqlDialect::supportsLimitOffset()：MySQL 原生支持
         *          "LIMIT n OFFSET m"（OFFSET 不能单独出现，本实现用无符号上界常量补出 LIMIT）。
         * @return true 恒为 true
         */
        [[nodiscard]] bool supportsLimitOffset() const noexcept override;

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

    private:
        /**
         * @brief 渲染字段引用：纯标识符加反引号，表达式原样输出
         * @details 处理三种情形：单个标识符（加反引号）、限定名（users.id → `users`.`id`）、
         *          含运算符/括号/逗号等结构字符的表达式（原样输出，例如 COUNT(*)）。
         *          判定依据是「文本里有没有结构字符」而不是「有没有空格」：含空格的标识符
         *          （如 foo bar 这种列名）仍是标识符，会被引用成 `foo bar`，不被误判为表达式。
         * @param fieldText 字段引用文本（列名或表达式）
         * @return std::string 可直接写入 SQL 的字段片段
         */
        [[nodiscard]] std::string renderFieldReference(std::string_view fieldText) const;

        /**
         * @brief 渲染表引用：加反引号的表名，附带可选的 "AS 别名"
         * @details SELECT / INSERT / UPDATE / DELETE 四个方向共用，保证表名与别名的
         *          引用方式在任何语句里都一致。
         * @param query 提供 tableName 与 tableAlias 的查询树
         * @return std::string `表名` 或 `表名` AS `别名`
         */
        [[nodiscard]] std::string renderTableReference(const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染 " WHERE 条件..." 子句（无条件时什么都不输出）
         * @details 这是本方言唯一的条件渲染入口，translate() / translateUpdate() /
         *          translateDelete() 全部走它：AND/OR/NOT 递归展开 children、IN 展开多个
         *          占位符、IS NULL 与列-列比较不占参数、每写一个占位符就同步压一个参数。
         *          一份实现意味着三个方向的参数顺序与文本顺序不可能出现分歧。
         * @param sqlText 输出缓冲区，" WHERE ..." 追加到末尾
         * @param parameters 输出参数列表，条件产生的取值按占位符出现顺序追加
         * @param query 提供 whereConditions 的查询树
         */
        void appendWhereClause(std::string &sqlText,
                               std::vector<DatabaseValue> &parameters,
                               const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染 INSERT 的列名列表
         * @details 形如 "`id`, `name`"，逐个走 quoteIdentifier()；
         *          四个写方向共用同一份列名引用逻辑。
         * @param sqlText 输出缓冲区，追加到末尾
         * @param query 提供列名列表（selectColumns）的查询树
         */
        void appendColumnList(std::string &sqlText, const Queryable::QueryNode &query) const;

        /**
         * @brief 渲染一行 VALUES 的占位符并收集其参数
         * @param sqlText 输出缓冲区，形如 "(?, ?)" 的片段追加到末尾
         * @param parameters 输出参数列表，本行取值按列序追加
         * @param rowValues 本行各列的取值
         */
        void appendValueRow(std::string &sqlText,
                            std::vector<DatabaseValue> &parameters,
                            std::span<const DatabaseValue> rowValues) const;

        /**
         * @brief 渲染分页子句 " LIMIT … [OFFSET …]" 并收集分页参数
         * @details 分页是 MySQL 与 SQLite 差异最大的子句，单独抽出：
         *          limit 与 offset 都有时产出 "LIMIT ? OFFSET ?"（先 limit 后 offset）；
         *          只有 limit 时产出 "LIMIT ?"；只有 offset 时先补出无符号上界常量再 "OFFSET ?"
         *          （MySQL 不允许 OFFSET 单独出现，且没有 SQLite 可用的 "LIMIT -1"）。
         *          什么都不给时本方法不输出任何内容。
         * @param sqlText 输出缓冲区，分页片段追加到末尾
         * @param parameters 输出参数列表，分页值按占位符出现顺序追加
         * @param query 提供 limit / offset 的查询树
         */
        void appendLimitOffsetClause(std::string &sqlText,
                                     std::vector<DatabaseValue> &parameters,
                                     const Queryable::QueryNode &query) const;

        /**
         * @brief 校验取值个数与待写列数一致
         * @details 个数不符说明调用方把列与值对错了位，生成出来的语句即使能执行也会写错列，
         *          因此在这里直接失败并给出中文原因，绝不生成半截语句。
         * @param query 提供 selectColumns 的查询树
         * @param valueCount 本次提供的取值个数
         * @throws std::invalid_argument 列数为空，或 valueCount 与列数不一致
         */
        static void requireMatchingColumnCount(const Queryable::QueryNode &query, std::size_t valueCount);

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
