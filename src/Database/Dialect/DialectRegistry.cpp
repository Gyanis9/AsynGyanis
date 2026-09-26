#include "Database/Dialect/DialectRegistry.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Dialect/MySqlDialect.h"
#include "Database/Dialect/SqliteDialect.h"

#include <string>

namespace AsynGyanis::Database
{
    std::shared_ptr<SqlDialect> DialectRegistry::dialectFor(const DatabaseType type)
    {
        switch (type)
        {
            case DatabaseType::Sqlite:
            {
                // 函数内静态对象：初始化由编译器保证只执行一次且线程安全，
                // 方言无状态，因此全局共用一个实例即可，不需要每次 new 一个。
                // 必须用花括号圈出独立作用域：static 局部变量的初始化不允许跨越 case 标签
                static const std::shared_ptr<SqlDialect> kSqliteDialectInstance = std::make_shared<SqliteDialect>();
                return kSqliteDialectInstance;
            }

            case DatabaseType::MySql:
            {
                // 与 SQLite 分支同构：反引号引用、START TRANSACTION 事务、LIMIT ? OFFSET ? 分页，
                // 这些差异全部封在 MySqlDialect 里，上层只认 DatabaseType
                static const std::shared_ptr<SqlDialect> kMySqlDialectInstance = std::make_shared<MySqlDialect>();
                return kMySqlDialectInstance;
            }

            case DatabaseType::Redis:
                throw Base::InvalidArgumentException("方言注册表：Redis 是键值存储，不参与 SQL 查询树翻译，请改用 RedisConnection 的命令接口");

            default:
                // 新增枚举值却忘了登记方言时走这里，异常文本带上数值便于定位
                throw Base::InvalidArgumentException("方言注册表：不支持的数据库类型（枚举值 " + std::to_string(static_cast<int>(type)) +
                                                     "）：请为该类型实现 SqlDialect 后在本注册表登记");
        }
    }

    bool DialectRegistry::supports(const DatabaseType type) noexcept
    {
        // 与 dialectFor() 的分支保持一一对应：新增方言时两处必须同步修改
        return type == DatabaseType::Sqlite || type == DatabaseType::MySql;
    }

} // namespace AsynGyanis::Database
