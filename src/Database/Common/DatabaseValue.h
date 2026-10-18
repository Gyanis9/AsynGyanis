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
     * @note 哈希取值不保证遍历顺序，调用方需要稳定顺序时自行排序。
     * @note 新增备选一律追加在末尾：既有备选的下标是已发布的契约，调整顺序会让所有
     *       按固定下标取值的调用点静默取错类型。
     */
    using DatabaseValue = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>, std::unordered_map<std::string, std::string>,
                                       std::vector<std::uint8_t>, Decimal>;

    /**
     * @brief 取得数据库值的类型名称，用于日志与断言
     * @param value 数据库统一值
     * @return const char* 类型名称字面量，例如 "Int64"、"Hash"
     */
    ASYN_DATABASE_API const char *databaseValueTypeName(const DatabaseValue &value) noexcept;

} // namespace AsynGyanis::Database
