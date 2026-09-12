/**
 * @file DatabaseValue.h
 * @brief 数据库统一值类型与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

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
     * @details 用 std::variant 屏蔽各数据库的类型方言：NULL / 布尔 / 整数 / 浮点 / 文本 / 列表 / 哈希 / 二进制。
     *          二进制单独成一个备选而不复用 std::string，是因为绑定方式必须能被驱动区分：按文本类型绑定
     *          二进制字节时服务端会按连接字符集解释载荷（BLOB 要用 sqlite3_bind_blob 才落成真正的存储类），
     *          非该字符集的合法序列可能被替换或报错，属静默改数据。
     * @note 哈希取值不保证遍历顺序，调用方需要稳定顺序时自行排序。
     * @note 新增备选一律**追加在末尾**：既有备选的下标是已发布的契约，调整顺序会让所有
     *       按固定下标取值的调用点静默取错类型。
     */
    using DatabaseValue = std::variant<
        std::monostate,
        bool,
        std::int64_t,
        double,
        std::string,
        std::vector<std::string>,
        std::unordered_map<std::string, std::string>,
        std::vector<std::uint8_t> >;

    /**
     * @brief 取得数据库值的类型名称，用于日志与断言
     * @param value 数据库统一值
     * @return const char* 类型名称字面量，例如 "Int64"、"Hash"
     */
    const char *databaseValueTypeName(const DatabaseValue &value) noexcept;

} // namespace AsynGyanis::Database
