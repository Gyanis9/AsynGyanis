/**
 * @file TestParserValue.cpp
 * @brief ParserValue 单元测试：构造路径、类型查询、强类型与安全访问、嵌套结构与底层变体读写
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Value/ParserValue.h"
#include "Base/Parser/Value/ParserValueType.h"
#include "Base/Parser/Value/ValueAccessError.h"

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
         * @return ParserValue 数组配置值
         */
        ParserValue makeSampleArray()
        {
            ParserValueArray array;
            array.emplace_back(kSampleInteger);
            array.emplace_back(std::string("two"));
            array.emplace_back(true);
            return ParserValue(std::move(array));
        }

        /**
         * @brief 构造一个包含两个键的对象值
         * @return ParserValue 对象配置值
         */
        ParserValue makeSampleObject()
        {
            ParserValueObject object;
            object["host"] = ParserValue(std::string("localhost"));
            object["port"] = ParserValue(kSampleInteger);
            return ParserValue(std::move(object));
        }
    } // namespace

    // ============================================================================
    // 构造路径与 type() 对应关系
    // ============================================================================

    TEST(ParserValueTest, DefaultConstructionYieldsNull)
    {
        const ParserValue value;

        EXPECT_EQ(value.type(), ParserValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_TRUE(value.is<std::nullptr_t>());
        EXPECT_TRUE(value.empty());
    }

    TEST(ParserValueTest, NullPointerLiteralConstructionYieldsNull)
    {
        const ParserValue value(nullptr);

        EXPECT_EQ(value.type(), ParserValueType::Null);
        EXPECT_TRUE(value.isNull());
        EXPECT_FALSE(value.is<std::string>());
    }

    TEST(ParserValueTest, BoolConstructionYieldsBool)
    {
        const ParserValue trueValue(true);
        const ParserValue falseValue(false);

        EXPECT_EQ(trueValue.type(), ParserValueType::Bool);
        EXPECT_TRUE(trueValue.is<bool>());
        EXPECT_TRUE(trueValue.asBool());
        EXPECT_FALSE(trueValue.empty());

        EXPECT_EQ(falseValue.type(), ParserValueType::Bool);
        EXPECT_FALSE(falseValue.asBool());
        EXPECT_FALSE(falseValue.empty());
    }

    TEST(ParserValueTest, IntegerConstructionsAreStoredAsInt64)
    {
        const ParserValue fromInt(42);
        const ParserValue fromInt64(static_cast<std::int64_t>(42));

        EXPECT_EQ(fromInt.type(), ParserValueType::Int);
        EXPECT_TRUE(fromInt.is<std::int64_t>());
        EXPECT_EQ(fromInt.asInt(), 42);

        EXPECT_EQ(fromInt64.type(), ParserValueType::Int);
        EXPECT_EQ(fromInt64.asInt(), fromInt.asInt());

        EXPECT_EQ(ParserValue(std::numeric_limits<std::int64_t>::max()).asInt(), std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(ParserValue(std::numeric_limits<std::int64_t>::min()).asInt(), std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(ParserValue(static_cast<std::int64_t>(0)).asInt(), 0);
    }

    TEST(ParserValueTest, DoubleConstructionYieldsDoubleIncludingSpecialValues)
    {
        const ParserValue value(kSampleDouble);

        EXPECT_EQ(value.type(), ParserValueType::Double);
        EXPECT_DOUBLE_EQ(value.asDouble(), kSampleDouble);

        const ParserValue infiniteValue(std::numeric_limits<double>::infinity());
        EXPECT_TRUE(std::isinf(infiniteValue.asDouble()));

        const ParserValue nanValue(std::numeric_limits<double>::quiet_NaN());
        EXPECT_TRUE(std::isnan(nanValue.asDouble()));
    }

    TEST(ParserValueTest, StringConstructionsYieldString)
    {
        const ParserValue fromCString("hello");
        const ParserValue fromString(std::string("world"));

        EXPECT_EQ(fromCString.type(), ParserValueType::String);
        EXPECT_EQ(fromCString.asString(), "hello");

        EXPECT_EQ(fromString.type(), ParserValueType::String);
        EXPECT_TRUE(fromString.is<std::string>());
        EXPECT_EQ(fromString.asString(), "world");
    }

    TEST(ParserValueTest, NullCStringPointerYieldsEmptyStringNotNull)
    {
        const char *textPointer = nullptr;
        const ParserValue value(textPointer);

        EXPECT_EQ(value.type(), ParserValueType::String);
        EXPECT_FALSE(value.isNull());
        EXPECT_TRUE(value.empty());
        EXPECT_TRUE(value.asString().empty());
    }

    TEST(ParserValueTest, ContainerConstructionsYieldArrayAndObject)
    {
        const ParserValue arrayValue = makeSampleArray();
        const ParserValue objectValue = makeSampleObject();

        EXPECT_EQ(arrayValue.type(), ParserValueType::Array);
        EXPECT_TRUE(arrayValue.is<ParserValueArray>());
        EXPECT_EQ(arrayValue.asArray().size(), 3U);

        EXPECT_EQ(objectValue.type(), ParserValueType::Object);
        EXPECT_TRUE(objectValue.is<ParserValueObject>());
        EXPECT_EQ(objectValue.asObject().size(), 2U);

        EXPECT_EQ(ParserValue(ParserValueArray{}).type(), ParserValueType::Array);
        EXPECT_EQ(ParserValue(ParserValueObject{}).type(), ParserValueType::Object);
    }

    TEST(ParserValueTest, VariantIndexMatchesTypeEnumeratorForEveryConstructionPath)
    {
        const std::vector<ParserValue> samples = {
                ParserValue(nullptr),
                ParserValue(true),
                ParserValue(kSampleInteger),
                ParserValue(kSampleDouble),
                ParserValue(std::string("text")),
                makeSampleArray(),
                makeSampleObject(),
        };

        for (const ParserValue &value: samples)
        {
            EXPECT_EQ(value.variant().index(), static_cast<size_t>(value.type())) << "type=" << typeName(value.type());
        }
    }

    // ============================================================================
    // is<T> / isNull
    // ============================================================================

    TEST(ParserValueTest, IsDetectsExactAlternativeOnly)
    {
        const ParserValue value(kSampleInteger);

        EXPECT_TRUE(value.is<std::int64_t>());
        EXPECT_FALSE(value.is<bool>());
        EXPECT_FALSE(value.is<double>());
        EXPECT_FALSE(value.is<std::string>());
        EXPECT_FALSE(value.is<std::nullptr_t>());
        EXPECT_FALSE(value.is<ParserValueArray>());
        EXPECT_FALSE(value.is<ParserValueObject>());
    }

    TEST(ParserValueTest, IsNullRejectsEveryFalsyLookingValue)
    {
        EXPECT_TRUE(ParserValue().isNull());
        EXPECT_TRUE(ParserValue(nullptr).isNull());
        EXPECT_FALSE(ParserValue(false).isNull());
        EXPECT_FALSE(ParserValue(static_cast<std::int64_t>(0)).isNull());
        EXPECT_FALSE(ParserValue(0.0).isNull());
        EXPECT_FALSE(ParserValue(std::string("")).isNull());
        EXPECT_FALSE(ParserValue(ParserValueArray{}).isNull());
        EXPECT_FALSE(ParserValue(ParserValueObject{}).isNull());
    }

    // ============================================================================
    // empty() / size() 语义
    // ============================================================================

    TEST(ParserValueTest, EmptyCoversNullAndEmptyContainers)
    {
        EXPECT_TRUE(ParserValue().empty());
        EXPECT_TRUE(ParserValue(nullptr).empty());
        EXPECT_TRUE(ParserValue(std::string("")).empty());
        EXPECT_TRUE(ParserValue(ParserValueArray{}).empty());
        EXPECT_TRUE(ParserValue(ParserValueObject{}).empty());

        EXPECT_FALSE(ParserValue(false).empty());
        EXPECT_FALSE(ParserValue(static_cast<std::int64_t>(0)).empty());
        EXPECT_FALSE(ParserValue(0.0).empty());
        EXPECT_FALSE(ParserValue(std::string("x")).empty());
        EXPECT_FALSE(makeSampleArray().empty());
        EXPECT_FALSE(makeSampleObject().empty());
    }

    TEST(ParserValueTest, SizeReportsSemanticLengthPerType)
    {
        EXPECT_EQ(ParserValue(std::string("hello")).size(), 5U);
        EXPECT_EQ(ParserValue(std::string("")).size(), 0U);
        EXPECT_EQ(makeSampleArray().size(), 3U);
        EXPECT_EQ(makeSampleObject().size(), 2U);

        EXPECT_EQ(ParserValue().size(), 0U);
        EXPECT_EQ(ParserValue(false).size(), 0U);
        EXPECT_EQ(ParserValue(kSampleInteger).size(), 0U);
        EXPECT_EQ(ParserValue(kSampleDouble).size(), 0U);
    }

    // ============================================================================
    // as<T>() 强类型访问
    // ============================================================================

    TEST(ParserValueTest, AsReturnsReferenceToStoredValue)
    {
        ParserValue textValue(std::string("original"));

        EXPECT_EQ(textValue.as<std::string>(), "original");
        EXPECT_EQ(&textValue.as<std::string>(), &std::get<std::string>(textValue.variant()));

        textValue.as<std::string>() = "changed";
        EXPECT_EQ(textValue.asString(), "changed");
    }

    TEST(ParserValueTest, AsThrowsValueAccessErrorOnMismatch)
    {
        const ParserValue value(kSampleInteger);

        EXPECT_THROW(static_cast<void>(value.as<std::string>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<bool>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<double>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<ParserValueArray>()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(value.as<ParserValueObject>()), ValueAccessError);
    }

    TEST(ParserValueTest, AsExceptionCarriesExpectedAndActualTypeNames)
    {
        const ParserValue value(kSampleDouble);

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

    TEST(ParserValueTest, TypedAccessorsReturnMatchingValues)
    {
        const ParserValue arrayValue = makeSampleArray();
        const ParserValue objectValue = makeSampleObject();

        EXPECT_TRUE(ParserValue(true).asBool());
        EXPECT_EQ(ParserValue(kSampleInteger).asInt(), kSampleInteger);
        EXPECT_DOUBLE_EQ(ParserValue(kSampleDouble).asDouble(), kSampleDouble);
        EXPECT_EQ(ParserValue(std::string("data")).asString(), "data");
        EXPECT_EQ(arrayValue.asArray().size(), 3U);
        EXPECT_EQ(objectValue.asObject().size(), 2U);
    }

    TEST(ParserValueTest, TypedAccessorsThrowOnMismatch)
    {
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger).asBool()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(true).asInt()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger).asDouble()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger).asString()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger).asArray()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger).asObject()), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue().asString()), ValueAccessError);
    }

    // ============================================================================
    // get<T>() / getXxx() 安全访问
    // ============================================================================

    TEST(ParserValueTest, TemplatedGetReturnsOptionalOnMatch)
    {
        const ParserValue integerValue(kSampleInteger);
        const ParserValue textValue(std::string("text"));

        const std::optional<std::int64_t> extractedInteger = integerValue.get<std::int64_t>();
        ASSERT_TRUE(extractedInteger.has_value());
        EXPECT_EQ(*extractedInteger, kSampleInteger);

        const std::optional<std::string> extractedText = textValue.get<std::string>();
        ASSERT_TRUE(extractedText.has_value());
        EXPECT_EQ(*extractedText, "text");
    }

    TEST(ParserValueTest, TemplatedGetReturnsNulloptOnMismatch)
    {
        const ParserValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::string>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_FALSE(integerValue.get<ParserValueArray>().has_value());
        EXPECT_FALSE(integerValue.get<ParserValueObject>().has_value());
        EXPECT_FALSE(ParserValue().get<std::int64_t>().has_value());
    }

    TEST(ParserValueTest, GetDoesNotConvertBetweenIntegerAndFloatingPoint)
    {
        const ParserValue integerValue(kSampleInteger);
        const ParserValue doubleValue(kSampleDouble);

        EXPECT_FALSE(integerValue.get<double>().has_value());
        EXPECT_FALSE(doubleValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(doubleValue.get<int>().has_value());

        EXPECT_TRUE(integerValue.get<std::int64_t>().has_value());
        EXPECT_TRUE(doubleValue.get<double>().has_value());
    }

    TEST(ParserValueTest, GetDoesNotConvertBetweenBoolAndNumericTypes)
    {
        const ParserValue boolValue(true);
        const ParserValue integerValue(static_cast<std::int64_t>(1));

        EXPECT_FALSE(boolValue.get<std::int64_t>().has_value());
        EXPECT_FALSE(boolValue.get<double>().has_value());
        EXPECT_FALSE(integerValue.get<bool>().has_value());
        EXPECT_TRUE(boolValue.get<bool>().has_value());
    }

    TEST(ParserValueTest, GetNarrowsStoredIntegerToIntegralTypes)
    {
        const ParserValue integerValue(static_cast<std::int64_t>(1000));

        const std::optional<int> narrowedInteger = integerValue.get<int>();
        ASSERT_TRUE(narrowedInteger.has_value());
        EXPECT_EQ(*narrowedInteger, 1000);

        const std::optional<std::uint32_t> narrowedUnsigned = integerValue.get<std::uint32_t>();
        ASSERT_TRUE(narrowedUnsigned.has_value());
        EXPECT_EQ(*narrowedUnsigned, 1000U);

        const ParserValue doubleValue(kSampleDouble);
        const std::optional<float> narrowedFloat = doubleValue.get<float>();
        ASSERT_TRUE(narrowedFloat.has_value());
        EXPECT_FLOAT_EQ(*narrowedFloat, static_cast<float>(kSampleDouble));
    }

    TEST(ParserValueTest, GetIgnoresTypesOutsideTheVariant)
    {
        const ParserValue integerValue(kSampleInteger);

        EXPECT_FALSE(integerValue.get<std::vector<std::int64_t>>().has_value());
        EXPECT_FALSE(integerValue.get<const char *>().has_value());
    }

    TEST(ParserValueTest, NamedGettersMirrorTemplatedGet)
    {
        const ParserValue arrayValue = makeSampleArray();
        const ParserValue objectValue = makeSampleObject();

        EXPECT_EQ(ParserValue(true).getBool().value_or(false), true);
        EXPECT_EQ(ParserValue(kSampleInteger).getInt().value_or(0), kSampleInteger);
        EXPECT_DOUBLE_EQ(ParserValue(kSampleDouble).getDouble().value_or(0.0), kSampleDouble);
        EXPECT_EQ(ParserValue(std::string("text")).getString().value_or(""), "text");
        EXPECT_EQ(arrayValue.getArray().value_or(ParserValueArray{}).size(), 3U);
        EXPECT_EQ(objectValue.getObject().value_or(ParserValueObject{}).size(), 2U);

        EXPECT_FALSE(ParserValue(kSampleInteger).getBool().has_value());
        EXPECT_FALSE(ParserValue(true).getInt().has_value());
        EXPECT_FALSE(ParserValue(std::string("x")).getDouble().has_value());
        EXPECT_FALSE(ParserValue().getString().has_value());
        EXPECT_FALSE(arrayValue.getObject().has_value());
        EXPECT_FALSE(objectValue.getArray().has_value());
    }

    // ============================================================================
    // valueOr / boolOr / intOr / doubleOr / stringOr
    // ============================================================================

    TEST(ParserValueTest, ValueOrReturnsStoredValueOnMatch)
    {
        EXPECT_EQ(ParserValue(kSampleInteger).valueOr(static_cast<std::int64_t>(99)), kSampleInteger);
        EXPECT_EQ(ParserValue(std::string("kept")).valueOr(std::string("fallback")), "kept");
        EXPECT_EQ(ParserValue(true).valueOr(false), true);
    }

    TEST(ParserValueTest, ValueOrFallsBackToDefaultOnMismatch)
    {
        EXPECT_EQ(ParserValue(std::string("text")).valueOr(static_cast<std::int64_t>(99)), 99);
        EXPECT_EQ(ParserValue(kSampleInteger).valueOr(std::string("fallback")), "fallback");
        EXPECT_EQ(ParserValue().valueOr(static_cast<std::int64_t>(-1)), -1);
    }

    TEST(ParserValueTest, NamedDefaultsFallBackOnTypeMismatch)
    {
        EXPECT_EQ(ParserValue(true).boolOr(false), true);
        EXPECT_EQ(ParserValue(kSampleInteger).boolOr(true), true);
        EXPECT_EQ(ParserValue(kSampleInteger).boolOr(false), false);

        EXPECT_EQ(ParserValue(kSampleInteger).intOr(0), kSampleInteger);
        EXPECT_EQ(ParserValue(std::string("text")).intOr(100), 100);
        EXPECT_EQ(ParserValue(kSampleDouble).intOr(100), 100);

        EXPECT_DOUBLE_EQ(ParserValue(kSampleDouble).doubleOr(0.0), kSampleDouble);
        EXPECT_DOUBLE_EQ(ParserValue(kSampleInteger).doubleOr(1.5), 1.5);

        EXPECT_EQ(ParserValue(std::string("hello")).stringOr("default"), "hello");
        EXPECT_EQ(ParserValue(kSampleInteger).stringOr("default"), "default");
        EXPECT_EQ(ParserValue().stringOr("default"), "default");
    }

    // ============================================================================
    // 对象与数组访问
    // ============================================================================

    TEST(ParserValueTest, ContainsMatchesObjectKeysOnly)
    {
        const ParserValue objectValue = makeSampleObject();

        EXPECT_TRUE(objectValue.contains("host"));
        EXPECT_TRUE(objectValue.contains("port"));
        EXPECT_FALSE(objectValue.contains("nonexistent"));
        EXPECT_FALSE(objectValue.contains(""));

        EXPECT_FALSE(ParserValue(kSampleInteger).contains("host"));
        EXPECT_FALSE(ParserValue(std::string("host")).contains("host"));
        EXPECT_FALSE(makeSampleArray().contains("host"));
        EXPECT_FALSE(ParserValue().contains("host"));
    }

    TEST(ParserValueTest, KeyIndexerReturnsObjectMember)
    {
        const ParserValue objectValue = makeSampleObject();

        EXPECT_EQ(objectValue["host"].asString(), "localhost");
        EXPECT_EQ(objectValue["port"].asInt(), kSampleInteger);
    }

    TEST(ParserValueTest, KeyIndexerThrowsForMissingMember)
    {
        const ParserValue objectValue = makeSampleObject();

        try
        {
            static_cast<void>(objectValue["nonexistent"]);
            FAIL() << "缺失键应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "nonexistent");
        }
    }

    TEST(ParserValueTest, KeyIndexerThrowsTypeExceptionForNonObject)
    {
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger)["key"]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(std::string("text"))["key"]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(makeSampleArray()["key"]), ValueAccessError);
    }

    TEST(ParserValueTest, IndexIndexerReturnsArrayElement)
    {
        const ParserValue arrayValue = makeSampleArray();

        EXPECT_EQ(arrayValue[0].asInt(), kSampleInteger);
        EXPECT_EQ(arrayValue[1].asString(), "two");
        EXPECT_EQ(arrayValue[2].asBool(), true);
    }

    TEST(ParserValueTest, IndexIndexerThrowsForOutOfRangeIndex)
    {
        const ParserValue arrayValue = makeSampleArray();

        try
        {
            static_cast<void>(arrayValue[3]);
            FAIL() << "越界下标应当抛出 ValueAccessError";
        } catch (const ValueAccessError &exception) {
            EXPECT_EQ(exception.key(), "[3]");
        }

        EXPECT_THROW(static_cast<void>(static_cast<void>(arrayValue[100])), ValueAccessError);
        EXPECT_THROW(static_cast<void>(static_cast<void>(ParserValue(ParserValueArray{})[0])), ValueAccessError);
    }

    TEST(ParserValueTest, IndexIndexerThrowsTypeExceptionForNonArray)
    {
        EXPECT_THROW(static_cast<void>(ParserValue(kSampleInteger)[0]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(ParserValue(std::string("text"))[0]), ValueAccessError);
        EXPECT_THROW(static_cast<void>(makeSampleObject()[0]), ValueAccessError);
    }

    TEST(ParserValueTest, KeyBasedGetReturnsReferenceWrapperForExistingMember)
    {
        const ParserValue objectValue = makeSampleObject();

        const std::optional<std::reference_wrapper<const ParserValue> > member = objectValue.get("port");
        ASSERT_TRUE(member.has_value());
        EXPECT_EQ(member->get().asInt(), kSampleInteger);

        EXPECT_FALSE(objectValue.get("nonexistent").has_value());
        EXPECT_FALSE(ParserValue(kSampleInteger).get("any").has_value());
        EXPECT_FALSE(ParserValue().get("any").has_value());
    }

    TEST(ParserValueTest, TemplatedKeyBasedGetCombinesLookupAndTypeCheck)
    {
        const ParserValue objectValue = makeSampleObject();

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

    TEST(ParserValueTest, NestedObjectAccessChainsThroughIndexers)
    {
        ParserValueObject inner;
        inner["value"] = ParserValue(kSampleInteger);

        ParserValueObject middle;
        middle["inner"] = ParserValue(std::move(inner));

        ParserValueObject outer;
        outer["middle"] = ParserValue(std::move(middle));

        const ParserValue value(std::move(outer));

        EXPECT_TRUE(value.contains("middle"));
        EXPECT_TRUE(value["middle"].contains("inner"));
        EXPECT_EQ(value["middle"]["inner"]["value"].asInt(), kSampleInteger);
        EXPECT_EQ(value["middle"]["inner"].size(), 1U);
    }

    TEST(ParserValueTest, NestedArrayElementsKeepTheirOwnTypes)
    {
        ParserValueObject namedElement;
        namedElement["name"] = ParserValue(std::string("alpha"));

        ParserValueArray array;
        array.emplace_back(static_cast<std::int64_t>(1));
        array.emplace_back(std::move(namedElement));
        array.emplace_back(ParserValueArray{});

        const ParserValue value(std::move(array));

        EXPECT_EQ(value.size(), 3U);
        EXPECT_EQ(value[0].asInt(), 1);
        EXPECT_EQ(value[1]["name"].asString(), "alpha");
        EXPECT_EQ(value[2].type(), ParserValueType::Array);
        EXPECT_TRUE(value[2].empty());
    }

    // ============================================================================
    // variant() 读写访问
    // ============================================================================

    TEST(ParserValueTest, ConstVariantExposesStoredAlternative)
    {
        const ParserValue integerValue(kSampleInteger);

        EXPECT_EQ(integerValue.variant().index(), 2U);
        EXPECT_TRUE(std::holds_alternative<std::int64_t>(integerValue.variant()));
        EXPECT_EQ(std::get<std::int64_t>(integerValue.variant()), kSampleInteger);
    }

    TEST(ParserValueTest, MutableVariantAssignmentChangesType)
    {
        ParserValue value(kSampleInteger);

        value.variant() = std::string("changed");

        EXPECT_EQ(value.type(), ParserValueType::String);
        EXPECT_EQ(value.asString(), "changed");
        EXPECT_FALSE(value.get<std::int64_t>().has_value());
    }

    // ============================================================================
    // 拷贝与移动语义
    // ============================================================================

    TEST(ParserValueTest, CopyConstructionProducesIndependentValue)
    {
        ParserValue original(std::string("shared-name"));
        const ParserValue copy(original);

        EXPECT_EQ(copy.type(), ParserValueType::String);
        EXPECT_EQ(copy.asString(), "shared-name");
        EXPECT_NE(&copy.as<std::string>(), &original.as<std::string>());

        original.as<std::string>() = "mutated";
        EXPECT_EQ(copy.asString(), "shared-name");
    }

    TEST(ParserValueTest, CopyAssignmentReplacesTargetValue)
    {
        ParserValue target(std::string("initial"));
        const ParserValue source(kSampleInteger);

        target = source;

        EXPECT_EQ(target.type(), ParserValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
        EXPECT_EQ(source.asInt(), kSampleInteger);
    }

    TEST(ParserValueTest, MoveConstructionTransfersPayload)
    {
        ParserValue original(ParserValueArray{ParserValue(kSampleInteger), ParserValue(kSampleDouble)});
        const ParserValue moved(std::move(original));

        EXPECT_EQ(moved.type(), ParserValueType::Array);
        EXPECT_EQ(moved.size(), 2U);
        EXPECT_EQ(moved[0].asInt(), kSampleInteger);
    }

    TEST(ParserValueTest, MoveAssignmentReplacesTargetAndKeepsPayload)
    {
        ParserValue target(std::string("discarded"));
        ParserValue source(kSampleInteger);

        target = std::move(source);

        EXPECT_EQ(target.type(), ParserValueType::Int);
        EXPECT_EQ(target.asInt(), kSampleInteger);
    }

    TEST(ParserValueTest, ValueSemanticsTraitsHold)
    {
        static_assert(std::is_copy_constructible_v<ParserValue>);
        static_assert(std::is_copy_assignable_v<ParserValue>);
        static_assert(std::is_nothrow_move_constructible_v<ParserValue>);
        static_assert(std::is_nothrow_move_assignable_v<ParserValue>);
        static_assert(std::is_nothrow_default_constructible_v<ParserValue>);

        SUCCEED();
    }

    TEST(ParserValueTest, CopiedContainerValueKeepsAllMembers)
    {
        const ParserValue original = makeSampleObject();
        const ParserValue copy(original);

        EXPECT_EQ(copy["port"].asInt(), kSampleInteger);
        EXPECT_EQ(copy["host"].asString(), "localhost");
        EXPECT_EQ(copy.size(), original.size());
    }

    // ============================================================================
    // 边界与有界压力
    // ============================================================================

    TEST(ParserValueTest, LongStringValueKeepsExactSize)
    {
        const std::string longText(4096, 'X');
        const ParserValue value(longText);

        EXPECT_EQ(value.size(), longText.size());
        EXPECT_EQ(value.asString().size(), longText.size());
        EXPECT_FALSE(value.empty());
    }

    TEST(ParserValueTest, EmbeddedNullCharacterIsPartOfTheString)
    {
        const std::string textWithNull("hello\0world", 11);
        const ParserValue value(textWithNull);

        EXPECT_EQ(value.type(), ParserValueType::String);
        EXPECT_EQ(value.asString().size(), 11U);
        EXPECT_FALSE(value.empty());
    }

    TEST(ParserValueTest, ArrayWithManyElementsIsIndexableAtBothEnds)
    {
        constexpr std::size_t kElementCount = 1000;

        ParserValueArray array;
        array.reserve(kElementCount);
        for (std::size_t index = 0; index < kElementCount; ++index)
        {
            array.emplace_back(static_cast<std::int64_t>(index));
        }

        const ParserValue value(std::move(array));

        EXPECT_EQ(value.size(), kElementCount);
        EXPECT_EQ(value[0].asInt(), 0);
        EXPECT_EQ(value[kElementCount - 1].asInt(), static_cast<std::int64_t>(kElementCount - 1));
        EXPECT_THROW(static_cast<void>(static_cast<void>(value[kElementCount])), ValueAccessError);
    }

    TEST(ParserValueTest, RepeatedVariantSwitchingStaysConsistent)
    {
        // 迁移自旧 Catch2 压力用例：迭代次数由 10000 降至 1000，断言保持确定性
        ParserValue value(nullptr);
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            value = ParserValue(static_cast<std::int64_t>(iteration));
            ASSERT_EQ(value.type(), ParserValueType::Int);
            EXPECT_EQ(value.asInt(), iteration);

            value = ParserValue(std::to_string(iteration));
            ASSERT_EQ(value.type(), ParserValueType::String);
            EXPECT_EQ(value.asString(), std::to_string(iteration));

            value = ParserValue(iteration % 2 == 0);
            ASSERT_EQ(value.type(), ParserValueType::Bool);
            EXPECT_EQ(value.asBool(), iteration % 2 == 0);
        }
    }
} // namespace AsynGyanis::Base
