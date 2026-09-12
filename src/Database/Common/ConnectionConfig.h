/**
 * @file ConnectionConfig.h
 * @brief 数据库连接配置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include <cstdint>
#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库连接配置
     *
     * @details 只保留各驱动确实会读取的字段：原先的 poolSize 没有任何驱动消费，
     *          属于「写了没人用」的假配置，故移除；接入连接池时再随实现一起加入。
     *          超时不在这里配置，由 DatabaseConnection 的 setConnectTimeout /
     *          setQueryTimeout 管理，避免出现两处互相覆盖的真值来源。
     */
    struct ConnectionConfig
    {
        std::string host;      ///< 主机地址；SQLite 忽略该字段
        std::uint16_t port = 0; ///< 端口号；0 表示未指定，SQLite 不使用端口
        std::string userName;   ///< 用户名；SQLite 忽略
        std::string password;   ///< 密码；SQLite 忽略
        std::string database;   ///< 数据库名、Redis 键空间编号或 SQLite 文件路径

        /**
         * @brief 构造一份 MySQL 默认配置
         * @return ConnectionConfig 指向本机 3306 的 root/test 配置
         */
        static ConnectionConfig mySqlDefault()
        {
            ConnectionConfig configuration;
            configuration.host     = "127.0.0.1";
            configuration.port     = 3306;
            configuration.userName = "root";
            configuration.database = "test";
            return configuration;
        }

        /**
         * @brief 构造一份 Redis 默认配置
         * @return ConnectionConfig 指向本机 6379 的配置
         */
        static ConnectionConfig redisDefault()
        {
            ConnectionConfig configuration;
            configuration.host = "127.0.0.1";
            configuration.port = 6379;
            return configuration;
        }

        /**
         * @brief 构造一份 PostgreSQL 默认配置
         * @return ConnectionConfig 指向本机 5432 的 postgres/postgres 配置
         * @note 默认库与默认用户名都取 "postgres"：那是 PostgreSQL 初始化集群时创建的
         *       超级用户与同名维护库，任何一台刚装好的服务端上必然存在这两个名字
         */
        static ConnectionConfig postgresDefault()
        {
            ConnectionConfig configuration;
            configuration.host     = "127.0.0.1";
            configuration.port     = 5432;
            configuration.userName = "postgres";
            configuration.database = "postgres";
            return configuration;
        }

        /**
         * @brief 构造一份 SQLite 配置
         * @param databasePath 数据库文件路径，传 ":memory:" 得到内存库
         * @return ConnectionConfig 仅填充 database 字段的配置
         */
        static ConnectionConfig sqliteDefault(std::string databasePath = ":memory:")
        {
            ConnectionConfig configuration;
            configuration.database = std::move(databasePath);
            return configuration;
        }
    };

} // namespace AsynGyanis::Database
