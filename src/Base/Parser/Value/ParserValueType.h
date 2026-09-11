/**
 * @file ParserValueType.h
 * @brief 文档值的类型枚举与类型名映射
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <typeinfo>

namespace AsynGyanis::Base
{
    /**
     * @brief 文档值的基础类型
     *
     * @details 枚举顺序与 ParserValue::VariantType 的变体下标一一对应，
     *          ParserValue::type() 依赖该映射关系，不得随意插入或重排。
     */
    enum class ParserValueType : std::uint8_t
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
     * @brief 将文档值类型枚举转换为可读字符串。
     * @param type 文档值类型枚举。
     * @return const char* 对应的类型名称，未知类型返回 "unknown"。
     */
    [[nodiscard]] const char *typeName(ParserValueType type) noexcept;

    /**
     * @brief 编译期类型名称映射，替代 typeid(T).name() 的编译器乱码名。
     *
     * @details 覆盖基本类型；数组与对象的特化声明在 Base/Parser/Value/ParserValue.h 中
     *          （须先看到这两个别名才能特化）。
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
