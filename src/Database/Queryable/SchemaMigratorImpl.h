/**
 * @file SchemaMigratorImpl.h
 * @brief SchemaMigrator 模板方法的实现
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 由 SchemaMigrator.h 在类定义之后包含；只放实现，文档留在类内声明处。
 */

#pragma once

namespace AsynGyanis::Database::Queryable
{
    template<RowMappable T>
    inline SqlStatement SchemaMigrator::createTableStatement(const SqlDialect &dialect, const bool ifNotExists)
    {
        // 列类型不受支持时给出中文编译错误，而不是让模板在深处爆出一长串实例化回溯；
        // 判定与 RowMapper 共用同一份 trait，因此「能建表」与「能读写」永远等价
        static_assert(Detail::allColumnTypesSupported<T>(), "SchemaMigrator：TableSchema<T>::kColumns 中存在不支持的列类型。"
                                                            "仅支持整型、bool、浮点、std::string、精确十进制（Decimal）、"
                                                            "日期时间（Timestamp / LocalDateTime）、二进制载荷"
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

    template<RowMappable T>
    inline SqlStatement SchemaMigrator::dropTableStatement(const SqlDialect &dialect, const bool ifExists)
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

    template<RowMappable T>
    inline bool SchemaMigrator::createTable(ConnectionPool &pool, const bool ifNotExists, std::string *errorText)
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

    template<RowMappable T>
    inline bool SchemaMigrator::dropTable(ConnectionPool &pool, const bool ifExists, std::string *errorText)
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

    template<RowMappable T>
    inline bool SchemaMigrator::tableExists(ConnectionPool &pool, std::string *errorText)
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

    template<RowMappable T>
    inline SqlStatement SchemaMigrator::addColumnStatement(const SqlDialect &dialect, const std::string_view columnName)
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

    template<RowMappable T>
    inline bool SchemaMigrator::addMissingColumns(ConnectionPool &pool, std::size_t *addedCount, std::string *errorText)
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

        const bool isCaseSensitiveDialect = dialect->type() == DatabaseType::PostgreSql;

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
                existingColumns.push_back(isCaseSensitiveDialect ? *existingName : toLowerAscii(*existingName));
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
                                const std::string comparableName = isCaseSensitiveDialect ? columnName : toLowerAscii(columnName);
                                if (std::find(existingColumns.begin(), existingColumns.end(), comparableName) != existingColumns.end())
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

    [[nodiscard]] inline std::string SchemaMigrator::checksumOfStep(const MigrationStep &step)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        const auto    mix  = [&hash](const std::string_view text)
        {
            for (const char character: text)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ULL;
            }
            // 分隔符避免 "ab"+"c" 与 "a"+"bc" 撞成同一个值
            hash ^= 0xFFU;
            hash *= 1099511628211ULL;
        };
        mix(step.name);
        for (const SqlStatement &statement: step.upStatements)
        {
            mix(statement.sql);
        }

        std::string text(16, '0');
        for (std::size_t index = text.size(); index > 0; --index)
        {
            text[index - 1U] = "0123456789abcdef"[hash & 0xFU];
            hash >>= 4U;
        }
        return text;
    }

    [[nodiscard]] inline std::shared_ptr<SqlDialect> SchemaMigrator::dialectOf(ConnectionPool &pool, std::string *errorText)
    {
        clearError(errorText);
        return resolveDialect(pool, errorText);
    }

    [[nodiscard]] inline bool SchemaMigrator::ensureMigrationLedger(ConnectionPool &pool, std::string *errorText)
    {
        return createTable<MigrationLedgerRow>(pool, true, errorText);
    }

    [[nodiscard]] inline bool SchemaMigrator::appliedMigrations(ConnectionPool &pool, std::vector<std::string> *appliedNames, std::string *errorText)
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

        const std::string listingSql                  = "SELECT " + dialect->quoteIdentifier(kMigrationNameColumn) + " FROM " + dialect->quoteIdentifier(kMigrationLedgerTableName);
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

