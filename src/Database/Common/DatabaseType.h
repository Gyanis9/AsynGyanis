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
     * @details 只收录本框架已有驱动实现的引擎：MySQL、Redis、SQLite 各自都有连接实现。
     *          没有驱动实现的引擎一律不列在这里——否则工厂会出现永远走不到的分支，
     *          端口猜测也会返回一个用不了的类型。
     *
     * @note 新成员一律追加在末尾：枚举值可能被序列化进配置文件或日志（如连接配置里的类型
     *       字符串转换），往中间插入会让既有取值的数值含义整体位移。
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
