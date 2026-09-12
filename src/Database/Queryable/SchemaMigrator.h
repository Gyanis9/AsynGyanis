/**
 * @file SchemaMigrator.h
 * @brief 建表迁移工具 —— 从 TableSchema<T> 生成并执行 DDL
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details ORM 只管数据（SELECT/INSERT/UPDATE/DELETE），建表这件事此前只能手写原生 SQL；
 *          本类把「表结构」的唯一真值来源（TableSchema<T>）也用于 DDL，两个方向因此不会脱节：
 *          改了 kColumns 却忘了改手工建表语句，是 ORM 项目里最常见的环境不一致来源。
 *
 * ## 列定义规则（与 RowMapper 的映射规则严格对称）
 * 每一列渲染成「引用后的列名 + 类型名 [ + NOT NULL] [ + PRIMARY KEY]」，逐项依据：
 * - 列名：一律经 SqlDialect::quoteIdentifier() 引用，含空格、保留字、引号的列名都能落地；
 * - 类型名：由成员类型推出逻辑类型（ColumnType），再交给 SqlDialect::columnTypeName()
 *   换成目标引擎的物理类型名（各引擎的类型名差异见方言实现处的注释）；
 * - 可空：std::optional<X> → 不加约束（允许 NULL），X → NOT NULL。与 RowMapper 的读方向
 *   完全一致：NULL 只能落进 std::optional 成员，非 optional 成员遇到 NULL 会抛异常，
 *   所以「非 optional 列允许 NULL」在数据库侧就是一颗定时炸弹，这里直接禁止；
 * - 主键：列名等于 TableSchema<T>::kPrimaryKey 的列加 PRIMARY KEY。主键列若是非 optional
 *   成员，约束文本形如 `"id" BIGINT NOT NULL PRIMARY KEY`（PK 本身已隐含 NOT NULL，
 *   显式写出是为了让生成的 DDL 与结构体声明一一对应，读起来不需要脑补隐含规则）。
 *   若 kPrimaryKey 非空却在 kColumns 里找不到同名列，一律抛 Base::LogicException：
 *   那通常意味着列名拼写不一致，静默建出「没有主键的表」比直接失败危险得多。
 *
 * ## 为什么 createTableStatement() 需要方言参数
 * 类型名与标识符引用都是引擎知识（SQLite 的 INTEGER 与 MySQL 的 BIGINT 不是一回事），
 * 因此离线生成也必须先知道目标是哪个引擎。无参版本无法同时满足两个方言，
 * 所以这里显式要求传入 const SqlDialect&：离线断言文本时直接构造 SqliteDialect / MySqlDialect。
 *
 * ## 在线方法如何取得方言
 * 与 Queryable 一致：从池中借出一条连接读它的真实 DatabaseType，再向 DialectRegistry 要方言
 * （池配置里没有类型信息，直接问连接最可靠），读完立刻归还。方言类型不支持时不抛异常，
 * 而是返回 false 并把原因写进 errorText——本类的在线方法统一用「返回值 + 可选错误文本」报错。
 *
 * ## 失败语义
 * - 编译期错误（缺少 TableSchema 特化、成员类型不受支持）：static_assert，给出中文提示；
 * - 编程错误（kTableName 为空、kPrimaryKey 在 kColumns 中不存在）：抛 Base::LogicException；
 *   注意它**不属于**本模块的 DatabaseException 家族——那是给运行期故障用的，而这类错误是
 *   声明写错了，重试无意义，因此刻意留在 std::logic_error 这条标准分支上，
 *   不被 `catch (const Base::Exception &)` 吞掉；
 * - 运行期失败（取连接失败、方言不支持、DDL 被引擎拒绝）：返回 false，原因见 errorText（可选出参）。
 *
 * @code
 *   // 离线：只生成文本，不接触数据库
 *   const SqliteDialect dialect;
 *   SqlStatement statement = SchemaMigrator::createTableStatement<User>(dialect);
 *   // => CREATE TABLE IF NOT EXISTS "users" ("id" INTEGER NOT NULL PRIMARY KEY, "name" TEXT NOT NULL)
 *
 *   // 在线：建表 → 用 ORM 读写 → 查表是否存在 → 删表
 *   SchemaMigrator::createTable<User>(pool);
 *   Queryable<User> query(pool);
 *   query.insert(User{1, "张三", 18});
 *   SchemaMigrator::tableExists<User>(pool);   // => true
 *   SchemaMigrator::dropTable<User>(pool);
 * @endcode
 */
#pragma once

