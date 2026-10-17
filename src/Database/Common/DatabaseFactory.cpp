#include "Database/Common/DatabaseFactory.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Common/DatabaseDriverCatalog.h"

#include <optional>
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

        // SQLite 不需要端口：未指定端口、且没有任何网络库特征（主机、账号、口令全空）时
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
        // 构造入口在驱动目录里（表里一行一条）：新增驱动只要登记进目录，本函数与下面的
        // 名字/端口映射都自动跟随，不再各补一个 switch 分支
        const std::optional<std::size_t> index = databaseDriverIndex(type);
        if (!index.has_value())
        {
            // 枚举取值超出已知范围（反序列化出错或内存被写坏），或新增枚举成员却忘了加表行——
            // 两种都当场拒绝，不给静默默认值
            throw Base::InvalidArgumentException(std::string("不支持的数据库类型：") + databaseTypeName(type) + "（枚举值 " + std::to_string(static_cast<int>(type)) +
                                                 "）：请改用 MySql / PostgreSql / Redis / Sqlite 四者之一，或修正反序列化来源");
        }

        return databaseDriverCatalog()[*index].createConnection(config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createMySql(const ConnectionConfig &config)
    {
        // 便利入口只是 create(类型) 的转发：构造口径（目录里的工厂）只有一份
        return create(DatabaseType::MySql, config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createPostgres(const ConnectionConfig &config)
    {
        // 与 create(DatabaseType::PostgreSql) 同源，见 createMySql 的说明
        return create(DatabaseType::PostgreSql, config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createRedis(const ConnectionConfig &config)
    {
        // 与 create(DatabaseType::Redis) 同源，见 createMySql 的说明
        return create(DatabaseType::Redis, config);
    }

    std::unique_ptr<DatabaseConnection> DatabaseFactory::createSqlite(const ConnectionConfig &config)
    {
        // 与 create(DatabaseType::Sqlite) 同源，见 createMySql 的说明
        return create(DatabaseType::Sqlite, config);
    }

    std::optional<DatabaseType> DatabaseFactory::guessType(const std::uint16_t port) noexcept
    {
        // 端口表也在驱动目录里（每行的 defaultPort；0 表示该引擎没有网络端口，不参与匹配）。
        // 未登记的端口一律返回空值交由调用方显式指定
        for (const DatabaseDriverEntry &entry: databaseDriverCatalog())
        {
            if (entry.defaultPort != 0 && entry.defaultPort == port)
            {
                return entry.type;
            }
        }

        return std::nullopt;
    }

} // namespace AsynGyanis::Database
