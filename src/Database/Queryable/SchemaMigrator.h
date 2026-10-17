/**
 * @file SchemaMigrator.h
 * @brief 建表迁移工具 —— 从 TableSchema<T> 生成并执行 DDL
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 把 TableSchema<T> 同时用于 DDL 与读写映射，两个方向不会脱节。列定义规则与 RowMapper
 *          严格对称：std::optional<X> 允许 NULL，X 一律 NOT NULL（NULL 只能落进 optional 成员，
 *          否则数据库侧就是一颗定时炸弹）；kPrimaryKey 在 kColumns 中找不到同名列时抛
 *          Base::LogicException；在线方法失败返回 false，原因写进 errorText。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/LogicException.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/RowMapper.h"
#include "Database/Queryable/TableSchema.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Database::Queryable
{
    namespace Detail
    {
        /**
         * @brief 取出列的实际存储类型：std::optional<X> 取 X，其余取自身
         *
         * @details 用偏特化而不是 std::conditional_t：后者要求两个候选类型都成立，
         *          遇到非 optional 成员去取 ::%value_type 会直接编译失败。
         * @tparam MemberType 结构体成员类型
         * @tparam IsOptionalWrapper MemberType 是否为 std::optional 包装
         */
        template<typename MemberType, bool IsOptionalWrapper>
        struct ColumnStorageType
        {
            using Type = MemberType; ///< 非 optional：成员类型本身就是列类型
        };

        template<typename MemberType>
        struct ColumnStorageType<MemberType, true>
        {
            using Type = typename MemberType::value_type; ///< optional：去掉可空包装才是列类型
        };

        /**
         * @brief ColumnStorageType 的便捷别名
         * @tparam MemberType 结构体成员类型（可为 std::optional 包装）
         */
        template<typename MemberType>
        using ColumnStorageTypeOf = typename ColumnStorageType<MemberType, IsOptional<MemberType>::value>::Type;

    } // namespace Detail

} // namespace AsynGyanis::Database::Queryable

namespace AsynGyanis::Database::Queryable
{
    /**
     * @brief 迁移记录表的形状：表名、列名与那一行
     *
     * @details 这三样必须排在 SchemaMigrator **之前**：类内的模板方法会实例化这张表的登记
     *          （`createTable<MigrationLedgerRow>()`），而显式特化必须早于任何隐式实例化——
     *          排在类后面会直接撞 C2908「显式特化在实例化之后」。因此它单独成一个命名空间，
     *          表名与列名也只有这一处真源（建表、查询、写入三处都取它）。
     */
    namespace MigrationLedger
    {
        inline constexpr std::string_view kTableName  = "asyn_schema_migrations"; ///< 迁移记录表名
        inline constexpr std::string_view kNameColumn = "migration_name";         ///< 记录列：一条迁移一行，值就是 MigrationStep::name

        /// 迁移记录表的一行（迁移名即主键）
        struct Row
        {
            std::string migrationName; ///< 迁移名
        };
    } // namespace MigrationLedger
} // namespace AsynGyanis::Database::Queryable

/**
 * @brief 迁移记录表的结构登记
 * @details 迁移表由 SchemaMigrator 自己建，但它与其他表走**同一份登记**：
 *          `createTableStatement<MigrationLedgerRow>()` 生成它的 DDL，
 *          `addMissingColumns<MigrationLedgerRow>()` 也认得它（干净时增补 0 列）。
 *          手写一份 DDL 就做不到这一点——那份 DDL 与登记的形状会各走各的。
 * @note 它排在这里而不是文件末尾：类内的 `createTable<MigrationLedgerRow>()` 会隐式实例化
 *       这张登记，显式特化必须早于那次实例化
 */
template<>
struct AsynGyanis::Database::Queryable::TableSchema<AsynGyanis::Database::Queryable::MigrationLedger::Row>
{
    static constexpr std::string_view kTableName = AsynGyanis::Database::Queryable::MigrationLedger::kTableName;

    static constexpr auto kColumns = std::tuple{
            AsynGyanis::Database::Queryable::Column(&AsynGyanis::Database::Queryable::MigrationLedger::Row::migrationName,
                                                    AsynGyanis::Database::Queryable::MigrationLedger::kNameColumn),
    };

    static constexpr std::string_view kPrimaryKey = AsynGyanis::Database::Queryable::MigrationLedger::kNameColumn;
};

namespace AsynGyanis::Database::Queryable
{
    /**
     * @brief 建表迁移工具
     *
     * @details 纯静态类，不允许实例化。全部方法都是模板方法，按 TableSchema<T> 生成 DDL。
     *          本类不持有任何状态，可被多线程并发调用（每个调用自带连接）。
     */
    class SchemaMigrator
    {
    public:
        SchemaMigrator() = delete;

