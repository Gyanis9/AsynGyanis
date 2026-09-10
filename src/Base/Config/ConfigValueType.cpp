#include "Base/Config/ConfigValueType.h"

namespace AsynGyanis::Base
{
    const char *typeName(const ConfigValueType type) noexcept
    {
        switch (type)
        {
            case ConfigValueType::Null:
                return "null";
            case ConfigValueType::Bool:
                return "bool";
            case ConfigValueType::Int:
                return "int";
            case ConfigValueType::Double:
                return "double";
            case ConfigValueType::String:
                return "string";
            case ConfigValueType::Array:
                return "array";
            case ConfigValueType::Object:
                return "object";
            default:
                return "unknown";
        }
    }

    namespace
    {
        /**
         * @brief 判断路径是否以指定后缀结尾（大小写不敏感）。
         * @param filePath 待检查路径。
         * @param suffix 目标后缀（小写）。
         * @return bool 后缀匹配返回 true。
         */
        [[nodiscard]] bool hasSuffixIgnoreCase(const std::string_view filePath, const std::string_view suffix) noexcept
        {
            if (filePath.size() < suffix.size())
            {
                return false;
            }

            for (size_t index = 0; index < suffix.size(); ++index)
            {
                char character = filePath[filePath.size() - suffix.size() + index];
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
                if (character != suffix[index])
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool isYamlFile(const std::string_view filePath) noexcept
    {
        return hasSuffixIgnoreCase(filePath, ".yaml") || hasSuffixIgnoreCase(filePath, ".yml");
    }

    bool isJsonFile(const std::string_view filePath) noexcept
    {
        return hasSuffixIgnoreCase(filePath, ".json");
    }

    bool isConfigFile(const std::string_view filePath) noexcept
    {
        return isYamlFile(filePath) || isJsonFile(filePath);
    }

    std::vector<std::string> splitKey(std::string_view key, const char delimiter)
    {
        std::vector<std::string> parts;
        if (key.empty())
        {
            return parts;
        }

        size_t start = 0;
        size_t end   = key.find(delimiter);

        while (end != std::string_view::npos)
        {
            if (end > start)
            {
                parts.emplace_back(key.substr(start, end - start));
            }
            start = end + 1;
            end   = key.find(delimiter, start);
        }

        if (start < key.length())
        {
            parts.emplace_back(key.substr(start));
        }

        return parts;
    }
} // namespace AsynGyanis::Base
