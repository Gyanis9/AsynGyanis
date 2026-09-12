/**
 * @file FormatValue.h
 * @brief 文档值类型封装，提供类型安全的访问接口、DOM 增删改查与遍历
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/Value/ValueAccessError.h"

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Base
{
    class FormatValue;

    using FormatValueArray  = std::vector<FormatValue>;                         ///< 文档数组：FormatValue 有序列表
    using FormatValueObject = std::map<std::string, FormatValue, std::less<> >; ///< 文档对象：按键有序的嵌套映射

    // 为 FormatValueArray / FormatValueObject 特化 typeNameOf（必须在 FormatValueType.h 主模板之后）
    template<>
    [[nodiscard]] constexpr const char *typeNameOf<FormatValueArray>() noexcept
    {
        return "array";
    }

    template<>
    [[nodiscard]] constexpr const char *typeNameOf<FormatValueObject>() noexcept
    {
        return "object";
    }

    /**
     * @brief 判断类型是否为 FormatValue 底层变体的直接存储类型。
     * @details 只接受变体的精确替代类型，隐式可转换类型（如 int、const char *）不满足；
     *          新增 UInt 后本概念同步追加 uint64_t。
     * @tparam T 待判定的类型
     */
    template<typename T>
    concept FormatValueAlternative = std::same_as<T, std::nullptr_t> ||
                                     std::same_as<T, bool> ||
                                     std::same_as<T, std::int64_t> ||
                                     std::same_as<T, std::uint64_t> ||
                                     std::same_as<T, double> ||
                                     std::same_as<T, std::string> ||
                                     std::same_as<T, FormatValueArray> ||
                                     std::same_as<T, FormatValueObject>;

    /**
     * @brief 文档值类
     *
     * 使用 std::variant 存储多种类型的文档值，提供类型安全的访问接口。
     * 支持以下类型（下标与 FormatValueType 一一对应）：
     *   - 0 std::nullptr_t (Null)
     *   - 1 bool (Bool)
     *   - 2 int64_t (Int)
     *   - 3 double (Double)
     *   - 4 std::string (String)
     *   - 5 FormatValueArray (Array)
     *   - 6 FormatValueObject (Object)
     *   - 7 std::uint64_t (UInt)：追加在末尾，既有 7 个下标与语义完全不变
     *
     * 访问方法分为三类：
     *   - as&lt;T&gt;() / asXxx()：强类型精确取用，类型不匹配抛 ValueAccessError
     *   - get&lt;T&gt;() / getXxx()：返回 std::optional，类型不匹配返回 std::nullopt
     *   - xxxView()：返回内部容器指针，零拷贝，非目标类型返回 nullptr
     *
     * 类型判定命名（便捷方法，语义与 is&lt;T&gt;() 完全一致）：
     *   - isNull()：仅 Null
     *   - isBool()：仅 Bool
     *   - isString()：仅 String
     *   - isArray()：仅 Array
     *   - isObject()：仅 Object
     *   - isUInt()：仅 UInt
     *
     * 数值判定命名（新增）：
     *   - isNumber()：Int、UInt 或 Double
     *   - isIntegralNumber()：Int 或 UInt
     *   - isFloatingNumber()：Double
     *
     * 数值取用规则（刻意不做全类型互通，避免精度陷阱）：
     *   - get&lt;std::uint64_t&gt;() / getUInt()：优先精确取 UInt；持有非负 Int 时可无损加宽取出
     *   - get&lt;std::int64_t&gt;() / getInt()：优先精确取 Int；持有不超过 INT64_MAX 的 UInt 时可无损取出
     *   - 更窄的整型目标（int、uint32_t 等）：沿用既有语义，只从 Int 取，不做隐式类别转换
     *   - 整数与浮点之间、bool 与数值之间：永不互相转换
     *
     * 比较规则（新增，按值语义）：
     *   - 类型不同 ⇒ 不相等；三路比较按变体下标（即 FormatValueType 枚举序）排序
     *   - 类型相同 ⇒ 按值比较；数组按元素字典序、对象按键再按值的字典序，前缀等价时短者为小
     *   - 含 NaN 的 double 与任何值都不可比（partial_ordering::unordered），
     *     因此相等判定对 NaN 恒为 false
     *   - Int(1) 与 Double(1.0)、Int(1) 与 UInt(1) 既不相等也不比大小，杜绝跨类型精度陷阱
     */
    class FormatValue
    {
    public:
        using VariantType = std::variant<
            std::nullptr_t,    ///< Null
            bool,              ///< Bool
            int64_t,           ///< Int
            double,            ///< Double
            std::string,       ///< String
            FormatValueArray,  ///< Array
            FormatValueObject, ///< Object
            std::uint64_t      ///< UInt
        >;

        using ArrayIterator       = FormatValueArray::iterator;        ///< 数组元素可写迭代器
        using ArrayConstIterator  = FormatValueArray::const_iterator;  ///< 数组元素只读迭代器
        using MemberIterator      = FormatValueObject::iterator;       ///< 对象成员可写迭代器
        using MemberConstIterator = FormatValueObject::const_iterator; ///< 对象成员只读迭代器

        // 变体下标必须与 FormatValueType 枚举值一一对应，type() 依赖该映射
        static_assert(std::is_same_v<std::variant_alternative_t<0, VariantType>, std::nullptr_t>);
        static_assert(std::is_same_v<std::variant_alternative_t<1, VariantType>, bool>);
        static_assert(std::is_same_v<std::variant_alternative_t<2, VariantType>, int64_t>);
        static_assert(std::is_same_v<std::variant_alternative_t<3, VariantType>, double>);
        static_assert(std::is_same_v<std::variant_alternative_t<4, VariantType>, std::string>);
        static_assert(std::is_same_v<std::variant_alternative_t<5, VariantType>, FormatValueArray>);
        static_assert(std::is_same_v<std::variant_alternative_t<6, VariantType>, FormatValueObject>);
        static_assert(std::is_same_v<std::variant_alternative_t<7, VariantType>, std::uint64_t>);

        /**
         * @brief 对象成员可写遍历视图
         *
         * @details 供 range-for 直接遍历对象成员，元素为
         *          std::pair&lt;const std::string, FormatValue&gt;，可结构化绑定为
         *          (const std::string &amp;, FormatValue &amp;)。视图不持有数据，仅引用
         *          所属 FormatValue；当其不是对象时 begin() 与 end() 都是值初始化的
         *          迭代器，比较相等即空范围，跨类型遍历不会触碰未构造的容器。
         */
        class MemberRange
        {
        public:
            /**
             * @brief 获取首成员迭代器
             * @return MemberIterator 首成员；非对象时返回值初始化的空迭代器
             */
            [[nodiscard]] MemberIterator begin() const noexcept;

            /**
             * @brief 获取尾后迭代器
             * @return MemberIterator 尾后位置；非对象时返回值初始化的空迭代器
             */
            [[nodiscard]] MemberIterator end() const noexcept;

            /**
             * @brief 判断视图是否为空
             * @return bool 非对象或无成员时返回 true
             */
            [[nodiscard]] bool empty() const noexcept;

            /**
             * @brief 获取成员数量
             * @return std::size_t 成员数；非对象时为 0
             */
            [[nodiscard]] std::size_t size() const noexcept;

        private:
            /**
             * @brief 绑定目标对象构造视图
             * @param object 目标对象指针，非对象时传 nullptr
             */
            explicit MemberRange(FormatValueObject *object) noexcept;

            FormatValueObject *m_object; ///< 目标对象，当前值不是对象时为 nullptr

            friend class FormatValue;
        };

        /**
         * @brief 对象成员只读遍历视图
         *
         * @details 与 MemberRange 语义一致，但只暴露只读迭代器，元素为
         *          std::pair&lt;const std::string, const FormatValue&gt;。
         */
        class ConstMemberRange
        {
        public:
            /**
             * @brief 获取首成员只读迭代器
             * @return MemberConstIterator 首成员；非对象时返回值初始化的空迭代器
             */
            [[nodiscard]] MemberConstIterator begin() const noexcept;

            /**
             * @brief 获取尾后只读迭代器
             * @return MemberConstIterator 尾后位置；非对象时返回值初始化的空迭代器
             */
            [[nodiscard]] MemberConstIterator end() const noexcept;

            /**
             * @brief 判断视图是否为空
             * @return bool 非对象或无成员时返回 true
             */
            [[nodiscard]] bool empty() const noexcept;

            /**
             * @brief 获取成员数量
             * @return std::size_t 成员数；非对象时为 0
             */
            [[nodiscard]] std::size_t size() const noexcept;

        private:
            /**
             * @brief 绑定目标对象构造只读视图
             * @param object 目标对象指针，非对象时传 nullptr
             */
            explicit ConstMemberRange(const FormatValueObject *object) noexcept;

            const FormatValueObject *m_object; ///< 目标对象，当前值不是对象时为 nullptr

            friend class FormatValue;
        };

        /**
         * @brief 构造空文档值（Null）。
         */
        FormatValue() noexcept;

        /**
         * @brief 从空指针字面量构造 Null 文档值。
         * @param value 空指针字面量，仅用于选择重载。
         */
        explicit FormatValue(std::nullptr_t value) noexcept;

        /**
         * @brief 从布尔值构造文档值。
         * @param value 布尔值。
         */
        explicit FormatValue(bool value) noexcept;

        /**
         * @brief 从整型值构造文档值。
         * @param value 整型值，内部提升为 int64_t 存储。
         */
        explicit FormatValue(int value) noexcept;

        /**
         * @brief 从 64 位有符号整型构造文档值。
         * @param value 64 位有符号整型值。
         */
        explicit FormatValue(int64_t value) noexcept;

        /**
         * @brief 从 64 位无符号整型构造文档值。
         * @details 精确构造 UInt 备选；与 Int 是不同类型，asInt() 之类的精确取用会失败，
         *          需要宽窄互通时请用 getInt()/getUInt()。
         * @param value 64 位无符号整型值。
         */
        explicit FormatValue(std::uint64_t value) noexcept;

        /**
         * @brief 从其它无符号整型构造文档值（UInt）。
         * @details 若没有本重载，unsigned int、size_t（Linux 下为 unsigned long）等类型会在
         *          int/int64_t/uint64_t/double 四个构造间产生二义性，调用点必须到处写
         *          static_cast；此处用受约束模板精确匹配所有无符号整型，统一按 UInt 存储。
         * @tparam UnsignedInteger 无符号整型（bool 与 uint64_t 由上面的非模板重载接管）
         * @param value 无符号整型值。
         */
        template<typename UnsignedInteger>
            requires std::unsigned_integral<UnsignedInteger> &&
                     (!std::same_as<UnsignedInteger, std::uint64_t>) &&
                     (!std::same_as<UnsignedInteger, bool>)
        explicit FormatValue(UnsignedInteger value) noexcept :
            m_value(static_cast<std::uint64_t>(value))
        {
        }

        /**
         * @brief 从双精度浮点值构造文档值。
         * @param value 浮点值。
         */
        explicit FormatValue(double value) noexcept;

        /**
         * @brief 从 C 字符串构造文档值，空指针按空字符串处理。
         * @param value C 字符串。
         */
        explicit FormatValue(const char *value);

        /**
         * @brief 从字符串构造文档值。
         * @param value 字符串值。
         */
        explicit FormatValue(std::string value) noexcept;

        /**
         * @brief 从数组对象构造文档值。
         * @param value 数组值。
         */
        explicit FormatValue(FormatValueArray value) noexcept;

        /**
         * @brief 从对象映射构造文档值。
         * @param value 对象值。
         */
        explicit FormatValue(FormatValueObject value) noexcept;

        /**
         * @brief 拷贝构造函数。
         * @details 默认逐成员拷贝底层变体，文档值之间完全独立、不共享缓冲。
         * @param other 待拷贝的源文档值。
         */
        FormatValue(const FormatValue &other) = default;

        /**
         * @brief 移动构造函数。
         * @details 默认转移底层变体所有权，源对象退化为有效但未指定的状态。
         * @param other 待移源的文档值。
         */
        FormatValue(FormatValue &&other) noexcept = default;

        /**
         * @brief 拷贝赋值运算符。
         * @details 默认逐成员赋值，自赋值安全。
         * @param other 待拷贝的源文档值。
         * @return FormatValue& 本对象引用。
         */
        FormatValue &operator=(const FormatValue &other) = default;

        /**
         * @brief 移动赋值运算符。
         * @details 默认转移底层变体后释放原值。
         * @param other 待移源的文档值。
         * @return FormatValue& 本对象引用。
         */
        FormatValue &operator=(FormatValue &&other) noexcept = default;

        /**
         * @brief 析构函数。
         * @details 变体析构自动释放当前持有的字符串、数组或对象缓冲。
         */
        ~FormatValue() = default;

        /**
         * @brief 按值语义判断两个文档值是否相等
         * @details 类型不同一律不相等（Int(1) 不等于 Double(1.0)，也不等于 UInt(1)）；
         *          类型相同则逐层按值比较。含 NaN 的 double 与任何值都不相等，
         *          与 IEEE 754 及 partial_ordering 的语义一致。
         * @param other 待比较的文档值
         * @return bool 类型相同且值相等时返回 true
         */
        [[nodiscard]] bool operator==(const FormatValue &other) const;

        /**
         * @brief 按值语义进行三路比较
         * @details 类型不同时只比较类型序（变体下标，即 FormatValueType 枚举序），
         *          绝不跨类型比较数值，因此不存在 int 与 double 混比的精度陷阱；
         *          类型相同时递归比较内容：数组按元素字典序、对象按键再按值的字典序，
         *          前缀完全等价时元素或成员少者更小（与 std::vector / std::map 一致）。
         *          含 NaN 时返回 std::partial_ordering::unordered。
         * @param other 待比较的文档值
         * @return std::partial_ordering 比较结果
         * @note 结果类型是 partial_ordering：含 NaN 的值不构成全序，不能直接用于
         *       std::sort 之类的严格弱序场景。
         */
        [[nodiscard]] std::partial_ordering operator<=>(const FormatValue &other) const;

        /**
         * @brief 获取值的类型
         * @return FormatValueType 当前值的类型枚举。
         */
        [[nodiscard]] FormatValueType type() const noexcept;

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
         * @brief 判断当前值是否为 UInt（无符号 64 位整数）。
         * @return bool 持有 uint64_t 备选时返回 true。
         */
        [[nodiscard]] bool isUInt() const noexcept;

        /**
         * @brief 判断当前值是否为任意数值（Int、UInt 或 Double）。
         * @details 仅做类型类别判定，不涉及取值范围；bool 不算数值。
         * @return bool 持有 int64_t、uint64_t 或 double 时返回 true。
         */
        [[nodiscard]] bool isNumber() const noexcept;

        /**
         * @brief 判断当前值是否为整数数值（Int 或 UInt）。
         * @return bool 持有 int64_t 或 uint64_t 时返回 true。
         */
        [[nodiscard]] bool isIntegralNumber() const noexcept;

        /**
         * @brief 判断当前值是否为浮点数值。
         * @return bool 持有 double 时返回 true。
         */
        [[nodiscard]] bool isFloatingNumber() const noexcept;

        /**
         * @brief 判断当前值是否为布尔值。
         * @details 仅做类型判定，不做真假值转换：Int(0)、空字符串等假值都不是 Bool。
         * @return bool 持有 bool 时返回 true。
         */
        [[nodiscard]] bool isBool() const noexcept;

        /**
         * @brief 判断当前值是否为字符串。
         * @return bool 持有 std::string 时返回 true。
         */
        [[nodiscard]] bool isString() const noexcept;

        /**
         * @brief 判断当前值是否为数组。
         * @details 与 is&lt;FormatValueArray&gt;() 等价，便于调用点省去显式模板参数。
         * @return bool 持有 FormatValueArray 时返回 true。
         */
        [[nodiscard]] bool isArray() const noexcept;

        /**
         * @brief 判断当前值是否为对象。
         * @details 与 is&lt;FormatValueObject&gt;() 等价，便于调用点省去显式模板参数。
         * @return bool 持有 FormatValueObject 时返回 true。
         */
        [[nodiscard]] bool isObject() const noexcept;

        /**
         * @brief 判断值是否为空（Null、空字符串、空数组或空对象）。
         * @return bool 满足空语义时返回 true。
         */
        [[nodiscard]] bool empty() const noexcept;

        /**
         * @brief 以强类型方式访问底层值。
         * @tparam T 目标类型。
         * @return const T& 底层值常量引用。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        template<typename T>
        [[nodiscard]] const T &as() const
        {
            if (auto *pointer = std::get_if<T>(&m_value))
            {
                return *pointer;
            }
            throw ValueAccessError("<unknown>", typeNameOf<T>(), typeName(type()));
        }

        /**
         * @brief 以强类型方式访问底层值（可变版本）。
         * @tparam T 目标类型。
         * @return T& 底层值可变引用。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        template<typename T>
        T &as()
        {
            if (auto *pointer = std::get_if<T>(&m_value))
            {
                return *pointer;
            }
            throw ValueAccessError("<unknown>", typeNameOf<T>(), typeName(type()));
        }

        /**
         * @brief 将值转换为布尔类型。
         * @return bool 布尔值。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] bool asBool() const;

        /**
         * @brief 将值转换为有符号整型。
         * @return int64_t 整型值。
         * @throws ValueAccessError 类型不匹配时抛出（持有 UInt 也视为不匹配）。
         */
        [[nodiscard]] int64_t asInt() const;

        /**
         * @brief 将值转换为无符号整型。
         * @details 精确取 UInt；持有 Int、Double 等其它类型时抛异常，
         *          需要宽松取用时请改用 getUInt()/uintOr()。
         * @return std::uint64_t 无符号整型值。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] std::uint64_t asUInt() const;

        /**
         * @brief 将值转换为浮点类型。
         * @return double 浮点值。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] double asDouble() const;

        /**
         * @brief 将值转换为字符串类型。
         * @return const std::string& 字符串引用。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] const std::string &asString() const;

        /**
         * @brief 将值转换为数组类型。
         * @return const FormatValueArray& 数组引用。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] const FormatValueArray &asArray() const;

        /**
         * @brief 将值转换为对象类型。
         * @return const FormatValueObject& 对象引用。
         * @throws ValueAccessError 类型不匹配时抛出。
         */
        [[nodiscard]] const FormatValueObject &asObject() const;

        /**
         * @brief 安全获取指定类型的值。
         * @tparam T 目标类型。
         * @return std::optional<std::decay_t<T>> 类型匹配时返回值，否则返回空。
         * @details 本重载为兼容既有契约保留按值返回（std::string、数组、对象会被拷贝）；
         *          需要零拷贝时用 getStringView()/getArrayView()/getObjectView()。
         *          取用规则：
         *          - 目标 uint64_t：精确取 UInt；持有非负 Int 时无损加宽取出
         *          - 目标 int64_t：精确取 Int；持有不超过 INT64_MAX 的 UInt 时无损取出
         *          - 其它整型（bool 除外）：仅从 Int 取并按既有语义窄化
         *          - 浮点目标：仅从 Double 取
         *          - 其它类型：仅当与底层变体存储类型一致时才可取
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
            } else if constexpr (std::is_same_v<Target, std::uint64_t>)
            {
                if (auto *pointer = std::get_if<std::uint64_t>(&m_value))
                {
                    return *pointer;
                }
                // 非负 Int 到 UInt 是无损加宽，唯一允许的整数家族内互通
                if (auto *pointer = std::get_if<std::int64_t>(&m_value); pointer != nullptr && *pointer >= 0)
                {
                    return static_cast<std::uint64_t>(*pointer);
                }
                return std::nullopt;
            } else if constexpr (std::is_same_v<Target, std::int64_t>)
            {
                if (auto *pointer = std::get_if<std::int64_t>(&m_value))
                {
                    return *pointer;
                }
                // 落在 int64 正数范围内的 UInt 同样是无损取用
                if (auto *pointer = std::get_if<std::uint64_t>(&m_value); pointer != nullptr && *pointer <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    return static_cast<std::int64_t>(*pointer);
                }
                return std::nullopt;
            } else if constexpr (std::is_integral_v<Target>)
            {
                if (auto *pointer = std::get_if<std::int64_t>(&m_value))
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
            } else if constexpr (FormatValueAlternative<Target>)
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
         * @brief 安全获取有符号整型值。
         * @return std::optional<int64_t> 匹配时返回值，否则返回空（UInt 超范围时也返回空）。
         */
        [[nodiscard]] std::optional<int64_t> getInt() const noexcept;

        /**
         * @brief 安全获取无符号整型值。
         * @return std::optional<std::uint64_t> 匹配时返回值；持有非负 Int 时无损加宽返回。
         */
        [[nodiscard]] std::optional<std::uint64_t> getUInt() const noexcept;

        /**
         * @brief 安全获取浮点值。
         * @return std::optional<double> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<double> getDouble() const noexcept;

        /**
         * @brief 安全获取字符串值。
         * @details 与既有契约一致按值返回，会拷贝字符串；零拷贝请用 getStringView()。
         * @return std::optional<std::string> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<std::string> getString() const noexcept;

        /**
         * @brief 安全获取数组值。
         * @details 与既有契约一致按值返回，会深拷贝整个数组；零拷贝请用 getArrayView()。
         * @return std::optional<FormatValueArray> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<FormatValueArray> getArray() const noexcept;

        /**
         * @brief 安全获取对象值。
         * @details 与既有契约一致按值返回，会深拷贝整个对象；零拷贝请用 getObjectView()。
         * @return std::optional<FormatValueObject> 类型匹配时返回值，否则返回空。
         */
        [[nodiscard]] std::optional<FormatValueObject> getObject() const noexcept;

        /**
         * @brief 获取字符串内部缓冲指针（零拷贝）。
         * @return const std::string* 字符串指针；当前值不是字符串时返回 nullptr。
         */
        [[nodiscard]] const std::string *getStringView() const noexcept;

        /**
         * @brief 获取字符串内部缓冲指针（零拷贝，可写）。
         * @return std::string* 字符串指针；当前值不是字符串时返回 nullptr。
         */
        [[nodiscard]] std::string *getStringView() noexcept;

        /**
         * @brief 获取数组内部缓冲指针（零拷贝）。
         * @return const FormatValueArray* 数组指针；当前值不是数组时返回 nullptr。
         */
        [[nodiscard]] const FormatValueArray *getArrayView() const noexcept;

        /**
         * @brief 获取数组内部缓冲指针（零拷贝，可写）。
         * @return FormatValueArray* 数组指针；当前值不是数组时返回 nullptr。
         */
        [[nodiscard]] FormatValueArray *getArrayView() noexcept;

        /**
         * @brief 获取对象内部映射指针（零拷贝）。
         * @return const FormatValueObject* 对象指针；当前值不是对象时返回 nullptr。
         */
        [[nodiscard]] const FormatValueObject *getObjectView() const noexcept;

        /**
         * @brief 获取对象内部映射指针（零拷贝，可写）。
         * @return FormatValueObject* 对象指针；当前值不是对象时返回 nullptr。
         */
        [[nodiscard]] FormatValueObject *getObjectView() noexcept;

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
         * @brief 获取有符号整型值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return int64_t 整型结果。
         */
        [[nodiscard]] int64_t intOr(int64_t defaultValue) const noexcept;

        /**
         * @brief 获取无符号整型值，类型不匹配时返回默认值。
         * @param defaultValue 默认值。
         * @return std::uint64_t 无符号整型结果。
         */
        [[nodiscard]] std::uint64_t uintOr(std::uint64_t defaultValue) const noexcept;

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
         * @return const FormatValue& 对应文档值引用。
         * @throws ValueAccessError 当前值不是对象时抛出。
         * @throws ValueAccessError 键不存在时抛出。
         */
        const FormatValue &operator[](std::string_view key) const;

        /**
         * @brief 安全通过键访问对象成员。
         * @param key 配置键。
         * @return std::optional<std::reference_wrapper<const FormatValue>> 键存在时返回值引用。
         */
        [[nodiscard]] std::optional<std::reference_wrapper<const FormatValue> > get(std::string_view key) const noexcept;

        /**
         * @brief 通过键安全访问子文档值并转换为指定类型
         * @tparam T 目标类型 (bool, int64_t, uint64_t, double, std::string, FormatValueArray, FormatValueObject)
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
         * @brief 按索引访问数组元素，越界时抛出异常。
         * @param index 数组下标。
         * @return const FormatValue& 对应元素引用。
         * @throws ValueAccessError 当前值不是数组时抛出。
         * @throws ValueAccessError 下标越界时抛出。
         */
        const FormatValue &operator[](size_t index) const;

        /**
         * @brief 按索引访问数组元素（可写），越界时抛出异常。
         * @details 与只读版本语义完全一致：不存在的下标一律抛异常，绝不自动扩容，
         *          避免「读一个不存在的元素反而把数组撑大」这类隐蔽缺陷。
         * @param index 数组下标。
         * @return FormatValue& 对应元素引用。
         * @throws ValueAccessError 当前值不是数组时抛出。
         * @throws ValueAccessError 下标越界时抛出。
         */
        FormatValue &operator[](size_t index);

        /**
         * @brief 查找对象成员，不抛异常也不创建新成员。
         * @param key 成员键。
         * @return FormatValue* 成员指针；键不存在或当前值不是对象时返回 nullptr。
         */
        [[nodiscard]] FormatValue *find(std::string_view key) noexcept;

        /**
         * @brief 查找对象成员（只读），不抛异常也不创建新成员。
         * @param key 成员键。
         * @return const FormatValue* 成员指针；键不存在或当前值不是对象时返回 nullptr。
         */
        [[nodiscard]] const FormatValue *find(std::string_view key) const noexcept;

        /**
         * @brief 查找数组元素，不抛异常。
         * @param index 数组下标。
         * @return FormatValue* 元素指针；越界或当前值不是数组时返回 nullptr。
         */
        [[nodiscard]] FormatValue *find(size_t index) noexcept;

        /**
         * @brief 查找数组元素（只读），不抛异常。
         * @param index 数组下标。
         * @return const FormatValue* 元素指针；越界或当前值不是数组时返回 nullptr。
         */
        [[nodiscard]] const FormatValue *find(size_t index) const noexcept;

        /**
         * @brief 写入对象成员：键不存在则插入，存在则覆盖。
         * @param key 成员键。
         * @param value 待写入的值（按值传入，内部移动）。
         * @return FormatValue& 写入后的成员引用，便于链式修改。
         * @throws ValueAccessError 当前值不是对象时抛出（不会隐式把当前值改造成对象）。
         */
        FormatValue &set(std::string_view key, FormatValue value);

        /**
         * @brief 删除对象成员，不抛异常。
         * @param key 成员键。
         * @return bool 删除成功返回 true；键不存在或当前值不是对象时返回 false。
         */
        bool erase(std::string_view key) noexcept;

        /**
         * @brief 删除数组元素（后续元素整体前移），不抛异常。
         * @param index 数组下标。
         * @return bool 删除成功返回 true；越界或当前值不是数组时返回 false。
         */
        bool erase(size_t index) noexcept;

        /**
         * @brief 向数组尾部追加元素。
         * @param value 待追加的值（按值传入，内部移动）。
         * @throws ValueAccessError 当前值不是数组时抛出。
         */
        void pushBack(FormatValue value);

        /**
         * @brief 在数组指定位置插入元素，后续元素整体后移。
         * @details index 等于当前元素个数时等价于 pushBack，允许作为「追加」使用；
         *          index 大于元素个数属于越界，直接抛异常，不自动补齐空位。
         * @param index 插入位置，取值范围 [0, size()]。
         * @param value 待插入的值（按值传入，内部移动）。
         * @return FormatValue& 插入后的元素引用。
         * @throws ValueAccessError 当前值不是数组时抛出。
         * @throws ValueAccessError index 大于元素个数时抛出。
         */
        FormatValue &insert(size_t index, FormatValue value);

        /**
         * @brief 获取数组首元素的迭代器。
         * @details 仅遍历数组元素；当前值不是数组时返回值初始化的迭代器，与 end() 相等，
         *          形成合法空范围，因此对任意类型做 range-for 都是安全的。
         * @return ArrayIterator 首元素或空迭代器。
         */
        [[nodiscard]] ArrayIterator begin() noexcept;

        /**
         * @brief 获取数组尾后迭代器。
         * @return ArrayIterator 尾后位置；当前值不是数组时返回值初始化的迭代器。
         */
        [[nodiscard]] ArrayIterator end() noexcept;

        /**
         * @brief 获取数组首元素的只读迭代器。
         * @return ArrayConstIterator 首元素或空迭代器。
         */
        [[nodiscard]] ArrayConstIterator begin() const noexcept;

        /**
         * @brief 获取数组尾后只读迭代器。
         * @return ArrayConstIterator 尾后位置；当前值不是数组时返回值初始化的迭代器。
         */
        [[nodiscard]] ArrayConstIterator end() const noexcept;

        /**
         * @brief 获取对象成员可写遍历视图。
         * @details 元素为 (const std::string &, FormatValue &)，可直接结构化绑定；
         *          当前值不是对象时返回空范围，遍历安全。
         * @return MemberRange 成员视图（不持有数据，仅引用本值内部的映射）。
         */
        [[nodiscard]] MemberRange members() noexcept;

        /**
         * @brief 获取对象成员只读遍历视图。
         * @return ConstMemberRange 只读成员视图；当前值不是对象时为空范围。
         */
        [[nodiscard]] ConstMemberRange members() const noexcept;

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
        VariantType m_value; ///< 文档值底层存储变体
    };
} // namespace AsynGyanis::Base
