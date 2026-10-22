#include "Base/Config/ConfigEnvironmentOverride.h"

#include "Base/Log/LogMacros.h"
#include "Platform/System/ProcessInfo.h"

#include <charconv>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base::Detail
{
    char toAsciiLower(const char character) noexcept
    {
        return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
    }

    std::optional<std::string> configKeyFromEnvironmentSuffix(const std::string_view body)
    {
        std::string key;
        key.reserve(body.size());
        for (std::size_t index = 0; index < body.size(); ++index)
        {
            const char character = body[index];
            if (character == '_' && index + 1 < body.size() && body[index + 1] == '_')
            {
                // 前导与尾随的分隔符都映射不出合法的段名，宁可不认也不造一个空段
                if (key.empty() || key.back() == '.')
                {
                    return std::nullopt;
                }
                key.push_back('.');
                ++index;
                continue;
            }
            key.push_back(toAsciiLower(character));
        }
        if (key.empty() || key.back() == '.')
        {
            return std::nullopt;
        }
        return key;
    }

    std::optional<ConfigValue> coerceEnvironmentValue(const std::string_view text, const ConfigValueType type, std::string &error)
    {
        const auto reject = [&text, &error](const std::string &reason) -> std::optional<ConfigValue>
        {
            error = std::format("{}，原值 '{}' 不是合法写法", reason, text);
            return std::nullopt;
        };

        switch (type)
        {
            case ConfigValueType::boolean:
            {
                std::string lowered;
                lowered.reserve(text.size());
                for (const char character: text)
                {
                    lowered.push_back(toAsciiLower(character));
                }
                if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")
                {
                    return ConfigValue(true);
                }
                if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off")
                {
                    return ConfigValue(false);
                }
                return reject("布尔项只认 1/0、true/false、yes/no、on/off");
            }
            case ConfigValueType::number_integer:
            {
                long long parsed = 0;
                if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                {
                    return ConfigValue(parsed);
                }
                return reject("取值不是合法整数");
            }
            case ConfigValueType::number_unsigned:
            {
                unsigned long long parsed = 0;
                if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                {
                    return ConfigValue(parsed);
                }
                return reject("取值不是合法非负整数");
            }
            case ConfigValueType::number_float:
            {
                double parsed = 0.0;
                if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                {
                    return ConfigValue(parsed);
                }
                return reject("取值不是合法浮点数");
            }
            case ConfigValueType::object:
            case ConfigValueType::array:
            {
                // 结构与列表只能按 JSON 收：`ASYN_SERVER__HOSTS=["a","b"]` 这种写法没有别的解释法
                ConfigValue parsed;
                try
                {
                    parsed = ConfigValue::parse(std::string(text));
                } catch (const std::exception &)
                {
                    return reject("对象与数组项的值必须是合法 JSON");
                }
                // 合法 JSON 不等于形态对得上：`42` 也解析得动，但把数组顶成数字等于偷换类型
                if (type == ConfigValueType::object && !parsed.is_object())
                {
                    return reject("对象项的值必须是合法 JSON 对象");
                }
                if (type == ConfigValueType::array && !parsed.is_array())
                {
                    return reject("数组项的值必须是合法 JSON 数组");
                }
                return parsed;
            }
            default:
                return ConfigValue(std::string(text));
        }
    }

    std::vector<EnvironmentOverrideCandidate> collectEnvironmentOverrideCandidates(const std::string &prefix)
    {
        std::vector<EnvironmentOverrideCandidate> candidates;
        for (const auto &[variableName, rawValue]: Platform::ProcessInfo::environmentVariablesWithPrefix(prefix))
        {
            const std::optional<std::string> key = configKeyFromEnvironmentSuffix(std::string_view(variableName).substr(prefix.size()));
            if (!key.has_value())
            {
                // 名字畸形的那条直接出声：调用方以为自己在覆盖一个键，实际上什么都没发生
                LOG_ERROR_FMT("ConfigManager：环境变量 '{}' 的名字映射不出合法配置键（'__' 是层级分隔符，不能出现在开头或结尾），本条覆盖被忽略", variableName);
                continue;
            }
            candidates.push_back({variableName, *key, rawValue});
        }
        return candidates;
    }
} // namespace AsynGyanis::Base::Detail
