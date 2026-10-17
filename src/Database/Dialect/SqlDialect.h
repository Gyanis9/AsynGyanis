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

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Queryable/QueryNode.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 事务隔离级别（SQL:1992 的四档 + 「随引擎默认」）
     * @details Default 表示引擎自己的默认（MySQL 是 REPEATABLE READ、SQLite 是 SERIALIZABLE），
     *          不额外发任何语句；其余四档由各方言翻译成「BEGIN 之前的设置语句」。
     *          引擎不支持按事务请求时拒绝并点名（见 `Transaction` 的构造说明），不静默降级
     */
    enum class TransactionIsolationLevel
    {
        Default,         ///< 引擎默认（不额外发语句）
        ReadUncommitted, ///< 读未提交（脏读可见）
        ReadCommitted,   ///< 读已提交
        RepeatableRead,  ///< 可重复读
        Serializable,    ///< 串行化
    };

    /**
     * @brief 隔离级别的人类可读名字（错误文案与日志用；不是 SQL 关键字文本）
     * @param level 隔离级别
     * @return 名字；未知枚举值给「未知」
     */
    [[nodiscard]] constexpr std::string_view isolationLevelName(const TransactionIsolationLevel level) noexcept
    {
        switch (level)
        {
            case TransactionIsolationLevel::Default:
                return "引擎默认";
            case TransactionIsolationLevel::ReadUncommitted:
                return "读未提交（READ UNCOMMITTED）";
            case TransactionIsolationLevel::ReadCommitted:
                return "读已提交（READ COMMITTED）";
            case TransactionIsolationLevel::RepeatableRead:
                return "可重复读（REPEATABLE READ）";
            case TransactionIsolationLevel::Serializable:
                return "串行化（SERIALIZABLE）";
        }
        return "未知";
    }

    /**
     * @brief SQL 方言抽象基类
     *
     * @details 每个受支持的数据库对应一个实现（当前只有 SqliteDialect）。
     *          实例由 DialectRegistry 按 DatabaseType 提供，调用方通过共享指针持有。
     *
     * @note 本类刻意不含任何数据成员：方言是"翻译规则"而不是"连接状态"，
     *       无状态才能被安全共享与并发调用。
     */
    class ASYN_DATABASE_API SqlDialect
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
         * @details 生成 "UPDATE 表 SET 列 = ?, … WHERE 条件"：WHERE 由 query.whereConditions
         *          渲染，复用与 SELECT 完全相同的条件逻辑（递归 children、IN 展开、
         *          IS NULL 不占参数）；参数顺序为「先赋值参数、后条件参数」，与文本一致。
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
         * @details 生成 "INSERT INTO 表 (列…) VALUES (?, …), (?, …), …"：参数按「行优先、
         *          行内按列序」展开；不支持多行 VALUES 的方言可改为生成多条语句，
         *          但绝不能静默丢掉任何一行。调用方需自行控制行数使参数总数不超过引擎上限。
         *
         * @param query 提供表名与待写列的查询树，本方法不修改它
         * @param rows 待插入的行，每行是该行各列的取值
         * @return SqlStatement 多行 INSERT 文本与按序排列的绑定参数
         * @throws Base::InvalidArgumentException 列数为空、rows 为空，或某行的取值个数与列数不一致
         */
        [[nodiscard]] virtual SqlStatement translateInsertBatch(const Queryable::QueryNode &query, std::span<const std::vector<DatabaseValue>> rows) const = 0;

        /**
         * @brief 给定冲突处置时，INSERT 语句开头的关键词短语（含结尾的 `INTO`，不含表名）
         *
         * @details 这是「引擎知识」而不是标准 SQL 的一部分：标准里没有冲突处置这一档，
         *          所以本方法由具体方言实现，共用渲染层只负责把返回的短语拼在最前面。
         *          `Queryable::InsertConflict::Fail` 必须映射到普通的 `INSERT INTO`——它是所有既有
         *          调用的默认，线上字节因此一分不变。
         * @param conflict 唯一键冲突的处置意图
         * @return std::string_view 短语原文，指向方言自己的字面量
         */
        [[nodiscard]] virtual std::string_view insertKeywordPhrase(Queryable::InsertConflict conflict) const = 0;

        /**
         * @brief 把冲突处置的尾部子句追加到 INSERT 文本末尾（默认不给子句）
         *
         * @details 与 insertKeywordPhrase() 相对：那一份表达写在最前面的处置
         *          （如 MySQL 的 `REPLACE INTO`），本方法表达只能写在 VALUES 之后的处置——
         *          PostgreSQL 的 `ON CONFLICT …` 就是后者，它没有可用的开头形式。
         *          默认实现什么也不追加；需要尾部处置的方言（PostgreSQL 的全部、
         *          MySQL/SQLite 的 Upsert 档）各自覆写。`Fail` 档在两种位置都必须是空档，
         *          否则既有调用的线上字节会变。
         *
         * @param sqlText 已渲染完 VALUES 行的 INSERT 文本，子句追加在其末尾
         * @param conflict 唯一键冲突的处置意图
         * @param query 提供表名与待写列的查询树（`ON CONFLICT DO UPDATE` 的 SET 子句需要列清单）
         */
        virtual void appendInsertConflictTail(std::string &sqlText, Queryable::InsertConflict conflict, const Queryable::QueryNode &query) const
        {
            // 默认空实现：本库覆盖的引擎里只有 PostgreSQL 的处置以尾部子句表达
            static_cast<void>(sqlText);
            static_cast<void>(conflict);
            static_cast<void>(query);
        }

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
         * @brief 开启事务之前要发的隔离级别设置语句
         *
         * @details 事务对象在 BEGIN 之前执行它，语句本身只影响紧随其后的那一笔事务
         *          （标准语法 `SET TRANSACTION ISOLATION LEVEL …` 即此语义，MySQL 同名语句同解）。
         *          引擎不提供「按事务请求隔离级别」这条路时给出空串——这是「拒绝」而不是
         *          「不支持就当默认」：调用方（Transaction 构造）见到空串就地抛出并点名，
         *          不静默降级成引擎默认。
         *
         * @param level 请求的隔离级别；传 Default 时一律给空串（无需设置语句）
         * @return std::string_view 设置语句；该引擎不支持该级别时为空
         */
        [[nodiscard]] virtual std::string_view isolationLevelStatement(TransactionIsolationLevel level) const noexcept = 0;

        /**
         * @brief 隔离级别设置语句该在 BEGIN 之后（而不是之前）执行吗
         *
         * @details 「设置语句相对 BEGIN 的位置」是引擎知识而不是调用方偏好：MySQL / SQLite 的
         *          `SET TRANSACTION …` 必须在事务开始前发出才对下一笔事务生效；PostgreSQL 恰好相反——
         *          事务块外发 `SET TRANSACTION` 只会得到「can only be used in transaction blocks」
         *          警告且不生效，它要求在 BEGIN 之后、本事务的第一条语句之前发出。
         *          默认 false（BEGIN 之前），PostgreSQL 覆写为 true；事务对象按此决定补发时机，
         *          失败路径与前置路径一致（BEGIN 后置时事务已开启，构造抛出前先标记活动，析构负责回滚）。
         *
         * @return true 表示事务对象应在 BEGIN 成功之后立刻执行 isolationLevelStatement()
         */
        [[nodiscard]] virtual bool isolationLevelStatementFollowsBegin() const noexcept
        {
            return false;
        }

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
         * @brief 获取在事务里立一个保存点的语句文本
         *
         * @details 保存点是「事务内部的可回退点」：`ROLLBACK TO SAVEPOINT` 只撤到那一点而不结束事务，
         *          所以嵌套回滚不必再开第二个事务——两个引擎都不吃嵌套 BEGIN（MySQL 的 START TRANSACTION
         *          会隐式提交上一笔，SQLite 直接报错），而再借一条连接开的更是另一笔事务。
         *
         * @note 默认实现放在接口上而不是某个方言里：这三条语法 SQLite、MySQL 与 PostgreSQL 通用，
         *       收在这里就只有一份文本；名字一律经本引擎的 `quoteIdentifier()` 包起来（内部的同字符翻倍），
         *       所以调用方给的串不会变成语句的一部分。哪天真出现不同写法的引擎，在它自己的方言里覆写即可。
         *
         * @param name 保存点名，非空且不含 NUL（那两道检查在事务对象那一层，见 `Transaction::savepoint`）
         * @return std::string 语句文本，保证不含分号
         */
        [[nodiscard]] virtual std::string savepointStatement(const std::string_view name) const
        {
            return "SAVEPOINT " + quoteIdentifier(name);
        }

        /**
         * @brief 获取「回退到某个保存点而保留事务」的语句文本
         * @details 写全 `TO SAVEPOINT` 而不是 SQLite 也接受的裸 `TO`：MySQL 只认带关键字的那条写法，
         *          一份文本两边都能跑。回退之后事务仍然开着——这是它与 `rollbackStatement()` 唯一而关键
         *          的区别。
         * @param name 保存点名
         * @return std::string 语句文本，保证不含分号
         */
        [[nodiscard]] virtual std::string rollbackToSavepointStatement(const std::string_view name) const
        {
            return "ROLLBACK TO SAVEPOINT " + quoteIdentifier(name);
        }

        /**
         * @brief 获取丢弃某个保存点的语句文本
         * @details 丢弃只是让这个名字之后不能被回退，不提交也不回滚它之后的工作；
         *          不调用也可以，事务结束时保存点随之消失，所以它只在需要早回收命名空间时有用。
         * @param name 保存点名
         * @return std::string 语句文本，保证不含分号
         */
        [[nodiscard]] virtual std::string releaseSavepointStatement(const std::string_view name) const
        {
            return "RELEASE SAVEPOINT " + quoteIdentifier(name);
        }

        /**
         * @brief 取「当前数据库名」的查询语句，供迁移锁把锁键限定到本库
         *
         * @details PostgreSQL 的咨询锁与 MySQL 的 GET_LOCK 都是服务器级的：同一台服务器上
         *          两个同名库若共用一个锁键，会互相挡住对方的迁移。读取当前库名后把它揉进锁键，
         *          不同库就各自有一把锁。返回空表示本引擎不需要这一层限定（SQLite 的锁随文件）。
         *
         * @return std::optional<std::string> 查询语句；本引擎不提供时为空
         */
        [[nodiscard]] virtual std::optional<std::string> currentDatabaseNameStatement() const
        {
            return std::nullopt;
        }

        /**
         * @brief 取「非阻塞尝试迁移会话锁」的语句
         *
         * @details 迁移开始前由 SchemaMigrator 在同一连接上反复执行本语句，直到拿到锁或超时。
         *          必须是会话级锁：迁移期间那条连接一直由迁移持有，结束（含异常路径）才释放。
         *          默认返回空表示本引擎没有会话级咨询锁——调用方退回「整段一笔写事务」的等价物。
         *
         * @param key 锁键（已由调用方混入库名并压成 63 位整数）
         * @return std::optional<std::string> 语句文本；本引擎不支持时为空
         */
        [[nodiscard]] virtual std::optional<std::string> tryAcquireMigrationLockStatement(const std::uint64_t key) const
        {
            static_cast<void>(key);
            return std::nullopt;
        }

        /**
         * @brief 取「释放迁移会话锁」的语句
         *
         * @details 与 tryAcquireMigrationLockStatement() 成对；执行失败时调用方会丢弃那条连接
         *          （MySQL 的会话复位不收 GET_LOCK），让服务端随会话结束释放。
         *
         * @param key 与取得锁时同一个键
         * @return std::optional<std::string> 语句文本；本引擎不支持时为空
         */
        [[nodiscard]] virtual std::optional<std::string> releaseMigrationLockStatement(const std::uint64_t key) const
        {
            static_cast<void>(key);
            return std::nullopt;
        }

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
         * @brief 把一个逻辑列类型翻译成可作主键的物理类型名
         *
         * @details 主键（以及任何索引列）对物理类型有额外要求：MySQL 的 TEXT/LONGBLOB
         *          不能直接进索引，建表语句会以 1170 号错误被拒，必须换成带长度的同族类型
         *          （VARCHAR/VARBINARY）。默认实现直接沿用 columnTypeName()——SQLite 的
         *          TEXT 主键本就合法，无需区分；只有引擎存在这种限制时才重写。
         *
         * @param type 逻辑列类型
         * @return std::string_view 该引擎可直接写进主键列定义的物理类型名
         * @note 需要为键列换类型时，换出的类型可能比原类型短（例如 TEXT → VARCHAR(255)），
         *       超长取值会被引擎按自己的规则拒绝——这是引擎侧的硬限制，不是本映射的取舍
         */
        [[nodiscard]] virtual std::string_view keyColumnTypeName(const ColumnType type) const noexcept
        {
            return columnTypeName(type);
        }

        /**
         * @brief 生成「自增主键」那一列的完整定义文本
         *
         * @details 各引擎的写法连位置都不一样：MySQL 是 `col` BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY，
         *          SQLite 是 "col" INTEGER PRIMARY KEY AUTOINCREMENT 且类型必须正好是 INTEGER，
         *          因此整串由方言给出而不是由迁移工具拼。返回空串表示本方言没有这种写法，
         *          调用方（SchemaMigrator）据此在建表前就拒绝——悄悄建出一张主键不会自增的表，
         *          省略主键的 INSERT 之后会以引擎错误失败，排查成本比这里报明「不支持」高得多。
         *
         * @param quotedColumnName 已按本方言引用好的列名
         * @param type 主键成员的逻辑列类型；只有整数类型能当自增列，其余取值本钩子给空串
         * @return std::string 该列的完整定义（含约束），给不出这种写法时为空串
         */
        [[nodiscard]] virtual std::string autoIncrementPrimaryKeyDefinition(const std::string_view quotedColumnName, const ColumnType type) const
        {
            // 默认实现刻意不使用两个参数：标准 SQL 没有自增列关键字，这里没有可给的文本
            static_cast<void>(quotedColumnName);
            static_cast<void>(type);
            return {};
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
         * @brief 列出一张表当前已有的列名（给「按结构体补列」用）
         *
         * @details 与 tableExistsStatement() 同属「引擎知识」：两家的元数据来源完全不同，
         *          调用方只按「一行一个列名」解读结果。迁移工具靠它判断哪些列还没补上，
         *          因此不需要调用方自己维护一份「上次建了什么」的账。
         *
         * @param tableName 目标表名（未加引用字符的原始名字）
         * @return SqlStatement 「一行一列」的列名清单语句，第一列是列名文本；表名以占位符 +
         *         绑定参数送出。表不存在时结果集为空（不报错），列序按引擎里的定义顺序
         */
        [[nodiscard]] virtual SqlStatement columnListingStatement(std::string_view tableName) const = 0;

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
         * @brief 把一个参数占位符追加到 SQL 文本末尾
         *
         * @details 两种风格：SQLite 与 MySQL 的位置参数写作 "?"（不读 index），PostgreSQL 的
         *          扩展查询协议要求 "$1" "$2" 这类显式带序号的写法并因此真正使用 index。
         *          写成「追加到缓冲」而不是「返回文本」：批量写入时每个占位符都要写一次，
         *          一次一条 150 行 × 6 列的语句就是 900 次；返回文本要么让各实现构造中间对象，
         *          要么让 $n 方言去持有一块共享缓冲——方言被多线程共享，实例成员缓冲即数据竞争。
         *          调用方必须先调用本方法、再压入对应的绑定参数，两处顺序一致（见本文件的参数顺序契约）。
         *
         * @param sqlText 目标 SQL 文本，占位符追加在其末尾
         * @param index 参数序号，从 0 开始，等于该占位符对应参数在 parameters 中的下标
         */
        virtual void appendPlaceholder(std::string &sqlText, std::size_t index) const = 0;

        /**
         * @brief 查询本方言是否支持 LIMIT / OFFSET 分页语法
         * @return true 支持（SQLite / MySQL / PostgreSQL）；false 需要各实现自行改写分页
         */
        [[nodiscard]] virtual bool supportsLimitOffset() const noexcept = 0;

        /**
         * @brief 获取单条语句允许的最大绑定参数个数
         *
         * @details 各引擎的上限不同（SQLite 的 SQLITE_MAX_VARIABLE_NUMBER 默认 999，
         *          MySQL 的占位符上限是 65535，PostgreSQL 同为 65535——后两者依据的都是
         *          各自协议里参数个数为 Int16 的报文字段），属于典型的引擎知识，因此由方言回答。
         *          批量写入的调用方据此把行数与列数换算成每批行数，避免一次绑定过多参数而被引擎拒绝。
         *
         * @return std::size_t 单条语句的参数个数上限，恒大于 0
         */
        [[nodiscard]] virtual std::size_t maximumStatementParameters() const noexcept = 0;
    };

} // namespace AsynGyanis::Database
