#include "Base/Config/ConfigSchema.h"

#include <cmath>
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

            // 数值范围约束：只对能比较的取值有意义，但「比较不了」不等于「通过」
            if (minimum || maximum)
            {
                if (const std::optional<double> numericValue = numericValueOf(value); !numericValue.has_value())
                {
                    // 设了界限却连值都不是数值，等于一条都没比对——与下面 NaN 那支同一口径：
                    // 判据取 fail-safe 一侧，宁可报「这条约束判不出来」，也不让它冒充「已校验通过」。
                    // 最现实的漏检现场是 schema 只写区间不写类型，而 YAML 把端口写成带引号的 "8080"
                    result.valid = false;
                    result.errors.push_back(std::format("配置键 {} 设了区间约束但值不是数值（实际是 {}），区间无法判定", key,
                                                        typeName(value.type())));
                }
                else if (!std::isfinite(*numericValue))
                {
                    // 非有限值过不了任何一次区间比较：NaN 让两侧都为假，而只给一侧界限时
                    // 另一侧的无穷大也落在界内——「设了界限却什么都比不出来」属于漏检
                    result.valid = false;
                    result.errors.push_back(std::format("配置键 {} 的值不是有限数值（NaN 或无穷），无法满足区间约束", key));
                }
                else
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
