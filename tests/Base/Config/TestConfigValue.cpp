// ConfigValue 严格取用工具单元测试：configValueAs 的类型与范围判定、configTypeNameOf 的名称映射。
// 钉住的核心口径：不取整、不回绕、不跨类型转换——类型或范围对不上就返回空，由调用方回落默认值。

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <gtest/gtest.h>

#include <cmath>
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

    /**
     * @brief 解析入口把语法错误翻成空 optional，而不是把异常抛给调用方
     */
    TEST(ConfigValueTest, ParseConfigValueReturnsNulloptForMalformedDocuments)
    {
        const std::optional<ConfigValue> parsed = parseConfigValue(R"({"a": 1, "b": [true, null]})");
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->at("a").get<std::int64_t>(), 1);
        EXPECT_TRUE(parsed->at("b").is_array());
        EXPECT_EQ(parsed->at("b").size(), 2U);

        EXPECT_FALSE(parseConfigValue("{").has_value());
        EXPECT_FALSE(parseConfigValue(R"({"a": })").has_value());
        EXPECT_FALSE(parseConfigValue("").has_value());
        // 顶层标量按 RFC 8259 是合法文档，这一层不替调用方拦「只准是对象」
        ASSERT_TRUE(parseConfigValue("42").has_value());
        EXPECT_TRUE(parseConfigValue("42")->is_number_integer());
    }

    /**
     * @brief 注释是显式开关：配置文件那条路开，HTTP 正文那条路不开
     * @details 这里宽容一次，两处就会变成两套解析器，差异只会以「同一份 JSON 一边能读一边不能」暴露
     */
    TEST(ConfigValueTest, ParseConfigValueTreatsCommentsAsAnOptInSwitch)
    {
        constexpr std::string_view textWithComment = R"({ "a": 1 // 一行说明
        })";

        EXPECT_FALSE(parseConfigValue(textWithComment).has_value());
        EXPECT_TRUE(parseConfigValue(textWithComment, true).has_value());
    }

    /**
     * @brief 序列化：紧凑与缩进两种形状，以及无法表示成 JSON 的值如实失败
     */
    TEST(ConfigValueTest, SerializeConfigValueRoundTripsAndRefusesUnrepresentableValues)
    {
        const ConfigValue value = ConfigValue::parse(std::string{R"({"b":2,"a":1})"});

        const std::optional<std::string> compact = serializeConfigValue(value);
        ASSERT_TRUE(compact.has_value());
        // 对象键按字典序输出：这里的映射是按键有序的 std::map，插入顺序不保留。
        // 写进用例是因为使用方常以为「我按什么顺序给的就会按什么顺序发出去」
        EXPECT_EQ(*compact, R"({"a":1,"b":2})");

        const std::optional<std::string> pretty = serializeConfigValue(value, 2);
        ASSERT_TRUE(pretty.has_value());
        EXPECT_NE(pretty->find("\n"), std::string::npos) << "给了缩进就该换行";

        // NaN 在 JSON 里没有表示法：静默改成 null 或 0 都是替调用方造数据，交回空才对
        const ConfigValue notANumber = std::nan("");
        EXPECT_FALSE(serializeConfigValue(notANumber).has_value());
        const ConfigValue infinite = ConfigValue{std::numeric_limits<double>::infinity()};
        EXPECT_FALSE(serializeConfigValue(infinite).has_value());
        const ConfigValue brokenText = std::string{"\xff\xfe", 2};
        EXPECT_FALSE(serializeConfigValue(brokenText).has_value());

        // 非有限值藏在结构里也要判出来：只查顶层等于给「数组里一个 NaN」留了个变形成 null 的后门
        ConfigValue inner = ConfigValue::array();
        inner.push_back(1);
        inner.push_back(std::numeric_limits<double>::quiet_NaN());
        ConfigValue nested;
        nested["list"]    = std::move(inner);
        nested["healthy"] = 2;
        EXPECT_FALSE(serializeConfigValue(nested).has_value());
    }
} // namespace AsynGyanis::Base
