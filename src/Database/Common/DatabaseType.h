/**
 * @file DatabaseType.h
 * @brief 数据库类型枚举与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

namespace AsynGyanis::Database
{
    /**
     * @brief 受支持的数据库类型
     *
     * @details 只收录本框架已有驱动实现的引擎。原先预留的 PostgreSql 没有任何对应实现，
     *          会让工厂出现永远走不到的分支、端口猜测返回不可用的类型，因此移除；
     *          将来接入 PostgreSQL 时再连同驱动一并补上。
     */
    enum class DatabaseType
    {
        MySql, ///< MySQL / MariaDB 服务
        Redis, ///< Redis 键值存储
        Sqlite ///< SQLite 嵌入式数据库
    };

    /**
     * @brief 将数据库类型转换为可读名称
     * @param type 数据库类型枚举值
     * @return const char* 类型名称字面量，未知取值返回 "Unknown"
     */
    const char *databaseTypeName(DatabaseType type) noexcept;

} // namespace AsynGyanis::Database
