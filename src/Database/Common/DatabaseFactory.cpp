#include "Database/Common/DatabaseFactory.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Redis/RedisConnection.h"
#include "Database/Sqlite/SqliteConnection.h"

#include <string>

namespace AsynGyanis::Database
{
    std::unique_ptr<DatabaseConnection> DatabaseFactory::create(const ConnectionConfig &config)
    {
        // 端口能判定类型时优先按端口走，这是远程数据库最常见的配置方式
        if (const auto guessed = guessType(config.port); guessed.has_value())
        {
            return create(*guessed, config);
        }

        // SQLite 不需要端口：未指定端口、且**没有任何网络库特征**（主机、账号、口令全空）时
        // 才按嵌入式库处理。只看「库名非空」会把「填了主机账号、忘了端口」的 MySQL 配置
        // 悄悄写成同名本地文件，那是最难排查的一类「连上了但不是你以为的那个库」
        if (config.port == 0 && !config.database.empty() && config.host.empty() && config.userName.empty() && config.password.empty())
        {
            return createSqlite(config);
        }

        throw Base::InvalidArgumentException("无法从配置判定数据库类型：端口 " + std::to_string(config.port) +
                                             " 未知，配置里还带着网络库特征（主机/账号/口令非空）："
                                             "请补上端口，或清空这些字段并按 SQLite 使用");
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::create(const DatabaseType type, const ConnectionConfig &config)
    {
        switch (type)
        {
            case DatabaseType::MySql:
                return createMySql(config);
            case DatabaseType::Redis:
                return createRedis(config);
            case DatabaseType::Sqlite:
                return createSqlite(config);
        }

        // 枚举取值超出已知范围（反序列化出错或内存被写坏）时不给静默默认值
        throw Base::InvalidArgumentException(std::string("不支持的数据库类型：") + databaseTypeName(type));
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createMySql(const ConnectionConfig &config)
    {
        return std::make_unique<MySqlConnection>(config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createRedis(const ConnectionConfig &config)
    {
        return std::make_unique<RedisConnection>(config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createSqlite(const ConnectionConfig &config)
    {
        return std::make_unique<SqliteConnection>(config);
    }

    std::optional<DatabaseType> DatabaseFactory::guessType(const std::uint16_t port) noexcept
    {
        // 只识别已实现驱动的默认端口，其余一律返回空值交由调用方显式指定
        switch (port)
        {
            case 3306:
                return DatabaseType::MySql;
            case 6379:
                return DatabaseType::Redis;
            default:
                return std::nullopt;
        }
    }

} // namespace AsynGyanis::Database
