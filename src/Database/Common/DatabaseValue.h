/**
 * @file DatabaseValue.h
 * @brief 数据库统一值类型与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/Decimal.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库统一值类型
     *
     * @details 用 std::variant 屏蔽各数据库的类型方言：NULL / 布尔 / 整数 / 浮点 / 文本 / 列表 / 哈希 / 二进制 /
     *          精确十进制。
     *          二进制单独成一个备选而不复用 std::string，是因为绑定方式必须能被驱动区分：按文本类型绑定
     *          二进制字节时服务端会按连接字符集解释载荷（BLOB 要用 sqlite3_bind_blob 才落成真正的存储类），
     *          非该字符集的合法序列可能被替换或报错，属静默改数据。
     *          十进制（Decimal）同样单独成一个备选：它在**送出**方向必须按十进制文本交给服务端，
     *          绝不能被降级成 double（末位精度不可逆地丢失）；而在**取回**方向三家引擎今天都以文本交出，
     *          因此本备选只用于「调用方明确写一个精确小数」，读回来落在哪个备选由各家驱动的列类型映射决定。
     *          日期时间分两个备选而不是一个带标志的结构：UTC 瞬时（sys_time）与无偏移的墙上时间
     *          （local_time）在三家引擎上的存储与渲染语义都不同——MySQL 的 TIMESTAMP 按会话时区换算而
     *          DATETIME 按字面存、PostgreSQL 的 timestamptz 与 timestamp 只差一个偏移、SQLite 两者都只是文本。
     *          把它们折成一个类型就是替三家抹平这个差别，而抹平的后果是「同一个文本被两家解释成不同瞬间」。
     *          承载精度取微秒，与三家的最高小数秒精度对齐（MySQL DATETIME(6)、PostgreSQL 的 6 位小数秒）。
     * @note 哈希取值不保证遍历顺序，调用方需要稳定顺序时自行排序。
     * @note 新增备选一律追加在末尾：既有备选的下标是已发布的契约，调整顺序会让所有
     *       按固定下标取值的调用点静默取错类型。
     */
    /**
     * @brief UTC 瞬时（微秒精度）—— 带时区语义的那一档日期时间
     * @details 值本身是「时间线上的一个点」，与哪个时区怎么显示它无关。三家里只有 PostgreSQL 的
     *          timestamptz 存的就是这个语义；MySQL 按本映射存成 DATETIME(6) 的 UTC 字面量
     *          （选它的理由见 MySqlDialect::columnTypeName），SQLite 则写成带 `Z` 的定宽 ISO 文本。
     */
    using Timestamp = std::chrono::sys_time<std::chrono::microseconds>;

    /**
     * @brief 无偏移的墙上时间（微秒精度）—— 只有历法读数、不属于任何时区的那一档
     * @details 「2026-03-01 02:30:00」这样一个读数，配上时区才构成时间线上的一个点。
     *          与 Timestamp 是两个类型而不是一个带标志的结构，理由见下面 DatabaseValue 的注释。
     */
    using LocalDateTime = std::chrono::local_time<std::chrono::microseconds>;

    using DatabaseValue = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>, std::unordered_map<std::string, std::string>,
                                       std::vector<std::uint8_t>, Decimal, Timestamp, LocalDateTime>;

    /**
     * @brief 取得数据库值的类型名称，用于日志与断言
     * @param value 数据库统一值
     * @return const char* 类型名称字面量，例如 "Int64"、"Hash"
     */
    ASYN_DATABASE_API const char *databaseValueTypeName(const DatabaseValue &value) noexcept;

} // namespace AsynGyanis::Database
