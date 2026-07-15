/**
 * @file DatabaseType.h
 * @brief 数据库类型枚举与通用定义
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_DATABASETYPE_H
#define DATABASE_DATABASETYPE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Database
{
    /**
     * @brief 支持的数据库类型枚举
     */
    enum class DatabaseType
    {
        MySql,   ///< MySQL / MariaDB
        Redis,   ///< Redis
        Sqlite,  ///< SQLite
        PostgreSql ///< PostgreSQL
    };

    /**
     * @brief 将数据库类型转换为字符串
     * @param type 数据库类型枚举值
     * @return 类型名称字符串
     */
    inline const char *databaseTypeName(const DatabaseType type)
    {
        switch (type)
        {
            case DatabaseType::MySql:
                return "MySql";
            case DatabaseType::Redis:
                return "Redis";
            case DatabaseType::Sqlite:
                return "Sqlite";
            case DatabaseType::PostgreSql:
                return "PostgreSql";
        }
        return "Unknown";
    }

    /**
     * @brief 数据库统一值类型
     *
     * 使用 std::variant 封装各种数据库返回的数据类型，屏蔽不同数据库的方言差异。
     * 支持的存储类型：
     * - std::monostate：空值（NULL）
     * - bool：布尔值
     * - int64_t：整数
     * - double：浮点数
     * - std::string：字符串
     * - std::vector<std::string>：列表（如 Redis List）
     * - std::unordered_map<std::string, std::string>：哈希表（如 Redis Hash）
     */
    using DatabaseValue = std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::vector<std::string>,
        std::unordered_map<std::string, std::string>
    >;

    /**
     * @brief 获取 DatabaseValue 的类型名称（用于调试）
     * @param value 数据库值
     * @return 类型名称字符串
     */
    inline const char *databaseValueTypeName(const DatabaseValue &value)
    {
        switch (value.index())
        {
            case 0: return "Null";
            case 1: return "Bool";
            case 2: return "Int64";
            case 3: return "Double";
            case 4: return "String";
            case 5: return "List";
            case 6: return "Hash";
        }
        return "Unknown";
    }

    /**
     * @brief 数据库连接配置
     *
     * 存储连接所需的通用参数，具体字段含义取决于数据库类型。
     */
    struct ConnectionConfig
    {
        std::string host;       ///< 主机地址
        uint16_t    port{0};    ///< 端口号
        std::string userName;   ///< 用户名
        std::string password;   ///< 密码
        std::string database;   ///< 数据库名称 / Redis 实例编号
        int         poolSize{1}; ///< 连接池大小（默认 1）

        /**
         * @brief 创建 MySQL 默认配置
         */
        static ConnectionConfig mySqlDefault()
        {
            ConnectionConfig config;
            config.host     = "127.0.0.1";
            config.port     = 3306;
            config.userName = "root";
            config.database = "test";
            return config;
        }

        /**
         * @brief 创建 Redis 默认配置
         */
        static ConnectionConfig redisDefault()
        {
            ConnectionConfig config;
            config.host = "127.0.0.1";
            config.port = 6379;
            return config;
        }
    };

} // namespace Database

#endif // DATABASE_DATABASETYPE_H
