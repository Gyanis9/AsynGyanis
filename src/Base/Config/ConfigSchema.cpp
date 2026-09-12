#include "Base/Config/ConfigSchema.h"

#include <format>

namespace AsynGyanis::Base
{
    ConfigValidationResult runSchemaValidation(const ConfigKeyValueMap &values, const ConfigSchema &schema)
    {
        ConfigValidationResult result;

        for (const auto &[key, expectedType, required, minimum, maximum]: schema)
        {
            const auto iterator = values.find(key);
            if (iterator == values.end())
            {
                if (required)
                {
                    result.valid = false;
                    result.errors.push_back(std::format("缺少必需配置键: {}", key));
                }
                continue;
            }

            const ConfigValue &value = iterator->second;

            // 类型约束（expectedType 为空表示不限制）
            if (expectedType && value.type() != *expectedType)
            {
                result.valid = false;
                result.errors.push_back(std::format("配置键 {} 类型不符：期望 {}，实际 {}", key, typeName(*expectedType), typeName(value.type())));
                continue;
            }

            // 数值范围约束（仅对 Int/Double 生效）
            if (minimum || maximum)
            {
                std::optional<double> numericValue;
                if (value.type() == ConfigValueType::Int)
                {
                    numericValue = static_cast<double>(value.asInt());
                } else if (value.type() == ConfigValueType::Double)
                {
                    numericValue = value.asDouble();
                }

                if (numericValue)
                {
                    if (minimum && *numericValue < *minimum)
                    {
                        result.valid = false;
                        result.errors.push_back(std::format("配置键 {} 的值 {} 低于下限 {}", key, *numericValue, *minimum));
                    }
                    if (maximum && *numericValue > *maximum)
                    {
                        result.valid = false;
                        result.errors.push_back(std::format("配置键 {} 的值 {} 高于上限 {}", key, *numericValue, *maximum));
                    }
                }
            }
        }

        return result;
    }
} // namespace AsynGyanis::Base
