#include "Base/Config/ConfigSchema.h"

#include <format>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 判断配置值是否满足 schema 的期望类型
         * @details 整数族互通：原生解析把非负整数放进 number_unsigned、负整数放进 number_integer，
         *          同一份 schema 在两种来源（JSON/YAML）下都要能命中，故两者互相接受。
         * @param value 配置值
         * @param expectedType 期望类型
         * @return true 满足约束
         */
        [[nodiscard]] bool satisfiesExpectedType(const ConfigValue &value, const ConfigValueType expectedType) noexcept
        {
            if (expectedType == ConfigValueType::number_integer || expectedType == ConfigValueType::number_unsigned)
            {
                return value.is_number_integer() || value.is_number_unsigned();
            }
            return value.type() == expectedType;
        }

        /**
         * @brief 取数值形式（整数或浮点），用于区间比较
         * @param value 配置值
         * @return std::optional<double> 非数值类型返回空
         */
        [[nodiscard]] std::optional<double> numericValueOf(const ConfigValue &value) noexcept
        {
            if (value.is_number_integer())
            {
                return static_cast<double>(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned())
            {
                return static_cast<double>(value.get<std::uint64_t>());
            }
            if (value.is_number_float())
            {
                return value.get<double>();
            }
            return std::nullopt;
        }
    } // namespace

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

            // 类型约束（expectedType 为空表示不限制）；类型不符时跳过该键的区间检查，
            // 一个键只留一条错误，避免同一个问题被报两遍
            if (expectedType && !satisfiesExpectedType(value, *expectedType))
            {
                result.valid = false;
                result.errors.push_back(std::format("配置键 {} 类型不符：期望 {}，实际 {}", key, typeName(*expectedType), typeName(value.type())));
                continue;
            }

            // 数值范围约束（对整数与浮点生效，其余类型跳过）
            if (minimum || maximum)
            {
                if (const std::optional<double> numericValue = numericValueOf(value))
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