        /**
         * @brief 生成建表语句（纯文本，不接触数据库）
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param dialect 目标引擎的方言，提供类型名映射与标识符引用
         * @param ifNotExists true 生成 "CREATE TABLE IF NOT EXISTS"（默认，便于重复执行）
         * @return SqlStatement 完整建表语句；DDL 不含值，因此 parameters 恒为空
         * @throws Base::LogicException TableSchema<T>::kTableName 为空或含点号，或 kPrimaryKey 非空
         *         但在 kColumns 中找不到同名列（列名拼写不一致）
         */
        template<RowMappable T>
        [[nodiscard]] static SqlStatement createTableStatement(const SqlDialect &dialect, const bool ifNotExists = true)
        {
            // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯；
            // 判定与 RowMapper 共用同一份 trait，因此「能建表」与「能读写」永远等价
            static_assert(Detail::allColumnTypesSupported<T>(), "SchemaMigrator：TableSchema<T>::kColumns 中存在不支持的列类型。"
                                                                "仅支持整型、bool、浮点、std::string、二进制载荷"
                                                                "（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                                                                "以及它们的 std::optional 包装");

            const std::string_view tableName = TableSchema<T>::kTableName;
            requireMigratableTableName(tableName);

            bool        primaryKeyDeclared = false;
            std::string columnDefinitions;

            std::apply(
                    [&](const auto &...columnDescriptors)
                    {
                        // 折叠表达式从左到右执行（逗号运算符），列序与 kColumns 声明顺序严格一致
                        (appendColumnDefinition(columnDefinitions, primaryKeyDeclared, dialect, columnDescriptors, TableSchema<T>::kPrimaryKey,
                                                Detail::declaresAutoIncrementPrimaryKey<T>(), tableName),
                         ...);
                    },
                    TableSchema<T>::kColumns);

            if (!TableSchema<T>::kPrimaryKey.empty() && !primaryKeyDeclared)
            {
                // 主键列名与任何列名都不同：多半是 kPrimaryKey 与 kColumns 里的列名拼写不一致。
                // 静默建出无主键表会让「按主键更新/删除」这类操作在运行期才暴露问题，因此当场失败
                throw Base::LogicException("SchemaMigrator: 表 " + std::string(tableName) + " 的主键列 \"" + std::string(TableSchema<T>::kPrimaryKey) +
                                           "\" 未在 kColumns 中声明，无法生成建表语句");
            }

            SqlStatement statement;
            statement.sql = "CREATE TABLE ";
            if (ifNotExists)
            {
                // IF NOT EXISTS 让建表可以重复执行：迁移工具最常见的用法是每次启动都跑一遍
                statement.sql += "IF NOT EXISTS ";
            }
            statement.sql += dialect.quoteIdentifier(tableName);
            statement.sql += " (";
            statement.sql += columnDefinitions;
            statement.sql += ')';
            // DDL 不含字段值，参数列表恒为空（列定义全是标识符与类型名，没有任何外部数据）
            return statement;
        }

        /**
         * @brief 生成删表语句（纯文本，不接触数据库）
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param dialect 目标引擎的方言，提供标识符引用
         * @param ifExists true 生成 "DROP TABLE IF EXISTS"（默认，便于收尾清理）
         * @return SqlStatement 完整删表语句，parameters 恒为空
         * @throws Base::LogicException TableSchema<T>::kTableName 为空或含点号
         */
        template<RowMappable T>
        [[nodiscard]] static SqlStatement dropTableStatement(const SqlDialect &dialect, const bool ifExists = true)
        {
            const std::string_view tableName = TableSchema<T>::kTableName;
            requireMigratableTableName(tableName);

            SqlStatement statement;
            statement.sql = "DROP TABLE ";
            if (ifExists)
            {
                statement.sql += "IF EXISTS ";
            }
            statement.sql += dialect.quoteIdentifier(tableName);
            return statement;
        }

        /**
         * @brief 建表
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param pool 提供连接的连接池，本方法只在执行期间占用其中一条连接
         * @param ifNotExists true 生成 "IF NOT EXISTS"（默认）：
         *        表已存在时不报错，也没有任何副作用，适合「每次启动都跑一遍」的迁移
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 语句已被引擎接受（表已存在时同样返回 true，因为目标状态已达成）
         * @return false 取连接失败、方言不支持或 DDL 被引擎拒绝，原因见 errorText
         * @throws Base::LogicException 表结构本身不合法（表名为空或含点号、列名含点号、主键列不存在），
         *         见 createTableStatement()
         * @warning 不做结构漂移检测：ifNotExists 为 true 时，表已存在就直接跳过整条语句，
         *          即使现有表的列与 TableSchema<T> 已经不一致（缺列、类型变了）也不会被发现，
         *          本方法不会生成 ALTER。改过 TableSchema 后需要调用方自行迁移或重建表。
         */
        template<RowMappable T>
        [[nodiscard]] static bool createTable(ConnectionPool &pool, const bool ifNotExists = true, std::string *errorText = nullptr)
        {
            // 成功返回前先清空出参，避免调用方读到上一次调用的残留失败原因
            clearError(errorText);

            // 先要方言：DDL 文本本身依赖类型名与引用规则，没有方言就生成不出语句
            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            // 语句生成可能因表结构不合法抛 Base::LogicException，那是编程错误，不在这里转成 false
            return executeStatement(pool, createTableStatement<T>(*dialect, ifNotExists), errorText);
        }

        /**
         * @brief 删表
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param pool 提供连接的连接池
         * @param ifExists true 生成 "IF EXISTS"（默认）：表本就不存在也算成功
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 语句已被引擎接受（表本就不存在时同样返回 true）
         * @return false 取连接失败、方言不支持或 DDL 被引擎拒绝，原因见 errorText
         * @throws Base::LogicException TableSchema<T>::kTableName 为空或含点号
         */
        template<RowMappable T>
        [[nodiscard]] static bool dropTable(ConnectionPool &pool, const bool ifExists = true, std::string *errorText = nullptr)
        {
            // 成功返回前先清空出参，避免调用方读到上一次调用的残留失败原因
            clearError(errorText);

            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            return executeStatement(pool, dropTableStatement<T>(*dialect, ifExists), errorText);
        }

        /**
         * @brief 查询表是否存在
         *
         * @details 走方言给出的元数据语句（SQLite 查 sqlite_master，MySQL 查 information_schema
         *          并按当前库过滤），结果为一行一列的计数：大于 0 即存在。
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param pool 提供连接的连接池
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 表存在
         * @return false 表不存在，**或**查询失败（用 errorText 区分：失败时它非空）
         * @throws Base::LogicException TableSchema<T>::kTableName 为空或含点号
         * @note 查询失败不抛异常而返回 false，是因为本方法以布尔语义对外；需要区分「不存在」
         *       与「查询失败」的调用方应传入 errorText 并检查它是否被写入
         */
        template<RowMappable T>
        [[nodiscard]] static bool tableExists(ConnectionPool &pool, std::string *errorText = nullptr)
        {
            // 清空后才能用「出参是否非空」判断本次是查询失败还是单纯表不存在
            clearError(errorText);

            const std::string_view tableName = TableSchema<T>::kTableName;
            requireMigratableTableName(tableName);

            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            const SqlStatement statement = dialect->tableExistsStatement(tableName);

            PooledConnection connection = pool.acquire();
            if (!connection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限、连接创建失败或等待名额被拒）");
                return false;
            }

            std::unique_ptr<DatabaseResult> result = connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                writeError(errorText, "SchemaMigrator: 查询表是否存在失败：" + connection->lastError());
                return false;
            }

            // 计数语句恒返回一行；游标推进失败属查询故障，不能与「表不存在」混为一谈
            if (!result->next())
            {
                writeError(errorText, "SchemaMigrator: 表存在性查询未返回计数行（驱动状态：" + connection->lastError() + "）");
                return false;
            }

            const DatabaseValue countValue = result->getValue(0);
            if (const auto *countedRows = std::get_if<std::int64_t>(&countValue))
            {
                return *countedRows > 0;
            }

            // 计数列不是整数说明方言语句与解读方式不匹配：如实报错，不静默当成「表不存在」
            writeError(errorText, "SchemaMigrator: 表存在性查询返回了非整数计数列（实际类型 " + std::string(databaseValueTypeName(countValue)) + "），请检查方言的表存在性语句");
            return false;
        }

