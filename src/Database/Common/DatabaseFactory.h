/**
 * @file DatabaseFactory.h
 * @brief 数据库连接工厂，根据配置或类型创建具体驱动实例
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseType.h"

#include <memory>
#include <optional>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库连接工厂
     *
     * @details 按类型或配置创建具体驱动实例，调用方只依赖 DatabaseConnection 接口。
     *          工厂只负责挑选驱动，不做连接：返回的连接仍需调用方 connect()。
     *
     * @code
     *   auto connection = DatabaseFactory::create(ConnectionConfig::sqliteDefault());
     *   if (connection->connect())
     *   {
     *       auto result = connection->execute("SELECT 1");
     *   }
     * @endcode
     */
    class DatabaseFactory
    {
    public:
        DatabaseFactory() = delete;

        /**
         * @brief 依据配置推断类型并创建连接
         * @param config 连接配置
         * @return std::unique_ptr<DatabaseConnection> 对应驱动的连接实例
         * @throws std::invalid_argument 端口无法判定类型且配置不足以推断为 SQLite
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> create(const ConnectionConfig &config);

        /**
         * @brief 按指定类型创建连接
         * @param type 数据库类型
         * @param config 连接配置
         * @return std::unique_ptr<DatabaseConnection> 对应驱动的连接实例
         * @throws std::invalid_argument 类型不可用
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> create(DatabaseType type, const ConnectionConfig &config);

        /**
         * @brief 创建 MySQL 连接
         * @param config 连接配置，读取 host/port/userName/password/database
         * @return std::unique_ptr<DatabaseConnection> MySQL 驱动实例
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> createMySql(const ConnectionConfig &config);

        /**
         * @brief 创建 PostgreSQL 连接
         * @param config 连接配置，读取 host/port/userName/password/database
         * @return std::unique_ptr<DatabaseConnection> PostgreSQL 驱动实例
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> createPostgres(const ConnectionConfig &config);

        /**
         * @brief 创建 Redis 连接
         * @param config 连接配置，读取 host/port/password
         * @return std::unique_ptr<DatabaseConnection> Redis 驱动实例
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> createRedis(const ConnectionConfig &config);

        /**
         * @brief 创建 SQLite 连接
         * @param config 连接配置，config.database 为文件路径或 ":memory:"
         * @return std::unique_ptr<DatabaseConnection> SQLite 驱动实例
         */
        [[nodiscard]] static std::unique_ptr<DatabaseConnection> createSqlite(const ConnectionConfig &config);

        /**
         * @brief 按默认端口推断数据库类型
         * @details 只认识本框架已实现的引擎默认端口：3306 → MySQL，5432 → PostgreSQL，
         *          6379 → Redis。旧实现对任意未知端口都回退成 MySQL，会把 SQLite 之类误判成
         *          永远连不上的 MySQL，故改为返回空值，由调用方显式决定类型。
         * @param port 端口号
         * @return std::optional<DatabaseType> 推断结果；无法判定时为空
         */
        [[nodiscard]] static std::optional<DatabaseType> guessType(std::uint16_t port) noexcept;
    };

} // namespace AsynGyanis::Database
