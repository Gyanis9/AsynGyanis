/**
 * @file DatabaseType.cpp
 * @brief 数据库类型名称映射实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Common/DatabaseType.h"

namespace AsynGyanis::Database
{
    const char *databaseTypeName(const DatabaseType type) noexcept
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
            default:
                // 枚举值可能来自反序列化或越界转换，兜底返回可读名称而不是崩溃
                return "Unknown";
        }
    }

} // namespace AsynGyanis::Database