    [[nodiscard]] inline bool SchemaMigrator::executeStepStatements(const SqlDialect &dialect, DatabaseConnection &connection, const MigrationStep &step, const bool isRollingBack,
                                                                    std::string *errorText)
    {
        const std::span<const SqlStatement> statements = isRollingBack ? std::span<const SqlStatement>(step.downStatements) : std::span<const SqlStatement>(step.upStatements);
        const std::string_view              phase      = isRollingBack ? std::string_view{"回退"} : std::string_view{"应用"};
        for (std::size_t index = 0; index < statements.size(); ++index)
        {
            const SqlStatement &statement = statements[index];
            if (statement.sql.empty())
            {
                continue; // 空语句＝这一步只是版本标记，没有结构改动
            }
            if (connection.execute(std::string_view{statement.sql}, statement.parameters) == nullptr)
            {
                if (errorText != nullptr)
                {
                    *errorText = "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的第 " + std::to_string(index + 1) + " 条语句失败（本次已回滚）：" +
                                 connection.lastError();
                }
                return false;
            }
        }

        // 占位符由方言给：PostgreSQL 的扩展查询协议只认 $1，写死 "?" 会被服务端以
        // 42601（syntax error at or near ")"）拒绝——三种引擎里只有它会这样
        std::string ledgerSql;
        if (isRollingBack)
        {
            ledgerSql = "DELETE FROM " + dialect.quoteIdentifier(kMigrationLedgerTableName) + " WHERE " + dialect.quoteIdentifier(kMigrationNameColumn) + " = ";
            dialect.appendPlaceholder(ledgerSql, 0);
        } else
        {
            ledgerSql = "INSERT INTO " + dialect.quoteIdentifier(kMigrationLedgerTableName) + " (" + dialect.quoteIdentifier(kMigrationNameColumn) + ") VALUES (";
            dialect.appendPlaceholder(ledgerSql, 0);
            ledgerSql += ')';
        }

        const std::vector<DatabaseValue> ledgerParameters{DatabaseValue(step.name)};
        if (connection.execute(std::string_view{ledgerSql}, std::span<const DatabaseValue>(ledgerParameters)) == nullptr)
        {
            if (errorText != nullptr)
            {
                *errorText = "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的记账失败（本次已回滚）：" + connection.lastError();
            }
            return false;
        }

        // 校验和与记录同生共死：应用时写入首次内容摘要，回退时连同记录一起删
        std::string checksumSql;
        if (isRollingBack)
        {
            checksumSql = "DELETE FROM " + dialect.quoteIdentifier(kMigrationChecksumTableName) + " WHERE " + dialect.quoteIdentifier(kMigrationNameColumn) + " = ";
            dialect.appendPlaceholder(checksumSql, 0);
        } else
        {
            checksumSql = "INSERT INTO " + dialect.quoteIdentifier(kMigrationChecksumTableName) + " (" + dialect.quoteIdentifier(kMigrationNameColumn) + ", " +
                          dialect.quoteIdentifier(kMigrationChecksumValueColumn) + ") VALUES (";
            dialect.appendPlaceholder(checksumSql, 0);
            checksumSql += ", ";
            dialect.appendPlaceholder(checksumSql, 1);
            checksumSql += ')';
        }
        const std::vector<DatabaseValue> checksumParameters =
                isRollingBack ? std::vector<DatabaseValue>{DatabaseValue(step.name)} : std::vector<DatabaseValue>{DatabaseValue(step.name), DatabaseValue(checksumOfStep(step))};
        if (connection.execute(std::string_view{checksumSql}, std::span<const DatabaseValue>(checksumParameters)) == nullptr)
        {
            if (errorText != nullptr)
            {
                *errorText = "SchemaMigrator: " + std::string(phase) + "迁移「" + step.name + "」的校验和记账失败（本次已回滚）：" + connection.lastError();
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] inline bool SchemaMigrator::plan(ConnectionPool &pool, const std::span<const MigrationStep> steps, std::vector<std::string> *pendingNames, std::string *errorText)
    {
        clearError(errorText);
        if (pendingNames == nullptr)
        {
            writeError(errorText, "SchemaMigrator: plan() 的 pendingNames 出参为空");
            return false;
        }
        pendingNames->clear();
        requireDistinctStepNames(steps);

        const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
        if (dialect == nullptr)
        {
            return false;
        }

        std::string existenceError;
        if (!tableExists<MigrationLedgerRow>(pool, &existenceError))
        {
            if (!existenceError.empty())
            {
                writeError(errorText, std::move(existenceError));
                return false;
            }
            for (const MigrationStep &step: steps)
            {
                pendingNames->push_back(step.name);
            }
            return true;
        }

        // 伴生表的存在性要在借出下面那条连接之前问：tableExists() 自己也要借连接，
        // 池上限为 1 时握着连接再问就是把自己排在自己后面
        std::string checksumTableError;
        const bool  hasChecksumTable = tableExists<MigrationLedger::ChecksumRow>(pool, &checksumTableError);
        if (!hasChecksumTable && !checksumTableError.empty())
        {
            writeError(errorText, std::move(checksumTableError));
            return false;
        }

        const PooledConnection connection = pool.acquire();
        if (!connection)
        {
            writeError(errorText, "SchemaMigrator: plan() 从连接池获取连接失败");
            return false;
        }

        const std::string listingSql                  = "SELECT " + dialect->quoteIdentifier(kMigrationNameColumn) + " FROM " + dialect->quoteIdentifier(kMigrationLedgerTableName);
        const std::unique_ptr<DatabaseResult> listing = connection->execute(std::string_view{listingSql});
        if (listing == nullptr)
        {
            writeError(errorText, "SchemaMigrator: plan() 读迁移记录失败：" + connection->lastError());
            return false;
        }

        std::vector<std::string>                     appliedNames;
        std::unordered_map<std::string, std::string> appliedChecksums;
        while (listing->next())
        {
            const DatabaseValue value = listing->getValue(std::size_t{0});
            if (const auto *nameText = std::get_if<std::string>(&value); nameText != nullptr)
            {
                appliedNames.push_back(*nameText);
                continue;
            }
            writeError(errorText, "SchemaMigrator: plan() 读到的迁移名不是文本");
            return false;
        }

        if (!isAppliedPrefixOfSteps(appliedNames, steps, errorText))
        {
            return false;
        }

        if (hasChecksumTable)
        {
            const std::string checksumSql = "SELECT " + dialect->quoteIdentifier(kMigrationNameColumn) + ", " + dialect->quoteIdentifier(kMigrationChecksumValueColumn) + " FROM " +
                                            dialect->quoteIdentifier(kMigrationChecksumTableName);
            const std::unique_ptr<DatabaseResult> checksumListing = connection->execute(std::string_view{checksumSql});
            if (checksumListing == nullptr)
            {
                writeError(errorText, "SchemaMigrator: plan() 读迁移校验和失败：" + connection->lastError());
                return false;
            }
            while (checksumListing->next())
            {
                const DatabaseValue nameValue     = checksumListing->getValue(std::size_t{0});
                const DatabaseValue checksumValue = checksumListing->getValue(std::size_t{1});
                const auto         *nameText      = std::get_if<std::string>(&nameValue);
                const auto         *checksumText  = std::get_if<std::string>(&checksumValue);
                if (nameText == nullptr || checksumText == nullptr)
                {
                    writeError(errorText, "SchemaMigrator: plan() 读到的迁移校验和不是文本");
                    return false;
                }
                appliedChecksums.insert_or_assign(*nameText, *checksumText);
            }
        }

        const std::unordered_set<std::string> appliedSet(appliedNames.begin(), appliedNames.end());
        for (const MigrationStep &step: steps)
        {
            if (!appliedSet.contains(step.name))
            {
                pendingNames->push_back(step.name);
                continue;
            }
            const auto stored = appliedChecksums.find(step.name);
            if (stored != appliedChecksums.end() && stored->second != checksumOfStep(step))
            {
                writeError(errorText, "SchemaMigrator: 迁移「" + step.name + "」的内容与台账记录的校验和不一致（台账 " + stored->second + "，当前 " + checksumOfStep(step) +
                                              "）。已应用的迁移不应修改；请新增一条迁移来承载这次变更");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool SchemaMigrator::migrate(ConnectionPool &pool, const std::span<const MigrationStep> steps, std::size_t *appliedCount, std::string *errorText)
    {
        return migrate(pool, steps, appliedCount, errorText, MigrationLockOptions{});
    }

    [[nodiscard]] inline bool SchemaMigrator::migrate(ConnectionPool &pool, const std::span<const MigrationStep> steps, std::size_t *appliedCount, std::string *errorText,
                                                      const MigrationLockOptions &lockOptions)
    {
        clearError(errorText);
        if (appliedCount != nullptr)
        {
            *appliedCount = 0;
        }

        requireDistinctStepNames(steps);

        // 方言必须在借出长占连接之前取：resolveDialect() 自己也要借一条连接，
        // 池上限为 1 时它必须在那条连接被长占之前完成（否则死等借用超时）
        const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
        if (dialect == nullptr)
        {
            return false;
        }

        // 取锁到释放全程占同一条连接：会话级咨询锁与跑迁移的连接必须是同一条
        MigrationSession session(pool, dialect, lockOptions, errorText);
        if (!session.isReady())
        {
            return false;
        }

        if (!session.ensureLedger(errorText))
        {
            return false;
        }

        // 已应用清单必须在拿到锁之后读：先读后锁的窗口里，另一个实例可能刚好应用了同一批步骤
        std::vector<std::string> appliedNames;
        if (!session.readAppliedMigrations(&appliedNames, errorText))
        {
            return false;
        }

        if (!isAppliedPrefixOfSteps(appliedNames, steps, errorText))
        {
            return false;
        }

        // 已应用的每一步都要与台账里的内容校验和对上：改了已发布迁移的 SQL 却想悄悄重跑，
        // 在这里被拦下（旧部署没有校验和记录时，按当前内容补一份基线，不追溯判断漂移）
        std::unordered_map<std::string, std::string> appliedChecksums;
        if (!session.readAppliedChecksums(&appliedChecksums, errorText))
        {
            return false;
        }
        const std::unordered_set<std::string> appliedSet(appliedNames.begin(), appliedNames.end());
        for (const MigrationStep &step: steps)
        {
            if (!appliedSet.contains(step.name))
            {
                continue;
            }
            const std::string expectedChecksum = checksumOfStep(step);
            const auto        stored           = appliedChecksums.find(step.name);
            if (stored == appliedChecksums.end())
            {
                if (!session.backfillChecksum(step, errorText))
                {
                    return false;
                }
            } else if (stored->second != expectedChecksum)
            {
                writeError(errorText, "SchemaMigrator: 迁移「" + step.name + "」的内容与台账记录的校验和不一致（台账 " + stored->second + "，当前 " + expectedChecksum +
                                              "）。已应用的迁移不应修改；请新增一条迁移来承载这次变更");
                return false;
            }
        }

        for (const MigrationStep &step: steps)
        {
            if (appliedSet.contains(step.name))
            {
                continue;
            }

            if (!session.runStep(step, /*isRollingBack*/ false, errorText))
            {
                return false;
            }
            if (appliedCount != nullptr)
            {
                ++(*appliedCount);
            }
        }
        return session.finish(errorText);
    }

    [[nodiscard]] inline bool SchemaMigrator::rollbackLast(ConnectionPool &pool, const std::span<const MigrationStep> steps, const std::size_t stepCount,
                                                           std::size_t *rolledBackCount, std::string *errorText)
    {
        return rollbackLast(pool, steps, stepCount, rolledBackCount, errorText, MigrationLockOptions{});
    }

    [[nodiscard]] inline bool SchemaMigrator::rollbackLast(ConnectionPool &pool, const std::span<const MigrationStep> steps, const std::size_t stepCount,
                                                           std::size_t *rolledBackCount, std::string *errorText, const MigrationLockOptions &lockOptions)
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

        // 方言在借出长占连接之前取（理由同 migrate()）
        const std::shared_ptr<SqlDialect> dialect = resolveDialect(pool, errorText);
        if (dialect == nullptr)
        {
            return false;
        }

        MigrationSession session(pool, dialect, lockOptions, errorText);
        if (!session.isReady())
        {
            return false;
        }

        if (!session.ensureLedger(errorText))
        {
            return false;
        }

        std::vector<std::string> appliedNames;
        if (!session.readAppliedMigrations(&appliedNames, errorText))
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

        // 逆序撤：先退最后应用的那一条，退回的结构因此按相反顺序合上
        for (std::size_t index = 0; index < stepCount; ++index)
        {
            const std::size_t position = appliedNames.size() - 1 - index;
            if (!session.runStep(steps[position], /*isRollingBack*/ true, errorText))
            {
                return false;
            }
            if (rolledBackCount != nullptr)
            {
                ++(*rolledBackCount);
            }
        }
        return session.finish(errorText);
    }

    template<typename RowType>
    inline bool SchemaMigrator::seed(ConnectionPool &pool, const std::span<const RowType> rows, std::size_t *insertedCount, std::string *errorText)
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

    template<typename RowType>
    inline bool SchemaMigrator::seed(ConnectionPool &pool, const std::vector<RowType> &rows, std::size_t *insertedCount, std::string *errorText)
    {
        return seed<RowType>(pool, std::span<const RowType>{rows}, insertedCount, errorText);
    }

    [[nodiscard]] inline std::string SchemaMigrator::toLowerAscii(const std::string_view text)
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

    template<typename ValueType>
    inline constexpr ColumnType SchemaMigrator::columnTypeOf() noexcept
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
        } else if constexpr (std::is_same_v<BareType, AsynGyanis::Database::Decimal>)
        {
            return ColumnType::Decimal;
        } else if constexpr (AsynGyanis::Database::Queryable::Detail::IsSystemClockTime<BareType>::value)
        {
            return ColumnType::Timestamp;
        } else if constexpr (AsynGyanis::Database::Queryable::Detail::IsLocalClockTime<BareType>::value)
        {
            return ColumnType::LocalDateTime;
        } else if constexpr (AsynGyanis::Database::Detail::kIsBinaryBytes<BareType>)
        {
            // 两种成员拼法都落到 Blob：DDL 只关心「这一列是二进制」，
            // 由 value_type 是 uint8_t 还是 std::byte 决定的差异在值映射处已经归一
            return ColumnType::Blob;
        } else
        {
            // 不受支持的类型已被 createTableStatement() 的 static_assert 拦住，
            // 这里只是让 if constexpr 的所有分支都有返回值
            static_assert(Detail::kAlwaysFalse<ValueType>, "SchemaMigrator：不支持的列类型。仅支持整型、bool、浮点、std::string、精确十进制（Decimal）、"
                                                           "日期时间（Timestamp / LocalDateTime）、"
                                                           "二进制载荷（std::vector<std::uint8_t> 或 std::vector<std::byte>），"
                                                           "以及它们的 std::optional 包装");
            return ColumnType::Text;
        }
    }

    inline void SchemaMigrator::requireMigratableTableName(const std::string_view tableName)
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

    template<typename ColumnDescriptorType>
    inline void SchemaMigrator::appendColumnDefinition(std::string &columnDefinitions, bool &primaryKeyDeclared, const SqlDialect &dialect,
                                                       const ColumnDescriptorType &columnDescriptor, const std::string_view primaryKeyName, const bool isAutoIncrementPrimaryKey,
                                                       const std::string_view tableName)
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
            const std::string autoIncrementDefinition = dialect.autoIncrementPrimaryKeyDefinition(dialect.quoteIdentifier(columnDescriptor.columnName), columnTypeOf<ValueType>());
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

    [[nodiscard]] inline std::shared_ptr<SqlDialect> SchemaMigrator::resolveDialect(ConnectionPool &pool, std::string *errorText)
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

    inline void SchemaMigrator::requireDistinctStepNames(const std::span<const MigrationStep> steps)
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

    [[nodiscard]] inline bool SchemaMigrator::isAppliedPrefixOfSteps(const std::vector<std::string> &appliedNames, const std::span<const MigrationStep> steps,
                                                                     std::string *errorText)
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

    [[nodiscard]] inline bool SchemaMigrator::executeStatement(ConnectionPool &pool, const SqlStatement &statement, std::string *errorText)
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

    inline void SchemaMigrator::clearError(std::string *errorText) noexcept
    {
        if (errorText != nullptr)
        {
            errorText->clear();
        }
    }

    inline void SchemaMigrator::writeError(std::string *errorText, std::string reason)
    {
        if (errorText != nullptr)
        {
            *errorText = std::move(reason);
        }
    }

} // namespace AsynGyanis::Database::Queryable
