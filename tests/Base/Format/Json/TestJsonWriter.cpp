/**
 * @file TestJsonWriter.cpp
 * @brief JsonWriter 单元测试：各类型输出、转义、缩进与解析回环一致性
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonWriter.h"

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/FormatError.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 手工构造指定层数的嵌套数组
         * @details 绕过解析器的深度上限，用于验证序列化侧的深度守护；
         *          最外层数组本身算第 0 层。
         * @param depth 容器层数
         * @return FormatValue 最外层数组
         */
        FormatValue makeNestedArrays(const std::size_t depth)
        {
            FormatValue value(FormatValueArray{});
            for (std::size_t round = 0; round < depth; ++round)
            {
                FormatValueArray wrapper;
                wrapper.push_back(std::move(value));
                value = FormatValue(std::move(wrapper));
            }
            return value;
        }
    } // namespace

    TEST(JsonWriter, WritesScalarsInCanonicalForm)
    {
        EXPECT_EQ(JsonWriter::write(FormatValue(nullptr)), "null");
        EXPECT_EQ(JsonWriter::write(FormatValue(true)), "true");
        EXPECT_EQ(JsonWriter::write(FormatValue(false)), "false");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::int64_t(42))), "42");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::int64_t(-7))), "-7");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("text"))), "\"text\"");
    }

    TEST(JsonWriter, KeepsFloatingPointShapeParseable)
    {
        // 整数值浮点必须带小数点，否则回读会变成 int64
        EXPECT_EQ(JsonWriter::write(FormatValue(3.0)), "3.0");
        EXPECT_EQ(JsonWriter::write(FormatValue(3.5)), "3.5");
        EXPECT_EQ(JsonWriter::write(FormatValue(-0.25)), "-0.25");
    }

    TEST(JsonWriter, PreservesFullDoublePrecision)
    {
        const std::string serialized = JsonWriter::write(FormatValue(0.1234567890123));

        // ostream 默认精度只会写到 0.123457，这里必须能无损回读
        EXPECT_DOUBLE_EQ(JsonParser::parse(serialized).asDouble(), 0.1234567890123);
    }

    TEST(JsonWriter, WritesNonFiniteDoublesAsNull)
    {
        const double notANumber = std::numeric_limits<double>::quiet_NaN();
        const double infinity   = std::numeric_limits<double>::infinity();

        EXPECT_EQ(JsonWriter::write(FormatValue(notANumber)), "null");
        EXPECT_EQ(JsonWriter::write(FormatValue(infinity)), "null");
        EXPECT_EQ(JsonWriter::write(FormatValue(-infinity)), "null");
    }

    TEST(JsonWriter, WritesEmptyContainersWithoutInnerSpace)
    {
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueArray{})), "[]");
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueObject{})), "{}");
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueArray{}), true), "[]");
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueObject{}), true), "{}");
    }

    TEST(JsonWriter, WritesCompactArrayInOrder)
    {
        const FormatValueArray elements = {FormatValue(std::int64_t(1)), FormatValue(std::string("two")), FormatValue(true)};

        EXPECT_EQ(JsonWriter::write(FormatValue(elements)), "[1,\"two\",true]");
    }

    TEST(JsonWriter, WritesCompactObjectWithSortedKeys)
    {
        FormatValueObject members;
        members.emplace("b", FormatValue(std::int64_t(2)));
        members.emplace("a", FormatValue(std::int64_t(1)));

        EXPECT_EQ(JsonWriter::write(FormatValue(std::move(members))), "{\"a\":1,\"b\":2}");
    }

    TEST(JsonWriter, IndentsNestedStructuresWithTwoSpaces)
    {
        FormatValueArray inner;
        inner.push_back(FormatValue(std::int64_t(1)));

        FormatValueObject server;
        server.emplace("ports", FormatValue(std::move(inner)));

        FormatValueObject root;
        root.emplace("server", FormatValue(std::move(server)));

        const std::string output = JsonWriter::write(FormatValue(std::move(root)), true);

        EXPECT_NE(output.find("{\n  \"server\": {\n"), std::string::npos) << output;
        EXPECT_NE(output.find("    \"ports\": [\n      1\n    ]"), std::string::npos) << output;
    }

    TEST(JsonWriter, EscapesControlCharactersAndQuotes)
    {
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("say \"hi\""))), "\"say \\\"hi\\\"\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("back\\slash"))), "\"back\\\\slash\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("tab\there"))), "\"tab\\there\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("line\nbreak"))), "\"line\\nbreak\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("feed\bhere"))), "\"feed\\bhere\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string(1, '\x01'))), "\"\\u0001\"");
    }

    TEST(JsonWriter, KeepsNonAsciiTextAsUtf8Bytes)
    {
        const std::string output = JsonWriter::write(FormatValue(std::string("中文")));

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
        FormatValueObject members;
        members.emplace("ratio", FormatValue(1.5));
        members.emplace("count", FormatValue(std::int64_t(3)));

        const std::string indented = JsonWriter::write(FormatValue(std::move(members)), true);
        const std::string reparsed = JsonWriter::write(JsonParser::parse(indented), true);

        EXPECT_EQ(indented, reparsed);
    }

    // ============================================================================
    // 无符号 64 位整数
    // ============================================================================

    TEST(JsonWriter, WritesUnsignedIntegersInDecimalForm)
    {
        EXPECT_EQ(JsonWriter::write(FormatValue(std::uint64_t(0))), "0");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::uint64_t(42))), "42");
        EXPECT_EQ(JsonWriter::write(FormatValue(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U)), "9223372036854775808");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::numeric_limits<std::uint64_t>::max())), "18446744073709551615");
    }

    TEST(JsonWriter, RoundTripsUnsignedIntegersWithoutLosingType)
    {
        const std::string source     = "[0,42,9223372036854775808,18446744073709551615]";
        const std::string serialized = JsonWriter::write(JsonParser::parse(source));

        EXPECT_EQ(serialized, source);

        // asArray() 返回的是内部数组的零拷贝引用，其生命周期依附于所属的 FormatValue：
        // 若把引用直接绑到 parse(...) 的临时返回值上，临时值在整条表达式结束处即析构
        // （vector 析构会把首尾指针置空、size() 归零），随后 elements[0] 就是在空数组上
        // 取下标，触发调试断言 "vector subscript out of range"。因此必须先让解析结果落地。
        const FormatValue       parsed   = JsonParser::parse(serialized);
        const FormatValueArray &elements = parsed.asArray();
        EXPECT_EQ(elements[0].type(), FormatValueType::Int);
        EXPECT_EQ(elements[2].type(), FormatValueType::UInt);
        EXPECT_EQ(elements[3].asUInt(), std::numeric_limits<std::uint64_t>::max());
    }

    // ============================================================================
    // JsonWriteOptions：indentWidth
    // ============================================================================

    TEST(JsonWriter, OptionsOverloadReproducesTheLegacyOutput)
    {
        FormatValueObject members;
        members.emplace("b", FormatValue(std::uint64_t(2)));
        members.emplace("a", FormatValue(std::string("文本")));
        const FormatValue value(std::move(members));

        // 默认选项 = 历史上的 indentWidth 2 + 键升序 + 原样 UTF-8
        EXPECT_EQ(JsonWriter::write(value, true), JsonWriter::write(value, JsonWriteOptions{}));
        EXPECT_EQ(JsonWriter::write(value), JsonWriter::write(value, JsonWriteOptions{.indentWidth = 0}));
        EXPECT_EQ(JsonWriter::write(value), R"({"a":"文本","b":2})");
    }

    TEST(JsonWriter, ZeroIndentWidthProducesCompactOutput)
    {
        FormatValueArray elements{FormatValue(std::int64_t(1)), FormatValue(std::int64_t(2))};
        FormatValueObject members;
        members.emplace("list", FormatValue(std::move(elements)));

        const std::string compact = JsonWriter::write(FormatValue(std::move(members)), JsonWriteOptions{.indentWidth = 0});

        EXPECT_EQ(compact, R"({"list":[1,2]})");
        EXPECT_EQ(compact.find('\n'), std::string::npos);
        EXPECT_EQ(compact.find(": "), std::string::npos);
    }

    TEST(JsonWriter, CustomIndentWidthControlsLeadingSpaces)
    {
        FormatValueObject members;
        members.emplace("key", FormatValue(std::uint64_t(1)));
        const FormatValue value(std::move(members));

        EXPECT_EQ(JsonWriter::write(value, JsonWriteOptions{.indentWidth = 4}), "{\n    \"key\": 1\n}");
        EXPECT_EQ(JsonWriter::write(value, JsonWriteOptions{.indentWidth = 1}), "{\n \"key\": 1\n}");

        // 空容器不受缩进影响，与历史行为一致
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueArray{}), JsonWriteOptions{.indentWidth = 4}), "[]");
        EXPECT_EQ(JsonWriter::write(FormatValue(FormatValueObject{}), JsonWriteOptions{.indentWidth = 4}), "{}");
    }

    // ============================================================================
    // JsonWriteOptions：ensureAscii
    // ============================================================================

    TEST(JsonWriter, EnsureAsciiEscapesBasicMultilingualPlaneCharacters)
    {
        const JsonWriteOptions options{.ensureAscii = true};

        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("中文")), options), "\"\\u4e2d\\u6587\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("café")), options), "\"caf\\u00e9\"");

        // 转义后的文本必须能被解析回原文
        const std::string source = "中文日志";
        EXPECT_EQ(JsonParser::parse(JsonWriter::write(FormatValue(source), options)).asString(), source);
    }

    TEST(JsonWriter, EnsureAsciiEscapesSupplementaryPlaneAsSurrogatePair)
    {
        const JsonWriteOptions options{.ensureAscii = true};
        const std::string      rocket = "\xF0\x9F\x9A\x80"; // U+1F680

        const std::string output = JsonWriter::write(FormatValue(rocket), options);

        EXPECT_EQ(output, "\"\\ud83d\\ude80\"");
        EXPECT_EQ(JsonParser::parse(output).asString(), rocket);
    }

    TEST(JsonWriter, EnsureAsciiKeepsAsciiAndStillEscapesControlCharacters)
    {
        const JsonWriteOptions options{.ensureAscii = true};

        EXPECT_EQ(JsonWriter::write(FormatValue(std::string("plain \"text\"\n")), options), "\"plain \\\"text\\\"\\n\"");
        EXPECT_EQ(JsonWriter::write(FormatValue(std::string(1, '\x1F')), options), "\"\\u001f\"");
    }

    TEST(JsonWriter, EnsureAsciiRejectsMalformedUtf8FromHandBuiltValues)
    {
        // 手工构造的值绕过了解析器校验，转义路径必须自行发现非法序列
        const FormatValue broken(std::string("\xE4\xB8", 2));

        try
        {
            static_cast<void>(JsonWriter::write(broken, JsonWriteOptions{.ensureAscii = true}));
            FAIL() << "非法 UTF-8 序列在 ensureAscii 模式下应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::InvalidUtf8);
        }

        // 原样透传模式既不校验也不改写字节
        EXPECT_EQ(JsonWriter::write(broken, JsonWriteOptions{}), std::string("\"\xE4\xB8\"", 4));
    }

    // ============================================================================
    // JsonWriteOptions：对象键序（由值模型决定，没有开关）
    // ============================================================================

    TEST(JsonWriter, ObjectKeysAreWrittenInAscendingOrder)
    {
        FormatValueObject members;
        members.emplace("zeta", FormatValue(std::int64_t(1)));
        members.emplace("alpha", FormatValue(std::int64_t(2)));
        const FormatValue value(std::move(members));

        // 紧凑重载与显式选项必须给出同一份文本：键序由容器（std::map）决定，选项里没有键序开关
        // （JsonWriteOptions 的默认 indentWidth 为 2，与 write(value, true) 的历史行为一致）
        const std::string compact = JsonWriter::write(value, JsonWriteOptions{.indentWidth = 0});

        EXPECT_EQ(compact, R"({"alpha":2,"zeta":1})");
        EXPECT_EQ(JsonWriter::write(value), compact);
    }

    // ============================================================================
    // JsonWriteOptions：maximumDepth
    // ============================================================================

    TEST(JsonWriter, DepthGuardRejectsValuesDeeperThanTheConfiguredLimit)
    {
        // 三层数组：根数组第 0 层，最内层空数组第 2 层
        const FormatValue deeplyNested(FormatValueArray{FormatValue(FormatValueArray{FormatValue(FormatValueArray{})})});

        // 断言的是紧凑文本，故显式 indentWidth = 0（默认选项为两空格缩进）
        EXPECT_EQ(JsonWriter::write(deeplyNested, JsonWriteOptions{.indentWidth = 0, .maximumDepth = 3}), "[[[]]]");

        try
        {
            static_cast<void>(JsonWriter::write(deeplyNested, JsonWriteOptions{.maximumDepth = 2}));
            FAIL() << "超过深度上限应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::DepthExceeded);
            EXPECT_NE(std::string(error.what()).find("嵌套深度"), std::string::npos);
        }
    }

    TEST(JsonWriter, DefaultDepthGuardStopsHandBuiltDeepValues)
    {
        const FormatValue deeplyNested = makeNestedArrays(300);

        // 默认上限 256 层：手搓的超深值会被拦下，而不是把调用栈写爆
        EXPECT_THROW(static_cast<void>(JsonWriter::write(deeplyNested)), FormatError);
        EXPECT_THROW(static_cast<void>(JsonWriter::write(deeplyNested, JsonWriteOptions{.maximumDepth = 256})), FormatError);

        // 上限置 0 表示不限制，可以正常写出；显式 indentWidth = 0 以紧凑文本核对括号总数
        const std::string output = JsonWriter::write(deeplyNested, JsonWriteOptions{.indentWidth = 0, .maximumDepth = 0});
        EXPECT_EQ(output.size(), 602U); // 最内层 "[]" 加 300 层括号
        EXPECT_EQ(output.front(), '[');
        EXPECT_EQ(output.back(), ']');
    }

    TEST(JsonWriter, ParserAcceptedNestingAlwaysSurvivesTheDefaultWriterGuard)
    {
        // 解析侧与序列化侧对「深度」的定义一致，默认选项下解析成功的值必然能写回
        const std::string nested(200, '[');
        const std::string closed = nested + std::string(200, ']');

        const FormatValue value       = JsonParser::parse(closed);
        const std::string serialized  = JsonWriter::write(value);

        EXPECT_TRUE(JsonParser::parse(serialized) == value);
    }
} // namespace AsynGyanis::Base
