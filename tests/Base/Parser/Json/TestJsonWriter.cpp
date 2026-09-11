/**
 * @file TestJsonWriter.cpp
 * @brief JsonWriter 单元测试：各类型输出、转义、缩进与解析回环一致性
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Json/JsonWriter.h"

#include "Base/Parser/Value/ParserValue.h"
#include "Base/Parser/Json/JsonParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace AsynGyanis::Base
{
    TEST(JsonWriter, WritesScalarsInCanonicalForm)
    {
        EXPECT_EQ(JsonWriter::write(ParserValue(nullptr)), "null");
        EXPECT_EQ(JsonWriter::write(ParserValue(true)), "true");
        EXPECT_EQ(JsonWriter::write(ParserValue(false)), "false");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::int64_t(42))), "42");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::int64_t(-7))), "-7");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("text"))), "\"text\"");
    }

    TEST(JsonWriter, KeepsFloatingPointShapeParseable)
    {
        // 整数值浮点必须带小数点，否则回读会变成 int64
        EXPECT_EQ(JsonWriter::write(ParserValue(3.0)), "3.0");
        EXPECT_EQ(JsonWriter::write(ParserValue(3.5)), "3.5");
        EXPECT_EQ(JsonWriter::write(ParserValue(-0.25)), "-0.25");
    }

    TEST(JsonWriter, PreservesFullDoublePrecision)
    {
        const std::string serialized = JsonWriter::write(ParserValue(0.1234567890123));

        // ostream 默认精度只会写到 0.123457，这里必须能无损回读
        EXPECT_DOUBLE_EQ(JsonParser::parse(serialized).asDouble(), 0.1234567890123);
    }

    TEST(JsonWriter, WritesNonFiniteDoublesAsNull)
    {
        const double notANumber = std::numeric_limits<double>::quiet_NaN();
        const double infinity   = std::numeric_limits<double>::infinity();

        EXPECT_EQ(JsonWriter::write(ParserValue(notANumber)), "null");
        EXPECT_EQ(JsonWriter::write(ParserValue(infinity)), "null");
        EXPECT_EQ(JsonWriter::write(ParserValue(-infinity)), "null");
    }

    TEST(JsonWriter, WritesEmptyContainersWithoutInnerSpace)
    {
        EXPECT_EQ(JsonWriter::write(ParserValue(ParserValueArray{})), "[]");
        EXPECT_EQ(JsonWriter::write(ParserValue(ParserValueObject{})), "{}");
        EXPECT_EQ(JsonWriter::write(ParserValue(ParserValueArray{}), true), "[]");
        EXPECT_EQ(JsonWriter::write(ParserValue(ParserValueObject{}), true), "{}");
    }

    TEST(JsonWriter, WritesCompactArrayInOrder)
    {
        const ParserValueArray elements = {ParserValue(std::int64_t(1)), ParserValue(std::string("two")), ParserValue(true)};

        EXPECT_EQ(JsonWriter::write(ParserValue(elements)), "[1,\"two\",true]");
    }

    TEST(JsonWriter, WritesCompactObjectWithSortedKeys)
    {
        ParserValueObject members;
        members.emplace("b", ParserValue(std::int64_t(2)));
        members.emplace("a", ParserValue(std::int64_t(1)));

        EXPECT_EQ(JsonWriter::write(ParserValue(std::move(members))), "{\"a\":1,\"b\":2}");
    }

    TEST(JsonWriter, IndentsNestedStructuresWithTwoSpaces)
    {
        ParserValueArray inner;
        inner.push_back(ParserValue(std::int64_t(1)));

        ParserValueObject server;
        server.emplace("ports", ParserValue(std::move(inner)));

        ParserValueObject root;
        root.emplace("server", ParserValue(std::move(server)));

        const std::string output = JsonWriter::write(ParserValue(std::move(root)), true);

        EXPECT_NE(output.find("{\n  \"server\": {\n"), std::string::npos) << output;
        EXPECT_NE(output.find("    \"ports\": [\n      1\n    ]"), std::string::npos) << output;
    }

    TEST(JsonWriter, EscapesControlCharactersAndQuotes)
    {
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("say \"hi\""))), "\"say \\\"hi\\\"\"");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("back\\slash"))), "\"back\\\\slash\"");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("tab\there"))), "\"tab\\there\"");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("line\nbreak"))), "\"line\\nbreak\"");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string("feed\bhere"))), "\"feed\\bhere\"");
        EXPECT_EQ(JsonWriter::write(ParserValue(std::string(1, '\x01'))), "\"\\u0001\"");
    }

    TEST(JsonWriter, KeepsNonAsciiTextAsUtf8Bytes)
    {
        const std::string output = JsonWriter::write(ParserValue(std::string("中文")));

        // 非 ASCII 直接以 UTF-8 字节输出，不转义成 \uXXXX
        EXPECT_EQ(output, "\"中文\"");
        EXPECT_EQ(JsonParser::parse(output).asString(), "中文");
    }

    TEST(JsonWriter, RoundTripsThroughTheParserWithoutLoss)
    {
        const std::string source = "{\"a\":[1,2.5,\"x\",true,null,{\"b\":[]}],\"c\":\"\\u0041\"}";

        const std::string firstPass  = JsonWriter::write(JsonParser::parse(source));
        const std::string secondPass = JsonWriter::write(JsonParser::parse(firstPass));

        EXPECT_EQ(firstPass, secondPass) << firstPass;
        EXPECT_EQ(firstPass, "{\"a\":[1,2.5,\"x\",true,null,{\"b\":[]}],\"c\":\"A\"}");
    }

    TEST(JsonWriter, RoundTripsIndentedOutputBackToItself)
    {
        ParserValueObject members;
        members.emplace("ratio", ParserValue(1.5));
        members.emplace("count", ParserValue(std::int64_t(3)));

        const std::string indented = JsonWriter::write(ParserValue(std::move(members)), true);
        const std::string reparsed = JsonWriter::write(JsonParser::parse(indented), true);

        EXPECT_EQ(indented, reparsed);
    }
} // namespace AsynGyanis::Base
