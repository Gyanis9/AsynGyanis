/**
 * @file TestFormatValue.cpp
 * @brief FormatValue 单元测试：构造路径、类型查询、强类型与安全访问、嵌套结构与底层变体读写
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/Value/ValueAccessError.h"

#include <gtest/gtest.h>

#include <cmath>
#include <compare>
#include <cstddef>
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
         * @return FormatValue 数组配置值
         */
        FormatValue makeSampleArray()
        {
            FormatValueArray array;
            array.emplace_back(kSampleInteger);
            array.emplace_back(std::string("two"));
            array.emplace_back(true);
            return FormatValue(std::move(array));
        }

        /**
         * @brief 构造一个包含两个键的对象值
         * @return FormatValue 对象配置值
         */
        FormatValue makeSampleObject()
        {
            FormatValueObject object;
            object["host"] = FormatValue(std::string("localhost"));
            object["port"] = FormatValue(kSampleInteger);
            return FormatValue(std::move(object));
        }
    } // namespace

    // ============================================================================
    // 构造路径与 type() 对应关系
    // ============================================================================

    TEST(FormatValueTest, DefaultConstructionYieldsNull)
    {
        const FormatValue value;

        EXPECT_EQ(value.type(), FormatValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_TRUE(value.is<std::nullptr_t>());
        EXPECT_TRUE(value.empty());
    }

    TEST(FormatValueTest, NullPointerLiteralConstructionYieldsNull)
    {
        const FormatValue value(nullptr);

        EXPECT_EQ(value.type(), FormatValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_FALSE(value.is<std::string>());
    }

    TEST(FormatValueTest, BoolConstructionYieldsBool)
    {
        const FormatValue trueValue(true);
        const FormatValue falseValue(false);

        EXPECT_EQ(trueValue.type(), FormatValueType::Bool);
        EXPECT_TRUE(trueValue.is<bool>());
        EXPECT_TRUE(trueValue.asBool());
        EXPECT_FALSE(trueValue.empty());

        EXPECT_EQ(falseValue.type(), FormatValueType::Bool);
        EXPECT_FALSE(falseValue.asBool());
        EXPECT_FALSE(falseValue.empty());
    }

    TEST(FormatValueTest, IntegerConstructionsAreStoredAsInt64)
    {
        const FormatValue fromInt(42);
        const FormatValue fromInt64(static_cast<std::int64_t>(42));

        EXPECT_EQ(fromInt.type(), FormatValueType::Int);
        EXPECT_TRUE(fromInt.is<std::int64_t>());
        EXPECT_EQ(fromInt.asInt(), 42);

        EXPECT_EQ(fromInt64.type(), FormatValueType::Int);
        EXPECT_EQ(fromInt64.asInt(), fromInt.asInt());

        EXPECT_EQ(FormatValue(std::numeric_limits<std::int64_t>::max()).asInt(), std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(FormatValue(std::numeric_limits<std::int64_t>::min()).asInt(), std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(FormatValue(static_cast<std::int64_t>(0)).asInt(), 0);
    }

    TEST(FormatValueTest, DoubleConstructionYieldsDoubleIncludingSpecialValues)
    {
        const FormatValue value(kSampleDouble);

        EXPECT_EQ(value.type(), FormatValueType::Double);
        EXPECT_DOUBLE_EQ(value.asDouble(), kSampleDouble);

        const FormatValue infiniteValue(std::numeric_limits<double>::infinity());
        EXPECT_TRUE(std::isinf(infiniteValue.asDouble()));

        const FormatValue nanValue(std::numeric_limits<double>::quiet_NaN());
        EXPECT_TRUE(std::isnan(nanValue.asDouble()));
    }

    TEST(FormatValueTest, StringConstructionsYieldString)
    {
        const FormatValue fromCString("hello");
        const FormatValue fromString(std::string("world"));

        EXPECT_EQ(fromCString.type(), FormatValueType::String);
        EXPECT_EQ(fromCString.asString(), "hello");

        EXPECT_EQ(fromString.type(), FormatValueType::String);
        EXPECT_TRUE(fromString.is<std::string>());
        EXPECT_EQ(fromString.asString(), "world");
    }

    TEST(FormatValueTest, NullCStringPointerYieldsEmptyStringNotNull)
    {
        const char *textPointer = nullptr;
        const FormatValue value(textPointer);

        EXPECT_EQ(value.type(), FormatValueType::String);
        EXPECT_FALSE(value.isNull());
        EXPECT_TRUE(value.empty());
        EXPECT_TRUE(value.asString().empty());
    }

    TEST(FormatValueTest, ContainerConstructionsYieldArrayAndObject)
    {
        const FormatValue arrayValue = makeSampleArray();
        const FormatValue objectValue = makeSampleObject();

        EXPECT_EQ(arrayValue.type(), FormatValueType::Array);
        EXPECT_TRUE(arrayValue.is<FormatValueArray>());
        EXPECT_EQ(arrayValue.asArray().size(), 3U);

        EXPECT_EQ(objectValue.type(), FormatValueType::Object);
        EXPECT_TRUE(objectValue.is<FormatValueObject>());
        EXPECT_EQ(objectValue.asObject().size(), 2U);

        EXPECT_EQ(FormatValue(FormatValueArray{}).type(), FormatValueType::Array);
        EXPECT_EQ(FormatValue(FormatValueObject{}).type(), FormatValueType::Object);
    }

    TEST(FormatValueTest, VariantIndexMatchesTypeEnumeratorForEveryConstructionPath)
    {
        const std::vector<FormatValue> samples = {
                FormatValue(nullptr),
                FormatValue(true),
                FormatValue(kSampleInteger),
                FormatValue(kSampleDouble),
                FormatValue(std::string("text")),
                makeSampleArray(),
                makeSampleObject(),
        };

        for (const FormatValue &value: samples)
        {
            EXPECT_EQ(value.variant().index(), static_cast<size_t>(value.type())) << "type=" << typeName(value.type());
        }
    }

    // ============================================================================
    // is<T> / isNull
    // ============================================================================

    TEST(FormatValueTest, IsDetectsExactAlternativeOnly)
    {
        const FormatValue value(kSampleInteger);

        EXPECT_TRUE(value.is<std::int64_t>());
        EXPECT_FALSE(value.is<bool>());
        EXPECT_FALSE(value.is<double>());
        EXPECT_FALSE(value.is<std::string>());
        EXPECT_FALSE(value.is<std::nullptr_t>());
        EXPECT_FALSE(value.is<FormatValueArray>());
        EXPECT_FALSE(value.is<FormatValueObject>());
    }

    TEST(FormatValueTest, IsNullRejectsEveryFalsyLookingValue)
    {
        EXPECT_TRUE(FormatValue().isNull());
        EXPECT_TRUE(FormatValue(nullptr).isNull());
        EXPECT_FALSE(FormatValue(false).isNull());
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(0)).isNull());
        EXPECT_FALSE(FormatValue(0.0).isNull());
        EXPECT_FALSE(FormatValue(std::string("")).isNull());
        EXPECT_FALSE(FormatValue(FormatValueArray{}).isNull());
        EXPECT_FALSE(FormatValue(FormatValueObject{}).isNull());
    }

    // ============================================================================
    // empty() / size() 语义
    // ============================================================================

    TEST(FormatValueTest, EmptyCoversNullAndEmptyContainers)
    {
        EXPECT_TRUE(FormatValue().empty());
        EXPECT_TRUE(FormatValue(nullptr).empty());
        EXPECT_TRUE(FormatValue(std::string("")).empty());
        EXPECT_TRUE(FormatValue(FormatValueArray{}).empty());
        EXPECT_TRUE(FormatValue(FormatValueObject{}).empty());

        EXPECT_FALSE(FormatValue(false).empty());
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(0)).empty());
        EXPECT_FALSE(FormatValue(0.0).empty());
        EXPECT_FALSE(FormatValue(std::string("x")).empty());
        EXPECT_FALSE(makeSampleArray().empty());
        EXPECT_FALSE(makeSampleObject().empty());
    }

    TEST(FormatValueTest, SizeReportsSemanticLengthPerType)
    {
        EXPECT_EQ(FormatValue(std::string("hello")).size(), 5U);
        EXPECT_EQ(FormatValue(std::string("")).size(), 0U);
        EXPECT_EQ(makeSampleArray().size(), 3U);
        EXPECT_EQ(makeSampleObject().size(), 2U);

        EXPECT_EQ(FormatValue().size(), 0U);
        EXPECT_EQ(FormatValue(false).size(), 0U);
        EXPECT_EQ(FormatValue(kSampleInteger).size(), 0U);
        EXPECT_EQ(FormatValue(kSampleDouble).size(), 0U);
    }

    // ============================================================================
    // as<T>() 强类型访问
    // ============================================================================

    TEST(FormatValueTest, AsReturnsReferenceToStoredValue)
    {
        FormatValue textValue(std::string("original"));

        EXPECT_EQ(textValue.as<std::string>(), "original");
        EXPECT_EQ(&textValue.as<std::string>(), &std::get<std::string>(textValue.variant()));

        textValue.as<std::string>() = "changed";
        EXPECT_EQ(textValue.asString(), "changed");
    }

    TEST(FormatValueTest, AsThrowsValueAccessErrorOnMismatch)
    {
        const FormatValue value(kSampleInteger);

        EXPECT_THROW(static_cast<void>(value.as<std::string>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<bool>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<double>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<FormatValueArray>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<FormatValueObject>()), ValueAccessError);
    }

    TEST(FormatValueTest, AsExceptionCarriesExpectedAndActualTypeNames)
    {
        const FormatValue value(kSampleDouble);

        try
        {
            static_cast<void>(value.as<std::string>());
            FAIL() << "as<std::string>() 在 Double 值上应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "<unknown>");
            EXPECT_EQ(exception.expectedType(), "string");
            EXPECT_EQ(exception.actualType(), "double");
            EXPECT_NE(std::string(exception.what()).find("类型不匹配"), std::string::npos);
        }
    }

    TEST(FormatValueTest, TypedAccessorsReturnMatchingValues)
    {
        const FormatValue arrayValue = makeSampleArray();
        const FormatValue objectValue = makeSampleObject();

        EXPECT_TRUE(FormatValue(true).asBool());
        EXPECT_EQ(FormatValue(kSampleInteger).asInt(), kSampleInteger);
        EXPECT_DOUBLE_EQ(FormatValue(kSampleDouble).asDouble(), kSampleDouble);
        EXPECT_EQ(FormatValue(std::string("data")).asString(), "data");
        EXPECT_EQ(arrayValue.asArray().size(), 3U);
        EXPECT_EQ(objectValue.asObject().size(), 2U);
    }

    TEST(FormatValueTest, TypedAccessorsThrowOnMismatch)
    {
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).asBool()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(true).asInt()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).asDouble()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).asString()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).asArray()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).asObject()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue().asString()), ValueAccessError);
    }

    // ============================================================================
    // get<T>() / getXxx() 安全访问
    // ============================================================================

    TEST(FormatValueTest, TemplatedGetReturnsOptionalOnMatch)
    {
        const FormatValue integerValue(kSampleInteger);
        const FormatValue textValue(std::string("text"));

        const std::optional<std::int64_t> extractedInteger = integerValue.get<std::int64_t>();
        ASSERT_TRUE(extractedInteger.has_value());
        EXPECT_EQ(*extractedInteger, kSampleInteger);

        const std::optional<std::string> extractedText = textValue.get<std::string>();
        ASSERT_TRUE(extractedText.has_value());
        EXPECT_EQ(*extractedText, "text");
    }

    TEST(FormatValueTest, TemplatedGetReturnsNulloptOnMismatch)
    {
        const FormatValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::string>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_FALSE(integerValue.get<FormatValueArray>().has_value());
        EXPECT_FALSE(integerValue.get<FormatValueObject>().has_value());
        EXPECT_FALSE(FormatValue().get<std::int64_t>().has_value());
    }

    TEST(FormatValueTest, GetDoesNotConvertBetweenIntegerAndFloatingPoint)
    {
        const FormatValue integerValue(kSampleInteger);
        const FormatValue doubleValue(kSampleDouble);

        EXPECT_FALSE(integerValue.get<double>().has_value());
        EXPECT_FALSE(doubleValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(doubleValue.get<int>().has_value());

        EXPECT_TRUE(integerValue.get<std::int64_t>().has_value());
        EXPECT_TRUE(doubleValue.get<double>().has_value());
    }

    TEST(FormatValueTest, GetDoesNotConvertBetweenBoolAndNumericTypes)
    {
        const FormatValue boolValue(true);
        const FormatValue integerValue(static_cast<std::int64_t>(1));

        EXPECT_FALSE(boolValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(boolValue.get<double>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_TRUE(boolValue.get<bool>().has_value());
    }

    TEST(FormatValueTest, GetNarrowsStoredIntegerToIntegralTypes)
    {
        const FormatValue integerValue(static_cast<std::int64_t>(1000));

        const std::optional<int> narrowedInteger = integerValue.get<int>();
        ASSERT_TRUE(narrowedInteger.has_value());
        EXPECT_EQ(*narrowedInteger, 1000);

        const std::optional<std::uint32_t> narrowedUnsigned = integerValue.get<std::uint32_t>();
        ASSERT_TRUE(narrowedUnsigned.has_value());
        EXPECT_EQ(*narrowedUnsigned, 1000U);

        const FormatValue doubleValue(kSampleDouble);
        const std::optional<float> narrowedFloat = doubleValue.get<float>();
        ASSERT_TRUE(narrowedFloat.has_value());
        EXPECT_FLOAT_EQ(*narrowedFloat, static_cast<float>(kSampleDouble));
    }

    TEST(FormatValueTest, GetIgnoresTypesOutsideTheVariant)
    {
        const FormatValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::vector<std::int64_t>>().has_value());
        EXPECT_FALSE(integerValue.get<const char *>().has_value());
    }

    TEST(FormatValueTest, NamedGettersMirrorTemplatedGet)
    {
        const FormatValue arrayValue = makeSampleArray();
        const FormatValue objectValue = makeSampleObject();

        EXPECT_EQ(FormatValue(true).getBool().value_or(false), true);
        EXPECT_EQ(FormatValue(kSampleInteger).getInt().value_or(0), kSampleInteger);
        EXPECT_DOUBLE_EQ(FormatValue(kSampleDouble).getDouble().value_or(0.0), kSampleDouble);
        EXPECT_EQ(FormatValue(std::string("text")).getString().value_or(""), "text");
        EXPECT_EQ(arrayValue.getArray().value_or(FormatValueArray{}).size(), 3U);
        EXPECT_EQ(objectValue.getObject().value_or(FormatValueObject{}).size(), 2U);

        EXPECT_FALSE(FormatValue(kSampleInteger).getBool().has_value());
        EXPECT_FALSE(FormatValue(true).getInt().has_value());
        EXPECT_FALSE(FormatValue(std::string("x")).getDouble().has_value());
        EXPECT_FALSE(FormatValue().getString().has_value());
        EXPECT_FALSE(arrayValue.getObject().has_value());
        EXPECT_FALSE(objectValue.getArray().has_value());
    }

    // ============================================================================
    // valueOr / boolOr / intOr / doubleOr / stringOr
    // ============================================================================

    TEST(FormatValueTest, ValueOrReturnsStoredValueOnMatch)
    {
        EXPECT_EQ(FormatValue(kSampleInteger).valueOr(static_cast<std::int64_t>(99)), kSampleInteger);
        EXPECT_EQ(FormatValue(std::string("kept")).valueOr(std::string("fallback")), "kept");
        EXPECT_EQ(FormatValue(true).valueOr(false), true);
    }

    TEST(FormatValueTest, ValueOrFallsBackToDefaultOnMismatch)
    {
        EXPECT_EQ(FormatValue(std::string("text")).valueOr(static_cast<std::int64_t>(99)), 99);
        EXPECT_EQ(FormatValue(kSampleInteger).valueOr(std::string("fallback")), "fallback");
        EXPECT_EQ(FormatValue().valueOr(static_cast<std::int64_t>(-1)), -1);
    }

    TEST(FormatValueTest, NamedDefaultsFallBackOnTypeMismatch)
    {
        EXPECT_EQ(FormatValue(true).boolOr(false), true);
        EXPECT_EQ(FormatValue(kSampleInteger).boolOr(true), true);
        EXPECT_EQ(FormatValue(kSampleInteger).boolOr(false), false);

        EXPECT_EQ(FormatValue(kSampleInteger).intOr(0), kSampleInteger);
        EXPECT_EQ(FormatValue(std::string("text")).intOr(100), 100);
        EXPECT_EQ(FormatValue(kSampleDouble).intOr(100), 100);

        EXPECT_DOUBLE_EQ(FormatValue(kSampleDouble).doubleOr(0.0), kSampleDouble);
        EXPECT_DOUBLE_EQ(FormatValue(kSampleInteger).doubleOr(1.5), 1.5);

        EXPECT_EQ(FormatValue(std::string("hello")).stringOr("default"), "hello");
        EXPECT_EQ(FormatValue(kSampleInteger).stringOr("default"), "default");
        EXPECT_EQ(FormatValue().stringOr("default"), "default");
    }

    // ============================================================================
    // 对象与数组访问
    // ============================================================================

    TEST(FormatValueTest, ContainsMatchesObjectKeysOnly)
    {
        const FormatValue objectValue = makeSampleObject();

        EXPECT_TRUE(objectValue.contains("host"));
        EXPECT_TRUE(objectValue.contains("port"));
        EXPECT_FALSE(objectValue.contains("nonexistent"));
        EXPECT_FALSE(objectValue.contains(""));

        EXPECT_FALSE(FormatValue(kSampleInteger).contains("host"));
        EXPECT_FALSE(FormatValue(std::string("host")).contains("host"));
        EXPECT_FALSE(makeSampleArray().contains("host"));
        EXPECT_FALSE(FormatValue().contains("host"));
    }

    TEST(FormatValueTest, KeyIndexerReturnsObjectMember)
    {
        const FormatValue objectValue = makeSampleObject();

        EXPECT_EQ(objectValue["host"].asString(), "localhost");
        EXPECT_EQ(objectValue["port"].asInt(), kSampleInteger);
    }

    TEST(FormatValueTest, KeyIndexerThrowsForMissingMember)
    {
        const FormatValue objectValue = makeSampleObject();

        try
        {
            static_cast<void>(objectValue["nonexistent"]);
            FAIL() << "缺失键应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "nonexistent");
        }
    }

    TEST(FormatValueTest, KeyIndexerThrowsTypeExceptionForNonObject)
    {
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger)["key"]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(std::string("text"))["key"]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(makeSampleArray()["key"]), ValueAccessError);
    }

    TEST(FormatValueTest, IndexIndexerReturnsArrayElement)
    {
        const FormatValue arrayValue = makeSampleArray();

        EXPECT_EQ(arrayValue[0].asInt(), kSampleInteger);
        EXPECT_EQ(arrayValue[1].asString(), "two");
        EXPECT_EQ(arrayValue[2].asBool(), true);
    }

    TEST(FormatValueTest, IndexIndexerThrowsForOutOfRangeIndex)
    {
        const FormatValue arrayValue = makeSampleArray();

        try
        {
            static_cast<void>(arrayValue[3]);
            FAIL() << "越界下标应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "[3]");
        }

        EXPECT_THROW(static_cast<void>(static_cast<void>(arrayValue[100])), ValueAccessError);
        EXPECT_THROW(static_cast<void>(static_cast<void>(FormatValue(FormatValueArray{})[0])), ValueAccessError);
    }

    TEST(FormatValueTest, IndexIndexerThrowsTypeExceptionForNonArray)
    {
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger)[0]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(std::string("text"))[0]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(makeSampleObject()[0]), ValueAccessError);
    }

    TEST(FormatValueTest, KeyBasedGetReturnsReferenceWrapperForExistingMember)
    {
        const FormatValue objectValue = makeSampleObject();

        const std::optional<std::reference_wrapper<const FormatValue> > member = objectValue.get("port");
        ASSERT_TRUE(member.has_value());
        EXPECT_EQ(member->get().asInt(), kSampleInteger);

        EXPECT_FALSE(objectValue.get("nonexistent").has_value());
        EXPECT_FALSE(FormatValue(kSampleInteger).get("any").has_value());
        EXPECT_FALSE(FormatValue().get("any").has_value());
    }

    TEST(FormatValueTest, TemplatedKeyBasedGetCombinesLookupAndTypeCheck)
    {
        const FormatValue objectValue = makeSampleObject();

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

    TEST(FormatValueTest, NestedObjectAccessChainsThroughIndexers)
    {
        FormatValueObject inner;
        inner["value"] = FormatValue(kSampleInteger);

        FormatValueObject middle;
        middle["inner"] = FormatValue(std::move(inner));

        FormatValueObject outer;
        outer["middle"] = FormatValue(std::move(middle));

        const FormatValue value(std::move(outer));

        EXPECT_TRUE(value.contains("middle"));
        EXPECT_TRUE(value["middle"].contains("inner"));
        EXPECT_EQ(value["middle"]["inner"]["value"].asInt(), kSampleInteger);
        EXPECT_EQ(value["middle"]["inner"].size(), 1U);
    }

    TEST(FormatValueTest, NestedArrayElementsKeepTheirOwnTypes)
    {
        FormatValueObject namedElement;
        namedElement["name"] = FormatValue(std::string("alpha"));

        FormatValueArray array;
        array.emplace_back(static_cast<std::int64_t>(1));
        array.emplace_back(std::move(namedElement));
        array.emplace_back(FormatValueArray{});

        const FormatValue value(std::move(array));

        EXPECT_EQ(value.size(), 3U);
        EXPECT_EQ(value[0].asInt(), 1);
        EXPECT_EQ(value[1]["name"].asString(), "alpha");
        EXPECT_EQ(value[2].type(), FormatValueType::Array);
        EXPECT_TRUE(value[2].empty());
    }

    // ============================================================================
    // variant() 读写访问
    // ============================================================================

    TEST(FormatValueTest, ConstVariantExposesStoredAlternative)
    {
        const FormatValue integerValue(kSampleInteger);

        EXPECT_EQ(integerValue.variant().index(), 2U);
        EXPECT_TRUE(std::holds_alternative<std::int64_t>(integerValue.variant()));
        EXPECT_EQ(std::get<std::int64_t>(integerValue.variant()), kSampleInteger);
    }

    TEST(FormatValueTest, MutableVariantAssignmentChangesType)
    {
        FormatValue value(kSampleInteger);

        value.variant() = std::string("changed");

        EXPECT_EQ(value.type(), FormatValueType::String);
        EXPECT_EQ(value.asString(), "changed");
        EXPECT_FALSE(value.get<std::int64_t>().has_value());
    }

    // ============================================================================
    // 拷贝与移动语义
    // ============================================================================

    TEST(FormatValueTest, CopyConstructionProducesIndependentValue)
    {
        FormatValue original(std::string("shared-name"));
        const FormatValue copy(original);

        EXPECT_EQ(copy.type(), FormatValueType::String);
        EXPECT_EQ(copy.asString(), "shared-name");
        EXPECT_NE(&copy.as<std::string>(), &original.as<std::string>());

        original.as<std::string>() = "mutated";
        EXPECT_EQ(copy.asString(), "shared-name");
    }

    TEST(FormatValueTest, CopyAssignmentReplacesTargetValue)
    {
        FormatValue target(std::string("initial"));
        const FormatValue source(kSampleInteger);

        target = source;

        EXPECT_EQ(target.type(), FormatValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
        EXPECT_EQ(source.asInt(), kSampleInteger);
    }

    TEST(FormatValueTest, MoveConstructionTransfersPayload)
    {
        FormatValue original(FormatValueArray{FormatValue(kSampleInteger), FormatValue(kSampleDouble)});
        const FormatValue moved(std::move(original));

        EXPECT_EQ(moved.type(), FormatValueType::Array);
        EXPECT_EQ(moved.size(), 2U);
        EXPECT_EQ(moved[0].asInt(), kSampleInteger);
    }

    TEST(FormatValueTest, MoveAssignmentReplacesTargetAndKeepsPayload)
    {
        FormatValue target(std::string("discarded"));
        FormatValue source(kSampleInteger);

        target = std::move(source);

        EXPECT_EQ(target.type(), FormatValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
    }

    TEST(FormatValueTest, ValueSemanticsTraitsHold)
    {
        static_assert(std::is_copy_constructible_v<FormatValue>);
        static_assert(std::is_copy_assignable_v<FormatValue>);
        static_assert(std::is_nothrow_move_constructible_v<FormatValue>);
        static_assert(std::is_nothrow_move_assignable_v<FormatValue>);
        static_assert(std::is_nothrow_default_constructible_v<FormatValue>);

        SUCCEED();
    }

    TEST(FormatValueTest, CopiedContainerValueKeepsAllMembers)
    {
        const FormatValue original = makeSampleObject();
        const FormatValue copy(original);

        EXPECT_EQ(copy["port"].asInt(), kSampleInteger);
        EXPECT_EQ(copy["host"].asString(), "localhost");
        EXPECT_EQ(copy.size(), original.size());
    }

    // ============================================================================
    // 边界与有界压力
    // ============================================================================

    TEST(FormatValueTest, LongStringValueKeepsExactSize)
    {
        const std::string longText(4096, 'X');
        const FormatValue value(longText);

        EXPECT_EQ(value.size(), longText.size());
        EXPECT_EQ(value.asString().size(), longText.size());
        EXPECT_FALSE(value.empty());
    }

    TEST(FormatValueTest, EmbeddedNullCharacterIsPartOfTheString)
    {
        const std::string textWithNull("hello\0world", 11);
        const FormatValue value(textWithNull);

        EXPECT_EQ(value.type(), FormatValueType::String);
        EXPECT_EQ(value.asString().size(), 11U);
        EXPECT_FALSE(value.empty());
    }

    TEST(FormatValueTest, ArrayWithManyElementsIsIndexableAtBothEnds)
    {
        constexpr std::size_t kElementCount = 1000;

        FormatValueArray array;
        array.reserve(kElementCount);
        for (std::size_t index = 0; index < kElementCount; ++index)
        {
            array.emplace_back(static_cast<std::int64_t>(index));
        }

        const FormatValue value(std::move(array));

        EXPECT_EQ(value.size(), kElementCount);
        EXPECT_EQ(value[0].asInt(), 0);
        EXPECT_EQ(value[kElementCount - 1].asInt(), static_cast<std::int64_t>(kElementCount - 1));
        EXPECT_THROW(static_cast<void>(static_cast<void>(value[kElementCount])), ValueAccessError);
    }

    TEST(FormatValueTest, RepeatedVariantSwitchingStaysConsistent)
    {
        // 迁移自旧 Catch2 压力用例：迭代次数由 10000 降至 1000，断言保持确定性
        FormatValue value(nullptr);
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            value = FormatValue(static_cast<std::int64_t>(iteration));
            ASSERT_EQ(value.type(), FormatValueType::Int);
            EXPECT_EQ(value.asInt(), iteration);

            value = FormatValue(std::to_string(iteration));
            ASSERT_EQ(value.type(), FormatValueType::String);
            EXPECT_EQ(value.asString(), std::to_string(iteration));

            value = FormatValue(iteration % 2 == 0);
            ASSERT_EQ(value.type(), FormatValueType::Bool);
            EXPECT_EQ(value.asBool(), iteration % 2 == 0);
        }
    }

    // ============================================================================
    // 无符号 64 位整数（UInt）构造与取用
    // ============================================================================

    TEST(FormatValueTest, UIntConstructionYieldsUIntAlternative)
    {
        const FormatValue value(std::uint64_t(42));

        EXPECT_EQ(value.type(), FormatValueType::UInt);
        EXPECT_TRUE(value.is<std::uint64_t>());
        EXPECT_TRUE(value.isUInt());
        EXPECT_EQ(value.asUInt(), 42U);
        EXPECT_EQ(value.variant().index(), 7U);
        EXPECT_EQ(std::string(typeName(value.type())), "uint");
        EXPECT_EQ(std::string(typeNameOf<std::uint64_t>()), "uint");

        EXPECT_FALSE(value.isNull());
        EXPECT_FALSE(value.empty());
        EXPECT_EQ(value.size(), 0U);
    }

    TEST(FormatValueTest, EveryUnsignedIntegerTypeMapsToUIntWithoutAmbiguity)
    {
        // 受约束的模板构造让 unsigned int、size_t、uint16_t 等不再与 int/int64_t/double 二义
        const FormatValue fromUnsignedInt(42U);
        const FormatValue fromSizeType(static_cast<std::size_t>(7));
        const FormatValue fromUnsignedShort(static_cast<std::uint16_t>(9));

        EXPECT_EQ(fromUnsignedInt.type(), FormatValueType::UInt);
        EXPECT_EQ(fromUnsignedInt.asUInt(), 42U);
        EXPECT_EQ(fromSizeType.asUInt(), 7U);
        EXPECT_EQ(fromUnsignedShort.asUInt(), 9U);
    }

    TEST(FormatValueTest, UIntCoversTheWholeUnsigned64Range)
    {
        const FormatValue maximumValue(std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(maximumValue.asUInt(), std::numeric_limits<std::uint64_t>::max());

        // 刚好越过 int64 上界的正数必须仍然是 UInt，而不是被挤到 Double
        const FormatValue justAboveInt64(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U);
        EXPECT_EQ(justAboveInt64.type(), FormatValueType::UInt);
        EXPECT_EQ(justAboveInt64.asUInt(), static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U);
    }

    TEST(FormatValueTest, NumberPredicatesSeparateIntegralUnsignedAndFloatingValues)
    {
        const FormatValue signedValue(static_cast<std::int64_t>(1));
        const FormatValue unsignedValue(std::uint64_t(1));
        const FormatValue floatingValue(1.5);
        const FormatValue boolValue(true);

        EXPECT_TRUE(signedValue.isNumber());
        EXPECT_TRUE(signedValue.isIntegralNumber());
        EXPECT_FALSE(signedValue.isFloatingNumber());

        EXPECT_TRUE(unsignedValue.isNumber());
        EXPECT_TRUE(unsignedValue.isIntegralNumber());
        EXPECT_FALSE(unsignedValue.isFloatingNumber());

        EXPECT_TRUE(floatingValue.isNumber());
        EXPECT_FALSE(floatingValue.isIntegralNumber());
        EXPECT_TRUE(floatingValue.isFloatingNumber());

        EXPECT_FALSE(boolValue.isNumber());
        EXPECT_FALSE(FormatValue(std::string("1")).isNumber());
        EXPECT_FALSE(FormatValue().isNumber());
    }

    TEST(FormatValueTest, StrongTypedUIntAccessorsDoNotConvertOtherTypes)
    {
        // asUInt 与 as<int64_t> 都是精确取用：跨类型一律抛异常
        EXPECT_THROW(static_cast<void>(FormatValue(static_cast<std::int64_t>(5)).asUInt()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(5.0).asUInt()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(std::uint64_t(5)).asInt()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(std::uint64_t(5)).asDouble()), ValueAccessError);

        const FormatValue value(std::uint64_t(5));
        EXPECT_EQ(value.as<std::uint64_t>(), 5U);
        EXPECT_THROW(static_cast<void>(value.as<bool>()), ValueAccessError);
    }

    TEST(FormatValueTest, SafeUIntAccessorsAllowLosslessIntegerWidening)
    {
        const FormatValue signedValue(static_cast<std::int64_t>(5));
        const FormatValue negativeValue(static_cast<std::int64_t>(-1));
        const FormatValue unsignedValue(std::uint64_t(5));
        const FormatValue hugeUnsignedValue(std::numeric_limits<std::uint64_t>::max());

        // UInt 目标：精确值 + 非负 Int 的无损加宽
        ASSERT_TRUE(unsignedValue.getUInt().has_value());
        EXPECT_EQ(*unsignedValue.getUInt(), 5U);
        ASSERT_TRUE(signedValue.getUInt().has_value());
        EXPECT_EQ(*signedValue.getUInt(), 5U);
        EXPECT_FALSE(negativeValue.getUInt().has_value());
        EXPECT_FALSE(FormatValue(5.0).getUInt().has_value());

        // Int 目标：精确值 + 落在 int64 范围内的 UInt
        ASSERT_TRUE(unsignedValue.getInt().has_value());
        EXPECT_EQ(*unsignedValue.getInt(), 5);
        EXPECT_FALSE(hugeUnsignedValue.getInt().has_value());
        EXPECT_FALSE(FormatValue(5.0).getInt().has_value());

        EXPECT_EQ(unsignedValue.uintOr(0), 5U);
        EXPECT_EQ(negativeValue.uintOr(99), 99U);
        EXPECT_EQ(FormatValue(std::string("x")).uintOr(7), 7U);
        EXPECT_EQ(signedValue.valueOr(static_cast<std::uint64_t>(3)), 5U);
    }

    TEST(FormatValueTest, UIntEmptyAndSizeFollowValueSemantics)
    {
        EXPECT_FALSE(FormatValue(std::uint64_t(0)).empty());
        EXPECT_EQ(FormatValue(std::uint64_t(0)).size(), 0U);
    }

    // ============================================================================
    // 等价与三路比较
    // ============================================================================

    TEST(FormatValueTest, EqualityRequiresIdenticalTypeAndValue)
    {
        EXPECT_TRUE(FormatValue() == FormatValue(nullptr));
        EXPECT_TRUE(FormatValue(kSampleInteger) == FormatValue(kSampleInteger));
        EXPECT_FALSE(FormatValue(kSampleInteger) == FormatValue(kSampleInteger + 1));
        EXPECT_TRUE(FormatValue(std::string("text")) == FormatValue(std::string("text")));
        EXPECT_FALSE(FormatValue(std::string("text")) == FormatValue(std::string("other")));
        EXPECT_TRUE(FormatValue(true) == FormatValue(true));
        EXPECT_FALSE(FormatValue(true) == FormatValue(false));
        EXPECT_TRUE(makeSampleArray() == makeSampleArray());
        EXPECT_TRUE(makeSampleObject() == makeSampleObject());

        // 跨类型一律不等：即使数值相同，也杜绝 Int/UInt/Double 之间的精度陷阱
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(1)) == FormatValue(std::uint64_t(1)));
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(1)) == FormatValue(1.0));
        EXPECT_FALSE(FormatValue(std::uint64_t(1)) == FormatValue(1.0));
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(0)) == FormatValue(false));
        EXPECT_FALSE(FormatValue(static_cast<std::int64_t>(0)) == FormatValue(nullptr));
    }

    TEST(FormatValueTest, EqualityRecursesIntoNestedContainers)
    {
        FormatValueObject leftMembers;
        leftMembers["value"] = FormatValue(kSampleInteger);

        const FormatValueObject sameMembers = leftMembers;

        FormatValueObject differentMembers;
        differentMembers["value"] = FormatValue(kSampleInteger + 1);

        FormatValueObject extraMembers = leftMembers;
        extraMembers["extra"] = FormatValue(nullptr);

        EXPECT_TRUE(FormatValue(leftMembers) == FormatValue(sameMembers));
        EXPECT_FALSE(FormatValue(leftMembers) == FormatValue(differentMembers));
        EXPECT_FALSE(FormatValue(leftMembers) == FormatValue(extraMembers));
    }

    TEST(FormatValueTest, EqualityWithNaNIsAlwaysFalse)
    {
        const double notANumber = std::numeric_limits<double>::quiet_NaN();
        const FormatValue left(notANumber);
        const FormatValue right(notANumber);

        EXPECT_FALSE(left == right);
        EXPECT_TRUE((left <=> right) == std::partial_ordering::unordered);

        // 容器内嵌 NaN 时同样不可等价
        const FormatValueArray elements{left};
        const FormatValueArray sameElements{right};
        EXPECT_FALSE(FormatValue(elements) == FormatValue(sameElements));
    }

    TEST(FormatValueTest, ComparisonOrdersAcrossTypesByTypeRank)
    {
        // 类型序即变体下标（等于 FormatValueType 枚举序）：
        // Null < Bool < Int < Double < String < Array < Object < UInt
        EXPECT_TRUE(FormatValue() < FormatValue(false));
        EXPECT_TRUE(FormatValue(false) < FormatValue(static_cast<std::int64_t>(1)));
        EXPECT_TRUE(FormatValue(static_cast<std::int64_t>(100)) < FormatValue(0.5));
        EXPECT_TRUE(FormatValue(0.5) < FormatValue(std::string("")));
        EXPECT_TRUE(FormatValue(std::string("z")) < makeSampleArray());
        EXPECT_TRUE(makeSampleArray() < makeSampleObject());
        EXPECT_TRUE(makeSampleObject() < FormatValue(std::uint64_t(0)));

        // 跨类型只比类型序、绝不比数值，因此 Int(100) 仍然小于 Double(0.5)
        EXPECT_TRUE(FormatValue(static_cast<std::int64_t>(100)) < FormatValue(0.5));
        EXPECT_FALSE(FormatValue(std::uint64_t(1)) < FormatValue(static_cast<std::int64_t>(2)));
    }

    TEST(FormatValueTest, ComparisonOrdersValuesOfTheSameType)
    {
        EXPECT_TRUE(FormatValue(static_cast<std::int64_t>(-1)) < FormatValue(static_cast<std::int64_t>(1)));
        EXPECT_TRUE(FormatValue(false) < FormatValue(true));
        EXPECT_TRUE(FormatValue(1.5) < FormatValue(2.5));
        EXPECT_TRUE(FormatValue(std::string("abc")) < FormatValue(std::string("abd")));
        EXPECT_TRUE(FormatValue(std::uint64_t(1)) < FormatValue(std::uint64_t(2)));

        EXPECT_TRUE(FormatValue(kSampleInteger) <= FormatValue(kSampleInteger));
        EXPECT_TRUE(FormatValue(kSampleInteger) >= FormatValue(kSampleInteger));
        EXPECT_TRUE(FormatValue(kSampleInteger) > FormatValue(kSampleInteger - 1));

        // 数组按字典序：前缀相同则短者为小
        const FormatValueArray shortArray{FormatValue(static_cast<std::int64_t>(1))};
        const FormatValueArray longArray{FormatValue(static_cast<std::int64_t>(1)), FormatValue(static_cast<std::int64_t>(2))};
        EXPECT_TRUE(FormatValue(shortArray) < FormatValue(longArray));
        EXPECT_TRUE(FormatValue(longArray) > FormatValue(shortArray));

        // 对象按键序，键相同再比值
        FormatValueObject leftMembers;
        leftMembers["a"] = FormatValue(static_cast<std::int64_t>(1));
        leftMembers["b"] = FormatValue(static_cast<std::int64_t>(1));

        FormatValueObject rightMembers;
        rightMembers["a"] = FormatValue(static_cast<std::int64_t>(1));
        rightMembers["b"] = FormatValue(static_cast<std::int64_t>(2));

        EXPECT_TRUE(FormatValue(leftMembers) < FormatValue(rightMembers));

        FormatValueObject longerMembers = leftMembers;
        longerMembers["c"] = FormatValue(static_cast<std::int64_t>(0));
        EXPECT_TRUE(FormatValue(leftMembers) < FormatValue(longerMembers));
    }

    // ============================================================================
    // 零拷贝视图
    // ============================================================================

    TEST(FormatValueTest, StringViewReturnsTheInternalBuffer)
    {
        FormatValue value(std::string("payload"));

        const std::string *readOnlyView = std::as_const(value).getStringView();
        ASSERT_NE(readOnlyView, nullptr);
        EXPECT_EQ(readOnlyView, &std::get<std::string>(value.variant()));

        std::string *writableView = value.getStringView();
        ASSERT_NE(writableView, nullptr);
        writableView->append("-tail");
        EXPECT_EQ(value.asString(), "payload-tail");

        EXPECT_EQ(FormatValue(kSampleInteger).getStringView(), nullptr);

        const FormatValue readOnlyInteger(kSampleInteger);
        EXPECT_EQ(readOnlyInteger.getStringView(), nullptr);
    }

    TEST(FormatValueTest, ContainerViewsReturnTheInternalContainers)
    {
        FormatValue arrayValue(makeSampleArray());
        FormatValue objectValue(makeSampleObject());

        ASSERT_NE(arrayValue.getArrayView(), nullptr);
        EXPECT_EQ(arrayValue.getArrayView()->size(), 3U);
        EXPECT_EQ(std::as_const(arrayValue).getArrayView(), arrayValue.getArrayView());
        EXPECT_EQ(objectValue.getArrayView(), nullptr);

        ASSERT_NE(objectValue.getObjectView(), nullptr);
        EXPECT_EQ(objectValue.getObjectView()->size(), 2U);
        EXPECT_EQ(std::as_const(objectValue).getObjectView(), objectValue.getObjectView());
        EXPECT_EQ(arrayValue.getObjectView(), nullptr);

        // 视图可写，改动直接落在原值上
        objectValue.getObjectView()->at("port") = FormatValue(static_cast<std::int64_t>(8080));
        EXPECT_EQ(objectValue["port"].asInt(), 8080);

        arrayValue.getArrayView()->push_back(FormatValue(std::string("appended")));
        EXPECT_EQ(arrayValue.size(), 4U);
    }

    TEST(FormatValueTest, NamedContainerGettersStillReturnIndependentCopies)
    {
        // 兼容契约：getArray()/getObject()/getString() 按值返回，改动副本不影响原值；
        // 需要零拷贝时用 getArrayView()/getObjectView()/getStringView()
        FormatValue value(makeSampleArray());

        std::optional<FormatValueArray> copy = value.getArray();
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->size(), 3U);
        (*copy)[0] = FormatValue(std::string("changed"));
        EXPECT_EQ(value[0].asInt(), kSampleInteger);
    }

    // ============================================================================
    // DOM 增删改查
    // ============================================================================

    TEST(FormatValueTest, FindByKeyReturnsWritablePointerWithoutCreatingMembers)
    {
        FormatValue value(makeSampleObject());

        FormatValue *port = value.find("port");
        ASSERT_NE(port, nullptr);
        EXPECT_EQ(port->asInt(), kSampleInteger);

        port->variant() = std::string("rewritten");
        EXPECT_EQ(value["port"].asString(), "rewritten");

        EXPECT_EQ(value.find("nonexistent"), nullptr);
        EXPECT_EQ(value.size(), 2U); // 查找绝不创建成员

        EXPECT_EQ(FormatValue(kSampleInteger).find("any"), nullptr);
        EXPECT_EQ(FormatValue().find("any"), nullptr);
        EXPECT_EQ(makeSampleArray().find("any"), nullptr);
    }

    TEST(FormatValueTest, ConstFindByKeyReturnsReadOnlyPointer)
    {
        const FormatValue value(makeSampleObject());

        const FormatValue *host = value.find("host");
        ASSERT_NE(host, nullptr);
        EXPECT_EQ(host->asString(), "localhost");

        EXPECT_EQ(value.find("nope"), nullptr);
        EXPECT_EQ(value.find(std::string_view("nope")), nullptr);
    }

    TEST(FormatValueTest, FindByIndexReturnsPointerWithinBoundsOnly)
    {
        FormatValue value(makeSampleArray());

        FormatValue *first = value.find(static_cast<std::size_t>(0));
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->asInt(), kSampleInteger);

        EXPECT_EQ(value.find(static_cast<std::size_t>(3)), nullptr);
        EXPECT_EQ(value.find(static_cast<std::size_t>(100)), nullptr);
        EXPECT_EQ(makeSampleObject().find(static_cast<std::size_t>(0)), nullptr);
        EXPECT_EQ(FormatValue().find(static_cast<std::size_t>(0)), nullptr);

        const FormatValue readOnlyValue(makeSampleArray());
        const FormatValue *second = readOnlyValue.find(static_cast<std::size_t>(1));
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(second->asString(), "two");
    }

    TEST(FormatValueTest, SetInsertsMissingKeysAndOverwritesExistingOnes)
    {
        FormatValue value(makeSampleObject());

        FormatValue &added = value.set("timeout", FormatValue(static_cast<std::int64_t>(30)));
        EXPECT_EQ(value.size(), 3U);
        EXPECT_EQ(added.asInt(), 30);

        added = FormatValue(static_cast<std::int64_t>(60));
        EXPECT_EQ(value["timeout"].asInt(), 60);

        FormatValue &overwritten = value.set("host", FormatValue(std::string("127.0.0.1")));
        EXPECT_EQ(value.size(), 3U); // 覆盖不新增成员
        EXPECT_EQ(overwritten.asString(), "127.0.0.1");

        // 键序仍由有序 map 保证
        std::vector<std::string> keys;
        for (const auto &[key, member]: value.members())
        {
            static_cast<void>(member);
            keys.push_back(key);
        }
        EXPECT_EQ(keys, (std::vector<std::string>{"host", "port", "timeout"}));
    }

    TEST(FormatValueTest, SetOnNonObjectThrowsTypeMismatch)
    {
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).set("key", FormatValue(1))), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue().set("key", FormatValue(1))), ValueAccessError);
        EXPECT_THROW(static_cast<void>(makeSampleArray().set("key", FormatValue(1))), ValueAccessError);
    }

    TEST(FormatValueTest, EraseByKeyReportsWhetherAnythingWasRemoved)
    {
        FormatValue value(makeSampleObject());

        EXPECT_TRUE(value.erase("host"));
        EXPECT_EQ(value.size(), 1U);
        EXPECT_FALSE(value.contains("host"));

        EXPECT_FALSE(value.erase("host"));
        EXPECT_FALSE(value.erase("nonexistent"));
        EXPECT_FALSE(FormatValue(kSampleInteger).erase("any"));
        EXPECT_FALSE(FormatValue().erase("any"));
        EXPECT_FALSE(makeSampleArray().erase("any"));
        EXPECT_EQ(value.size(), 1U);
    }

    TEST(FormatValueTest, EraseByIndexShiftsRemainingElements)
    {
        FormatValue value(makeSampleArray());

        EXPECT_TRUE(value.erase(static_cast<std::size_t>(0)));
        EXPECT_EQ(value.size(), 2U);
        EXPECT_EQ(value[0].asString(), "two");
        EXPECT_EQ(value[1].asBool(), true);

        EXPECT_FALSE(value.erase(static_cast<std::size_t>(2)));
        EXPECT_FALSE(value.erase(static_cast<std::size_t>(100)));
        EXPECT_FALSE(makeSampleObject().erase(static_cast<std::size_t>(0)));
        EXPECT_EQ(value.size(), 2U);
    }

    TEST(FormatValueTest, PushBackAppendsOnlyToArrays)
    {
        FormatValue value(makeSampleArray());

        value.pushBack(FormatValue(std::string("tail")));
        EXPECT_EQ(value.size(), 4U);
        EXPECT_EQ(value[3].asString(), "tail");

        EXPECT_THROW(FormatValue(kSampleInteger).pushBack(FormatValue(1)), ValueAccessError);
        EXPECT_THROW(FormatValue().pushBack(FormatValue(1)), ValueAccessError);
    }

    TEST(FormatValueTest, InsertShiftsElementsAndReturnsWritableReference)
    {
        FormatValue value(makeSampleArray());

        FormatValue &inserted = value.insert(static_cast<std::size_t>(1), FormatValue(std::string("middle")));
        EXPECT_EQ(value.size(), 4U);
        EXPECT_EQ(value[1].asString(), "middle");
        EXPECT_EQ(value[2].asString(), "two");
        EXPECT_EQ(value[3].asBool(), true);

        inserted = FormatValue(std::string("edited"));
        EXPECT_EQ(value[1].asString(), "edited");

        // index == size() 等价于尾插
        value.insert(value.size(), FormatValue(std::string("appended")));
        EXPECT_EQ(value.size(), 5U);
        EXPECT_EQ(value[4].asString(), "appended");
    }

    TEST(FormatValueTest, InsertBeyondSizeThrowsWithoutExpanding)
    {
        FormatValue value(makeSampleArray());

        EXPECT_THROW(static_cast<void>(value.insert(static_cast<std::size_t>(4), FormatValue(1))), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.insert(static_cast<std::size_t>(100), FormatValue(1))), ValueAccessError);
        EXPECT_THROW(static_cast<void>(FormatValue(kSampleInteger).insert(static_cast<std::size_t>(0), FormatValue(1))), ValueAccessError);
        EXPECT_EQ(value.size(), 3U);
    }

    TEST(FormatValueTest, MutableIndexOperatorThrowsOutOfRangeWithoutGrowing)
    {
        FormatValue value(makeSampleArray());

        EXPECT_THROW(static_cast<void>(value[3]), ValueAccessError);
        EXPECT_EQ(value.size(), 3U);

        try
        {
            value[5] = FormatValue(1);
            FAIL() << "越界写入应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "[5]");
        }
        EXPECT_EQ(value.size(), 3U); // 刻意不自动扩容

        EXPECT_EQ(value[2].asBool(), true);
    }

    // ============================================================================
    // 遍历：数组元素与对象成员
    // ============================================================================

    TEST(FormatValueTest, ArrayIterationVisitsElementsInOrderAndAllowsMutation)
    {
        FormatValue value(makeSampleArray());

        std::size_t visitedCount = 0;
        for (FormatValue &element: value)
        {
            element = FormatValue(static_cast<std::int64_t>(visitedCount));
            ++visitedCount;
        }

        EXPECT_EQ(visitedCount, 3U);
        EXPECT_EQ(value[0].asInt(), 0);
        EXPECT_EQ(value[1].asInt(), 1);
        EXPECT_EQ(value[2].asInt(), 2);
    }

    TEST(FormatValueTest, ConstantArrayIterationYieldsReadOnlyElements)
    {
        const FormatValue value(makeSampleArray());

        std::size_t visitedCount = 0;
        for (const FormatValue &element: value)
        {
            EXPECT_FALSE(element.empty());
            ++visitedCount;
        }
        EXPECT_EQ(visitedCount, 3U);
        EXPECT_EQ(value.begin() + 3, value.end());
    }

    TEST(FormatValueTest, IteratingNonArrayValuesYieldsAnEmptyRange)
    {
        const std::vector<FormatValue> samples = {
                FormatValue(),
                FormatValue(true),
                FormatValue(kSampleInteger),
                FormatValue(std::uint64_t(1)),
                FormatValue(kSampleDouble),
                FormatValue(std::string("text")),
                makeSampleObject(),
        };

        for (const FormatValue &value: samples)
        {
            std::size_t visitedCount = 0;
            for (const FormatValue &element: value)
            {
                static_cast<void>(element);
                ++visitedCount;
            }
            EXPECT_EQ(visitedCount, 0U) << "type=" << typeName(value.type());
            EXPECT_EQ(value.begin(), value.end()) << "type=" << typeName(value.type());
        }

        // 非数组的可写迭代同样是空范围
        FormatValue mutableValue(kSampleInteger);
        EXPECT_EQ(mutableValue.begin(), mutableValue.end());
    }

    TEST(FormatValueTest, MembersRangeExposesKeysAndWritableValues)
    {
        FormatValue value(makeSampleObject());

        std::vector<std::string> keys;
        for (auto &[key, member]: value.members())
        {
            keys.push_back(key);
            if (key == "port")
            {
                member = FormatValue(static_cast<std::int64_t>(8080));
            }
        }

        EXPECT_EQ(keys, (std::vector<std::string>{"host", "port"}));
        EXPECT_EQ(value["port"].asInt(), 8080);
        EXPECT_EQ(value.members().size(), 2U);
        EXPECT_FALSE(value.members().empty());
    }

    TEST(FormatValueTest, ConstMembersRangeExposesReadOnlyMembers)
    {
        const FormatValue value(makeSampleObject());

        std::size_t memberCount = 0;
        for (const auto &[key, member]: value.members())
        {
            EXPECT_FALSE(key.empty());
            EXPECT_FALSE(member.isNull());
            ++memberCount;
        }

        EXPECT_EQ(memberCount, 2U);
        EXPECT_EQ(value.members().size(), 2U);
        EXPECT_FALSE(value.members().empty());
    }

    TEST(FormatValueTest, MembersRangeIsEmptyForNonObjectValues)
    {
        FormatValue integerValue(kSampleInteger);
        FormatValue arrayValue(makeSampleArray());

        EXPECT_TRUE(integerValue.members().empty());
        EXPECT_EQ(integerValue.members().size(), 0U);
        EXPECT_TRUE(arrayValue.members().empty());
        EXPECT_TRUE(FormatValue().members().empty());

        std::size_t visitedCount = 0;
        for (const auto &[key, member]: integerValue.members())
        {
            static_cast<void>(key);
            static_cast<void>(member);
            ++visitedCount;
        }
        EXPECT_EQ(visitedCount, 0U);

        const FormatValue readOnlyInteger(kSampleInteger);
        EXPECT_TRUE(readOnlyInteger.members().empty());
        EXPECT_EQ(readOnlyInteger.members().size(), 0U);
    }
} // namespace AsynGyanis::Base
