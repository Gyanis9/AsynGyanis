/**
 * @file DatabaseDriverCatalog.h
 * @brief 驱动目录（库内私有）：一个驱动的全部静态知识集中在表里一行
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这是**库内私有**头而不是对外契约：数据库类型名、默认端口、连接工厂与方言工厂
 *          都从这一张表回答，工厂、方言注册表与 databaseTypeName 只是三个查询入口，
 *          不再各写一份 switch——新增驱动只需要「新建驱动目录 + 在表里加一行」。
 *          忘了加表行时有两道拦截：配置期 asyn_configure_database_driver 的清单检查，
 *          以及用例 EverySupportedDatabaseTypeHasACatalogEntry（名字回落成 "Unknown" 即红）。
 */
#pragma once

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Dialect/SqlDialect.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 一个驱动的静态知识与构造入口
     */
    struct DatabaseDriverEntry
    {
        DatabaseType     type;                                                                          ///< 枚举值
        std::string_view name;                                                                          ///< 展示名，与 databaseTypeName 的返回值同源（指向方言层自己的字面量）
        std::uint16_t    defaultPort;                                                                   ///< 默认端口；0 表示该引擎没有网络端口（SQLite）
        std::unique_ptr<DatabaseConnection> (*createConnection)(const ConnectionConfig &configuration); ///< 连接工厂
        std::shared_ptr<SqlDialect> (*createDialect)();                                                 ///< 方言工厂；nullptr 表示该引擎不参与 SQL 查询树翻译（Redis）
    };

    /**
     * @brief 取驱动目录的只读视图
     * @return std::span<const DatabaseDriverEntry> 目录；顺序与 DatabaseType 的声明顺序一致
     */
    [[nodiscard]] std::span<const DatabaseDriverEntry> databaseDriverCatalog() noexcept;

    /**
     * @brief 按类型查目录下标
     * @param type 数据库类型；未登记（含越界转换）的取值返回空
     * @return std::optional<std::size_t> 目录下标；未登记时为空
     */
    [[nodiscard]] std::optional<std::size_t> databaseDriverIndex(DatabaseType type) noexcept;

} // namespace AsynGyanis::Database