        /**
         * @brief 生成「给已有的表补一列」的 ALTER 语句（纯文本，不接触数据库）
         *
         * @details 补的是「结构体加了一个成员，已有的表却不会跟着变」这一格：此前本类只能建表与删表，
         *          成员加完后第一次读就报「列不存在」，而唯一的绕开办法是手写 `ALTER TABLE` 的原语 SQL——
         *          那句是方言专有的，还得自己对齐列名引用与类型映射。本方法把同一份 TableSchema 用到
         *          加列上：列名引用、类型映射、可空规则全部复用建表那条出口（appendColumnDefinition）。
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param dialect 目标引擎的方言
         * @param columnName kColumns 里要补的那一列的列名（原文，不含引用字符）
         * @return SqlStatement 完整 ALTER 语句；DDL 不含值，parameters 恒为空
         * @throws Base::LogicException kColumns 里没有这个名字的列；或该列是主键；或该列不可空——
         *         后两条是刻意的拒绝面：往已有的表上加一个 NOT NULL 且没有默认值的列，已有行没有值可填，
         *         两个引擎都会当场拒（SQLite 直接报 NOT NULL constraint failed）。把成员声明成
         *         std::optional<X> 就是「可空、允许旧行为 NULL」的正确表达；确认表为空又确实要
         *         NOT NULL 的调用方，可以自己执行带 DEFAULT 的原语 SQL
         */
        template<RowMappable T>
        [[nodiscard]] static SqlStatement addColumnStatement(const SqlDialect &dialect, const std::string_view columnName)
        {
            const std::string_view tableName = TableSchema<T>::kTableName;
            requireMigratableTableName(tableName);

            bool        matchedColumn    = false;
            bool        isPrimaryKey     = false;
            bool        isNullable       = false;
            bool        primaryKeyMarked = false;
            std::string columnDefinition;

            std::apply(
                    [&](const auto &...columnDescriptors)
                    {
                        (
                                [&](const auto &columnDescriptor)
                                {
                                    // 只认第一个同名条目：kColumns 里重名本身是结构体写错了，
                                    // 而那由 RowMapper 一侧的读写路径负责报，这里不重复判
                                    if (matchedColumn || columnDescriptor.columnName != columnName)
                                    {
                                        return;
                                    }
                                    matchedColumn        = true;
                                    isPrimaryKey         = columnDescriptor.columnName == TableSchema<T>::kPrimaryKey;
                                    using BareMemberType = std::remove_cv_t<typename std::remove_cvref_t<decltype(columnDescriptor)>::MemberType>;
                                    isNullable           = Detail::IsOptional<BareMemberType>::value;
                                    appendColumnDefinition(columnDefinition, primaryKeyMarked, dialect, columnDescriptor, TableSchema<T>::kPrimaryKey,
                                                           Detail::declaresAutoIncrementPrimaryKey<T>(), tableName);
                                }(columnDescriptors),
                                ...);
                    },
                    TableSchema<T>::kColumns);

            if (!matchedColumn)
            {
                throw Base::LogicException("SchemaMigrator: 表 " + std::string(tableName) + " 的 kColumns 里没有列 \"" + std::string(columnName) + "\"，无法生成加列语句");
            }
            if (isPrimaryKey)
            {
                throw Base::LogicException("SchemaMigrator: 主键列 \"" + std::string(columnName) + "\" 不能后补到已有的表上：已有行拿不出可填的键值");
            }
            if (!isNullable)
            {
                throw Base::LogicException("SchemaMigrator: 列 \"" + std::string(columnName) +
                                           "\" 不可空，补到已有的表上时已有行没有值可填；"
                                           "请把该成员声明成 std::optional<...>，或在确认表为空时自己执行带 DEFAULT 的原语 SQL");
            }

            SqlStatement statement;
            statement.sql = "ALTER TABLE ";
            statement.sql += dialect.quoteIdentifier(tableName);
            statement.sql += " ADD COLUMN ";
            statement.sql += columnDefinition;
            return statement;
        }

        /**
         * @brief 按结构体把表里缺的列补上（ALTER TABLE ADD COLUMN，逐列执行）
         *
         * @details 判据来自引擎本身：先问「这张表现在有哪些列」（`SqlDialect::columnListingStatement()`），
         *          结构体里有而表里没有的每一列各发一条 ALTER。也就是说**不需要一份「上次建了什么」的账**，
         *          重复调用是幂等的（已存在的列被跳过），换机器、换库、手工加过列都不需要额外状态。
         * @details 列名比对按小写：MySQL 的列名不区分大小写，而 SQLite 按声明原样给出，不折一次就会把
         *          同一个列看成两个。
         *
         * @tparam T 已特化 TableSchema 的聚合类型
         * @param pool 目标库的连接池
         * @param addedCount 可空出参：本次真正补了几列（全都在时是 0）
         * @param errorText 可空出参：失败原因（中文）。加列规则本身不成立（不可空列、主键列）也从这里出声，
         *        而不是把 SchemaMigrator 内部的抛异常语义漏给布尔出口的调用方
         * @return true 表存在且缺的列都补上了（含「一列都不缺」）
         * @return false 表不存在、取不到连接、查询或 ALTER 失败、或结构体要求补一个不可空列
         * @note 只在表已有列的基础上加列，不改、不删、不收窄任何已有列——那些都是需要数据搬迁的
         *       决定，本层没有等价物（改类型请走原语 SQL 前先想清楚旧数据怎么迁）
         */
        template<RowMappable T>
        [[nodiscard]] static bool addMissingColumns(ConnectionPool &pool, std::size_t *addedCount = nullptr, std::string *errorText = nullptr)
        {
            clearError(errorText);
            if (addedCount != nullptr)
            {
                *addedCount = 0;
            }

            if (!tableExists<T>(pool, errorText))
            {
                // tableExists 只在「查询失败」时写原因；「表不存在」是空手返回，这里补一句指引，
                // 免得调用方拿到一个 false 而读不出是哪种
                if (errorText != nullptr && errorText->empty())
                {
                    writeError(errorText, "SchemaMigrator: 表 " + std::string(TableSchema<T>::kTableName) + " 不存在，补列没有对象；请先用 createTable<T>() 建表");
                }
                return false;
            }

            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            std::vector<std::string> existingColumns;
            {
                PooledConnection connection = pool.acquire();
                if (!connection)
                {
                    writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限、连接创建失败或等待名额被拒）");
                    return false;
                }

                const SqlStatement              listing       = dialect->columnListingStatement(TableSchema<T>::kTableName);
                std::unique_ptr<DatabaseResult> listingResult = connection->execute(std::string_view{listing.sql}, listing.parameters);
                if (listingResult == nullptr)
                {
                    writeError(errorText, "SchemaMigrator: 查询表当前的列清单失败：" + connection->lastError());
                    return false;
                }
                while (listingResult->next())
                {
                    // getValue 交的是值而不是结果集内部的引用，先落到局部再取地址（与表存在性查询同一写法）
                    const DatabaseValue columnNameValue = listingResult->getValue(0);
                    const auto         *existingName    = std::get_if<std::string>(&columnNameValue);
                    if (existingName == nullptr)
                    {
                        writeError(errorText, "SchemaMigrator: 列清单查询的第一列不是文本，无法与结构体的列名比对");
                        return false;
                    }
                    existingColumns.push_back(toLowerAscii(*existingName));
                }
            }

            bool isAborted = false;
            std::apply(
                    [&](const auto &...columnDescriptors)
                    {
                        (
                                [&](const auto &columnDescriptor)
                                {
                                    if (isAborted)
                                    {
                                        return;
                                    }

                                    const std::string columnName(columnDescriptor.columnName);
                                    if (std::find(existingColumns.begin(), existingColumns.end(), toLowerAscii(columnName)) != existingColumns.end())
                                    {
                                        return;
                                    }

                                    SqlStatement alterStatement;
                                    try
                                    {
                                        alterStatement = addColumnStatement<T>(*dialect, columnName);
                                    } catch (const std::exception &exception)
                                    {
                                        // 规则只有一处实现（上面那个纯函数），这里只换出口：布尔语义的
                                        // 调用方不该收到异常。基类要抓 std::exception —— 本仓的异常体系有
                                        // 两根（LogicException 走 std::logic_error、Exception 走
                                        // std::runtime_error），只抓 Base::Exception 会漏掉 LogicException
                                        writeError(errorText, std::string{exception.what()});
                                        isAborted = true;
                                        return;
                                    }

                                    PooledConnection connection = pool.acquire();
                                    if (!connection)
                                    {
                                        writeError(errorText, "SchemaMigrator: 补列 \"" + columnName + "\" 时取不到连接（池已达上限、连接创建失败或等待名额被拒）");
                                        isAborted = true;
                                        return;
                                    }
                                    if (connection->execute(std::string_view{alterStatement.sql}, alterStatement.parameters) == nullptr)
                                    {
                                        writeError(errorText, "SchemaMigrator: 补列 \"" + columnName + "\" 失败：" + connection->lastError());
                                        isAborted = true;
                                        return;
                                    }
                                    if (addedCount != nullptr)
                                    {
                                        ++(*addedCount);
                                    }
                                }(columnDescriptors),
                                ...);
                    },
                    TableSchema<T>::kColumns);

            return !isAborted;
        }

