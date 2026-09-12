/**
 * @file DialectRegistry.cpp
 * @brief 方言注册表实现
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Dialect/DialectRegistry.h"

#include "Database/Dialect/SqliteDialect.h"

#include <stdexcept>
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
                static const std::shared_ptr<SqlDialect> sqliteDialectInstance = std::make_shared<SqliteDialect>();
                return sqliteDialectInstance;
            }

            case DatabaseType::MySql:
                throw std::invalid_argument(
                    "方言注册表：暂未提供 MySQL 方言实现。MySQL 的标识符引用符（反引号）与分页语法"
                    "（OFFSET 必须与 LIMIT 同时出现）与 SQLite 不同，不能直接复用 SqliteDialect；"
                    "请实现 MySqlDialect 后在本注册表登记");

            case DatabaseType::Redis:
                throw std::invalid_argument(
                    "方言注册表：Redis 是键值存储，不参与 SQL 查询树翻译，请改用 RedisConnection 的命令接口");

            default:
                // 新增枚举值却忘了登记方言时走这里，异常文本带上数值便于定位
                throw std::invalid_argument(
                    "方言注册表：不支持的数据库类型（枚举值 " +
                    std::to_string(static_cast<int>(type)) + "）");
        }
    }

    bool DialectRegistry::supports(const DatabaseType type) noexcept
    {
        // 与 dialectFor() 的分支保持一一对应：新增方言时两处必须同步修改
        return type == DatabaseType::Sqlite;
    }

} // namespace AsynGyanis::Database
