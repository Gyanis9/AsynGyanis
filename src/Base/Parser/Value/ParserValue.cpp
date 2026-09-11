#include "Base/Parser/Value/ParserValue.h"
#include "Base/Parser/Value/ValueAccessError.h"

namespace AsynGyanis::Base
{
    ParserValue::ParserValue() noexcept :
        m_value(nullptr)
    {
    }

    ParserValue::ParserValue(std::nullptr_t) noexcept :
        m_value(nullptr)
    {
    }

    ParserValue::ParserValue(const bool value) noexcept :
        m_value(value)
    {
    }

    ParserValue::ParserValue(const int value) noexcept :
        m_value(static_cast<int64_t>(value))
    {
    }

    ParserValue::ParserValue(const int64_t value) noexcept :
        m_value(value)
    {
    }

    ParserValue::ParserValue(const double value) noexcept :
        m_value(value)
    {
    }

    ParserValue::ParserValue(const char *value) :
        m_value(value != nullptr ? std::string(value) : std::string())
    {
    }

    ParserValue::ParserValue(std::string value) noexcept :
        m_value(std::move(value))
    {
    }

    ParserValue::ParserValue(ParserValueArray value) noexcept :
        m_value(std::move(value))
    {
    }

    ParserValue::ParserValue(ParserValueObject value) noexcept :
        m_value(std::move(value))
    {
    }

    ParserValueType ParserValue::type() const noexcept
    {
        switch (m_value.index())
        {
            case 0:
                return ParserValueType::Null;
            case 1:
                return ParserValueType::Bool;
            case 2:
                return ParserValueType::Int;
            case 3:
                return ParserValueType::Double;
            case 4:
                return ParserValueType::String;
            case 5:
                return ParserValueType::Array;
            case 6:
                return ParserValueType::Object;
            default:
                return ParserValueType::Null;
        }
    }

    bool ParserValue::isNull() const noexcept
    {
        return is<std::nullptr_t>();
    }

    bool ParserValue::empty() const noexcept
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
            } else if constexpr (std::is_same_v<Type, ParserValueArray>)
            {
                return value.empty();
            } else if constexpr (std::is_same_v<Type, ParserValueObject>)
            {
                return value.empty();
            } else
            {
                return false;
            }
        }, m_value);
    }

    bool ParserValue::asBool() const
    {
        return as<bool>();
    }

    int64_t ParserValue::asInt() const
    {
        return as<int64_t>();
    }

    double ParserValue::asDouble() const
    {
        return as<double>();
    }

    const std::string &ParserValue::asString() const
    {
        return as<std::string>();
    }

    const ParserValueArray &ParserValue::asArray() const
    {
        return as<ParserValueArray>();
    }

    const ParserValueObject &ParserValue::asObject() const
    {
        return as<ParserValueObject>();
    }

    std::optional<bool> ParserValue::getBool() const noexcept
    {
        return get<bool>();
    }

    std::optional<int64_t> ParserValue::getInt() const noexcept
    {
        return get<int64_t>();
    }

    std::optional<double> ParserValue::getDouble() const noexcept
    {
        return get<double>();
    }

    std::optional<std::string> ParserValue::getString() const noexcept
    {
        return get<std::string>();
    }

    std::optional<ParserValueArray> ParserValue::getArray() const noexcept
    {
        return get<ParserValueArray>();
    }

    std::optional<ParserValueObject> ParserValue::getObject() const noexcept
    {
        return get<ParserValueObject>();
    }

    bool ParserValue::boolOr(const bool defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    int64_t ParserValue::intOr(const int64_t defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    double ParserValue::doubleOr(const double defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    std::string ParserValue::stringOr(const std::string &defaultValue) const
    {
        return valueOr(defaultValue);
    }

    bool ParserValue::contains(const std::string_view key) const noexcept
    {
        if (!is<ParserValueObject>())
        {
            return false;
        }
        const auto &object = std::get<ParserValueObject>(m_value);
        return object.contains(key);
    }

    const ParserValue &ParserValue::operator[](const std::string_view key) const
    {
        const auto &object   = as<ParserValueObject>();
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            throw ValueAccessError::missingMember(std::string(key));
        }
        return iterator->second;
    }

    std::optional<std::reference_wrapper<const ParserValue> > ParserValue::get(const std::string_view key) const noexcept
    {
        if (!is<ParserValueObject>())
        {
            return std::nullopt;
        }
        const auto &object   = std::get<ParserValueObject>(m_value);
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            return std::nullopt;
        }
        return std::cref(iterator->second);
    }

    const ParserValue &ParserValue::operator[](const size_t index) const
    {
        const auto &array = as<ParserValueArray>();
        if (index >= array.size())
        {
            throw ValueAccessError::missingMember("[" + std::to_string(index) + "]");
        }
        return array[index];
    }

    size_t ParserValue::size() const noexcept
    {
        if (is<ParserValueArray>())
        {
            return std::get<ParserValueArray>(m_value).size();
        }
        if (is<ParserValueObject>())
        {
            return std::get<ParserValueObject>(m_value).size();
        }
        if (is<std::string>())
        {
            return std::get<std::string>(m_value).size();
        }
        return 0;
    }

    const ParserValue::VariantType &ParserValue::variant() const noexcept
    {
        return m_value;
    }

    ParserValue::VariantType &ParserValue::variant() noexcept
    {
        return m_value;
    }
} // namespace AsynGyanis::Base