        /**
         * @brief 一条命名迁移：升序应用、可回退，应用记录落在迁移表里
         *
         * @details 与 `setQueryTimeout` 那一族的「时机」不同，迁移是**部署动作**：它按名字记录
         *          「这条已经跑过了」，因此重复执行同一份列表是幂等的，而回退要知道撤的是哪一条。
         *          up/down 都是 `SqlStatement` 列表：用 `createTableStatement<T>()` 那组生成器
         *          现拼、或者直接写原生 SQL 都行（`SqlStatement` 允许为空文本，那时这一步只是
         *          一条版本标记）。
         */
        struct MigrationStep
        {
            std::string               name;           ///< 唯一名（迁移表的主键）；重复或为空都会在应用前被拒
            std::vector<SqlStatement> upStatements;   ///< 应用时按序执行
            std::vector<SqlStatement> downStatements; ///< 回退时按序执行
        };

        /// 迁移记录表名（取自 MigrationLedger，唯一真源在那里）
        static constexpr std::string_view kMigrationLedgerTableName = MigrationLedger::kTableName;
        /// 迁移记录表的列名（同上）
        static constexpr std::string_view kMigrationNameColumn = MigrationLedger::kNameColumn;

        /// 迁移记录表的一行：由 MigrationLedger 定义，登记也排在那边（见那一处的说明）
        using MigrationLedgerRow = MigrationLedger::Row;

        /**
         * @brief 取本池对应的方言（给「要在池之外先拼好迁移步骤」的调用方用）
         * @param pool 提供连接与方言登记表的连接池
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return std::shared_ptr<SqlDialect> 方言；登记表里没有该数据库类型时为空
         */
        [[nodiscard]] static std::shared_ptr<SqlDialect> dialectOf(ConnectionPool &pool, std::string *errorText = nullptr)
        {
            clearError(errorText);
            return resolveDialect(pool, errorText);
        }

        /**
         * @brief 建迁移记录表（不存在才建）
         * @details 走的是与其他表同一条路：`createTable<MigrationLedgerRow>(pool, true)`。
         *          应用与回退都会先确保它存在，因此调用方通常不必单独调它；它单独开着一个口，
         *          是为了让「只想看看迁移状态」的调用方不必先跑一次迁移
         * @param pool 提供连接的连接池
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 表已存在或刚建好
         * @return false 取连接失败、方言缺登记或 DDL 被拒，原因见 errorText()
         */
        [[nodiscard]] static bool ensureMigrationLedger(ConnectionPool &pool, std::string *errorText = nullptr)
        {
            return createTable<MigrationLedgerRow>(pool, true, errorText);
        }

        /**
         * @brief 读已应用的迁移名
         * @details 查询走方言的标识符引用（表名与列名都取本类的常量），因此三种引擎同一份文本生成。
         *          **顺序未定义**：迁移表只记名字，它是一份集合；要按顺序处理请以调用方的步骤表为准
         * @param pool 提供连接的连接池
         * @param appliedNames 出参：已应用的名字（每次调用先清空）
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 读取成功（列表可能为空）
         * @return false 迁移表读不出来（未建、方言缺登记、查询失败），原因见 errorText()
         */
        [[nodiscard]] static bool appliedMigrations(ConnectionPool &pool, std::vector<std::string> *appliedNames, std::string *errorText = nullptr)
        {
            clearError(errorText);
            if (appliedNames != nullptr)
            {
                appliedNames->clear();
            }

            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            const PooledConnection connection = pool.acquire();
            if (!connection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限、连接创建失败或等待名额被拒）");
                return false;
            }

            const std::string listingSql = "SELECT " + dialect->quoteIdentifier(kMigrationNameColumn) + " FROM " + dialect->quoteIdentifier(kMigrationLedgerTableName);
            const std::unique_ptr<DatabaseResult> listing = connection->execute(std::string_view{listingSql});
            if (listing == nullptr)
            {
                writeError(errorText, "SchemaMigrator: 读迁移记录失败（表可能还没建，先调 ensureMigrationLedger()）：" + connection->lastError());
                return false;
            }

            while (listing->next())
            {
                const DatabaseValue nameValue = listing->getValue(std::size_t{0});
                const auto         *nameText  = std::get_if<std::string>(&nameValue);
                if (nameText == nullptr)
                {
                    writeError(errorText,
                               "SchemaMigrator: 迁移记录的名字列读不出文本（实际类型 " + std::string(databaseValueTypeName(nameValue)) + "），请检查这张表是不是被别的程序改过");
                    return false;
                }
                if (appliedNames != nullptr)
                {
                    appliedNames->push_back(*nameText);
                }
            }
            return true;
        }

