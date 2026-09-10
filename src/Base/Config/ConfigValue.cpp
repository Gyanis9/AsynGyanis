#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"

namespace AsynGyanis::Base
{
    ConfigValue::ConfigValue() noexcept :
        m_value(nullptr)
    {
    }

    ConfigValue::ConfigValue(std::nullptr_t) noexcept :
        m_value(nullptr)
    {
    }

    ConfigValue::ConfigValue(const bool value) noexcept :
        m_value(value)
    {
    }

    ConfigValue::ConfigValue(const int value) noexcept :
        m_value(static_cast<int64_t>(value))
    {
    }

    ConfigValue::ConfigValue(const int64_t value) noexcept :
        m_value(value)
    {
    }

    ConfigValue::ConfigValue(const double value) noexcept :
        m_value(value)
    {
    }

    ConfigValue::ConfigValue(const char *value) :
        m_value(value != nullptr ? std::string(value) : std::string())
    {
    }

    ConfigValue::ConfigValue(std::string value) noexcept :
        m_value(std::move(value))
    {
    }

    ConfigValue::ConfigValue(ConfigArray value) noexcept :
        m_value(std::move(value))
    {
    }

    ConfigValue::ConfigValue(ConfigObject value) noexcept :
        m_value(std::move(value))
    {
    }

    ConfigValueType ConfigValue::type() const noexcept
    {
        switch (m_value.index())
        {
            case 0:
                return ConfigValueType::Null;
            case 1:
                return ConfigValueType::Bool;
            case 2:
                return ConfigValueType::Int;
            case 3:
                return ConfigValueType::Double;
            case 4:
                return ConfigValueType::String;
            case 5:
                return ConfigValueType::Array;
            case 6:
                return ConfigValueType::Object;
            default:
                return ConfigValueType::Null;
        }
    }

    bool ConfigValue::isNull() const noexcept
    {
        return is<std::nullptr_t>();
    }

    bool ConfigValue::empty() const noexcept
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
            } else if constexpr (std::is_same_v<Type, ConfigArray>)
            {
                return value.empty();
            } else if constexpr (std::is_same_v<Type, ConfigObject>)
            {
                return value.empty();
            } else
            {
                return false;
            }
        }, m_value);
    }

    bool ConfigValue::asBool() const
    {
        return as<bool>();
    }

    int64_t ConfigValue::asInt() const
    {
        return as<int64_t>();
    }

    double ConfigValue::asDouble() const
    {
        return as<double>();
    }

    const std::string &ConfigValue::asString() const
    {
        return as<std::string>();
    }

    const ConfigArray &ConfigValue::asArray() const
    {
        return as<ConfigArray>();
    }

    const ConfigObject &ConfigValue::asObject() const
    {
        return as<ConfigObject>();
    }

    std::optional<bool> ConfigValue::getBool() const noexcept
    {
        return get<bool>();
    }

    std::optional<int64_t> ConfigValue::getInt() const noexcept
    {
        return get<int64_t>();
    }

    std::optional<double> ConfigValue::getDouble() const noexcept
    {
        return get<double>();
    }

    std::optional<std::string> ConfigValue::getString() const noexcept
    {
        return get<std::string>();
    }

    std::optional<ConfigArray> ConfigValue::getArray() const noexcept
    {
        return get<ConfigArray>();
    }

    std::optional<ConfigObject> ConfigValue::getObject() const noexcept
    {
        return get<ConfigObject>();
    }

    bool ConfigValue::boolOr(const bool defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    int64_t ConfigValue::intOr(const int64_t defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    double ConfigValue::doubleOr(const double defaultValue) const noexcept
    {
        return valueOr(defaultValue);
    }

    std::string ConfigValue::stringOr(const std::string &defaultValue) const
    {
        return valueOr(defaultValue);
    }

    bool ConfigValue::contains(const std::string_view key) const noexcept
    {
        if (!is<ConfigObject>())
        {
            return false;
        }
        const auto &object = std::get<ConfigObject>(m_value);
        return object.contains(key);
    }

    const ConfigValue &ConfigValue::operator[](const std::string_view key) const
    {
        const auto &object   = as<ConfigObject>();
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            throw ConfigKeyNotFoundException(std::string(key));
        }
        return iterator->second;
    }

    std::optional<std::reference_wrapper<const ConfigValue> > ConfigValue::get(const std::string_view key) const noexcept
    {
        if (!is<ConfigObject>())
        {
            return std::nullopt;
        }
        const auto &object   = std::get<ConfigObject>(m_value);
        const auto  iterator = object.find(key);
        if (iterator == object.end())
        {
            return std::nullopt;
        }
        return std::cref(iterator->second);
    }

    const ConfigValue &ConfigValue::operator[](const size_t index) const
    {
        const auto &array = as<ConfigArray>();
        if (index >= array.size())
        {
            throw ConfigKeyNotFoundException("[" + std::to_string(index) + "]");
        }
        return array[index];
    }

    size_t ConfigValue::size() const noexcept
    {
        if (is<ConfigArray>())
        {
            return std::get<ConfigArray>(m_value).size();
        }
        if (is<ConfigObject>())
        {
            return std::get<ConfigObject>(m_value).size();
        }
        if (is<std::string>())
        {
            return std::get<std::string>(m_value).size();
        }
        return 0;
    }

    const ConfigValue::VariantType &ConfigValue::variant() const noexcept
    {
        return m_value;
    }

    ConfigValue::VariantType &ConfigValue::variant() noexcept
    {
        return m_value;
    }
} // namespace AsynGyanis::Base
