#include "Database/Dialect/DialectRegistry.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Database/Common/DatabaseDriverCatalog.h"

#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{
    std::shared_ptr<SqlDialect> DialectRegistry::dialectFor(const DatabaseType type)
    {
        const std::optional<std::size_t> index = databaseDriverIndex(type);
        if (!index.has_value())
        {
            // 新增枚举值却忘了登记驱动目录时走这里，异常文本带上数值便于定位
            throw Base::InvalidArgumentException("方言注册表：不支持的数据库类型（枚举值 " + std::to_string(static_cast<int>(type)) +
                                                 "）：请为该类型实现 SqlDialect 并登记进驱动目录（DatabaseDriverCatalog.h）");
        }

        const DatabaseDriverEntry &entry = databaseDriverCatalog()[*index];
        if (entry.createDialect == nullptr)
        {
            throw Base::InvalidArgumentException("方言注册表：" + std::string(entry.name) + " 不参与 SQL 查询树翻译，请改用该驱动的命令接口");
        }

        // 每驱动的共享实例：函数内静态表在首次调用时进程级构造一次（初始化由编译器保证线程
        // 安全），方言无状态——共享一份与每次新建等价，用例钉着「两次取得同一对象」
        static const std::vector<std::shared_ptr<SqlDialect>> kDialectInstances = []
        {
            const std::span<const DatabaseDriverEntry> catalog = databaseDriverCatalog();
            std::vector<std::shared_ptr<SqlDialect>>   instances(catalog.size());
            for (std::size_t catalogIndex = 0; catalogIndex < catalog.size(); ++catalogIndex)
            {
                if (catalog[catalogIndex].createDialect != nullptr)
                {
                    instances[catalogIndex] = catalog[catalogIndex].createDialect();
                }
            }
            return instances;
        }();

        return kDialectInstances[*index];
    }

    bool DialectRegistry::supports(const DatabaseType type) noexcept
    {
        // 与 dialectFor() 同一判据：目录里有方言工厂才算「参与 SQL 查询树翻译」——
        // Redis 有目录条目、没有方言工厂，因此为假
        const std::optional<std::size_t> index = databaseDriverIndex(type);
        return index.has_value() && databaseDriverCatalog()[*index].createDialect != nullptr;
    }

} // namespace AsynGyanis::Database