        /**
         * @brief 按列表顺序应用尚未应用的迁移（重复调用幂等）
         *
         * @details 规则四条，都是「部署动作」这一格必须钉住的：①先确保迁移表存在；②已应用的名字
         *          必须是本次列表的**前缀**——前缀之外还有记录（顺序被改过、列表漏了一条、或者
         *          换了另一份列表）时当场拒绝，因为按错误的顺序改结构比不改更糟；③逐条应用，
         *          **一条一笔事务**：结构改动与记账在同一笔里提交，崩在两者之间不会留下
         *          「结构改了却没记账」（MySQL 侧有例外，见下）；④一条失败即停在那里，已提交的
         *          前面的步骤保持已应用状态，出参给出已应用的条数。
         *
         * @warning **MySQL 的 DDL 会隐式提交**，因此那一侧的「一条一笔事务」是尽力而为：UP 语句
         *          已经执行到一半时失败，回滚撤不回结构（记账那一条也随隐式提交落下或不落）。
         *          SQLite 与 PostgreSQL 的 DDL 可回滚，那一侧这条保证是完整的。要在 MySQL 上把
         *          风险降到最低，就把一条迁移的 UP 拆得尽量小（一条 DDL 一条迁移），并用
         *          `downStatements` 写清怎么手工退。
         *
         * @param pool 提供连接的连接池
         * @param steps 迁移列表，按应应用的顺序给出；名字必须两两不同
         * @param appliedCount 可选出参：本次真正应用了几条（幂等的那次是 0）
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 列表中的每一条现在都已应用
         * @return false 列表不合法（空名、重名）、与已应用的不一致（不是前缀）、或某一条执行失败
         * @throws Base::LogicException 列表里有空名或重名（调用方把列表写错了，不是运行期故障）
         */
        [[nodiscard]] static bool migrate(ConnectionPool &pool, const std::span<const MigrationStep> steps, std::size_t *appliedCount = nullptr, std::string *errorText = nullptr)
        {
            clearError(errorText);
            if (appliedCount != nullptr)
            {
                *appliedCount = 0;
            }

            requireDistinctStepNames(steps);
            if (!ensureMigrationLedger(pool, errorText))
            {
                return false;
            }

            std::vector<std::string> appliedNames;
            if (!appliedMigrations(pool, &appliedNames, errorText))
            {
                return false;
            }

            if (!isAppliedPrefixOfSteps(appliedNames, steps, errorText))
            {
                return false;
            }

            // 方言在开事务之前取：resolveDialect() 自己要借一条连接，而池上限为 1 的部署里
            // 事务正握着那条连接——取在事务里就是死等（实测等满 5 秒的借用超时）
            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            const std::unordered_set<std::string> appliedSet(appliedNames.begin(), appliedNames.end());
            for (const MigrationStep &step: steps)
            {
                if (appliedSet.contains(step.name))
                {
                    continue;
                }

                if (!runStep(dialect, pool, step, /*isRollingBack*/ false, errorText))
                {
                    return false;
                }
                if (appliedCount != nullptr)
                {
                    ++(*appliedCount);
                }
            }
            return true;
        }

        /**
         * @brief 回退最后应用的若干条迁移（按列表顺序的**逆序**撤）
         *
         * @details 「最后应用的」由列表顺序决定：已应用的必须是列表前缀（同 migrate()），
         *          因此最后一条就是前缀的最后一项。逐条执行 `downStatements` 并删掉它的记账，
         *          同样是**一条一笔事务**；一条失败即停，已退掉的保持已退状态，出参给出退了几条。
         *          退回的迁移还在列表里，因此回退之后再 `migrate()` 会重新应用它们。
         *
         * @warning 与 migrate() 同一条 MySQL 例外：那一侧撤结构的那句 DDL 隐式提交，
         *          「撤结构 + 删记账」不是一笔原子事务。SQLite / PostgreSQL 侧完整。
         *
         * @param pool 提供连接的连接池
         * @param steps 与 migrate() 同一份列表
         * @param stepCount 要回退几条；必须 ≥ 1 且不超过已应用的条数
         * @param rolledBackCount 可选出参：实际退掉了几条
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 指定的条数都已回退
         * @return false 参数越界、与已应用的不一致、或某一条回退失败
         * @throws Base::LogicException 列表里有空名或重名
         */
        [[nodiscard]] static bool rollbackLast(ConnectionPool &pool, const std::span<const MigrationStep> steps, const std::size_t stepCount,
                                               std::size_t *rolledBackCount = nullptr, std::string *errorText = nullptr)
        {
            clearError(errorText);
            if (rolledBackCount != nullptr)
            {
                *rolledBackCount = 0;
            }

            requireDistinctStepNames(steps);
            if (stepCount == 0)
            {
                writeError(errorText, "SchemaMigrator: 回退条数为 0——本方法只做「退最后 N 条」，什么都不退请直接不调用它");
                return false;
            }

            if (!ensureMigrationLedger(pool, errorText))
            {
                return false;
            }

            std::vector<std::string> appliedNames;
            if (!appliedMigrations(pool, &appliedNames, errorText))
            {
                return false;
            }

            if (!isAppliedPrefixOfSteps(appliedNames, steps, errorText))
            {
                return false;
            }

            if (stepCount > appliedNames.size())
            {
                writeError(errorText, "SchemaMigrator: 要回退 " + std::to_string(stepCount) + " 条，而当前只应用了 " + std::to_string(appliedNames.size()) + " 条");
                return false;
            }

            // 方言在开事务之前取（理由同 migrate()：池上限为 1 时在事务里再取就是死等）
            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            // 逆序撤：先退最后应用的那一条，退回的结构因此按相反顺序合上
            for (std::size_t index = 0; index < stepCount; ++index)
            {
                const std::size_t position = appliedNames.size() - 1 - index;
                if (!runStep(dialect, pool, steps[position], /*isRollingBack*/ true, errorText))
                {
                    return false;
                }
                if (rolledBackCount != nullptr)
                {
                    ++(*rolledBackCount);
                }
            }
            return true;
        }

        /**
         * @brief 幂等落一份种子数据：已有行不动，只补缺失的行
         *
         * @details 「种子数据」这一格在部署动作里的形状是：结构由迁移改、数据由本方法补，
         *          二者属于**同一个部署步骤**（先 `migrate()` 再 `seed()`），并且可以反复执行——
         *          已经存在的行不产生第二条，只有缺失的行被补进来。写入走
         *          `Queryable::insertBatch(rows, InsertConflict::Ignore)`（SQLite 是 `INSERT OR IGNORE`、
         *          MySQL 是 `INSERT IGNORE`），因此分块、参数上限、取值转换与唯一的写入路径完全复用，
         *          不另起一份实现。
         *
         * @tparam RowType 行类型（与 `TableSchema<RowType>` 配套，同 ORM 的其余入口）
         * @param pool 提供连接的连接池
         * @param rows 种子行；幂等键取它们在**表上的唯一键**（通常是显式给出的主键）。空集合什么都不做
         * @param insertedCount 可选出参：本次真正新增了几行（已存在的不计入；空列表恒为 0）
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 每一行现在都在表里
         * @return false 表声明了自增主键（写不出冲突键，做不到幂等），或写入失败
         * @note 自增主键的表被判为不可用：写入清单里不含主键列，`INSERT OR IGNORE` 撞不上任何唯一键，
         *       每跑一轮都会追加一份——与其静默追加重复行，不如当场拒。这类表要落种子数据，
         *       请自己写幂等 SQL，或给表声明一个业务唯一键后直接用 `insertBatch(rows, InsertConflict::Ignore)`
         */
        template<typename RowType>
        [[nodiscard]] static bool seed(ConnectionPool &pool, const std::span<const RowType> rows, std::size_t *insertedCount = nullptr, std::string *errorText = nullptr)
        {
            clearError(errorText);
            if (insertedCount != nullptr)
            {
                *insertedCount = 0;
            }

            // 声明了自增主键的表不能走这条路（理由见 @note）
            if (Detail::declaresAutoIncrementPrimaryKey<RowType>())
            {
                writeError(errorText, "SchemaMigrator: 表 " + std::string(TableSchema<RowType>::kTableName) +
                                              " 声明了自增主键，种子数据无法幂等——写入清单里不含主键列，重复执行会追加重复行。"
                                              "请自己写幂等 SQL，或给表声明一个业务唯一键后直接用 Queryable::insertBatch(rows, InsertConflict::Ignore)");
                return false;
            }

            try
            {
                // 空数据集由 insertBatch 自己早退（它承诺「空集合直接返回 0，不产生任何语句」），
                // 这里不再重复判一次——多一道守卫就是多一个永远走不到的分支
                const std::int64_t insertedRows = Queryable<RowType>(pool).insertBatch(rows, InsertConflict::Ignore);
                if (insertedCount != nullptr)
                {
                    *insertedCount = static_cast<std::size_t>(std::max<std::int64_t>(0, insertedRows));
                }
                return true;
            } catch (const std::exception &exception)
            {
                // 与 addMissingColumns 同款出口：布尔语义的调用方不该收到异常。
                // 基类要抓 std::exception——本仓的异常体系有两根（LogicException 走 std::logic_error、
                // Exception 走 std::runtime_error），只抓 Base::Exception 会漏掉 LogicException
                writeError(errorText, std::string{exception.what()});
                return false;
            }
        }

