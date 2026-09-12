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
            default:
                // 枚举值可能来自反序列化或越界转换，兜底返回可读名称而不是崩溃
                return "Unknown";
        }
    }

} // namespace AsynGyanis::Database
