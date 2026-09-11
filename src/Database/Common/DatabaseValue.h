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
     * @details 用 std::variant 屏蔽各数据库的类型方言，支持的取值：
     *          - std::monostate：NULL / 空值
     *          - bool：布尔列与 Redis 状态
     *          - int64_t：所有整数列（含 MySQL 的 TINYINT..BIGINT）
     *          - double：浮点列
     *          - std::string：文本与二进制字节序列（std::string 按长度保存，可含 \0）
     *          - std::vector<std::string>：列表（如 Redis List）
     *          - std::unordered_map<std::string, std::string>：哈希表（如 Redis Hash）
     * @note 哈希取值不保证遍历顺序，调用方需要稳定顺序时自行排序。
     */
    using DatabaseValue = std::variant<
        std::monostate,
        bool,
        std::int64_t,
        double,
        std::string,
        std::vector<std::string>,
        std::unordered_map<std::string, std::string>>;

    /**
     * @brief 取得数据库值的类型名称，用于日志与断言
     * @param value 数据库统一值
     * @return const char* 类型名称字面量，例如 "Int64"、"Hash"
     */
    const char *databaseValueTypeName(const DatabaseValue &value) noexcept;

} // namespace AsynGyanis::Database
