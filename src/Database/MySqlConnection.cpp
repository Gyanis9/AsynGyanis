/**
 * @file MySqlConnection.cpp
 * @brief MySQL 连接实现
 * @copyright Copyright (c) 2026
 */

#include "MySqlConnection.h"
#include "MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL
#include <mysql/mysql.h>
#include <mysql/errmsg.h>
#endif

namespace Database
{

#ifdef DATABASE_HAS_MYSQL

    MySqlConnection::MySqlConnection(const ConnectionConfig &config)
    {
        m_configuration = config;
        m_mysqlPointer = mysql_init(nullptr);
        if (m_mysqlPointer)
        {
            mysql_options(m_mysqlPointer, MYSQL_OPT_CONNECT_TIMEOUT, &m_connectTimeout);
            mysql_options(m_mysqlPointer, MYSQL_OPT_READ_TIMEOUT, &m_queryTimeout);
        }
    }

    MySqlConnection::~MySqlConnection() { disconnect(); }

    bool MySqlConnection::connect()
    {
        if (m_isConnected) return true;
        if (!m_mysqlPointer) m_mysqlPointer = mysql_init(nullptr);
        if (!m_mysqlPointer) { m_lastError = " MySQL 初始化失败"; return false; }

        if (!mysql_real_connect(m_mysqlPointer,
                m_configuration.host.c_str(), m_configuration.userName.c_str(),
                m_configuration.password.c_str(), m_configuration.database.c_str(),
                m_configuration.port, nullptr, 0))
        { captureError(); return false; }

        mysql_set_character_set(m_mysqlPointer, "utf8mb4");
        m_isConnected = true; m_lastError.clear();
        return true;
    }

    void MySqlConnection::disconnect()
    {
        if (!m_isConnected) return;
        if (m_mysqlPointer) { mysql_close(m_mysqlPointer); m_mysqlPointer = nullptr; }
        m_isConnected = false;
    }

    bool MySqlConnection::isConnected() const
    {
        return m_isConnected && m_mysqlPointer && mysql_ping(m_mysqlPointer) == 0;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::execute(std::string_view command)
    {
        if (!m_isConnected || !m_mysqlPointer)
        { m_lastError = "未连接到数据库"; return nullptr; }
        if (mysql_real_query(m_mysqlPointer, command.data(), command.size()) != 0)
        { captureError(); return nullptr; }

        MYSQL_RES *resultPointer = mysql_store_result(m_mysqlPointer);
        if (!resultPointer)
        {
            if (mysql_field_count(m_mysqlPointer) == 0)
                return std::make_unique<MySqlResult>(nullptr, m_mysqlPointer);
            captureError(); return nullptr;
        }
        return std::make_unique<MySqlResult>(resultPointer, m_mysqlPointer);
    }

    DatabaseType MySqlConnection::databaseType() const { return DatabaseType::MySql; }
    std::string MySqlConnection::lastError() const { return m_lastError; }
    bool MySqlConnection::beginTransaction() { return execute("START TRANSACTION") != nullptr; }
    bool MySqlConnection::commit() { return execute("COMMIT") != nullptr; }
    bool MySqlConnection::rollback() { return execute("ROLLBACK") != nullptr; }

    std::string MySqlConnection::serverVersion() const
    {
        return (m_isConnected && m_mysqlPointer) ? mysql_get_server_info(m_mysqlPointer) : "";
    }

    void MySqlConnection::captureError()
    {
        m_lastError = m_mysqlPointer ? mysql_error(m_mysqlPointer) : "MySQL 连接未初始化";
    }

#else // DATABASE_HAS_MYSQL — 桩实现

    MySqlConnection::MySqlConnection(const ConnectionConfig &config)
    { m_configuration = config; m_lastError = "MySQL 支持未编译（缺少 libmysqlclient）"; }
    MySqlConnection::~MySqlConnection() = default;
    bool MySqlConnection::connect() { return false; }
    void MySqlConnection::disconnect() { m_isConnected = false; }
    bool MySqlConnection::isConnected() const { return false; }
    std::unique_ptr<DatabaseResult> MySqlConnection::execute(std::string_view)
    { return nullptr; }
    DatabaseType MySqlConnection::databaseType() const { return DatabaseType::MySql; }
    std::string MySqlConnection::lastError() const { return m_lastError; }
    bool MySqlConnection::beginTransaction() { return false; }
    bool MySqlConnection::commit() { return false; }
    bool MySqlConnection::rollback() { return false; }
    std::string MySqlConnection::serverVersion() const { return {}; }
    void MySqlConnection::captureError() {}

#endif

} // namespace Database