        /**
         * @brief 幂等落一份种子数据（容器版）
         * @details 与 span 版完全等价；两个重载并存的理由是**模板实参推导**：从
         *          `const std::vector<RowType>&` 推不出 `std::span<const RowType>`（推导不考虑
         *          用户定义转换），只给 span 版会让最常见的调用写法编不过
         * @param pool 提供连接的连接池
         * @param rows 种子行
         * @param insertedCount 可选出参：本次真正新增了几行
         * @param errorText 可选出参；进入调用时先清空，仅失败时写入中文原因
         * @return true 每一行现在都在表里
         */
        template<typename RowType>
        [[nodiscard]] static bool seed(ConnectionPool &pool, const std::vector<RowType> &rows, std::size_t *insertedCount = nullptr, std::string *errorText = nullptr)
        {
            return seed<RowType>(pool, std::span<const RowType>{rows}, insertedCount, errorText);
        }

    private:
        /**
         * @brief 把标识符折成小写用于比对（只折 ASCII 大写）
         * @details MySQL 的列名不区分大小写，SQLite 按声明原样给出；不折一次就会把同一个列看成两个，
         *          结果是重复加列（引擎报错）或漏判已有列。非 ASCII 部分原样保留，两侧折法一致，
         *          比较仍然成立
         * @param text 原始文本
         * @return std::string 折过小写后的副本
         */
        [[nodiscard]] static std::string toLowerAscii(const std::string_view text)
        {
            std::string lowered(text);
            for (char &character: lowered)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return lowered;
        }

