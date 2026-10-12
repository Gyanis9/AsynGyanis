#include "Database/Common/DatabaseType.h"

#include "Database/Common/DatabaseDriverCatalog.h"

#include <optional>
#include <string_view>

namespace AsynGyanis::Database
{
    const char *databaseTypeName(const DatabaseType type) noexcept
    {
        // 名字与驱动目录同源（表里一行一条）：新增驱动只要登记进目录，这里自动跟随；
        // 越界取值与「加了枚举却忘了加表行」都回落到 "Unknown"。返回的指针指向目录里的
        // 静态字面量，生命周期与旧实现（switch 里的字面量）一致。
        const std::optional<std::size_t> index = databaseDriverIndex(type);
        if (!index.has_value())
        {
            return "Unknown";
        }

        return databaseDriverCatalog()[*index].name.data();
    }

} // namespace AsynGyanis::Database