#include "Base/Exception/LogicException.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqlStatement.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/RowMapper.h"
#include "Database/Queryable/TableSchema.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace AsynGyanis::Database::Queryable
{
    namespace Detail
    {
        /**
         * @brief 取出列的实际存储类型：std::optional<X> 取 X，其余取自身
         *
         * @details 用偏特化而不是 std::conditional_t：后者要求两个候选类型都成立，
         *          遇到非 optional 成员去取 ::value_type 会直接编译失败。
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
         * @throws Base::LogicException TableSchema<T>::kTableName 为空，或 kPrimaryKey 非空
         *         但在 kColumns 中找不到同名列（列名拼写不一致）
         */
        template<RowMappable T>
        [[nodiscard]] static SqlStatement createTableStatement(const SqlDialect &dialect,
                                                               const bool ifNotExists = true)
        {
            // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯；
            // 判定与 RowMapper 共用同一份 trait，因此「能建表」与「能读写」永远等价
            static_assert(Detail::allColumnTypesSupported<T>(),
                          "SchemaMigrator：TableSchema<T>::kColumns 中存在不支持的列类型。"
                          "仅支持整型、bool、浮点、std::string、二进制载荷"
                          "（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                          "以及它们的 std::optional 包装");

            const std::string_view tableName = TableSchema<T>::kTableName;
            if (tableName.empty())
            {
                // 表名为空（主模板的默认值，或完全特化里被显式留空）时生成 "CREATE TABLE """ 毫无意义
                throw Base::LogicException("SchemaMigrator: TableSchema<T>::kTableName 为空，"
                                           "请先特化 TableSchema 并填写表名");
            }

            bool        primaryKeyDeclared = false;
            std::string columnDefinitions;

            std::apply(
                [&](const auto &...columnDescriptors)
                {
                    // 折叠表达式从左到右执行（逗号运算符），列序与 kColumns 声明顺序严格一致
                    (appendColumnDefinition(columnDefinitions, primaryKeyDeclared, dialect,
                                            columnDescriptors, TableSchema<T>::kPrimaryKey), ...);
                },
                TableSchema<T>::kColumns);

            if (!TableSchema<T>::kPrimaryKey.empty() && !primaryKeyDeclared)
            {
                // 主键列名与任何列名都不同：多半是 kPrimaryKey 与 kColumns 里的列名拼写不一致。
                // 静默建出无主键表会让「按主键更新/删除」这类操作在运行期才暴露问题，因此当场失败
                throw Base::LogicException("SchemaMigrator: 表 " + std::string(tableName) + " 的主键列 \"" +
                                           std::string(TableSchema<T>::kPrimaryKey) +
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
         * @throws Base::LogicException TableSchema<T>::kTableName 为空
         */
        template<RowMappable T>
        [[nodiscard]] static SqlStatement dropTableStatement(const SqlDialect &dialect, const bool ifExists = true)
        {
            const std::string_view tableName = TableSchema<T>::kTableName;
            if (tableName.empty())
            {
                throw Base::LogicException("SchemaMigrator: TableSchema<T>::kTableName 为空，"
                                           "请先特化 TableSchema 并填写表名");
            }

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
         * @throws Base::LogicException 表结构本身不合法（表名为空、主键列不存在），见 createTableStatement()
         */
        template<RowMappable T>
        [[nodiscard]] static bool createTable(ConnectionPool &pool,
                                             const bool ifNotExists = true,
                                             std::string *errorText = nullptr)
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
         * @throws Base::LogicException TableSchema<T>::kTableName 为空
         */
        template<RowMappable T>
        [[nodiscard]] static bool dropTable(ConnectionPool &pool,
                                            const bool ifExists = true,
                                            std::string *errorText = nullptr)
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
         * @throws Base::LogicException TableSchema<T>::kTableName 为空
         * @note 查询失败不抛异常而返回 false，是因为本方法以布尔语义对外；需要区分「不存在」
         *       与「查询失败」的调用方应传入 errorText 并检查它是否被写入
         */
        template<RowMappable T>
        [[nodiscard]] static bool tableExists(ConnectionPool &pool, std::string *errorText = nullptr)
        {
            // 清空后才能用「出参是否非空」判断本次是查询失败还是单纯表不存在
            clearError(errorText);

            const std::string_view tableName = TableSchema<T>::kTableName;
            if (tableName.empty())
            {
                throw Base::LogicException("SchemaMigrator: TableSchema<T>::kTableName 为空，"
                                           "请先特化 TableSchema 并填写表名");
            }

            const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
            if (dialect == nullptr)
            {
                return false;
            }

            const SqlStatement statement = dialect->tableExistsStatement(tableName);

            PooledConnection connection = pool.acquire();
            if (!connection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限或连接创建失败）");
                return false;
            }

            std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{statement.sql}, statement.parameters);
            if (result == nullptr)
            {
                writeError(errorText, "SchemaMigrator: 查询表是否存在失败：" + connection->lastError());
                return false;
            }

            // 计数语句恒返回一行；没有行说明语句形态不符预期，按「不存在」处理
            if (!result->next())
            {
                return false;
            }

            const DatabaseValue countValue = result->getValue(0);
            if (const auto *countedRows = std::get_if<std::int64_t>(&countValue))
            {
                return *countedRows > 0;
            }

            // 计数列不是整数说明方言的语句与解读方式不匹配（正常路径不可达），按不存在处理
            return false;
        }

    private:
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
            }
            else if constexpr (std::is_integral_v<BareType>)
            {
                // 有符号/无符号分开：二者在 MySQL 上是不同的物理类型，取值范围也不一样
                return std::is_unsigned_v<BareType> ? ColumnType::UInt64 : ColumnType::Int64;
            }
            else if constexpr (std::is_floating_point_v<BareType>)
            {
                return ColumnType::Double;
            }
            else if constexpr (std::is_same_v<BareType, std::string>)
            {
                return ColumnType::Text;
            }
            else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
            {
                // 两种成员拼法都落到 Blob：DDL 只关心「这一列是二进制」，
                // 由 value_type 是 uint8_t 还是 std::byte 决定的差异在值映射处已经归一
                return ColumnType::Blob;
            }
            else
            {
                // 不受支持的类型已被 createTableStatement() 的 static_assert 拦住，
                // 这里只是让 if constexpr 的所有分支都有返回值
                static_assert(Detail::kAlwaysFalse<ValueType>,
                              "SchemaMigrator：不支持的列类型。仅支持整型、bool、浮点、std::string、"
                              "二进制载荷（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                              "以及它们的 std::optional 包装");
                return ColumnType::Text;
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
         */
        template<typename ColumnDescriptorType>
        static void appendColumnDefinition(std::string &columnDefinitions,
                                           bool &primaryKeyDeclared,
                                           const SqlDialect &dialect,
                                           const ColumnDescriptorType &columnDescriptor,
                                           const std::string_view primaryKeyName)
        {
            using MemberType = typename ColumnDescriptorType::MemberType;
            using BareType   = std::remove_cv_t<MemberType>;

            // 可空规则：std::optional<X> 允许 NULL，其余一律 NOT NULL。
            // optional 只是可空标记而不是存储类型，去掉包装后的类型才是真正的列类型
            constexpr bool isNullable = Detail::IsOptional<BareType>::value;
            using ValueType = Detail::ColumnStorageTypeOf<BareType>;

            // 分隔符前置而不是后置：后置会在最后一列留下一个尾逗号，还得在拼接处再裁一次
            if (!columnDefinitions.empty())
            {
                columnDefinitions += ", ";
            }

            // 列名一律引用：含空格、保留字或引用字符的列名只有被引用才能作为标识符出现
            columnDefinitions += dialect.quoteIdentifier(columnDescriptor.columnName);
            columnDefinitions += ' ';
            columnDefinitions += dialect.columnTypeName(columnTypeOf<ValueType>());

            if constexpr (!isNullable)
            {
                columnDefinitions += " NOT NULL";
            }

            if (columnDescriptor.columnName == primaryKeyName)
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
        [[nodiscard]] static std::shared_ptr<SqlDialect> resolveDialect(ConnectionPool &pool,
                                                                              std::string *errorText)
        {
            PooledConnection probeConnection = pool.acquire();
            if (!probeConnection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败，无法推导数据库类型"
                                      "（池已达上限或连接创建失败）");
                return nullptr;
            }

            try
            {
                // 未实现的类型由注册表抛带中文提示的异常，这里转成返回值 + 原因文本
                return DialectRegistry::dialectFor(probeConnection->databaseType());
            }
            catch (const std::exception &error)
            {
                writeError(errorText, std::string("SchemaMigrator: 该数据库类型尚无方言实现：") + error.what());
                return nullptr;
            }
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
        [[nodiscard]] static bool executeStatement(ConnectionPool &pool,
                                                         const SqlStatement &statement,
                                                         std::string *errorText)
        {
            PooledConnection connection = pool.acquire();
            if (!connection)
            {
                writeError(errorText, "SchemaMigrator: 从连接池获取连接失败（池已达上限或连接创建失败）");
                return false;
            }

            // 连接在语句执行完、结果集销毁之后才归还：结果集持有连接句柄的非拥有指针（见 SqliteConnection）
            std::unique_ptr<DatabaseResult> result =
                connection->execute(std::string_view{statement.sql}, statement.parameters);
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
