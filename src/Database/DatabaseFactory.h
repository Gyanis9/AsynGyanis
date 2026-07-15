/**
 * @file DatabaseFactory.h
 * @brief 数据库连接工厂，根据配置创建对应的数据库连接实例
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_DATABASEFACTORY_H
#define DATABASE_DATABASEFACTORY_H

#include "DatabaseConnection.h"
#include "DatabaseType.h"

#include <memory>
#include <stdexcept>
#include <string>

// 前向声明各数据库实现类
namespace Database
{
    class MySqlConnection;
    class RedisConnection;
    class SqliteConnection;

    /**
     * @brief 数据库连接工厂
     *
     * 根据配置或数据库类型创建对应的连接实例，隐藏具体实现细节。
     * 调用者只需依赖 DatabaseConnection 抽象接口，无需知道底层是 MySQL 还是 Redis。
     *
     * @code
     *   auto config  = ConnectionConfig::mySqlDefault();
     *   config.userName = "admin";
     *   config.password = "secret";
     *   auto connection = DatabaseFactory::create(config);
     *   if (connection->connect()) {
     *       auto result = connection->execute("SELECT VERSION()");
     *   }
     * @endcode
     */
    class DatabaseFactory
    {
    public:
        /**
         * @brief 根据连接配置自动判断类型并创建连接
         * @param config 连接配置（需包含有效的端口等信息）
         * @return 数据库连接智能指针
         * @throws std::invalid_argument 配置无效或不支持的数据库类型
         */
        static std::unique_ptr<DatabaseConnection> create(const ConnectionConfig &config);

        /**
         * @brief 根据指定类型创建连接
         * @param type   数据库类型
         * @param config 连接配置
         * @return 数据库连接智能指针
         */
        static std::unique_ptr<DatabaseConnection> create(
            DatabaseType type, const ConnectionConfig &config);

        /**
         * @brief 创建 MySQL 连接
         * @param config 连接配置
         * @return MySQL 连接智能指针
         */
        static std::unique_ptr<DatabaseConnection> createMySql(
            const ConnectionConfig &config);

        /**
         * @brief 创建 Redis 连接
         * @param config 连接配置
         * @return Redis 连接智能指针
         */
        static std::unique_ptr<DatabaseConnection> createRedis(
            const ConnectionConfig &config);

        /**
         * @brief 创建 SQLite 连接
         * @param config 连接配置（config.database 为文件路径或 ":memory:"）
         * @return SQLite 连接智能指针
         */
        static std::unique_ptr<DatabaseConnection> createSqlite(
            const ConnectionConfig &config);

        /**
         * @brief 根据端口号推断数据库类型
         * @param port 端口号
         * @return 推断的数据库类型，无法推断时返回 MySql
         */
        static DatabaseType guessType(uint16_t port);
    };

} // namespace Database

#endif // DATABASE_DATABASEFACTORY_H
