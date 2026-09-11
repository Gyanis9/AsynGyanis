/**
 * @file ConfigValue.h
 * @brief 配置值类型封装，提供类型安全的访问接口
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValueType.h"
#include "Base/Exception/ConfigTypeException.h"

#include <concepts>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace AsynGyanis::Base
{
    class ConfigValue;

    using ConfigArray  = std::vector<ConfigValue>;                         ///< 配置数组：ConfigValue 有序列表
    using ConfigObject = std::map<std::string, ConfigValue, std::less<> >; ///< 配置对象：按键有序的嵌套映射

    // 为 ConfigArray / ConfigObject 特化 typeNameOf（必须在 ConfigValueType.h 主模板之后）
    template<>
    [[nodiscard]] inline const char *typeNameOf<ConfigArray>() noexcept
    {
        return "array";
    }

    template<>
    [[nodiscard]] inline const char *typeNameOf<ConfigObject>() noexcept
    {
        return "object";
    }

    /**
     * @brief 判断类型是否为 ConfigValue 底层变体的直接存储类型。
     * @details 取代原先按类型逐个特化 std::true_type 的写法，约束语义不变：
     *          只接受变体的精确替代类型，隐式可转换类型（如 int、const char *）不满足。
     * @tparam T 待判定的类型
     */
    template<typename T>
    concept ConfigVariantAlternative = std::same_as<T, std::nullptr_t> || std::same_as<T, bool> ||
                                       std::same_as<T, std::int64_t> || std::same_as<T, double> ||
                                       std::same_as<T, std::string> || std::same_as<T, ConfigArray> ||
                                       std::same_as<T, ConfigObject>;

    /**
     * @brief 配置值类
     *
     * 使用 std::variant 存储多种类型的配置值，提供类型安全的访问接口。
     * 支持以下类型：
     *   - std::nullptr_t (Null)
     *   - bool
     *   - int64_t (Int)
     *   - double (Double)
     *   - std::string (String)
     *   - ConfigArray (Array)
     *   - ConfigObject (Object)
     *
     * 访问方法分为两类：
     *   - as<T>()：强类型转换，类型不匹配时抛出异常
     *   - get<T>()：返回 std::optional<T>，类型不匹配时返回 std::nullopt
     */
    class ConfigValue
    {
    public:
        using VariantType = std::variant<
            std::nullptr_t, ///< Null
            bool,           ///< Bool
            int64_t,        ///< Int
            double,         ///< Double
            std::string,    ///< String
            ConfigArray,    ///< Array
            ConfigObject    ///< Object
        >;

        // 变体下标必须与 ConfigValueType 枚举值一一对应，type() 依赖该映射
        static_assert(std::is_same_v<std::variant_alternative_t<0, VariantType>, std::nullptr_t>);
        static_assert(std::is_same_v<std::variant_alternative_t<1, VariantType>, bool>);
        static_assert(std::is_same_v<std::variant_alternative_t<2, VariantType>, int64_t>);
        static_assert(std::is_same_v<std::variant_alternative_t<3, VariantType>, double>);
        static_assert(std::is_same_v<std::variant_alternative_t<4, VariantType>, std::string>);
        static_assert(std::is_same_v<std::variant_alternative_t<5, VariantType>, ConfigArray>);
        static_assert(std::is_same_v<std::variant_alternative_t<6, VariantType>, ConfigObject>);

        /**
         * @brief 构造空配置值（Null）。
         */
        ConfigValue() noexcept;

        /**
         * @brief 从空指针字面量构造 Null 配置值。
         * @param value 空指针字面量，仅用于选择重载。
         */
        explicit ConfigValue(std::nullptr_t value) noexcept;

        /**
         * @brief 从布尔值构造配置值。
         * @param value 布尔值。
         */
        explicit ConfigValue(bool value) noexcept;

        /**
         * @brief 从整型值构造配置值。
         * @param value 整型值，内部提升为 int64_t 存储。
         */
        explicit ConfigValue(int value) noexcept;

        /**
         * @brief 从 64 位整型构造配置值。
         * @param value 64 位整型值。
         */
        explicit ConfigValue(int64_t value) noexcept;

        /**
         * @brief 从双精度浮点值构造配置值。
         * @param value 浮点值。
         */
        explicit ConfigValue(double value) noexcept;

        /**
         * @brief 从 C 字符串构造配置值，空指针按空字符串处理。
         * @param value C 字符串。
         */
        explicit ConfigValue(const char *value);

        /**
         * @brief 从字符串构造配置值。
         * @param value 字符串值。
         */
        explicit ConfigValue(std::string value) noexcept;

        /**
         * @brief 从数组对象构造配置值。
         * @param value 数组值。
         */
        explicit ConfigValue(ConfigArray value) noexcept;

        /**
         * @brief 从对象映射构造配置值。
         * @param value 对象值。
         */
        explicit ConfigValue(ConfigObject value) noexcept;

        /**
         * @brief 拷贝构造函数。
         * @details 默认逐成员拷贝底层变体，配置值之间完全独立、不共享缓冲。
         * @param other 待拷贝的源配置值。
         */
        ConfigValue(const ConfigValue &other) = default;

        /**
         * @brief 移动构造函数。
         * @details 默认转移底层变体所有权，源对象退化为有效但未指定的状态。
         * @param other 待移源的配置值。
         */
        ConfigValue(ConfigValue &&other) noexcept = default;

        /**
         * @brief 拷贝赋值运算符。
         * @details 默认逐成员赋值，自赋值安全。
         * @param other 待拷贝的源配置值。
         * @return ConfigValue& 本对象引用。
         */
        ConfigValue &operator=(const ConfigValue &other) = default;

        /**
         * @brief 移动赋值运算符。
         * @details 默认转移底层变体后释放原值。
         * @param other 待移源的配置值。
         * @return ConfigValue& 本对象引用。
         */
        ConfigValue &operator=(ConfigValue &&other) noexcept = default;

        /**
         * @brief 析构函数。
         * @details 变体析构自动释放当前持有的字符串、数组或对象缓冲。
         */
        ~ConfigValue() = default;

        /**
         * @brief 获取值的类型
         * @return ConfigValueType 当前值的类型枚举。
         */
        [[nodiscard]] ConfigValueType type() const noexcept;

        /**
         * @brief 检查是否为指定类型
         * @tparam T 待检查的目标类型。
         * @return bool 类型匹配时返回 true。
         */
        template<typename T>
        [[nodiscard]] bool is() const noexcept
        {
            return std::holds_alternative<T>(m_value);
        }

        /**
         * @brief 判断当前值是否为 Null。
         * @return bool 为 Null 时返回 true。
         */
        [[nodiscard]] bool isNull() const noexcept;

        /**
         * @brief 判断值是否为空（Null、空字符串、空数组或空对象）。
         * @return bool 满足空语义时返回 true。
         */
        [[nodiscard]] bool empty() const noexcept;

        /**
         * @brief 以强类型方式访问底层值。
         * @tparam T 目标类型。
         * @return const T& 底层值常量引用。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        template<typename T>
        [[nodiscard]] const T &as() const
        {
            if (auto *pointer = std::get_if<T>(&m_value))
            {
                return *pointer;
            }
            throw ConfigTypeException("<unknown>", typeNameOf<T>(), typeName(type()));
        }

        /**
         * @brief 以强类型方式访问底层值（可变版本）。
         * @tparam T 目标类型。
         * @return T& 底层值可变引用。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        template<typename T>
        T &as()
        {
            if (auto *pointer = std::get_if<T>(&m_value))
            {
                return *pointer;
            }
            throw ConfigTypeException("<unknown>", typeNameOf<T>(), typeName(type()));
        }

        /**
         * @brief 将值转换为布尔类型。
         * @return bool 布尔值。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] bool asBool() const;

        /**
         * @brief 将值转换为整型。
         * @return int64_t 整型值。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] int64_t asInt() const;

        /**
         * @brief 将值转换为浮点类型。
         * @return double 浮点值。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] double asDouble() const;

        /**
         * @brief 将值转换为字符串类型。
         * @return const std::string& 字符串引用。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] const std::string &asString() const;

        /**
         * @brief 将值转换为数组类型。
         * @return const ConfigArray& 数组引用。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] const ConfigArray &asArray() const;

        /**
         * @brief 将值转换为对象类型。
         * @return const ConfigObject& 对象引用。
         * @throws ConfigTypeException 类型不匹配时抛出。
         */
        [[nodiscard]] const ConfigObject &asObject() const;

        /**
         * @brief 安全获取指定类型的值。
         * @tparam T 目标类型。
         * @return std::optional<std::decay_t<T>> 类型匹配时返回值，否则返回空。
         * @details 整型（bool 除外）统一映射到 int64_t，浮点统一映射到 double，
         *          其他类型仅当与底层变体存储类型一致时才可取出。
         */
        template<typename T>
        [[nodiscard]] std::optional<std::decay_t<T> > get() const noexcept
        {
            using Target = std::decay_t<T>;
            if constexpr (std::is_same_v<Target, bool>)
            {
                if (auto *pointer = std::get_if<bool>(&m_value))
                {
                    return *pointer;
                }
                return std::nullopt;
            } else if constexpr (std::is_integral_v<Target>)
            {
                if (auto *pointer = std::get_if<int64_t>(&m_value))
                {
                    return static_cast<Target>(*pointer);
                }
                return std::nullopt;
            } else if constexpr (std::is_floating_point_v<Target>)
            {
                if (auto *pointer = std::get_if<double>(&m_value))
                {
                    return static_cast<Target>(*pointer);
                }
                return std::nullopt;
            } else if constexpr (ConfigVariantAlternative<Target>)
            {
                if (auto *pointer = std::get_if<Target>(&m_value))
                {
                    return *pointer;
                }
                return std::nullopt;
            } else
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 安全获取布尔值。
         * @return std::optional<bool> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<bool> getBool() const noexcept;

        /**
         * @brief 安全获取整型值。
         * @return std::optional<int64_t> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<int64_t> getInt() const noexcept;

        /**
         * @brief 安全获取浮点值。
         * @return std::optional<double> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<double> getDouble() const noexcept;

        /**
         * @brief 安全获取字符串值。
         * @return std::optional<std::string> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<std::string> getString() const noexcept;

        /**
         * @brief 安全获取数组值。
         * @return std::optional<ConfigArray> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<ConfigArray> getArray() const noexcept;

        /**
         * @brief 安全获取对象值。
         * @return std::optional<ConfigObject> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<ConfigObject> getObject() const noexcept;

        /**
         * @brief 获取值，类型不匹配时返回默认值。
         * @tparam T 目标类型。
         * @param defaultValue 默认值。
         * @return std::decay_t<T> 匹配时的值或默认值。
         */
        template<typename T>
        [[nodiscard]] std::decay_t<T> valueOr(T &&defaultValue) const noexcept
        {
            using ValueType = std::decay_t<T>;
            return get<ValueType>().value_or(ValueType(std::forward<T>(defaultValue)));
        }

        /**
         * @brief 获取布尔值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return bool 布尔结果。
         */
        [[nodiscard]] bool boolOr(bool defaultValue) const noexcept;

        /**
         * @brief 获取整型值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return int64_t 整型结果。
         */
        [[nodiscard]] int64_t intOr(int64_t defaultValue) const noexcept;

        /**
         * @brief 获取浮点值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return double 浮点结果。
         */
        [[nodiscard]] double doubleOr(double defaultValue) const noexcept;

        /**
         * @brief 获取字符串值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return std::string 字符串结果。
         */
        [[nodiscard]] std::string stringOr(const std::string &defaultValue) const;

        /**
         * @brief 判断对象中是否包含指定键。
         * @param key 配置键。
         * @return bool 键存在返回 true，非对象类型返回 false。
         */
        [[nodiscard]] bool contains(std::string_view key) const noexcept;

        /**
         * @brief 通过键访问对象成员，不存在时抛出异常。
         * @param key 配置键。
         * @return const ConfigValue& 对应配置值引用。
         * @throws ConfigTypeException 当前值不是对象时抛出。
         * @throws ConfigKeyNotFoundException 键不存在时抛出。
         */
        const ConfigValue &operator[](std::string_view key) const;

        /**
         * @brief 安全通过键访问对象成员。
         * @param key 配置键。
         * @return std::optional<std::reference_wrapper<const ConfigValue>> 键存在时返回值引用。
         */
        [[nodiscard]] std::optional<std::reference_wrapper<const ConfigValue> > get(std::string_view key) const noexcept;

        /**
         * @brief 通过键安全访问子配置值并转换为指定类型
         * @tparam T 目标类型 (bool, int64_t, double, std::string, ConfigArray, ConfigObject)
         * @param key 配置键
         * @return std::optional<std::decay_t<T>> 如果键存在且类型匹配则返回值，否则返回 std::nullopt
         */
        template<typename T>
        [[nodiscard]] std::optional<std::decay_t<T> > get(std::string_view key) const noexcept
        {
            if (const auto optionalValue = get(key))
            {
                return optionalValue->get().get<T>();
            }
            return std::nullopt;
        }

        /**
         * @brief 通过索引访问数组元素，越界时抛出异常。
         * @param index 数组下标。
         * @return const ConfigValue& 对应元素引用。
         * @throws ConfigTypeException 当前值不是数组时抛出。
         * @throws ConfigKeyNotFoundException 下标越界时抛出。
         */
        const ConfigValue &operator[](size_t index) const;

        /**
         * @brief 获取当前值的大小语义（字符串长度、数组/对象元素数）。
         * @return size_t 语义大小，不适用类型返回 0。
         */
        [[nodiscard]] size_t size() const noexcept;

        /**
         * @brief 获取底层变体常量引用。
         * @return const VariantType& 变体常量引用。
         */
        [[nodiscard]] const VariantType &variant() const noexcept;

        /**
         * @brief 获取底层变体可变引用。
         * @return VariantType& 变体可变引用。
         */
        VariantType &variant() noexcept;

    private:
        VariantType m_value; ///< 配置值底层存储变体
    };

    /**
     * @brief 透明字符串哈希，统一 std::string 与 std::string_view 的哈希结果。
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

    /// 配置键值映射类型（透明哈希 + std::equal_to<> 异质查找，避免每次 get() 分配临时 string）
    using ConfigKeyValueMap = std::unordered_map<std::string, ConfigValue, TransparentStringHash, std::equal_to<>>;
} // namespace AsynGyanis::Base
