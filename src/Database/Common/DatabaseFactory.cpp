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

        // 报错要说准「缺的到底是哪一样」。原先一条文案同时断言两件事，于是一份什么都没填的默认配置
        // 会被告知「配置里还带着网络库特征」——把排查方向引向并不存在的字段（规范第 10 条：文案要可操作）
        const bool hasNetworkFeatures = !config.host.empty() || !config.userName.empty() || !config.password.empty();

        std::string message = "无法从配置判定数据库类型：端口 " + std::to_string(config.port) + " 推不出驱动，";
        message += hasNetworkFeatures ? "且配置里带着网络库特征（主机/账号/口令非空），不能当嵌入式库处理："
                                        "请补上端口，或清空这些字段并按 SQLite 使用"
                                      : "且 database 为空，没有可作为嵌入式库依据的库名或路径："
                                        "连远端请补 host/port（或显式指定驱动类型），用本地库请把 database 填成 \":memory:\" 或文件路径";

        throw Base::InvalidArgumentException(message);
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
        throw Base::InvalidArgumentException(std::string("不支持的数据库类型：") + databaseTypeName(type) + "（枚举值 " + std::to_string(static_cast<int>(type)) +
                                             "）：请改用 MySql / Sqlite / Redis 三者之一，或修正反序列化来源");
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
