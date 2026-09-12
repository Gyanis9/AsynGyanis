/**
 * @file DialectRegistry.h
 * @brief 方言注册表 —— 按数据库类型取得对应的 SQL 方言实现
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 上层（Queryable 执行器）只持有 DatabaseType，需要一个把类型映射到 SqlDialect
 *          实例的统一入口。注册表用函数内静态共享指针持有无状态方言实例，
 *          因此多次取用得到同一个对象（线程安全的 magic static，无需加锁）。
 *
 * ## 未实现的类型如何处理
 * 尚未提供方言的类型（如 Oracle）在这里抛中文异常而不是返回空指针：返回空会让每个调用点
 * 都必须写判空分支，漏写一处就是解引用空指针崩溃；抛异常把错误收敛到一处，
 * 且异常文本能直接告诉使用者缺什么、怎么补。调用方若需要「先问有没有」而不想捕获异常，
 * 可用 supports() 预判。
 */
#pragma once

#include "Database/Common/DatabaseType.h"
#include "Database/Dialect/SqlDialect.h"

#include <memory>

namespace AsynGyanis::Database
{
    /**
     * @brief SQL 方言注册表
     *
     * @details 纯静态类，不允许实例化。当前注册的方言有 SQLite、MySQL 与 PostgreSQL。
     *
     * @code
     *   std::shared_ptr<SqlDialect> dialect = DialectRegistry::dialectFor(DatabaseType::Sqlite);
     *   SqlStatement statement = dialect->translate(query);
     * @endcode
     */
    class DialectRegistry
    {
    public:
        DialectRegistry() = delete;

        /**
         * @brief 取得指定数据库类型的方言实现
         *
         * @details 返回的实例是无状态的共享单例，调用方可以长期持有并在多线程上并发使用。
         *
         * @param type 数据库类型
         * @return std::shared_ptr<SqlDialect> 该类型的方言实现，恒非空
         * @throws std::invalid_argument 该类型尚未提供方言实现（PostgreSQL 等），
         *         或该类型不是 SQL 数据库（Redis），异常文本为中文提示
         */
        [[nodiscard]] static std::shared_ptr<SqlDialect> dialectFor(DatabaseType type);

        /**
         * @brief 判断指定类型是否已有方言实现
         * @details 供调用方在不想捕获异常时预判，例如构造连接池后先校验再执行查询。
         * @param type 数据库类型
         * @return true 已有方言实现（SQLite / MySQL），dialectFor() 必定成功
         */
        [[nodiscard]] static bool supports(DatabaseType type) noexcept;
    };

} // namespace AsynGyanis::Database