        /**
         * @brief 把成员类型映射成逻辑列类型
         *
         * @details 规则与 RowMapper 的取值映射一一对应：bool 先于整型判定（它是整型但不是 1/0 语义），
         *          无符号整型单独成一类（SQLite 无无符号类型，MySQL 有），浮点一律 Double，
         *          二进制载荷（std::vector<std::uint8_t> 或 std::vector<std::byte>）一律 Blob。
         *          因此 ColumnType 的每个取值都有成员类型能产生它，不存在只为将来预留的分支。
         *
         * @tparam ValueType 去掉 std::optional 包装后的成员类型
         * @return ColumnType 该成员对应的逻辑列类型
         */
        template<typename ValueType>
        [[nodiscard]] static constexpr ColumnType columnTypeOf() noexcept
        {
            using BareType = std::remove_cv_t<ValueType>;

            if constexpr (std::is_same_v<BareType, bool>)
            {
                return ColumnType::Bool;
            } else if constexpr (std::is_integral_v<BareType>)
            {
                // 有符号/无符号分开：二者在 MySQL 上是不同的物理类型，取值范围也不一样
                return std::is_unsigned_v<BareType> ? ColumnType::UInt64 : ColumnType::Int64;
            } else if constexpr (std::is_floating_point_v<BareType>)
            {
                return ColumnType::Double;
            } else if constexpr (std::is_same_v<BareType, std::string>)
            {
                return ColumnType::Text;
            } else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
            {
                // 两种成员拼法都落到 Blob：DDL 只关心「这一列是二进制」，
                // 由 value_type 是 uint8_t 还是 std::byte 决定的差异在值映射处已经归一
                return ColumnType::Blob;
            } else
            {
                // 不受支持的类型已被 createTableStatement() 的 static_assert 拦住，
                // 这里只是让 if constexpr 的所有分支都有返回值
                static_assert(Detail::kAlwaysFalse<ValueType>, "SchemaMigrator：不支持的列类型。仅支持整型、bool、浮点、std::string、"
                                                               "二进制载荷（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                                                               "以及它们的 std::optional 包装");
                return ColumnType::Text;
            }
        }

        /**
         * @brief 校验表名能被迁移器使用：非空，且不带点号
         *
         * @details 迁移器的一切都锚在「本连接的默认库」上：存在性检查按 MySQL 的 DATABASE() 与 SQLite 的
         *          sqlite_master 取，建表与删表也只有那一个落点。带库/模式前缀的名字没有一致解释
         *          （检查报「不存在」、建表建到别处、查询读第三张表），因此与空表名一起在生成语句前拒掉。
         *
         * @param tableName TableSchema<T>::kTableName
         * @throws Base::LogicException 表名为空，或含点号
         */
        static void requireMigratableTableName(const std::string_view tableName)
        {
            if (tableName.empty())
            {
                // 表名为空（主模板的默认值，或完全特化里被显式留空）时生成 "CREATE TABLE """ 毫无意义
                throw Base::LogicException("SchemaMigrator: TableSchema<T>::kTableName 为空，"
                                           "请先特化 TableSchema 并填写表名");
            }

            if (tableName.find('.') != std::string_view::npos)
            {
                throw Base::LogicException("SchemaMigrator: 表名 \"" + std::string(tableName) +
                                           "\" 含点号，迁移器不支持带库/模式前缀的表名：存在性检查只看本连接的默认库。"
                                           "要换库请改 ConnectionConfig.database，或直接用原生 execute() 发这条 DDL");
            }
        }

        /**
         * @brief 生成一列的完整定义并追加到列定义串
         *
         * @tparam ColumnDescriptorType ColumnDescriptor<T, MemberType> 的推导类型
         * @param columnDefinitions 输出缓冲区，"列名 类型 [NOT NULL] [PRIMARY KEY]" 追加到末尾
         * @param primaryKeyDeclared 出参：本列是否是主键（命中时置 true）
         * @param dialect 提供类型名映射与标识符引用的方言
         * @param columnDescriptor 列的元信息（列名 + 成员指针）
         * @param primaryKeyName TableSchema<T>::kPrimaryKey
         * @param isAutoIncrementPrimaryKey 主键是否声明为数据库自增生成
         * @param tableName 表名，只用于拒绝自增写法时把原因指到具体的表
         */
        template<typename ColumnDescriptorType>
        static void appendColumnDefinition(std::string &columnDefinitions, bool &primaryKeyDeclared, const SqlDialect &dialect, const ColumnDescriptorType &columnDescriptor,
                                           const std::string_view primaryKeyName, const bool isAutoIncrementPrimaryKey, const std::string_view tableName)
        {
            using MemberType = typename ColumnDescriptorType::MemberType;
            using BareType   = std::remove_cv_t<MemberType>;

            // 列定义里的点号没有一致解释：DDL 会把 "a.b" 建成一个真名叫 a.b 的列，而 INSERT 的列清单
            // 与读侧按「表.列」分段引用，那张表此后没有一行能写进这一列。限定名属于查询侧的
            // where()/orderBy()，不该出现在 kColumns 里
            if (columnDescriptor.columnName.find('.') != std::string_view::npos)
            {
                throw Base::LogicException("SchemaMigrator: 表 " + std::string(tableName) + " 的列名 \"" + std::string(columnDescriptor.columnName) +
                                           "\" 含点号，列名请填裸名；要按「表.列」限定名查询，请写在 where()/orderBy() 里");
            }

            // 可空规则：std::optional<X> 允许 NULL，其余一律 NOT NULL。
            // optional 只是可空标记而不是存储类型，去掉包装后的类型才是真正的列类型
            constexpr bool kIsNullable = Detail::IsOptional<BareType>::value;
            using ValueType            = Detail::ColumnStorageTypeOf<BareType>;

            // 分隔符前置而不是后置：后置会在最后一列留下一个尾逗号，还得在拼接处再裁一次
            if (!columnDefinitions.empty())
            {
                columnDefinitions += ", ";
            }

            const bool isKeyColumn = columnDescriptor.columnName == primaryKeyName;

            if (isKeyColumn && isAutoIncrementPrimaryKey)
            {
                // 自增写法的位置与关键字两个引擎都不同，整串交给方言；给不出文本就是这套引擎
                // 不支持把该列建成自增列——在建表前失败，而不是建出一张主键不会自增的表让
                // 省略主键的 INSERT 之后以引擎错误收场
                const std::string autoIncrementDefinition =
                        dialect.autoIncrementPrimaryKeyDefinition(dialect.quoteIdentifier(columnDescriptor.columnName), columnTypeOf<ValueType>());
                if (autoIncrementDefinition.empty())
                {
                    throw Base::LogicException("SchemaMigrator: 表 " + std::string(tableName) + " 的主键列 \"" + std::string(columnDescriptor.columnName) +
                                               "\" 声明为自增，但当前方言不能把它建成自增列（自增列要求整数类型的主键）");
                }

                columnDefinitions += autoIncrementDefinition;
                primaryKeyDeclared = true;
                return;
            }

            // 列名一律引用：含空格、保留字或引用字符的列名只有被引用才能作为标识符出现
            columnDefinitions += dialect.quoteIdentifier(columnDescriptor.columnName);
            columnDefinitions += ' ';
            // 主键列走可作键的类型映射：MySQL 的 TEXT/LONGBLOB 进不了索引，
            // 直接用 columnTypeName() 的表征会让建表语句以 1170 号错误被整个拒掉
            columnDefinitions += isKeyColumn ? dialect.keyColumnTypeName(columnTypeOf<ValueType>()) : dialect.columnTypeName(columnTypeOf<ValueType>());

            if constexpr (!kIsNullable)
            {
                columnDefinitions += " NOT NULL";
            }

            if (isKeyColumn)
            {
                // PRIMARY KEY 写在约束的最后：类型与可空性在前，读起来与结构体声明顺序一致。
                // 命中主键的列同时记入出参，供调用方判断 kPrimaryKey 是否落在 kColumns 里
                columnDefinitions += " PRIMARY KEY";
                primaryKeyDeclared = true;
            }
        }

        /**
         * @brief 取得池中连接对应的方言
         *
         * @details 与 Queryable 同一套推导方式：借一条连接读它的真实 DatabaseType，读完立刻归还
         *          （RAII 包装在作用域结束时自动归还，不会额外占用池容量）。
         *
         * @param pool 提供探测连接的连接池
         * @param errorText 可选出参；失败时写入中文原因
         * @return std::shared_ptr<SqlDialect> 方言实例；失败时为空指针
         */
        [[nodiscard]] static std::shared_ptr<SqlDialect> resolveDialect(ConnectionPool &pool, std::string *errorText)
        {
            const PooledConnection probeConnection = pool.acquire();
            if (!probeConnection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败，无法推导数据库类型"
                                      "（池已达上限、连接创建失败或等待名额被拒）");
                return nullptr;
            }

            // 未实现的类型由注册表抛 Base::InvalidArgumentException（用法错误）：刻意不在此处转成
            // 「查询失败 + 原因文本」，用法错误应当由调用方看见，而不是被折叠成可恢复的返回值
            return DialectRegistry::dialectFor(probeConnection->databaseType());
        }

