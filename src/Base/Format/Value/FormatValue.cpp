#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/ValueAccessError.h"

#include <charconv>
#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 把数组下标或索引转为错误文本中的 "[N]" 形式
         * @details 用 std::to_chars 而非 std::to_string，避免额外分配与本地化影响。
         * @param index 下标
         * @return std::string 形如 "[3]" 的描述
         */
        std::string describeIndex(const std::size_t index)
        {
            char       buffer[24];
            const auto [pointer, errorCode] = std::to_chars(buffer, buffer + sizeof(buffer), index);
            std::string description("[");
            if (errorCode == std::errc())
            {
                description.append(buffer, pointer);
            }
            description.push_back(']');
            return description;
        }

        /**
         * @brief 比较两个相同备选类型的文档值
         * @details 不依赖库的三路比较（std::string 在 C++20 中并未提供 operator<=>），
         *          逐类型显式实现，保证跨标准库行为一致；数组与对象按字典序比较
         *          （前缀完全等价时短者为小），与 std::vector / std::map 的语义一致。
         * @tparam Alternative 备选类型
         * @param left 左值
         * @param right 右值
         * @return std::partial_ordering 比较结果
         */
        template<typename Alternative>
        [[nodiscard]] std::partial_ordering compareSameAlternative(const Alternative &left, const Alternative &right) noexcept
        {
            if constexpr (std::is_same_v<Alternative, std::nullptr_t>)
            {
                // null 只有一种取值，任意两个 null 恒等价
                return std::partial_ordering::equivalent;
            } else if constexpr (std::is_same_v<Alternative, std::string>)
            {
                const int comparison = left.compare(right);
                return comparison < 0 ? std::partial_ordering::less : (comparison > 0 ? std::partial_ordering::greater : std::partial_ordering::equivalent);
            } else if constexpr (std::is_same_v<Alternative, double>)
            {
                if (left < right)
                {
                    return std::partial_ordering::less;
                }
                if (right < left)
                {
                    return std::partial_ordering::greater;
                }
                // 两个方向都不小于：相等则为等价，否则必有 NaN 参与
                return left == right ? std::partial_ordering::equivalent : std::partial_ordering::unordered;
            } else if constexpr (std::is_same_v<Alternative, FormatValueArray>)
            {
                auto leftIterator  = left.begin();
                auto rightIterator = right.begin();
                while (leftIterator != left.end() && rightIterator != right.end())
                {
                    if (const std::partial_ordering elementOrder = (*leftIterator <=> *rightIterator);
                        elementOrder != std::partial_ordering::equivalent)
                    {
                        return elementOrder;
                    }
                    ++leftIterator;
                    ++rightIterator;
                }
                // 前缀逐元素等价：字典序下短者为小，左序列仍有剩余元素即左者更大
                if (leftIterator != left.end())
                {
                    return std::partial_ordering::greater;
                }
                if (rightIterator != right.end())
                {
                    return std::partial_ordering::less;
                }
                return std::partial_ordering::equivalent;
            } else if constexpr (std::is_same_v<Alternative, FormatValueObject>)
            {
                auto leftIterator  = left.begin();
                auto rightIterator = right.begin();
                while (leftIterator != left.end() && rightIterator != right.end())
                {
                    // std::map 迭代序即键序，先比键名再比值
                    if (leftIterator->first != rightIterator->first)
                    {
                        return leftIterator->first < rightIterator->first ? std::partial_ordering::less : std::partial_ordering::greater;
                    }
                    if (const std::partial_ordering memberOrder = (leftIterator->second <=> rightIterator->second);
                        memberOrder != std::partial_ordering::equivalent)
                    {
                        return memberOrder;
                    }
                    ++leftIterator;
                    ++rightIterator;
                }
                // 键序列全等价：与数组同理按字典序，成员少者为小
                if (leftIterator != left.end())
                {
                    return std::partial_ordering::greater;
                }
                if (rightIterator != right.end())
                {
                    return std::partial_ordering::less;
                }
                return std::partial_ordering::equivalent;
            } else
            {
                // bool、int64_t、uint64_t：同类型直接比数值；bool 的 false < true 与整型语义一致
                return left < right ? std::partial_ordering::less : (right < left ? std::partial_ordering::greater : std::partial_ordering::equivalent);
            }
        }

        /**
         * @brief 比较两个底层变体（要求下标相同）
         * @details 按变体下标分派到 compareSameAlternative，避免 8×8 的 std::visit 展开，
         *          也无需依赖变体自身的三路比较是否可用。
         * @param left 左变体
         * @param right 右变体
         * @return std::partial_ordering 比较结果
         */
        [[nodiscard]] std::partial_ordering compareVariants(const FormatValue::VariantType &left, const FormatValue::VariantType &right) noexcept
        {
            switch (left.index())
            {
                case 0:
                    return compareSameAlternative(std::get<std::nullptr_t>(left), std::get<std::nullptr_t>(right));
                case 1:
                    return compareSameAlternative(std::get<bool>(left), std::get<bool>(right));
                case 2:
                    return compareSameAlternative(std::get<std::int64_t>(left), std::get<std::int64_t>(right));
                case 3:
                    return compareSameAlternative(std::get<double>(left), std::get<double>(right));
                case 4:
                    return compareSameAlternative(std::get<std::string>(left), std::get<std::string>(right));
                case 5:
                    return compareSameAlternative(std::get<FormatValueArray>(left), std::get<FormatValueArray>(right));
                case 6:
                    return compareSameAlternative(std::get<FormatValueObject>(left), std::get<FormatValueObject>(right));
                case 7:
                    return compareSameAlternative(std::get<std::uint64_t>(left), std::get<std::uint64_t>(right));
                default:
                    return std::partial_ordering::unordered;
            }
        }
    } // namespace

    FormatValue::FormatValue() noexcept :
        m_value(nullptr)
    {
    }

    FormatValue::FormatValue(std::nullptr_t) noexcept :
        m_value(nullptr)
    {
    }

    FormatValue::FormatValue(const bool value) noexcept :
        m_value(value)
    {
    }

    FormatValue::FormatValue(const int value) noexcept :
        m_value(static_cast<int64_t>(value))
    {
    }

    FormatValue::FormatValue(const int64_t value) noexcept :
        m_value(value)
    {
    }

    FormatValue::FormatValue(const std::uint64_t value) noexcept :
        m_value(value)
    {
    }

    FormatValue::FormatValue(const double value) noexcept :
        m_value(value)
    {
    }

    FormatValue::FormatValue(const char *value) :
        m_value(value != nullptr ? std::string(value) : std::string())
    {
    }

    FormatValue::FormatValue(std::string value) noexcept :
        m_value(std::move(value))
    {
    }

    FormatValue::FormatValue(FormatValueArray value) noexcept :
        m_value(std::move(value))
    {
    }

    FormatValue::FormatValue(FormatValueObject value) noexcept :
        m_value(std::move(value))
    {
    }

    bool FormatValue::operator==(const FormatValue &other) const
    {
        // 类型不同直接不等：Int(1) 与 Double(1.0)、Int(1) 与 UInt(1) 都不相等
        if (m_value.index() != other.m_value.index())
        {
            return false;
        }
        // 用三路比较的等价判定，保证 == 与 <=> 永远一致；NaN 参与时不可比 ⇒ 不相等
        return compareVariants(m_value, other.m_value) == std::partial_ordering::equivalent;
    }

    std::partial_ordering FormatValue::operator<=>(const FormatValue &other) const
    {
        // 跨类型只比类型序（变体下标，即 FormatValueType 枚举序），绝不比较不同类别的数值
        if (m_value.index() != other.m_value.index())
        {
            return m_value.index() <=> other.m_value.index();
        }
        return compareVariants(m_value, other.m_value);
    }

    FormatValueType FormatValue::type() const noexcept
    {
        switch (m_value.index())
        {
            case 0:
                return FormatValueType::Null;
            case 1:
                return FormatValueType::Bool;
            case 2:
                return FormatValueType::Int;
            case 3:
                return FormatValueType::Double;
            case 4:
                return FormatValueType::String;
            case 5:
                return FormatValueType::Array;
            case 6:
                return FormatValueType::Object;
            case 7:
                return FormatValueType::UInt;
            default:
                return FormatValueType::Null;
        }
    }

    bool FormatValue::isNull() const noexcept
    {
        return is<std::nullptr_t>();
    }

    bool FormatValue::isUInt() const noexcept
    {
        return is<std::uint64_t>();
    }

    bool FormatValue::isNumber() const noexcept
    {
        return isIntegralNumber() || isFloatingNumber();
    }

    bool FormatValue::isIntegralNumber() const noexcept
    {
        return is<std::int64_t>() || is<std::uint64_t>();
    }

    bool FormatValue::isFloatingNumber() const noexcept
    {
        return is<double>();
    }

    bool FormatValue::isBool() const noexcept
    {
        // 与 is<bool>() 等价：只认变体里的 bool 备选
        return is<bool>();
    }

    bool FormatValue::isString() const noexcept
    {
        return is<std::string>();
    }

    bool FormatValue::isArray() const noexcept
    {
        return is<FormatValueArray>();
    }

    bool FormatValue::isObject() const noexcept
    {
        return is<FormatValueObject>();
    }

    bool FormatValue::empty() const noexcept
    {
        return std::visit([]<typename Alternative>(const Alternative &value) -> bool
        {
            using Type = std::decay_t<Alternative>;
            if constexpr (std::is_same_v<Type, std::nullptr_t>)
            {
                return true;
            } else if constexpr (std::is_same_v<Type, std::string>)
            {
                return value.empty();
            } else if constexpr (std::is_same_v<Type, FormatValueArray>)
            {
                return value.empty();
            } else if constexpr (std::is_same_v<Type, FormatValueObject>)
            {
                return value.empty();
            } else
            {
                return false;
            }
        }, m_value);
    }

    bool FormatValue::asBool() const
    {
        return as<bool>();
    }

    int64_t FormatValue::asInt() const
    {
        return as<int64_t>();
    }

    std::uint64_t FormatValue::asUInt() const
    {
        return as<std::uint64_t>();
    }

    double FormatValue::asDouble() const
    {
        return as<double>();
    }

    const std::string &FormatValue::asString() const
    {
        return as<std::string>();
    }

    const FormatValueArray &FormatValue::asArray() const
    {
        return as<FormatValueArray>();
    }

    const FormatValueObject &FormatValue::asObject() const
    {
        return as<FormatValueObject>();
    }

    std::optional<bool> FormatValue::getBool() const noexcept
    {
        return get<bool>();
    }

    std::optional<int64_t> FormatValue::getInt() const noexcept
    {
        return get<int64_t>();
    }

    std::optional<std::uint64_t> FormatValue::getUInt() const noexcept
    {
        return get<std::uint64_t>();
    }

    std::optional<double> FormatValue::getDouble() const noexcept
    {
        return get<double>();
    }

    std::optional<std::string> FormatValue::getString() const noexcept
    {
        return get<std::string>();
    }

    std::optional<FormatValueArray> FormatValue::getArray() const noexcept
    {
        return get<FormatValueArray>();
    }

    std::optional<FormatValueObject> FormatValue::getObject() const noexcept
    {
        return get<FormatValueObject>();
    }

    const std::string *FormatValue::getStringView() const noexcept
    {
        return std::get_if<std::string>(&m_value);
    }

    std::string *FormatValue::getStringView() noexcept
    {
        return std::get_if<std::string>(&m_value);
    }

    const FormatValueArray *FormatValue::getArrayView() const noexcept
    {
        return std::get_if<FormatValueArray>(&m_value);
    }

    FormatValueArray *FormatValue::getArrayView() noexcept
    {
        return std::get_if<FormatValueArray>(&m_value);
    }

    const FormatValueObject *FormatValue::getObjectView() const noexcept
    {
        return std::get_if<FormatValueObject>(&m_value);
    }

    FormatValueObject *FormatValue::getObjectView() noexcept
    {
        return std::get_if<FormatValueObject>(&m_value);
    }

    bool FormatValue::boolOr(const bool defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    int64_t FormatValue::intOr(const int64_t defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    std::uint64_t FormatValue::uintOr(const std::uint64_t defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    double FormatValue::doubleOr(const double defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    std::string FormatValue::stringOr(const std::string &defaultValue) const
    {
        return valueOr(defaultValue);
    }

    bool FormatValue::contains(const std::string_view key) const noexcept
    {
        if (!isObject())
        {
            return false;
        }
        const auto &object = std::get<FormatValueObject>(m_value);
        return object.contains(key);
    }

    const FormatValue &FormatValue::operator[](const std::string_view key) const
    {
        const auto &object   = as<FormatValueObject>();
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            throw ValueAccessError::missingMember(std::string(key));
        }
        return iterator->second;
    }

    std::optional<std::reference_wrapper<const FormatValue> > FormatValue::get(const std::string_view key) const noexcept
    {
        if (!isObject())
        {
            return std::nullopt;
        }
        const auto &object   = std::get<FormatValueObject>(m_value);
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            return std::nullopt;
        }
        return std::cref(iterator->second);
    }

    const FormatValue &FormatValue::operator[](const size_t index) const
    {
        const auto &array = as<FormatValueArray>();
        if (index >= array.size())
        {
            throw ValueAccessError::missingMember(describeIndex(index));
        }
        return array[index];
    }

    FormatValue &FormatValue::operator[](const size_t index)
    {
        FormatValueArray &array = as<FormatValueArray>();
        // 刻意不自动扩容：越界读取必然抛错，避免「读越界反而改结构」
        if (index >= array.size())
        {
            throw ValueAccessError::missingMember(describeIndex(index));
        }
        return array[index];
    }

    FormatValue *FormatValue::find(const std::string_view key) noexcept
    {
        FormatValueObject *members = getObjectView();
        if (members == nullptr)
        {
            return nullptr;
        }
        const auto iterator = members->find(key);
        return iterator == members->end() ? nullptr : &iterator->second;
    }

    const FormatValue *FormatValue::find(const std::string_view key) const noexcept
    {
        const FormatValueObject *members = getObjectView();
        if (members == nullptr)
        {
            return nullptr;
        }
        const auto iterator = members->find(key);
        return iterator == members->end() ? nullptr : &iterator->second;
    }

    FormatValue *FormatValue::find(const size_t index) noexcept
    {
        FormatValueArray *elements = getArrayView();
        if (elements == nullptr || index >= elements->size())
        {
            return nullptr;
        }
        return &(*elements)[index];
    }

    const FormatValue *FormatValue::find(const size_t index) const noexcept
    {
        const FormatValueArray *elements = getArrayView();
        if (elements == nullptr || index >= elements->size())
        {
            return nullptr;
        }
        return &(*elements)[index];
    }

    FormatValue &FormatValue::set(const std::string_view key, FormatValue value)
    {
        // 非对象直接抛类型不匹配：变更类接口从不隐式改造当前值的类型
        FormatValueObject &members = as<FormatValueObject>();
        return members.insert_or_assign(std::string(key), std::move(value)).first->second;
    }

    bool FormatValue::erase(const std::string_view key) noexcept
    {
        FormatValueObject *members = getObjectView();
        if (members == nullptr)
        {
            return false;
        }
        // 先透明查找再按迭代器删除：C++20 的 map 尚无异构 erase(K)，这样可避免为临时键分配
        const auto iterator = members->find(key);
        if (iterator == members->end())
        {
            return false;
        }
        members->erase(iterator);
        return true;
    }

    bool FormatValue::erase(const size_t index) noexcept
    {
        FormatValueArray *elements = getArrayView();
        if (elements == nullptr || index >= elements->size())
        {
            return false;
        }
        elements->erase(elements->begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void FormatValue::pushBack(FormatValue value)
    {
        as<FormatValueArray>().push_back(std::move(value));
    }

    FormatValue &FormatValue::insert(const size_t index, FormatValue value)
    {
        FormatValueArray &elements = as<FormatValueArray>();
        // index == size() 等价于尾插；超过则越界，不自动补齐空位
        if (index > elements.size())
        {
            throw ValueAccessError::missingMember(describeIndex(index));
        }
        return *elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
    }

    FormatValue::ArrayIterator FormatValue::begin() noexcept
    {
        FormatValueArray *elements = getArrayView();
        // 非数组返回与 end() 相同的值初始化迭代器，构成合法空范围
        return elements != nullptr ? elements->begin() : ArrayIterator{};
    }

    FormatValue::ArrayIterator FormatValue::end() noexcept
    {
        FormatValueArray *elements = getArrayView();
        return elements != nullptr ? elements->end() : ArrayIterator{};
    }

    FormatValue::ArrayConstIterator FormatValue::begin() const noexcept
    {
        const FormatValueArray *elements = getArrayView();
        return elements != nullptr ? elements->begin() : ArrayConstIterator{};
    }

    FormatValue::ArrayConstIterator FormatValue::end() const noexcept
    {
        const FormatValueArray *elements = getArrayView();
        return elements != nullptr ? elements->end() : ArrayConstIterator{};
    }

    FormatValue::MemberRange FormatValue::members() noexcept
    {
        return MemberRange(getObjectView());
    }

    FormatValue::ConstMemberRange FormatValue::members() const noexcept
    {
        return ConstMemberRange(getObjectView());
    }

    FormatValue::MemberRange::MemberRange(FormatValueObject *object) noexcept :
        m_object(object)
    {
    }

    FormatValue::MemberIterator FormatValue::MemberRange::begin() const noexcept
    {
        return m_object != nullptr ? m_object->begin() : MemberIterator{};
    }

    FormatValue::MemberIterator FormatValue::MemberRange::end() const noexcept
    {
        return m_object != nullptr ? m_object->end() : MemberIterator{};
    }

    bool FormatValue::MemberRange::empty() const noexcept
    {
        return m_object == nullptr || m_object->empty();
    }

    std::size_t FormatValue::MemberRange::size() const noexcept
    {
        return m_object != nullptr ? m_object->size() : 0;
    }

    FormatValue::ConstMemberRange::ConstMemberRange(const FormatValueObject *object) noexcept :
        m_object(object)
    {
    }

    FormatValue::MemberConstIterator FormatValue::ConstMemberRange::begin() const noexcept
    {
        return m_object != nullptr ? m_object->begin() : MemberConstIterator{};
    }

    FormatValue::MemberConstIterator FormatValue::ConstMemberRange::end() const noexcept
    {
        return m_object != nullptr ? m_object->end() : MemberConstIterator{};
    }

    bool FormatValue::ConstMemberRange::empty() const noexcept
    {
        return m_object == nullptr || m_object->empty();
    }

    std::size_t FormatValue::ConstMemberRange::size() const noexcept
    {
        return m_object != nullptr ? m_object->size() : 0;
    }

    size_t FormatValue::size() const noexcept
    {
        // 判定统一走便捷方法（与 is<T>() 等价，省去模板实参）
        if (isArray())
        {
            return std::get<FormatValueArray>(m_value).size();
        }
        if (isObject())
        {
            return std::get<FormatValueObject>(m_value).size();
        }
        if (isString())
        {
            return std::get<std::string>(m_value).size();
        }
        return 0;
    }

    const FormatValue::VariantType &FormatValue::variant() const noexcept
    {
        return m_value;
    }

    FormatValue::VariantType &FormatValue::variant() noexcept
    {
        return m_value;
    }
} // namespace AsynGyanis::Base
