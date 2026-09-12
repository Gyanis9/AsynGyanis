/**
 * @file FormatValueType.h
 * @brief 文档值的类型枚举与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 文档值的基础类型
     *
     * @details 枚举顺序与 FormatValue::VariantType 的变体下标一一对应，
     *          FormatValue::type() 依赖该映射关系，不得随意插入或重排。
     *          UInt 作为新增的第 8 个备选追加在末尾（值 7），既有 7 个枚举值
     *          的数值与顺序保持不变。
     */
    enum class FormatValueType : std::uint8_t
    {
        Null,   ///< 空值
        Bool,   ///< 布尔类型
        Int,    ///< 有符号整数类型 (int64_t)
        Double, ///< 浮点类型 (double)
        String, ///< 字符串类型
        Array,  ///< 数组类型
        Object, ///< 对象类型（嵌套）
        UInt    ///< 无符号整数类型 (uint64_t)，追加在末尾以保证既有下标不变
    };

    /**
     * @brief 将文档值类型枚举转换为可读字符串。
     * @param type 文档值类型枚举。
     * @return const char* 对应的类型名称，未知类型返回 "unknown"。
     */
    [[nodiscard]] const char *typeName(FormatValueType type) noexcept;

    namespace detail
    {
        /**
         * @brief 依赖模板参数的恒假值
         * @details 用于在 typeNameOf 的兜底分支中写出「只在实例化时才失败」的静态断言，
         *          避免直接写 false 导致断言在模板定义处立即触发。
         * @tparam T 任意类型
         */
        template<typename T>
        inline constexpr bool kAlwaysFalse = false;
    } // namespace detail

    /**
     * @brief 编译期类型名称映射，替代 typeid(T).name() 的编译器乱码名。
     *
     * @details 仅对 FormatValue 底层变体的替代类型提供显式特化（基本类型在本文件，
     *          数组与对象别名在 Base/Format/Value/FormatValue.h），名称是稳定字面量，
     *          因此跨平台、跨编译器完全一致。未特化的类型走本兜底定义并在编译期报错，
     *          而不是退化成 typeid 的平台相关名称。
     * @tparam T 待查询名称的类型
     * @return const char* 类型名称
     */
    template<typename T>
    [[nodiscard]] constexpr const char *typeNameOf() noexcept
    {
        static_assert(detail::kAlwaysFalse<T>,
                      "typeNameOf 仅支持 FormatValue 变体的替代类型：nullptr_t、bool、int64_t、uint64_t、double、std::string、FormatValueArray、FormatValueObject");
        return "unknown";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<std::nullptr_t>() noexcept
    {
        return "null";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<bool>() noexcept
    {
        return "bool";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<std::int64_t>() noexcept
    {
        return "int";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<std::uint64_t>() noexcept
    {
        return "uint";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<double>() noexcept
    {
        return "double";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<std::string>() noexcept
    {
        return "string";
    }
} // namespace AsynGyanis::Base
