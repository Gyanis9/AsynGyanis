/**
 * @file ConfigValue.h
 * @brief 配置值类型别名与严格取用工具
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置值类型别名集合
     *
     * @details 值模型直接使用 nlohmann_json 的原生类型：取成员用 at()、类型判定用 is_*()、
     *          序列化用 dump()。配置侧的名字只是别名，不存在第二份实现或转换开销。
     */
    using ConfigValue  = nlohmann::json;        ///< 配置值即 JSON 文档
    using ConfigArray  = ConfigValue::array_t;  ///< 配置数组（std::vector<ConfigValue>）
    using ConfigObject = ConfigValue::object_t; ///< 配置对象（按键有序的映射）

    /**
     * @brief 严格取用配置值：不做截断、不回绕、不跨类型转换
     *
     * @details nlohmann_json 的 get<T>() 允许算术类型互相转换（浮点取整、布尔当数字），
     *          本项目按「宁可回落默认值也不静默变形」的口径取用，因此统一走本函数：
     *          bool 只接受布尔；整数只接受整数且目标类型须能无损表示；浮点只接受浮点；
     *          其余类型一致才成功。
     * @tparam ValueType 目标类型（bool、整型、浮点、std::string、ConfigArray、ConfigObject）
     * @param value 待取用的配置值
     * @return std::optional<ValueType> 类型与取值范围都满足时返回取值，否则返回空
     */
    template<typename ValueType>
    [[nodiscard]] std::optional<ValueType> configValueAs(const ConfigValue &value)
    {
        if constexpr (std::is_same_v<ValueType, bool>)
        {
            // 布尔与数值互不转换：数字 0/1 不是「假/真」
            if (!value.is_boolean())
            {
                return std::nullopt;
            }
            return value.get<bool>();
        } else if constexpr (std::is_integral_v<ValueType>)
        {
            if (value.is_number_unsigned())
            {
                const std::uint64_t magnitude = value.get<std::uint64_t>();
                // 只有「窄于 64 位的目标」或「有符号目标」才可能装不下该取值；同宽的无符号目标
                // 必然装得下，直接比较会构成恒假比较（零告警要求下必须用编译期分支挡掉）
                if constexpr (sizeof(ValueType) < sizeof(std::uint64_t) || std::is_signed_v<ValueType>)
                {
                    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<ValueType>::max()))
                    {
                        return std::nullopt;
                    }
                }
                return static_cast<ValueType>(magnitude);
            }
            if (value.is_number_integer())
            {
                const std::int64_t signedValue = value.get<std::int64_t>();
                if (signedValue < 0)
                {
                    if constexpr (std::is_unsigned_v<ValueType>)
                    {
                        return std::nullopt;
                    } else if constexpr (sizeof(ValueType) < sizeof(std::int64_t))
                    {
                        // 同为 64 位的有符号目标不可能下溢，只有更窄的目标才需要判下限
                        if (signedValue < static_cast<std::int64_t>(std::numeric_limits<ValueType>::min()))
                        {
                            return std::nullopt;
                        }
                    }
                } else if constexpr (sizeof(ValueType) < sizeof(std::int64_t))
                {
                    if (static_cast<std::uint64_t>(signedValue) > static_cast<std::uint64_t>(std::numeric_limits<ValueType>::max()))
                    {
                        return std::nullopt;
                    }
                }
                return static_cast<ValueType>(signedValue);
            }
            // 浮点、布尔、字符串都不当整数：取整与真假转换都属于静默变形
            return std::nullopt;
        } else if constexpr (std::is_floating_point_v<ValueType>)
        {
            if (!value.is_number_float())
            {
                return std::nullopt;
            }
            const double number = value.get<double>();
            if constexpr (!std::is_same_v<ValueType, double>)
            {
                // 窄化到 float 前先判范围，避免溢出成无穷大这一步静默变形
                if (std::isfinite(number) &&
                    (number > static_cast<double>(std::numeric_limits<ValueType>::max()) || number < -static_cast<double>(std::numeric_limits<ValueType>::max())))
                {
                    return std::nullopt;
                }
            }
            return static_cast<ValueType>(number);
        } else if constexpr (std::is_same_v<ValueType, std::string>)
        {
            if (!value.is_string())
            {
                return std::nullopt;
            }
            return value.get<std::string>();
        } else if constexpr (std::is_same_v<ValueType, ConfigArray>)
        {
            if (!value.is_array())
            {
                return std::nullopt;
            }
            return value.get<ConfigArray>();
        } else if constexpr (std::is_same_v<ValueType, ConfigObject>)
        {
            if (!value.is_object())
            {
                return std::nullopt;
            }
            return value.get<ConfigObject>();
        } else
        {
            return std::nullopt;
        }
    }

    /**
     * @brief 取用目标类型对应的可读名称，用于类型不匹配的错误文案
     * @tparam ValueType 取用目标类型
     * @return const char* 类型名称（int/uint/double/bool/string/array/object），未知类型返回 "value"
     */
    template<typename ValueType>
    [[nodiscard]] constexpr const char *configTypeNameOf() noexcept
    {
        if constexpr (std::is_same_v<ValueType, bool>)
        {
            return "bool";
        } else if constexpr (std::is_same_v<ValueType, std::string>)
        {
            return "string";
        } else if constexpr (std::is_floating_point_v<ValueType>)
        {
            return "double";
        } else if constexpr (std::is_integral_v<ValueType>)
        {
            return std::is_signed_v<ValueType> ? "int" : "uint";
        } else if constexpr (std::is_same_v<ValueType, ConfigArray>)
        {
            return "array";
        } else if constexpr (std::is_same_v<ValueType, ConfigObject>)
        {
            return "object";
        } else
        {
            return "value";
        }
    }
} // namespace AsynGyanis::Base
