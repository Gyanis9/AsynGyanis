/**
 * @file ConfigKeyValueMap.h
 * @brief 配置扁平键值表类型，支持以 string_view 异质查找
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Parser/Value/ParserValue.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AsynGyanis::Base
{
    /**
     * @brief 透明字符串哈希，统一 std::string 与 std::string_view 的哈希结果
     *
     * @details 供 ConfigKeyValueMap 使用，使 unordered_map 能以 string_view 直接查找，
     *          避免每次取值都构造临时 std::string。只暴露 string_view 一个重载，
     *          std::string 经隐式转换走同一条哈希路径，从根上杜绝两种键哈希不一致。
     */
    struct TransparentStringHash
    {
        using is_transparent = void; ///< 启用异质查找的标记类型

        /**
         * @brief 计算字符串的哈希值。
         * @param value 字符串视图，std::string 实参隐式转换而来。
         * @return size_t 哈希值。
         */
        [[nodiscard]] size_t operator()(const std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    /**
     * @brief 配置键值映射类型
     *
     * @details 键为点号扁平化后的路径（如 server.port），透明哈希 + std::equal_to<>
     *          使 get("a.b") 这类 string_view 实参无需先构造 std::string。
     */
    using ConfigKeyValueMap = std::unordered_map<std::string, ParserValue, TransparentStringHash, std::equal_to<> >;
} // namespace AsynGyanis::Base
