// ConfigValue 严格取用工具单元测试：configValueAs 的类型与范围判定、configTypeNameOf 的名称映射。
// 钉住的核心口径：不取整、不回绕、不跨类型转换——类型或范围对不上就返回空，由调用方回落默认值。

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace AsynGyanis::Base
{
    // ============================================================================
    // configValueAs：布尔与整数
    // ============================================================================

    TEST(ConfigValueTest, BoolAcceptsOnlyBooleanValues)
    {
        EXPECT_EQ(configValueAs<bool>(ConfigValue(true)), std::optional<bool>(true));
        EXPECT_EQ(configValueAs<bool>(ConfigValue(false)), std::optional<bool>(false));

        // 数字 0/1、字符串 "true" 都不是布尔：不能静默当真假取用
        EXPECT_FALSE(configValueAs<bool>(ConfigValue(1)).has_value());
        EXPECT_FALSE(configValueAs<bool>(ConfigValue(std::string("true"))).has_value());
        EXPECT_FALSE(configValueAs<bool>(ConfigValue(nullptr)).has_value());
    }

    TEST(ConfigValueTest, SignedIntegerAcceptsBothIntegerKindsWithinRange)
    {
        // 非负整数在原生 JSON 里是无符号类型，取有符号值时按无损加宽处理
        EXPECT_EQ(configValueAs<std::int64_t>(ConfigValue(std::uint64_t{42})), std::optional<std::int64_t>(42));
        EXPECT_EQ(configValueAs<std::int64_t>(ConfigValue(std::int64_t{-7})), std::optional<std::int64_t>(-7));

        // 超出有符号可表示范围的无符号数不截断
        EXPECT_FALSE(configValueAs<std::int64_t>(ConfigValue(std::numeric_limits<std::uint64_t>::max())).has_value());

        // 浮点、布尔、字符串都不当整数：取整与真假转换都是静默变形
        EXPECT_FALSE(configValueAs<std::int64_t>(ConfigValue(1.5)).has_value());
        EXPECT_FALSE(configValueAs<std::int64_t>(ConfigValue(true)).has_value());
        EXPECT_FALSE(configValueAs<std::int64_t>(ConfigValue(std::string("7"))).has_value());
    }

    TEST(ConfigValueTest, UnsignedIntegerRejectsNegativeValues)
    {
        EXPECT_EQ(configValueAs<std::uint64_t>(ConfigValue(std::int64_t{5})), std::optional<std::uint64_t>(5));

        // 负值到无符号不回绕
        EXPECT_FALSE(configValueAs<std::uint64_t>(ConfigValue(std::int64_t{-1})).has_value());
        EXPECT_FALSE(configValueAs<std::uint64_t>(ConfigValue(2.5)).has_value());
    }

    TEST(ConfigValueTest, NarrowIntegerTargetsCheckRangeInsteadOfTruncating)
    {
        EXPECT_EQ(configValueAs<std::uint8_t>(ConfigValue(std::uint64_t{255})), std::optional<std::uint8_t>(255));
        EXPECT_FALSE(configValueAs<std::uint8_t>(ConfigValue(std::uint64_t{256})).has_value());
        EXPECT_EQ(configValueAs<std::int32_t>(ConfigValue(std::int64_t{-2147483648LL})), std::optional<std::int32_t>(std::numeric_limits<std::int32_t>::min()));
        EXPECT_FALSE(configValueAs<std::int32_t>(ConfigValue(std::uint64_t{2147483648ULL})).has_value());
    }

    // ============================================================================
    // configValueAs：浮点、字符串与容器
    // ============================================================================

    TEST(ConfigValueTest, FloatingTargetRequiresFloatingKind)
    {
        EXPECT_DOUBLE_EQ(configValueAs<double>(ConfigValue(2.5)).value_or(0.0), 2.5);

        // 整数与浮点互不转换：整数配置不会被静默当成小数取用
        EXPECT_FALSE(configValueAs<double>(ConfigValue(std::int64_t{2})).has_value());
        EXPECT_FALSE(configValueAs<double>(ConfigValue(true)).has_value());
    }

    TEST(ConfigValueTest, NarrowFloatTargetChecksRangeBeforeNarrowing)
    {
        EXPECT_EQ(configValueAs<float>(ConfigValue(0.5)).value(), 0.5F);

        // 超出 float 上界的 double 不静默变无穷大
        EXPECT_FALSE(configValueAs<float>(ConfigValue(1e300)).has_value());
    }

    TEST(ConfigValueTest, StringTargetKeepsBytesAndRejectsOtherKinds)
    {
        const ConfigValue text = ConfigValue(std::string("a\0b", 3));
        ASSERT_TRUE(configValueAs<std::string>(text).has_value());
        // 取回来的是完整字节序列：二进制安全，不受内嵌 NUL 影响
        EXPECT_EQ(configValueAs<std::string>(text)->size(), 3U);

        EXPECT_FALSE(configValueAs<std::string>(ConfigValue(std::int64_t{7})).has_value());
        EXPECT_FALSE(configValueAs<std::string>(ConfigValue(nullptr)).has_value());
    }

    TEST(ConfigValueTest, ArrayAndObjectTargetsRequireMatchingKinds)
    {
        const ConfigValue array = ConfigValue::array({ConfigValue(std::int64_t{1}), ConfigValue(std::int64_t{2})});
        EXPECT_EQ(configValueAs<ConfigArray>(array)->size(), 2U);
        EXPECT_FALSE(configValueAs<ConfigArray>(ConfigValue::object()).has_value());

        ConfigValue object = ConfigValue::object();
        object["key"]      = ConfigValue(std::string("value"));
        EXPECT_EQ(configValueAs<ConfigObject>(object)->at("key").get<std::string>(), "value");
        EXPECT_FALSE(configValueAs<ConfigObject>(ConfigValue::array()).has_value());
    }

    TEST(ConfigValueTest, UnsupportedTargetTypesYieldEmpty)
    {
        // 未支持的目标类型（如裸指针）返回空而不是编译期报错，调用方据此回落默认值
        EXPECT_FALSE(configValueAs<const char *>(ConfigValue(std::string("text"))).has_value());
    }

    // ============================================================================
    // configTypeNameOf：错误文案里的目标类型名
    // ============================================================================

    TEST(ConfigValueTest, ConfigTypeNameMapsEverySupportedTarget)
    {
        EXPECT_STREQ(configTypeNameOf<bool>(), "bool");
        EXPECT_STREQ(configTypeNameOf<std::int64_t>(), "int");
        EXPECT_STREQ(configTypeNameOf<int>(), "int");
        EXPECT_STREQ(configTypeNameOf<std::uint64_t>(), "uint");
        EXPECT_STREQ(configTypeNameOf<unsigned int>(), "uint");
        EXPECT_STREQ(configTypeNameOf<double>(), "double");
        EXPECT_STREQ(configTypeNameOf<float>(), "double");
        EXPECT_STREQ(configTypeNameOf<std::string>(), "string");
        EXPECT_STREQ(configTypeNameOf<ConfigArray>(), "array");
        EXPECT_STREQ(configTypeNameOf<ConfigObject>(), "object");
        EXPECT_STREQ(configTypeNameOf<const char *>(), "value");
    }
} // namespace AsynGyanis::Base
