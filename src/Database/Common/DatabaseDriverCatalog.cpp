#include "Database/Common/DatabaseDriverCatalog.h"

#include "Database/Dialect/MySqlDialect.h"
#include "Database/Dialect/PostgresDialect.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Postgres/PostgresConnection.h"
#include "Database/Redis/RedisConnection.h"
#include "Database/Sqlite/SqliteConnection.h"

#include <array>

namespace AsynGyanis::Database
{
    namespace
    {
        std::unique_ptr<DatabaseConnection> createMySqlConnection(const ConnectionConfig &configuration)
        {
            return std::make_unique<MySqlConnection>(configuration);
        }

        std::unique_ptr<DatabaseConnection> createPostgresConnection(const ConnectionConfig &configuration)
        {
            return std::make_unique<PostgresConnection>(configuration);
        }

        std::unique_ptr<DatabaseConnection> createRedisConnection(const ConnectionConfig &configuration)
        {
            return std::make_unique<RedisConnection>(configuration);
        }

        std::unique_ptr<DatabaseConnection> createSqliteConnection(const ConnectionConfig &configuration)
        {
            return std::make_unique<SqliteConnection>(configuration);
        }

        std::shared_ptr<SqlDialect> createMySqlDialect()
        {
            return std::make_shared<MySqlDialect>();
        }

        std::shared_ptr<SqlDialect> createSqliteDialect()
        {
            return std::make_shared<SqliteDialect>();
        }

        std::shared_ptr<SqlDialect> createPostgresDialect()
        {
            return std::make_shared<PostgresDialect>();
        }

        /// 驱动目录：新增驱动在这里加一行（顺序与 DatabaseType 的声明顺序一致）。工厂、
        /// 方言注册表与 databaseTypeName 都读它，别再往那三处补 switch 分支
        constexpr std::array<DatabaseDriverEntry, 4> kDatabaseDriverCatalog = {{
                {DatabaseType::MySql, "MySql", 3306U, &createMySqlConnection, &createMySqlDialect},
                {DatabaseType::Redis, "Redis", 6379U, &createRedisConnection, nullptr},
                {DatabaseType::Sqlite, "Sqlite", 0U, &createSqliteConnection, &createSqliteDialect},
                {DatabaseType::PostgreSql, "PostgreSql", 5432U, &createPostgresConnection, &createPostgresDialect},
        }};

    } // namespace

    std::span<const DatabaseDriverEntry> databaseDriverCatalog() noexcept
    {
        return kDatabaseDriverCatalog;
    }

    std::optional<std::size_t> databaseDriverIndex(const DatabaseType type) noexcept
    {
        // 线性扫四行；调用点都在低频路径（建连、方言解析、名字映射），不需要哈希
        for (std::size_t index = 0; index < kDatabaseDriverCatalog.size(); ++index)
        {
            if (kDatabaseDriverCatalog[index].type == type)
            {
                return index;
            }
        }

        // 未登记：越界转换，或新增枚举成员却忘了加表行——由调用方按各自的口径报错（工厂抛参数
        // 异常、名字映射回落 "Unknown"），本函数不替它们决定该说什么
        return std::nullopt;
    }

} // namespace AsynGyanis::Database
