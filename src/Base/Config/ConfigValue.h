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
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
     * @brief 解析一段 JSON 文本，语法不合法时交回空而不是抛异常。
     * @details 外部输入不该靠异常否定一次调用：配置正文、请求正文都可能有对端写错的一天，
     *          而 nlohmann 缺省的异常版 parse 会把语法错误一路抛到调用栈顶上。这里走
     *          allow_exceptions=false 的入口，把它的 discarded 哨兵翻成 std::nullopt。
     * @param text 待解析文本，允许首尾空白
     * @param allowComments 是否容忍注释（配置文件那一路开，HTTP 正文按 RFC 8259 不开）
     * @return std::optional<ConfigValue> 解析结果；不合法时为空
     * @note 顶层标量（`42`、`"s"`、`true`、`null`）都是合法 JSON，照样解析成功——
     *       是不是「只接受对象」是使用方的判据，不在这一层拦
     */
    [[nodiscard]] inline std::optional<ConfigValue> parseConfigValue(const std::string_view text, const bool allowComments = false) noexcept
    {
        const ConfigValue parsed = ConfigValue::parse(text.begin(), text.end(), nullptr, false, allowComments);
        if (parsed.is_discarded())
        {
            return std::nullopt;
        }
        return parsed;
    }

    /**
     * @brief 判断一个值能否表示成合法的 JSON 文本
     * @details 非有限浮点（NaN、±Inf）单独判不可表示：nlohmann 的 dump 对它们缺省是**替换成 null**
     *          而不报错，那是静默变形——与 configValueAs() 同一条口径（宁可失败也不变形）。
     *          非 UTF-8 的字符串不在这里判：dump 的严格错误处理会抛出来，由 serializeConfigValue 接住。
     * @param value 待判定的值，递归检查数组与对象的每个子节点
     * @return true 可以安全序列化
     */
    [[nodiscard]] inline bool isJsonRepresentable(const ConfigValue &value)
    {
        if (value.is_number_float())
        {
            return std::isfinite(value.get<double>());
        }
        if (value.is_array() || value.is_object())
        {
            for (const ConfigValue &child: value)
            {
                if (!isJsonRepresentable(child))
                {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * @brief 把配置值序列化成 JSON 文本
     * @details 直接交出 nlohmann 的 dump 并不够用：NaN 与 ±Inf 会被它悄悄换成 null，
     *          而非 UTF-8 字符串会抛 type_error——前者是静默变形，后者是会打到调用栈上的异常。
     *          两种都在这里翻成「交回空」。
     * @param value 待序列化的值
     * @param indent 缩进空格数，负数（缺省）表示紧凑输出
     * @return std::optional<std::string> 文本；该值无法表示成合法 JSON 时为空
     */
    [[nodiscard]] inline std::optional<std::string> serializeConfigValue(const ConfigValue &value, const int indent = -1) noexcept
    {
        if (!isJsonRepresentable(value))
        {
            return std::nullopt;
        }
        try
        {
            return value.dump(indent);
        } catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

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
