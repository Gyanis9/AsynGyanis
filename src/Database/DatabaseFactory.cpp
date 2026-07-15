/**
 * @file DatabaseFactory.cpp
 * @brief 数据库工厂实现
 * @copyright Copyright (c) 2026
 */

#include "DatabaseFactory.h"
#include "MySqlConnection.h"
#include "RedisConnection.h"
#include "SqliteConnection.h"

#include <stdexcept>

namespace Database
{

    std::unique_ptr<DatabaseConnection> DatabaseFactory::create(
        const ConnectionConfig &config)
    {
        const auto type = guessType(config.port);
        return create(type, config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::create(
        const DatabaseType type, const ConnectionConfig &config)
    {
        switch (type)
        {
            case DatabaseType::MySql:
                return createMySql(config);
            case DatabaseType::Redis:
                return createRedis(config);
            case DatabaseType::Sqlite:
                return createSqlite(config);
            case DatabaseType::PostgreSql:
                throw std::invalid_argument(
                    std::string(databaseTypeName(type)) + " 数据库尚未实现");
        }
        throw std::invalid_argument("不支持的数据库类型");
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createMySql(
        const ConnectionConfig &config)
    {
        return std::make_unique<MySqlConnection>(config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createRedis(
        const ConnectionConfig &config)
    {
        return std::make_unique<RedisConnection>(config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createSqlite(
        const ConnectionConfig &config)
    {
        return std::make_unique<SqliteConnection>(config);
    }

    DatabaseType DatabaseFactory::guessType(const uint16_t port)
    {
        // 根据常见默认端口推断数据库类型
        switch (port)
        {
            case 3306:
                return DatabaseType::MySql;
            case 6379:
                return DatabaseType::Redis;
            case 5432:
                return DatabaseType::PostgreSql;
            default:
                // 默认假设为 MySQL
                return DatabaseType::MySql;
        }
    }

} // namespace Database
