#include "Base/Config/ConfigValueType.h"

namespace AsynGyanis::Base
{
    const char *typeName(const ConfigValueType type) noexcept
    {
        // 以 type() 的返回值调用，因此二进制（binary）与丢弃（discarded）分支实际到不了，
        // 但仍逐一列出：枚举取值一旦进入配置快照，错误文案必须能说清它是什么
        switch (type)
        {
            case ConfigValueType::null:
                return "null";
            case ConfigValueType::boolean:
                return "bool";
            case ConfigValueType::number_integer:
                return "int";
            case ConfigValueType::number_unsigned:
                return "uint";
            case ConfigValueType::number_float:
                return "double";
            case ConfigValueType::string:
                return "string";
            case ConfigValueType::array:
                return "array";
            case ConfigValueType::object:
                return "object";
            case ConfigValueType::binary:
                return "binary";
            case ConfigValueType::discarded:
                return "discarded";
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
} // namespace AsynGyanis::Base
