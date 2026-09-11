/**
 * @file TestJsonParser.cpp
 * @brief JsonParser 单元测试：各类值、转义、数字格式与错误定位
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/JsonParser.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"
#include "Base/Parser/ParserError.h"
#include "Base/Parser/ParserPosition.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 执行一段应当失败的解析并取回解析错误
         * @param action 触发解析的动作
         * @return ParserError 捕获到的错误对象副本
         */
        ParserError catchParserError(const std::function<ConfigValue()> &action)
        {
            try
            {
                static_cast<void>(action());
            } catch (const ParserError &error)
            {
                return error;
            }
            catch (...)
            {
                ADD_FAILURE() << "预期抛出 ParserError，实际抛出了其他异常";
                return ParserError("wrong exception type", ParserPosition{});
            }

            ADD_FAILURE() << "预期抛出 ParserError，但解析成功";
            return ParserError("no exception thrown", ParserPosition{});
        }
    } // namespace

    // ============================================================================
    // 标量
    // ============================================================================

    TEST(JsonParser, ParsesTopLevelScalars)
    {
        EXPECT_EQ(JsonParser::parse("true").asBool(), true);
        EXPECT_EQ(JsonParser::parse("false").asBool(), false);
        EXPECT_TRUE(JsonParser::parse("null").isNull());
        EXPECT_EQ(JsonParser::parse("42").asInt(), 42);
        EXPECT_EQ(JsonParser::parse("-7").asInt(), -7);
        EXPECT_DOUBLE_EQ(JsonParser::parse("3.5").asDouble(), 3.5);
        EXPECT_DOUBLE_EQ(JsonParser::parse("-0.25").asDouble(), -0.25);
        EXPECT_DOUBLE_EQ(JsonParser::parse("1e3").asDouble(), 1000.0);
        EXPECT_DOUBLE_EQ(JsonParser::parse("1.5E-2").asDouble(), 0.015);
        EXPECT_EQ(JsonParser::parse("\"text\"").asString(), "text");
    }

    TEST(JsonParser, DistinguishesIntegerFromFloatingPoint)
    {
        EXPECT_EQ(JsonParser::parse("100").type(), ConfigValueType::Int);
        EXPECT_EQ(JsonParser::parse("100.0").type(), ConfigValueType::Double);
        EXPECT_EQ(JsonParser::parse("1e2").type(), ConfigValueType::Double);
    }

    TEST(JsonParser, PromotesIntegersBeyondInt64RangeToDouble)
    {
        const ConfigValue value = JsonParser::parse("99999999999999999999999");

        EXPECT_EQ(value.type(), ConfigValueType::Double);
        EXPECT_DOUBLE_EQ(value.asDouble(), 1e23);
    }

    // ============================================================================
    // 容器
    // ============================================================================

    TEST(JsonParser, ParsesEmptyContainers)
    {
        EXPECT_TRUE(JsonParser::parse("[]").asArray().empty());
        EXPECT_TRUE(JsonParser::parse("{}").asObject().empty());
    }

    TEST(JsonParser, ParsesArrayPreservingOrder)
    {
        const ConfigValue value = JsonParser::parse("[1, \"two\", false, null]");

        const ConfigArray &elements = value.asArray();
        ASSERT_EQ(elements.size(), 4U);
        EXPECT_EQ(elements[0].asInt(), 1);
        EXPECT_EQ(elements[1].asString(), "two");
        EXPECT_EQ(elements[2].asBool(), false);
        EXPECT_TRUE(elements[3].isNull());
    }

    TEST(JsonParser, ParsesNestedObjectsAndArrays)
    {
        const ConfigValue value = JsonParser::parse(R"({"server":{"ports":[80,443],"tls":true}})");

        const ConfigValue &server = value.asObject().at("server");
        EXPECT_EQ(server.asObject().at("tls").asBool(), true);

        const ConfigArray &ports = server.asObject().at("ports").asArray();
        ASSERT_EQ(ports.size(), 2U);
        EXPECT_EQ(ports[0].asInt(), 80);
        EXPECT_EQ(ports[1].asInt(), 443);
    }

    TEST(JsonParser, AcceptsWhitespaceBetweenEveryToken)
    {
        const ConfigValue value = JsonParser::parse("{\n  \"a\" : [ 1 ,\t2 ]\n}");

        EXPECT_EQ(value.asObject().at("a").asArray().size(), 2U);
    }

    // ============================================================================
    // 字符串与转义
    // ============================================================================

    TEST(JsonParser, DecodesAllSimpleEscapes)
    {
        const ConfigValue value = JsonParser::parse(R"("q\"b\\f\fnn\r\t\/u")");

        EXPECT_EQ(value.asString(), "q\"b\\f\fnn\r\t/u");
    }

    TEST(JsonParser, DecodesBasicMultilingualPlaneEscape)
    {
        const std::string letterEscape = "\"\\u0041\"";
        const std::string euroEscape   = "\"\\u20AC\"";

        EXPECT_EQ(JsonParser::parse(letterEscape).asString(), "A");
        EXPECT_EQ(JsonParser::parse(euroEscape).asString(), "\xE2\x82\xAC");
    }

    TEST(JsonParser, DecodesSurrogatePairIntoFourByteUtf8)
    {
        const std::string rocketEscape = "\"\\uD83D\\uDE80\"";

        const ConfigValue value = JsonParser::parse(rocketEscape);

        // 火箭表情在 UTF-8 下占 4 字节
        EXPECT_EQ(value.asString(), "\xF0\x9F\x9A\x80");
        EXPECT_EQ(value.asString().size(), 4U);
    }

    TEST(JsonParser, KeepsNonAsciiUtf8TextIntact)
    {
        const ConfigValue value = JsonParser::parse(R"("中文日志")");

        EXPECT_EQ(value.asString(), "中文日志");
    }

    TEST(JsonParser, EmptyStringParsesToEmptyValue)
    {
        EXPECT_EQ(JsonParser::parse("\"\"").asString(), "");
    }

    // ============================================================================
    // 错误路径与定位
    // ============================================================================

    TEST(JsonParser, RejectsEmptyInput)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("");
        });

        EXPECT_NE(std::string(error.what()).find("empty"), std::string::npos);
    }

    TEST(JsonParser, RejectsSingleQuotedStrings)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("'single'");
        });

        EXPECT_NE(std::string(error.what()).find("expected a JSON value"), std::string::npos);
    }

    TEST(JsonParser, RejectsComments)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("{ /* note */ }");
        });

        EXPECT_NE(std::string(error.what()).find("key"), std::string::npos);
    }

    TEST(JsonParser, RejectsTrailingCommaInArray)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("[1,2,]");
        });

        EXPECT_NE(std::string(error.what()).find("expected a JSON value"), std::string::npos);
    }

    TEST(JsonParser, RejectsTrailingCommaInObject)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(R"({"a":1,})");
        });

        EXPECT_NE(std::string(error.what()).find("double-quoted"), std::string::npos);
    }

    TEST(JsonParser, RejectsLeadingZeros)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("01");
        });

        EXPECT_NE(std::string(error.what()).find("leading zeros"), std::string::npos);
    }

    TEST(JsonParser, RejectsIncompleteFractionAndExponent)
    {
        const ParserError fractionError = catchParserError([]
        {
            return JsonParser::parse("1.");
        });
        const ParserError exponentError = catchParserError([]
        {
            return JsonParser::parse("1e");
        });

        EXPECT_NE(std::string(fractionError.what()).find("decimal point"), std::string::npos);
        EXPECT_NE(std::string(exponentError.what()).find("exponent"), std::string::npos);
    }

    TEST(JsonParser, RejectsNonFiniteLiterals)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("NaN");
        });

        EXPECT_NE(std::string(error.what()).find("expected a JSON value"), std::string::npos);
    }

    TEST(JsonParser, RejectsIncompleteKeyword)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("truely");
        });

        EXPECT_NE(std::string(error.what()).find("malformed keyword"), std::string::npos);
    }

    TEST(JsonParser, RejectsDuplicateObjectKeys)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(R"({"a":1,"a":2})");
        });

        EXPECT_NE(std::string(error.what()).find("duplicate object key: a"), std::string::npos);
    }

    TEST(JsonParser, RejectsUnterminatedString)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("\"unterminated");
        });

        EXPECT_NE(std::string(error.what()).find("unterminated string"), std::string::npos);
    }

    TEST(JsonParser, RejectsRawControlCharacterInsideString)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(std::string("\"line\0break\"", 12));
        });

        EXPECT_NE(std::string(error.what()).find("control characters"), std::string::npos);
    }

    TEST(JsonParser, RejectsLoneHighSurrogate)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(R"("\uD800")");
        });

        EXPECT_NE(std::string(error.what()).find("surrogate"), std::string::npos);
    }

    TEST(JsonParser, RejectsInvalidHexEscape)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(R"("\u00ZZ")");
        });

        EXPECT_NE(std::string(error.what()).find("hexadecimal"), std::string::npos);
    }

    TEST(JsonParser, RejectsUnknownEscapeLetter)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse(R"("\q")");
        });

        EXPECT_NE(std::string(error.what()).find("unsupported escape"), std::string::npos);
    }

    TEST(JsonParser, RejectsContentAfterTheDocument)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("1 2");
        });

        EXPECT_NE(std::string(error.what()).find("after the end of the document"), std::string::npos);
    }

    TEST(JsonParser, ReportsLineAndColumnOfTheFailure)
    {
        const ParserError error = catchParserError([]
        {
            return JsonParser::parse("{\n  \"a\": @\n}");
        });

        EXPECT_EQ(error.position().lineNumber, 2U);
        EXPECT_EQ(error.position().columnNumber, 8U);
    }

    TEST(JsonParser, EnforcesNestingDepthLimit)
    {
        const std::string deeplyNested(40, '[');
        const std::string closed = deeplyNested + std::string(40, ']');

        const ParserError error = catchParserError([&closed]
        {
            return JsonParser::parse(closed);
        });

        EXPECT_NE(std::string(error.what()).find("nesting depth"), std::string::npos);
    }
} // namespace AsynGyanis::Base