        /**
         * @brief 在池中的一条连接上执行 DDL 语句
         *
         * @param pool 提供连接的连接池
         * @param statement 待执行的 DDL 语句（parameters 恒为空）
         * @param errorText 可选出参；失败时写入中文原因
         * @return true 语句被引擎接受
         * @return false 取连接失败或语句执行失败，原因见 errorText
         */
        /**
         * @brief 执行一条迁移（应用或回退）并同步记账
         * @details 结构改动与记账在同一笔事务里：两者中间崩溃会留下「改了结构却查不到记录」（下次会重跑）

         * *          或「记了账却没改」（下次跳过）这类最难排查的状态。MySQL 的 DDL 隐式提交让这条保证
         *          打了折扣，那一侧的例外写在 migrate() 的 @warning 里

         * * @param dialect 目标引擎的方言：**由调用方先取好传进来**——本方法持有事务连接，而池上限为 1 的
         *        部署里 `resolveDialect()`
         * 自己也要借一条连接，在事务里再取就是死等（实测等满 5 秒借用超时）
         * @param pool 提供连接的连接池
         * @param step 待执行的那一条
         * @param
         * isRollingBack true 执行 downStatements 并删记账；false 执行 upStatements 并写记账
         * @param errorText 失败原因出参
         * @return true 该条已提交

         */
        [[nodiscard]] static bool runStep(const std::shared_ptr<SqlDialect> &dialect, ConnectionPool &pool, const MigrationStep &step, const bool isRollingBack,
                                          std::string *errorText)
        {
            Transaction transaction(pool);
            if (!transaction.isActive())
            {
                writeError(errorText, "SchemaMigrator: 开启迁移事务失败（取连接失败或引擎拒绝了开启事务语句）：" + transaction.lastError());
                return false;
            }

            const std::span<const SqlStatement> statements = isRollingBack ? std::span<const SqlStatement>(step.downStatements) : std::span<const SqlStatement>(step.upStatements);
            const std::string_view              phase      = isRollingBack ? std::string_view{"回退"} : std::string_view{"应用"};
            for (std::size_t index = 0; index < statements.size(); ++index)
            {
                const SqlStatement &statement = statements[index];
                if (statement.sql.empty())
                {
                    continue; // 空语句＝这一步只是版本标记，没有结构改动
                }
                if (transaction.connection().execute(std::string_view{statement.sql}, statement.parameters) == nullptr)
                {
                    writeError(errorText, "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的第 " + std::to_string(index + 1) + " 条语句失败（本次已回滚）：" +
                                                  transaction.connection().lastError());
                    return false;
                }
            }

            // 占位符由方言给：PostgreSQL 的扩展查询协议只认 $1，写死 "?" 会被服务端以
            // 42601（syntax error at or near ")"）拒绝——三种引擎里只有它会这样。
            std::string ledgerSql;
            if (isRollingBack)
            {
                ledgerSql = "DELETE FROM " + dialect->quoteIdentifier(kMigrationLedgerTableName) + " WHERE " + dialect->quoteIdentifier(kMigrationNameColumn) + " = ";
                dialect->appendPlaceholder(ledgerSql, 0);
            } else
            {
                ledgerSql = "INSERT INTO " + dialect->quoteIdentifier(kMigrationLedgerTableName) + " (" + dialect->quoteIdentifier(kMigrationNameColumn) + ") VALUES (";
                dialect->appendPlaceholder(ledgerSql, 0);
                ledgerSql += ')';
            }

            const std::vector<DatabaseValue> ledgerParameters{DatabaseValue(step.name)};
            if (transaction.connection().execute(std::string_view{ledgerSql}, std::span<const DatabaseValue>(ledgerParameters)) == nullptr)
            {
                writeError(errorText, "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的记账失败（本次已回滚）：" + transaction.connection().lastError());
                return false;
            }

            if (!transaction.commit())
            {
                writeError(errorText, "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的提交失败：" + transaction.lastError());
                return false;
            }
            return true;
        }

        /**
         * @brief 列表里的名字必须两两不同且非空
         * @details
         * 重名会让记账退化成一条记录对应两条迁移，回退时撤错结构；空名则让记账的主键

         * *
         * 变成一个谁都读不懂的值。两者都是调用方把列表写错了（编程错误），走 LogicException
         * @param steps 迁移列表
         *
         * @throws Base::LogicException 出现空名或重名
         */
        static void requireDistinctStepNames(const std::span<const MigrationStep> steps)
        {
            std::unordered_set<std::string> seenNames;
            for (const MigrationStep &step: steps)
            {
                if (step.name.empty())
                {
                    throw Base::LogicException("SchemaMigrator: 迁移列表里有空名字：迁移名是记账的主键，空名会让记录失去意义");
                }
                if (!seenNames.insert(step.name).second)
                {
                    throw Base::LogicException("SchemaMigrator: 迁移列表里有重名「" + step.name + "」：一条记账只能对应一条迁移，重名会让回退撤错结构");
                }
            }
        }

        /**
         * @brief 已应用的迁移必须是本次列表的前缀
         * @details
         * 前缀之外还有记账（列表漏了一条、顺序被改过、或者换了另一份列表）时继续应用，

         * *
         * 就是拿一个与落库历史不一致的顺序去改结构；宁可当场拒掉，让人先把两边的列表对齐
         * @param appliedNames
         * 迁移表里的名字（顺序未定义，这里按集合语义用）

         * *
         * @param steps 本次的列表
         * @param errorText 失败原因出参
         * @return true 一致；false 已写入原因
         */
        [[nodiscard]] static bool isAppliedPrefixOfSteps(const std::vector<std::string> &appliedNames, const std::span<const MigrationStep> steps, std::string *errorText)
        {
            if (appliedNames.size() > steps.size())
            {
                writeError(errorText, "SchemaMigrator: 迁移表里有 " + std::to_string(appliedNames.size()) + " 条记录，而本次列表只有 " + std::to_string(steps.size()) +
                                              " 条：已应用的必须是本次列表的前缀，两者不是同一份历史，请先把列表补回原样");
                return false;
            }

            const std::unordered_set<std::string> appliedSet(appliedNames.begin(), appliedNames.end());
            for (std::size_t index = 0; index < steps.size(); ++index)
            {
                const bool isApplied = appliedSet.contains(steps[index].name);
                if (index < appliedNames.size() && !isApplied)
                {
                    writeError(errorText, "SchemaMigrator: 已应用的历史不是本次列表的前缀——列表第 " + std::to_string(index + 1) + " 条「" + steps[index].name +
                                                  "」没有记账，而它前面那几条都记过；这说明列表漏了一条或顺序被改过，请先对齐再迁移");
                    return false;
                }
                if (index >= appliedNames.size() && isApplied)
                {
                    writeError(errorText, "SchemaMigrator: 列表第 " + std::to_string(index + 1) + " 条「" + steps[index].name + "」还排在前缀之外，却已经记过账；请把列表补回原样");
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] static bool executeStatement(ConnectionPool &pool, const SqlStatement &statement, std::string *errorText)
        {
            const PooledConnection connection = pool.acquire();
            if (!connection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限、连接创建失败或等待名额被拒）");
                return false;
            }

            // 连接在语句执行完、结果集销毁之后才归还：结果集持有连接句柄的非拥有指针（见 SqliteConnection）
            const std::unique_ptr<DatabaseResult> result = connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                writeError(errorText, "SchemaMigrator: DDL 执行失败：" + connection->lastError());
                return false;
            }

            // DDL 没有返回列，execute() 非空即代表引擎接受了这条语句
            return true;
        }

        /**
         * @brief 在对外方法入口清空可选出参
         * @details 必须在 resolveDialect() 之前调用：调用方常复用同一个字符串跨多次调用，
         *          若只在失败时写入，成功返回的调用会把上一次的失败原因留在出参里，
         *          tableExists() 赖以为生的「不存在 vs 查询失败」判据随即失效。
         * @param errorText 出参指针，为空时什么都不做
         */
        static void clearError(std::string *errorText) noexcept
        {
            if (errorText != nullptr)
            {
                errorText->clear();
            }
        }

        /**
         * @brief 把失败原因写入可选出参
         * @param errorText 出参指针，为空时什么都不做
         * @param reason 中文失败原因
         */
        static void writeError(std::string *errorText, std::string reason)
        {
            if (errorText != nullptr)
            {
                *errorText = std::move(reason);
            }
        }
    };

} // namespace AsynGyanis::Database::Queryable
