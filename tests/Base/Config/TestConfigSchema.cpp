// ConfigSchema 单元测试：约束条目结构、校验结果语义与 runSchemaValidation 校验算法。
// ConfigSchema 在源码中是 std::vector<ConfigSchemaEntry> 的别名、没有独立成员函数，因此本文件
// 覆盖其唯一可测行为 runSchemaValidation()（validateSchema/setSchema 均委托该纯函数）
// 以及 ConfigValidationResult 的语义。

#include "Base/Config/ConfigSchema.h"
#include "Base/Config/ConfigValidationResult.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 按键构造扁平配置字典
         * @param entries 键与配置值的配对列表
         * @return ConfigKeyValueMap 供校验使用的配置字典
         */
        ConfigKeyValueMap makeValues(const std::vector<std::pair<std::string, ConfigValue> > &entries)
        {
            ConfigKeyValueMap values;
            for (const auto &[key, value]: entries)
            {
                values[key] = value;
            }
            return values;
        }

        /**
         * @brief 判断校验错误列表中是否存在包含指定文本的条目
         * @param result 校验结果
         * @param needle 期望出现的子串
         * @return true 至少一条错误包含该子串
         */
        bool hasErrorContaining(const ConfigValidationResult &result, const std::string &needle)
        {
            for (const std::string &error: result.errors)
            {
                if (error.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    // ============================================================================
    // 结构与类型别名语义
    // ============================================================================

    TEST(ConfigSchemaTest, ValidationResultsDefaultToValidWithNoErrors)
    {
        const ConfigValidationResult result;

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
        EXPECT_TRUE(static_cast<bool>(result));
    }

    TEST(ConfigSchemaTest, ValidationResultConvertsToBoolFromValidFlag)
    {
        ConfigValidationResult result;
        result.valid = false;
        result.errors.push_back("示例错误");

        EXPECT_FALSE(static_cast<bool>(result));
        EXPECT_EQ(result.errors.size(), 1U);
    }

    TEST(ConfigSchemaTest, SchemaEntryDefaultsToUnconstrainedOptionalRequirement)
    {
        const ConfigSchemaEntry entry;

        EXPECT_TRUE(entry.key.empty());
        EXPECT_FALSE(entry.expectedType.has_value());
        EXPECT_FALSE(entry.required);
        EXPECT_FALSE(entry.minimum.has_value());
        EXPECT_FALSE(entry.maximum.has_value());
    }

    TEST(ConfigSchemaTest, SchemaIsAVectorOfEntries)
    {
        ConfigSchema schema;
        schema.push_back(ConfigSchemaEntry{"first", ConfigValueType::number_integer, true, std::nullopt, std::nullopt});
        schema.push_back(ConfigSchemaEntry{"second", std::nullopt, false, 0.0, 10.0});

        EXPECT_EQ(schema.size(), 2U);
        EXPECT_EQ(schema.front().key, "first");
        EXPECT_EQ(schema.back().maximum, 10.0);
    }

    // ============================================================================
    // runSchemaValidation：通过路径
    // ============================================================================

    TEST(ConfigSchemaTest, EmptySchemaAcceptsAnySnapshot)
    {
        const ConfigKeyValueMap values = makeValues({{"anything", ConfigValue(std::string("value"))}});

        const ConfigValidationResult result = runSchemaValidation(values, ConfigSchema{});

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    TEST(ConfigSchemaTest, MatchingTypesPassValidation)
    {
        const ConfigKeyValueMap values = makeValues({
                {"app.debug", ConfigValue(true)},
                {"server.port", ConfigValue(static_cast<std::int64_t>(8080))},
                {"server.ratio", ConfigValue(1.5)},
                {"server.host", ConfigValue(std::string("localhost"))},
                {"server.list", ConfigValue(ConfigArray{})},
                {"server.blank", ConfigValue(nullptr)},
        });

        const ConfigSchema schema = {
                ConfigSchemaEntry{"app.debug", ConfigValueType::boolean, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.port", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.ratio", ConfigValueType::number_float, false, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.host", ConfigValueType::string, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.list", ConfigValueType::array, false, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.blank", ConfigValueType::null, false, std::nullopt, std::nullopt},
        };

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    TEST(ConfigSchemaTest, IntegerConstraintAcceptsBothIntegerKinds)
    {
        // 原生解析把非负整数放进 number_unsigned、负整数放进 number_integer：
        // 声明整型约束必须同时接受两者，否则「port: 8080」这类最常见的配置会被误判为类型不符
        const ConfigKeyValueMap values = makeValues({
                {"server.port", ConfigValue(std::uint64_t{8080})},
                {"server.limit", ConfigValue(std::int64_t{-5})},
        });
        const ConfigSchema schema = {
                ConfigSchemaEntry{"server.port", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"server.limit", ConfigValueType::number_unsigned, true, std::nullopt, std::nullopt},
        };

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_TRUE(result.valid) << (result.errors.empty() ? "" : result.errors.front());
        EXPECT_TRUE(result.errors.empty());
    }

    TEST(ConfigSchemaTest, RangeConstraintsApplyToUnsignedValues)
    {
        // 区间检查覆盖无符号整数这一支：漏掉它会让非负整数的越界配置逃过校验
        const ConfigKeyValueMap values = makeValues({{"port", ConfigValue(std::uint64_t{70000})}});
        const ConfigSchema schema = {ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, 1.0, 65535.0}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        EXPECT_TRUE(hasErrorContaining(result, "高于上限"));
    }

    TEST(ConfigSchemaTest, EntryWithoutExpectedTypeAcceptsAnyStoredType)
    {
        const ConfigKeyValueMap values = makeValues({{"mixed", ConfigValue(std::string("text"))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"mixed", std::nullopt, true, std::nullopt, std::nullopt}};

        EXPECT_TRUE(runSchemaValidation(values, schema).valid);
    }

    TEST(ConfigSchemaTest, MissingKeyIsIgnoredWhenNotRequired)
    {
        const ConfigKeyValueMap values = makeValues({{"present", ConfigValue(static_cast<std::int64_t>(1))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"absent", ConfigValueType::number_integer, false, 0.0, 10.0}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    TEST(ConfigSchemaTest, NumericValueExactlyOnBothLimitsPasses)
    {
        const ConfigKeyValueMap values = makeValues({{"port", ConfigValue(static_cast<std::int64_t>(100))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, 1.0, 100.0}};

        EXPECT_TRUE(runSchemaValidation(values, schema).valid);
    }

    TEST(ConfigSchemaTest, RangeConstraintsApplyToBothIntAndDouble)
    {
        const ConfigKeyValueMap values = makeValues({
                {"count", ConfigValue(static_cast<std::int64_t>(50))},
                {"ratio", ConfigValue(2.5)},
        });
        const ConfigSchema schema = {
                ConfigSchemaEntry{"count", ConfigValueType::number_integer, true, 0.0, 100.0},
                ConfigSchemaEntry{"ratio", ConfigValueType::number_float, true, 1.0, 3.0},
        };

        EXPECT_TRUE(runSchemaValidation(values, schema).valid);
    }

    TEST(ConfigSchemaTest, RangeIsSkippedForNonNumericTypes)
    {
        const ConfigKeyValueMap values = makeValues({
                {"label", ConfigValue(std::string("text"))},
                {"flag", ConfigValue(true)},
                {"blank", ConfigValue(nullptr)},
        });
        const ConfigSchema schema = {
                ConfigSchemaEntry{"label", ConfigValueType::string, true, 10.0, 20.0},
                ConfigSchemaEntry{"flag", ConfigValueType::boolean, true, 10.0, 20.0},
                ConfigSchemaEntry{"blank", ConfigValueType::null, true, 10.0, 20.0},
        };

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }

    // ============================================================================
    // runSchemaValidation：失败路径与原因
    // ============================================================================

    TEST(ConfigSchemaTest, MissingRequiredKeyFailsWithKeyInReason)
    {
        const ConfigKeyValueMap values = makeValues({{"present", ConfigValue(static_cast<std::int64_t>(1))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"database.url", ConfigValueType::string, true, std::nullopt, std::nullopt}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        EXPECT_FALSE(static_cast<bool>(result));
        ASSERT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(hasErrorContaining(result, "缺少必需配置键"));
        EXPECT_TRUE(hasErrorContaining(result, "database.url"));
    }

    TEST(ConfigSchemaTest, TypeMismatchFailsWithExpectedAndActualTypeNames)
    {
        const ConfigKeyValueMap values = makeValues({{"server.port", ConfigValue(std::string("not-a-number"))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"server.port", ConfigValueType::number_integer, true, std::nullopt, std::nullopt}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        ASSERT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(hasErrorContaining(result, "类型不符"));
        EXPECT_TRUE(hasErrorContaining(result, "server.port"));
        EXPECT_TRUE(hasErrorContaining(result, "期望 int"));
        EXPECT_TRUE(hasErrorContaining(result, "实际 string"));
    }

    TEST(ConfigSchemaTest, TypeMismatchShortCircuitsRangeReportingForSameKey)
    {
        // 类型已不符时不再重复报数值越界，一个键只留一条错误
        const ConfigKeyValueMap values = makeValues({{"port", ConfigValue(std::string("text"))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"port", ConfigValueType::number_integer, true, 1000.0, 2000.0}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(hasErrorContaining(result, "类型不符"));
        EXPECT_FALSE(hasErrorContaining(result, "低于下限"));
    }

    TEST(ConfigSchemaTest, ValueBelowMinimumFailsWithReason)
    {
        const ConfigKeyValueMap values = makeValues({{"timeout", ConfigValue(static_cast<std::int64_t>(7))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"timeout", ConfigValueType::number_integer, true, 10.0, std::nullopt}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        ASSERT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(hasErrorContaining(result, "低于下限"));
        EXPECT_TRUE(hasErrorContaining(result, "timeout"));
    }

    TEST(ConfigSchemaTest, ValueAboveMaximumFailsWithReason)
    {
        const ConfigKeyValueMap values = makeValues({{"ratio", ConfigValue(9.5)}});
        const ConfigSchema schema = {ConfigSchemaEntry{"ratio", ConfigValueType::number_float, false, std::nullopt, 5.0}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        ASSERT_EQ(result.errors.size(), 1U);
        EXPECT_TRUE(hasErrorContaining(result, "高于上限"));
        EXPECT_TRUE(hasErrorContaining(result, "ratio"));
    }

    TEST(ConfigSchemaTest, BothBoundsViolatedReportTwoErrorsForSameKey)
    {
        const ConfigKeyValueMap values = makeValues({{"window", ConfigValue(static_cast<std::int64_t>(7))}});
        const ConfigSchema schema = {ConfigSchemaEntry{"window", ConfigValueType::number_integer, true, 10.0, 5.0}};

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.errors.size(), 2U);
        EXPECT_TRUE(hasErrorContaining(result, "低于下限"));
        EXPECT_TRUE(hasErrorContaining(result, "高于上限"));
    }

    TEST(ConfigSchemaTest, EveryViolationIsCollectedInSchemaOrder)
    {
        const ConfigKeyValueMap values = makeValues({
                {"alpha", ConfigValue(std::string("text"))},
                {"gamma", ConfigValue(static_cast<std::int64_t>(500))},
        });
        const ConfigSchema schema = {
                ConfigSchemaEntry{"alpha", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"beta", ConfigValueType::string, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"gamma", ConfigValueType::number_integer, true, 0.0, 100.0},
        };

        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        ASSERT_EQ(result.errors.size(), 3U);
        EXPECT_TRUE(result.errors[0].find("类型不符") != std::string::npos);
        EXPECT_TRUE(result.errors[0].find("alpha") != std::string::npos);
        EXPECT_TRUE(result.errors[1].find("缺少必需配置键") != std::string::npos);
        EXPECT_TRUE(result.errors[1].find("beta") != std::string::npos);
        EXPECT_TRUE(result.errors[2].find("高于上限") != std::string::npos);
        EXPECT_TRUE(result.errors[2].find("gamma") != std::string::npos);
    }

    TEST(ConfigSchemaTest, ValidationDoesNotModifySnapshot)
    {
        ConfigKeyValueMap values = makeValues({{"port", ConfigValue(static_cast<std::int64_t>(1))}});
        const ConfigSchema schema = {
                ConfigSchemaEntry{"absent", ConfigValueType::number_integer, true, std::nullopt, std::nullopt},
                ConfigSchemaEntry{"port", ConfigValueType::string, false, std::nullopt, std::nullopt},
        };

        const std::size_t sizeBeforeValidation = values.size();
        const ConfigValidationResult result = runSchemaValidation(values, schema);

        EXPECT_FALSE(result.valid);
        EXPECT_EQ(values.size(), sizeBeforeValidation);
        EXPECT_EQ(values.at("port").type(), ConfigValueType::number_integer);
    }

    TEST(ConfigSchemaTest, EmptySchemaOnEmptySnapshotIsValid)
    {
        const ConfigKeyValueMap values;

        const ConfigValidationResult result = runSchemaValidation(values, ConfigSchema{});

        EXPECT_TRUE(result.valid);
        EXPECT_TRUE(result.errors.empty());
    }
} // namespace AsynGyanis::Base
