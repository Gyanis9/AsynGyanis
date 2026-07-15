/**
 * @file SqliteConnection.cpp
 * @brief SQLite 连接实现
 * @copyright Copyright (c) 2026
 */

#include "SqliteConnection.h"
#include "SqliteResult.h"

#include <sqlite3.h>

namespace Database
{

    SqliteConnection::SqliteConnection(const ConnectionConfig &config)
    {
        m_configuration = config;
    }

    SqliteConnection::~SqliteConnection()
    {
        disconnect();
    }

    bool SqliteConnection::connect()
    {
        if (m_isConnected) return true;

        const char *dbPath = m_configuration.database.empty()
                                 ? ":memory:"
                                 : m_configuration.database.c_str();
        if (sqlite3_open(dbPath, &m_database) != SQLITE_OK)
        {
            captureError();
            return false;
        }
        sqlite3_exec(m_database, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        sqlite3_exec(m_database, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

        m_isConnected = true;
        m_lastError.clear();
        return true;
    }

    void SqliteConnection::disconnect()
    {
        if (!m_isConnected) return;
        if (m_database) { sqlite3_close(m_database); m_database = nullptr; }
        m_isConnected = false;
    }

    bool SqliteConnection::isConnected() const
    {
        return m_isConnected && m_database != nullptr;
    }

    std::unique_ptr<DatabaseResult> SqliteConnection::execute(const std::string_view command)
    {
        if (!m_isConnected || m_database == nullptr)
        {
            m_lastError = "未连接到数据库";
            return nullptr;
        }
        if (command.empty())
        {
            m_lastError = "SQL 命令为空";
            return nullptr;
        }

        // 使用 sqlite3_prepare_v2 + sqlite3_step 统一处理查询和写操作
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, command.data(),
                               static_cast<int>(command.size()),
                               &statement, nullptr) != SQLITE_OK)
        {
            captureError();
            return nullptr;
        }

        // 判断语句类型：有返回列的是查询，否则是写操作
        const int columnCount = sqlite3_column_count(statement);
        if (columnCount == 0)
        {
            // 写操作（INSERT/UPDATE/DELETE/CREATE 等）：直接 step 执行
            const int stepResult = sqlite3_step(statement);
            sqlite3_finalize(statement);
            if (stepResult != SQLITE_DONE && stepResult != SQLITE_ROW)
            {
                captureError();
                return nullptr;
            }
            // 返回空结果集表示执行成功
            return std::make_unique<SqliteResult>(nullptr, m_database);
        }

        // 查询操作（SELECT 等）：返回包含预编译语句的结果集
        return std::make_unique<SqliteResult>(statement, m_database);
    }

    DatabaseType SqliteConnection::databaseType() const
    {
        return DatabaseType::Sqlite;
    }

    std::string SqliteConnection::lastError() const
    {
        return m_lastError;
    }

    bool SqliteConnection::beginTransaction()
    {
        return execute("BEGIN TRANSACTION") != nullptr;
    }

    bool SqliteConnection::commit()
    {
        return execute("COMMIT") != nullptr;
    }

    bool SqliteConnection::rollback()
    {
        return execute("ROLLBACK") != nullptr;
    }

    std::string SqliteConnection::serverVersion() const
    {
        return m_isConnected ? sqlite3_libversion() : "";
    }

    int64_t SqliteConnection::lastInsertRowId() const
    {
        return m_database ? sqlite3_last_insert_rowid(m_database) : 0;
    }

    void SqliteConnection::captureError()
    {
        m_lastError = m_database ? sqlite3_errmsg(m_database) : "SQLite 连接未初始化";
    }

} // namespace Database
