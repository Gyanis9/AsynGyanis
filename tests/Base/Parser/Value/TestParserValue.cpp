/**
 * @file TestConfigValue.cpp
 * @brief ConfigValue 单元测试：构造路径、类型查询、强类型与安全访问、嵌套结构与底层变体读写
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigTypeException.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 一个明显落在 Int 上的取值，用作类型不匹配场景的基准值
        constexpr std::int64_t kSampleInteger = 42;

        /// 一个不会被误当作整数的浮点取值
        constexpr double kSampleDouble = 3.140625;

        /**
         * @brief 构造一个包含三个元素的数组值
         * @return ConfigValue 数组配置值
         */
        ConfigValue makeSampleArray()
        {
            ConfigArray array;
            array.emplace_back(kSampleInteger);
            array.emplace_back(std::string("two"));
            array.emplace_back(true);
            return ConfigValue(std::move(array));
        }

        /**
         * @brief 构造一个包含两个键的对象值
         * @return ConfigValue 对象配置值
         */
        ConfigValue makeSampleObject()
        {
            ConfigObject object;
            object["host"] = ConfigValue(std::string("localhost"));
            object["port"] = ConfigValue(kSampleInteger);
            return ConfigValue(std::move(object));
        }
    } // namespace

    // ============================================================================
    // 构造路径与 type() 对应关系
    // ============================================================================

    TEST(ConfigValueTest, DefaultConstructionYieldsNull)
    {
        const ConfigValue value;

        EXPECT_EQ(value.type(), ConfigValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_TRUE(value.is<std::nullptr_t>());
        EXPECT_TRUE(value.empty());
    }

    TEST(ConfigValueTest, NullPointerLiteralConstructionYieldsNull)
    {
        const ConfigValue value(nullptr);

        EXPECT_EQ(value.type(), ConfigValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_FALSE(value.is<std::string>());
    }

    TEST(ConfigValueTest, BoolConstructionYieldsBool)
    {
        const ConfigValue trueValue(true);
        const ConfigValue falseValue(false);

        EXPECT_EQ(trueValue.type(), ConfigValueType::Bool);
        EXPECT_TRUE(trueValue.is<bool>());
        EXPECT_TRUE(trueValue.asBool());
        EXPECT_FALSE(trueValue.empty());

        EXPECT_EQ(falseValue.type(), ConfigValueType::Bool);
        EXPECT_FALSE(falseValue.asBool());
        EXPECT_FALSE(falseValue.empty());
    }

    TEST(ConfigValueTest, IntegerConstructionsAreStoredAsInt64)
    {
        const ConfigValue fromInt(42);
        const ConfigValue fromInt64(static_cast<std::int64_t>(42));

        EXPECT_EQ(fromInt.type(), ConfigValueType::Int);
        EXPECT_TRUE(fromInt.is<std::int64_t>());
        EXPECT_EQ(fromInt.asInt(), 42);

        EXPECT_EQ(fromInt64.type(), ConfigValueType::Int);
        EXPECT_EQ(fromInt64.asInt(), fromInt.asInt());

        EXPECT_EQ(ConfigValue(std::numeric_limits<std::int64_t>::max()).asInt(), std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(ConfigValue(std::numeric_limits<std::int64_t>::min()).asInt(), std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(ConfigValue(static_cast<std::int64_t>(0)).asInt(), 0);
    }

    TEST(ConfigValueTest, DoubleConstructionYieldsDoubleIncludingSpecialValues)
    {
        const ConfigValue value(kSampleDouble);

        EXPECT_EQ(value.type(), ConfigValueType::Double);
        EXPECT_DOUBLE_EQ(value.asDouble(), kSampleDouble);

        const ConfigValue infiniteValue(std::numeric_limits<double>::infinity());
        EXPECT_TRUE(std::isinf(infiniteValue.asDouble()));

        const ConfigValue nanValue(std::numeric_limits<double>::quiet_NaN());
        EXPECT_TRUE(std::isnan(nanValue.asDouble()));
    }

    TEST(ConfigValueTest, StringConstructionsYieldString)
    {
        const ConfigValue fromCString("hello");
        const ConfigValue fromString(std::string("world"));

        EXPECT_EQ(fromCString.type(), ConfigValueType::String);
        EXPECT_EQ(fromCString.asString(), "hello");

        EXPECT_EQ(fromString.type(), ConfigValueType::String);
        EXPECT_TRUE(fromString.is<std::string>());
        EXPECT_EQ(fromString.asString(), "world");
    }

    TEST(ConfigValueTest, NullCStringPointerYieldsEmptyStringNotNull)
    {
        const char *textPointer = nullptr;
        const ConfigValue value(textPointer);

        EXPECT_EQ(value.type(), ConfigValueType::String);
        EXPECT_FALSE(value.isNull());
        EXPECT_TRUE(value.empty());
        EXPECT_TRUE(value.asString().empty());
    }

    TEST(ConfigValueTest, ContainerConstructionsYieldArrayAndObject)
    {
        const ConfigValue arrayValue = makeSampleArray();
        const ConfigValue objectValue = makeSampleObject();

        EXPECT_EQ(arrayValue.type(), ConfigValueType::Array);
        EXPECT_TRUE(arrayValue.is<ConfigArray>());
        EXPECT_EQ(arrayValue.asArray().size(), 3U);

        EXPECT_EQ(objectValue.type(), ConfigValueType::Object);
        EXPECT_TRUE(objectValue.is<ConfigObject>());
        EXPECT_EQ(objectValue.asObject().size(), 2U);

        EXPECT_EQ(ConfigValue(ConfigArray{}).type(), ConfigValueType::Array);
        EXPECT_EQ(ConfigValue(ConfigObject{}).type(), ConfigValueType::Object);
    }

    TEST(ConfigValueTest, VariantIndexMatchesTypeEnumeratorForEveryConstructionPath)
    {
        const std::vector<ConfigValue> samples = {
                ConfigValue(nullptr),
                ConfigValue(true),
                ConfigValue(kSampleInteger),
                ConfigValue(kSampleDouble),
                ConfigValue(std::string("text")),
                makeSampleArray(),
                makeSampleObject(),
        };

        for (const ConfigValue &value: samples)
        {
            EXPECT_EQ(value.variant().index(), static_cast<size_t>(value.type())) << "type=" << typeName(value.type());
        }
    }

    // ============================================================================
    // is<T> / isNull
    // ============================================================================

    TEST(ConfigValueTest, IsDetectsExactAlternativeOnly)
    {
        const ConfigValue value(kSampleInteger);

        EXPECT_TRUE(value.is<std::int64_t>());
        EXPECT_FALSE(value.is<bool>());
        EXPECT_FALSE(value.is<double>());
        EXPECT_FALSE(value.is<std::string>());
        EXPECT_FALSE(value.is<std::nullptr_t>());
        EXPECT_FALSE(value.is<ConfigArray>());
        EXPECT_FALSE(value.is<ConfigObject>());
    }

    TEST(ConfigValueTest, IsNullRejectsEveryFalsyLookingValue)
    {
        EXPECT_TRUE(ConfigValue().isNull());
        EXPECT_TRUE(ConfigValue(nullptr).isNull());
        EXPECT_FALSE(ConfigValue(false).isNull());
        EXPECT_FALSE(ConfigValue(static_cast<std::int64_t>(0)).isNull());
        EXPECT_FALSE(ConfigValue(0.0).isNull());
        EXPECT_FALSE(ConfigValue(std::string("")).isNull());
        EXPECT_FALSE(ConfigValue(ConfigArray{}).isNull());
        EXPECT_FALSE(ConfigValue(ConfigObject{}).isNull());
    }

    // ============================================================================
    // empty() / size() 语义
    // ============================================================================

    TEST(ConfigValueTest, EmptyCoversNullAndEmptyContainers)
    {
        EXPECT_TRUE(ConfigValue().empty());
        EXPECT_TRUE(ConfigValue(nullptr).empty());
        EXPECT_TRUE(ConfigValue(std::string("")).empty());
        EXPECT_TRUE(ConfigValue(ConfigArray{}).empty());
        EXPECT_TRUE(ConfigValue(ConfigObject{}).empty());

        EXPECT_FALSE(ConfigValue(false).empty());
        EXPECT_FALSE(ConfigValue(static_cast<std::int64_t>(0)).empty());
        EXPECT_FALSE(ConfigValue(0.0).empty());
        EXPECT_FALSE(ConfigValue(std::string("x")).empty());
        EXPECT_FALSE(makeSampleArray().empty());
        EXPECT_FALSE(makeSampleObject().empty());
    }

    TEST(ConfigValueTest, SizeReportsSemanticLengthPerType)
    {
        EXPECT_EQ(ConfigValue(std::string("hello")).size(), 5U);
        EXPECT_EQ(ConfigValue(std::string("")).size(), 0U);
        EXPECT_EQ(makeSampleArray().size(), 3U);
        EXPECT_EQ(makeSampleObject().size(), 2U);

        EXPECT_EQ(ConfigValue().size(), 0U);
        EXPECT_EQ(ConfigValue(false).size(), 0U);
        EXPECT_EQ(ConfigValue(kSampleInteger).size(), 0U);
        EXPECT_EQ(ConfigValue(kSampleDouble).size(), 0U);
    }

    // ============================================================================
    // as<T>() 强类型访问
    // ============================================================================

    TEST(ConfigValueTest, AsReturnsReferenceToStoredValue)
    {
        ConfigValue textValue(std::string("original"));

        EXPECT_EQ(textValue.as<std::string>(), "original");
        EXPECT_EQ(&textValue.as<std::string>(), &std::get<std::string>(textValue.variant()));

        textValue.as<std::string>() = "changed";
        EXPECT_EQ(textValue.asString(), "changed");
    }

    TEST(ConfigValueTest, AsThrowsConfigTypeExceptionOnMismatch)
    {
        const ConfigValue value(kSampleInteger);

        EXPECT_THROW(static_cast<void>(value.as<std::string>()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(value.as<bool>()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(value.as<double>()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(value.as<ConfigArray>()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(value.as<ConfigObject>()), ConfigTypeException);
    }

    TEST(ConfigValueTest, AsExceptionCarriesExpectedAndActualTypeNames)
    {
        const ConfigValue value(kSampleDouble);

        try
        {
            static_cast<void>(value.as<std::string>());
            FAIL() << "as<std::string>() 在 Double 值上应当抛出 ConfigTypeException";
        } catch (const ConfigTypeException &exception) {
            EXPECT_EQ(exception.key(), "<unknown>");
            EXPECT_EQ(exception.expectedType(), "string");
            EXPECT_EQ(exception.actualType(), "double");
            EXPECT_NE(std::string(exception.what()).find("Type mismatch"), std::string::npos);
        }
    }

    TEST(ConfigValueTest, TypedAccessorsReturnMatchingValues)
    {
        const ConfigValue arrayValue = makeSampleArray();
        const ConfigValue objectValue = makeSampleObject();

        EXPECT_TRUE(ConfigValue(true).asBool());
        EXPECT_EQ(ConfigValue(kSampleInteger).asInt(), kSampleInteger);
        EXPECT_DOUBLE_EQ(ConfigValue(kSampleDouble).asDouble(), kSampleDouble);
        EXPECT_EQ(ConfigValue(std::string("data")).asString(), "data");
        EXPECT_EQ(arrayValue.asArray().size(), 3U);
        EXPECT_EQ(objectValue.asObject().size(), 2U);
    }

    TEST(ConfigValueTest, TypedAccessorsThrowOnMismatch)
    {
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger).asBool()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(true).asInt()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger).asDouble()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger).asString()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger).asArray()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger).asObject()), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue().asString()), ConfigTypeException);
    }

    // ============================================================================
    // get<T>() / getXxx() 安全访问
    // ============================================================================

    TEST(ConfigValueTest, TemplatedGetReturnsOptionalOnMatch)
    {
        const ConfigValue integerValue(kSampleInteger);
        const ConfigValue textValue(std::string("text"));

        const std::optional<std::int64_t> extractedInteger = integerValue.get<std::int64_t>();
        ASSERT_TRUE(extractedInteger.has_value());
        EXPECT_EQ(*extractedInteger, kSampleInteger);

        const std::optional<std::string> extractedText = textValue.get<std::string>();
        ASSERT_TRUE(extractedText.has_value());
        EXPECT_EQ(*extractedText, "text");
    }

    TEST(ConfigValueTest, TemplatedGetReturnsNulloptOnMismatch)
    {
        const ConfigValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::string>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_FALSE(integerValue.get<ConfigArray>().has_value());
        EXPECT_FALSE(integerValue.get<ConfigObject>().has_value());
        EXPECT_FALSE(ConfigValue().get<std::int64_t>().has_value());
    }

    TEST(ConfigValueTest, GetDoesNotConvertBetweenIntegerAndFloatingPoint)
    {
        const ConfigValue integerValue(kSampleInteger);
        const ConfigValue doubleValue(kSampleDouble);

        EXPECT_FALSE(integerValue.get<double>().has_value());
        EXPECT_FALSE(doubleValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(doubleValue.get<int>().has_value());

        EXPECT_TRUE(integerValue.get<std::int64_t>().has_value());
        EXPECT_TRUE(doubleValue.get<double>().has_value());
    }

    TEST(ConfigValueTest, GetDoesNotConvertBetweenBoolAndNumericTypes)
    {
        const ConfigValue boolValue(true);
        const ConfigValue integerValue(static_cast<std::int64_t>(1));

        EXPECT_FALSE(boolValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(boolValue.get<double>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_TRUE(boolValue.get<bool>().has_value());
    }

    TEST(ConfigValueTest, GetNarrowsStoredIntegerToIntegralTypes)
    {
        const ConfigValue integerValue(static_cast<std::int64_t>(1000));

        const std::optional<int> narrowedInteger = integerValue.get<int>();
        ASSERT_TRUE(narrowedInteger.has_value());
        EXPECT_EQ(*narrowedInteger, 1000);

        const std::optional<std::uint32_t> narrowedUnsigned = integerValue.get<std::uint32_t>();
        ASSERT_TRUE(narrowedUnsigned.has_value());
        EXPECT_EQ(*narrowedUnsigned, 1000U);

        const ConfigValue doubleValue(kSampleDouble);
        const std::optional<float> narrowedFloat = doubleValue.get<float>();
        ASSERT_TRUE(narrowedFloat.has_value());
        EXPECT_FLOAT_EQ(*narrowedFloat, static_cast<float>(kSampleDouble));
    }

    TEST(ConfigValueTest, GetIgnoresTypesOutsideTheVariant)
    {
        const ConfigValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::vector<std::int64_t>>().has_value());
        EXPECT_FALSE(integerValue.get<const char *>().has_value());
    }

    TEST(ConfigValueTest, NamedGettersMirrorTemplatedGet)
    {
        const ConfigValue arrayValue = makeSampleArray();
        const ConfigValue objectValue = makeSampleObject();

        EXPECT_EQ(ConfigValue(true).getBool().value_or(false), true);
        EXPECT_EQ(ConfigValue(kSampleInteger).getInt().value_or(0), kSampleInteger);
        EXPECT_DOUBLE_EQ(ConfigValue(kSampleDouble).getDouble().value_or(0.0), kSampleDouble);
        EXPECT_EQ(ConfigValue(std::string("text")).getString().value_or(""), "text");
        EXPECT_EQ(arrayValue.getArray().value_or(ConfigArray{}).size(), 3U);
        EXPECT_EQ(objectValue.getObject().value_or(ConfigObject{}).size(), 2U);

        EXPECT_FALSE(ConfigValue(kSampleInteger).getBool().has_value());
        EXPECT_FALSE(ConfigValue(true).getInt().has_value());
        EXPECT_FALSE(ConfigValue(std::string("x")).getDouble().has_value());
        EXPECT_FALSE(ConfigValue().getString().has_value());
        EXPECT_FALSE(arrayValue.getObject().has_value());
        EXPECT_FALSE(objectValue.getArray().has_value());
    }

    // ============================================================================
    // valueOr / boolOr / intOr / doubleOr / stringOr
    // ============================================================================

    TEST(ConfigValueTest, ValueOrReturnsStoredValueOnMatch)
    {
        EXPECT_EQ(ConfigValue(kSampleInteger).valueOr(static_cast<std::int64_t>(99)), kSampleInteger);
        EXPECT_EQ(ConfigValue(std::string("kept")).valueOr(std::string("fallback")), "kept");
        EXPECT_EQ(ConfigValue(true).valueOr(false), true);
    }

    TEST(ConfigValueTest, ValueOrFallsBackToDefaultOnMismatch)
    {
        EXPECT_EQ(ConfigValue(std::string("text")).valueOr(static_cast<std::int64_t>(99)), 99);
        EXPECT_EQ(ConfigValue(kSampleInteger).valueOr(std::string("fallback")), "fallback");
        EXPECT_EQ(ConfigValue().valueOr(static_cast<std::int64_t>(-1)), -1);
    }

    TEST(ConfigValueTest, NamedDefaultsFallBackOnTypeMismatch)
    {
        EXPECT_EQ(ConfigValue(true).boolOr(false), true);
        EXPECT_EQ(ConfigValue(kSampleInteger).boolOr(true), true);
        EXPECT_EQ(ConfigValue(kSampleInteger).boolOr(false), false);

        EXPECT_EQ(ConfigValue(kSampleInteger).intOr(0), kSampleInteger);
        EXPECT_EQ(ConfigValue(std::string("text")).intOr(100), 100);
        EXPECT_EQ(ConfigValue(kSampleDouble).intOr(100), 100);

        EXPECT_DOUBLE_EQ(ConfigValue(kSampleDouble).doubleOr(0.0), kSampleDouble);
        EXPECT_DOUBLE_EQ(ConfigValue(kSampleInteger).doubleOr(1.5), 1.5);

        EXPECT_EQ(ConfigValue(std::string("hello")).stringOr("default"), "hello");
        EXPECT_EQ(ConfigValue(kSampleInteger).stringOr("default"), "default");
        EXPECT_EQ(ConfigValue().stringOr("default"), "default");
    }

    // ============================================================================
    // 对象与数组访问
    // ============================================================================

    TEST(ConfigValueTest, ContainsMatchesObjectKeysOnly)
    {
        const ConfigValue objectValue = makeSampleObject();

        EXPECT_TRUE(objectValue.contains("host"));
        EXPECT_TRUE(objectValue.contains("port"));
        EXPECT_FALSE(objectValue.contains("nonexistent"));
        EXPECT_FALSE(objectValue.contains(""));

        EXPECT_FALSE(ConfigValue(kSampleInteger).contains("host"));
        EXPECT_FALSE(ConfigValue(std::string("host")).contains("host"));
        EXPECT_FALSE(makeSampleArray().contains("host"));
        EXPECT_FALSE(ConfigValue().contains("host"));
    }

    TEST(ConfigValueTest, KeyIndexerReturnsObjectMember)
    {
        const ConfigValue objectValue = makeSampleObject();

        EXPECT_EQ(objectValue["host"].asString(), "localhost");
        EXPECT_EQ(objectValue["port"].asInt(), kSampleInteger);
    }

    TEST(ConfigValueTest, KeyIndexerThrowsForMissingMember)
    {
        const ConfigValue objectValue = makeSampleObject();

        try
        {
            static_cast<void>(objectValue["nonexistent"]);
            FAIL() << "缺失键应当抛出 ConfigKeyNotFoundException";
        } catch (const ConfigKeyNotFoundException &exception) {
            EXPECT_EQ(exception.key(), "nonexistent");
        }
    }

    TEST(ConfigValueTest, KeyIndexerThrowsTypeExceptionForNonObject)
    {
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger)["key"]), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(std::string("text"))["key"]), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(makeSampleArray()["key"]), ConfigTypeException);
    }

    TEST(ConfigValueTest, IndexIndexerReturnsArrayElement)
    {
        const ConfigValue arrayValue = makeSampleArray();

        EXPECT_EQ(arrayValue[0].asInt(), kSampleInteger);
        EXPECT_EQ(arrayValue[1].asString(), "two");
        EXPECT_EQ(arrayValue[2].asBool(), true);
    }

    TEST(ConfigValueTest, IndexIndexerThrowsForOutOfRangeIndex)
    {
        const ConfigValue arrayValue = makeSampleArray();

        try
        {
            static_cast<void>(arrayValue[3]);
            FAIL() << "越界下标应当抛出 ConfigKeyNotFoundException";
        } catch (const ConfigKeyNotFoundException &exception) {
            EXPECT_EQ(exception.key(), "[3]");
        }

        EXPECT_THROW(static_cast<void>(arrayValue[100]), ConfigKeyNotFoundException);
        EXPECT_THROW(static_cast<void>(ConfigValue(ConfigArray{})[0]), ConfigKeyNotFoundException);
    }

    TEST(ConfigValueTest, IndexIndexerThrowsTypeExceptionForNonArray)
    {
        EXPECT_THROW(static_cast<void>(ConfigValue(kSampleInteger)[0]), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(ConfigValue(std::string("text"))[0]), ConfigTypeException);
        EXPECT_THROW(static_cast<void>(makeSampleObject()[0]), ConfigTypeException);
    }

    TEST(ConfigValueTest, KeyBasedGetReturnsReferenceWrapperForExistingMember)
    {
        const ConfigValue objectValue = makeSampleObject();

        const std::optional<std::reference_wrapper<const ConfigValue> > member = objectValue.get("port");
        ASSERT_TRUE(member.has_value());
        EXPECT_EQ(member->get().asInt(), kSampleInteger);

        EXPECT_FALSE(objectValue.get("nonexistent").has_value());
        EXPECT_FALSE(ConfigValue(kSampleInteger).get("any").has_value());
        EXPECT_FALSE(ConfigValue().get("any").has_value());
    }

    TEST(ConfigValueTest, TemplatedKeyBasedGetCombinesLookupAndTypeCheck)
    {
        const ConfigValue objectValue = makeSampleObject();

        const std::optional<std::int64_t> port = objectValue.get<std::int64_t>("port");
        ASSERT_TRUE(port.has_value());
        EXPECT_EQ(*port, kSampleInteger);

        const std::optional<std::string> host = objectValue.get<std::string>("host");
        ASSERT_TRUE(host.has_value());
        EXPECT_EQ(*host, "localhost");

        EXPECT_FALSE(objectValue.get<bool>("port").has_value());
        EXPECT_FALSE(objectValue.get<std::int64_t>("host").has_value());
        EXPECT_FALSE(objectValue.get<std::int64_t>("nonexistent").has_value());
    }

    TEST(ConfigValueTest, NestedObjectAccessChainsThroughIndexers)
    {
        ConfigObject inner;
        inner["value"] = ConfigValue(kSampleInteger);

        ConfigObject middle;
        middle["inner"] = ConfigValue(std::move(inner));

        ConfigObject outer;
        outer["middle"] = ConfigValue(std::move(middle));

        const ConfigValue value(std::move(outer));

        EXPECT_TRUE(value.contains("middle"));
        EXPECT_TRUE(value["middle"].contains("inner"));
        EXPECT_EQ(value["middle"]["inner"]["value"].asInt(), kSampleInteger);
        EXPECT_EQ(value["middle"]["inner"].size(), 1U);
    }

    TEST(ConfigValueTest, NestedArrayElementsKeepTheirOwnTypes)
    {
        ConfigObject namedElement;
        namedElement["name"] = ConfigValue(std::string("alpha"));

        ConfigArray array;
        array.emplace_back(static_cast<std::int64_t>(1));
        array.emplace_back(std::move(namedElement));
        array.emplace_back(ConfigArray{});

        const ConfigValue value(std::move(array));

        EXPECT_EQ(value.size(), 3U);
        EXPECT_EQ(value[0].asInt(), 1);
        EXPECT_EQ(value[1]["name"].asString(), "alpha");
        EXPECT_EQ(value[2].type(), ConfigValueType::Array);
        EXPECT_TRUE(value[2].empty());
    }

    // ============================================================================
    // variant() 读写访问
    // ============================================================================

    TEST(ConfigValueTest, ConstVariantExposesStoredAlternative)
    {
        const ConfigValue integerValue(kSampleInteger);

        EXPECT_EQ(integerValue.variant().index(), 2U);
        EXPECT_TRUE(std::holds_alternative<std::int64_t>(integerValue.variant()));
        EXPECT_EQ(std::get<std::int64_t>(integerValue.variant()), kSampleInteger);
    }

    TEST(ConfigValueTest, MutableVariantAssignmentChangesType)
    {
        ConfigValue value(kSampleInteger);

        value.variant() = std::string("changed");

        EXPECT_EQ(value.type(), ConfigValueType::String);
        EXPECT_EQ(value.asString(), "changed");
        EXPECT_FALSE(value.get<std::int64_t>().has_value());
    }

    // ============================================================================
    // 拷贝与移动语义
    // ============================================================================

    TEST(ConfigValueTest, CopyConstructionProducesIndependentValue)
    {
        ConfigValue original(std::string("shared-name"));
        const ConfigValue copy(original);

        EXPECT_EQ(copy.type(), ConfigValueType::String);
        EXPECT_EQ(copy.asString(), "shared-name");
        EXPECT_NE(&copy.as<std::string>(), &original.as<std::string>());

        original.as<std::string>() = "mutated";
        EXPECT_EQ(copy.asString(), "shared-name");
    }

    TEST(ConfigValueTest, CopyAssignmentReplacesTargetValue)
    {
        ConfigValue target(std::string("initial"));
        const ConfigValue source(kSampleInteger);

        target = source;

        EXPECT_EQ(target.type(), ConfigValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
        EXPECT_EQ(source.asInt(), kSampleInteger);
    }

    TEST(ConfigValueTest, MoveConstructionTransfersPayload)
    {
        ConfigValue original(ConfigArray{ConfigValue(kSampleInteger), ConfigValue(kSampleDouble)});
        const ConfigValue moved(std::move(original));

        EXPECT_EQ(moved.type(), ConfigValueType::Array);
        EXPECT_EQ(moved.size(), 2U);
        EXPECT_EQ(moved[0].asInt(), kSampleInteger);
    }

    TEST(ConfigValueTest, MoveAssignmentReplacesTargetAndKeepsPayload)
    {
        ConfigValue target(std::string("discarded"));
        ConfigValue source(kSampleInteger);

        target = std::move(source);

        EXPECT_EQ(target.type(), ConfigValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
    }

    TEST(ConfigValueTest, ValueSemanticsTraitsHold)
    {
        static_assert(std::is_copy_constructible_v<ConfigValue>);
        static_assert(std::is_copy_assignable_v<ConfigValue>);
        static_assert(std::is_nothrow_move_constructible_v<ConfigValue>);
        static_assert(std::is_nothrow_move_assignable_v<ConfigValue>);
        static_assert(std::is_nothrow_default_constructible_v<ConfigValue>);

        SUCCEED();
    }

    TEST(ConfigValueTest, CopiedContainerValueKeepsAllMembers)
    {
        const ConfigValue original = makeSampleObject();
        const ConfigValue copy(original);

        EXPECT_EQ(copy["port"].asInt(), kSampleInteger);
        EXPECT_EQ(copy["host"].asString(), "localhost");
        EXPECT_EQ(copy.size(), original.size());
    }

    // ============================================================================
    // 边界与有界压力
    // ============================================================================

    TEST(ConfigValueTest, LongStringValueKeepsExactSize)
    {
        const std::string longText(4096, 'X');
        const ConfigValue value(longText);

        EXPECT_EQ(value.size(), longText.size());
        EXPECT_EQ(value.asString().size(), longText.size());
        EXPECT_FALSE(value.empty());
    }

    TEST(ConfigValueTest, EmbeddedNullCharacterIsPartOfTheString)
    {
        const std::string textWithNull("hello\0world", 11);
        const ConfigValue value(textWithNull);

        EXPECT_EQ(value.type(), ConfigValueType::String);
        EXPECT_EQ(value.asString().size(), 11U);
        EXPECT_FALSE(value.empty());
    }

    TEST(ConfigValueTest, ArrayWithManyElementsIsIndexableAtBothEnds)
    {
        constexpr std::size_t kElementCount = 1000;

        ConfigArray array;
        array.reserve(kElementCount);
        for (std::size_t index = 0; index < kElementCount; ++index)
        {
            array.emplace_back(static_cast<std::int64_t>(index));
        }

        const ConfigValue value(std::move(array));

        EXPECT_EQ(value.size(), kElementCount);
        EXPECT_EQ(value[0].asInt(), 0);
        EXPECT_EQ(value[kElementCount - 1].asInt(), static_cast<std::int64_t>(kElementCount - 1));
        EXPECT_THROW(static_cast<void>(value[kElementCount]), ConfigKeyNotFoundException);
    }

    TEST(ConfigValueTest, RepeatedVariantSwitchingStaysConsistent)
    {
        // 迁移自旧 Catch2 压力用例：迭代次数由 10000 降至 1000，断言保持确定性
        ConfigValue value(nullptr);
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            value = ConfigValue(static_cast<std::int64_t>(iteration));
            ASSERT_EQ(value.type(), ConfigValueType::Int);
            EXPECT_EQ(value.asInt(), iteration);

            value = ConfigValue(std::to_string(iteration));
            ASSERT_EQ(value.type(), ConfigValueType::String);
            EXPECT_EQ(value.asString(), std::to_string(iteration));

            value = ConfigValue(iteration % 2 == 0);
            ASSERT_EQ(value.type(), ConfigValueType::Bool);
            EXPECT_EQ(value.asBool(), iteration % 2 == 0);
        }
    }
} // namespace AsynGyanis::Base
