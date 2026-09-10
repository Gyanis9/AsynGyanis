/**
 * @file ConfigValueType.h
 * @brief 配置值基础类型枚举与类型名、文件后缀、键路径等通用工具函数
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置值的基础类型
     *
     * @details 枚举顺序与 ConfigValue::VariantType 的变体下标一一对应，
     *          ConfigValue::type() 依赖该映射关系，不得随意插入或重排。
     */
    enum class ConfigValueType : std::uint8_t
    {
        Null,   ///< 空值
        Bool,   ///< 布尔类型
        Int,    ///< 整数类型 (int64_t)
        Double, ///< 浮点类型 (double)
        String, ///< 字符串类型
        Array,  ///< 数组类型
        Object  ///< 对象类型（嵌套）
    };

    /**
     * @brief 将配置值类型枚举转换为可读字符串。
     * @param type 配置值类型枚举。
     * @return const char* 对应的类型名称，未知类型返回 "unknown"。
     */
    [[nodiscard]] const char *typeName(ConfigValueType type) noexcept;

    /**
     * @brief 判断文件路径是否为 YAML 配置文件后缀。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .yaml 或 .yml（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isYamlFile(std::string_view filePath) noexcept;

    /**
     * @brief 判断文件路径是否为 JSON 配置文件后缀。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .json（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isJsonFile(std::string_view filePath) noexcept;

    /**
     * @brief 判断文件路径是否为受支持的配置文件（JSON 或 YAML）。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .json/.yaml/.yml（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isConfigFile(std::string_view filePath) noexcept;

    /**
     * @brief 使用指定分隔符拆分配置键字符串。
     * @details 连续分隔符与首尾分隔符产生的空片段会被忽略。
     * @param key 待拆分的原始键字符串。
     * @param delimiter 分隔字符。
     * @return std::vector<std::string> 拆分后的键片段列表。
     */
    std::vector<std::string> splitKey(std::string_view key, char delimiter = '.');

    /**
     * @brief 编译期类型名称映射，替代 typeid(T).name() 的编译器乱码名。
     *
     * @details 覆盖基本类型；ConfigArray/ConfigObject 的特化声明在
     *          Base/Config/ConfigValue.h 中（须先看到这两个别名才能特化）。
     * @tparam T 待查询名称的类型。
     * @return const char* 类型名称。
     */
    template<typename T>
    [[nodiscard]] const char *typeNameOf() noexcept
    {
        if constexpr (std::is_same_v<T, bool>)
            return "bool";
        else if constexpr (std::is_same_v<T, int64_t>)
            return "int";
        else if constexpr (std::is_same_v<T, double>)
            return "double";
        else if constexpr (std::is_same_v<T, std::string>)
            return "string";
        else if constexpr (std::is_same_v<T, std::nullptr_t>)
            return "null";
        else
            return typeid(T).name();
    }
} // namespace AsynGyanis::Base
