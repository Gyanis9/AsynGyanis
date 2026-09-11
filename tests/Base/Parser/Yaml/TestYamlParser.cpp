/**
 * @file TestYamlParser.cpp
 * @brief YamlParser 单元测试：块结构、流式风格、类型推断、注释与错误定位
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Yaml/YamlParser.h"

#include "Base/Parser/Value/ParserValue.h"
#include "Base/Parser/Value/ParserValueType.h"
#include "Base/Parser/ParserError.h"
#include "Base/Parser/ParserPosition.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 执行一段应当失败的解析并取回解析错误
         * @param action 触发解析的动作
         * @return ParserError 捕获到的错误对象副本
         */
        ParserError catchParserError(const std::function<ParserValue()> &action)
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

        /**
         * @brief 取根对象中指定键的值
         * @param value 根值，必须是对象
         * @param key 键名
         * @return const ParserValue& 对应值
         */
        const ParserValue &member(const ParserValue &value, const std::string &key)
        {
            const ParserValueObject &members = value.asObject();
            const auto iterator         = members.find(key);
            EXPECT_NE(iterator, members.end()) << "缺少键: " << key;
            static const ParserValue missingValue(nullptr);
            return iterator == members.end() ? missingValue : iterator->second;
        }
    } // namespace

    // ============================================================================
    // 标量与类型推断
    // ============================================================================

    TEST(YamlParser, EmptyInputProducesEmptyObject)
    {
        EXPECT_TRUE(YamlParser::parse("").asObject().empty());
        EXPECT_TRUE(YamlParser::parse("   \n\n\t\n").asObject().empty());
        EXPECT_TRUE(YamlParser::parse("# 只有注释\n").asObject().empty());
    }

    TEST(YamlParser, InfersScalarTypes)
    {
        const ParserValue value = YamlParser::parse(
                "text: hello\n"
                "count: 8080\n"
                "ratio: 1.5\n"
                "exponent: 1e3\n"
                "negative: -0.25\n"
                "flag: true\n"
                "off_switch: off\n"
                "yes_switch: YES\n"
                "empty:\n"
                "tilde: ~\n"
                "explicit_null: null\n");

        EXPECT_EQ(member(value, "text").asString(), "hello");
        EXPECT_EQ(member(value, "count").type(), ParserValueType::Int);
        EXPECT_EQ(member(value, "count").asInt(), 8080);
        EXPECT_EQ(member(value, "ratio").type(), ParserValueType::Double);
        EXPECT_DOUBLE_EQ(member(value, "ratio").asDouble(), 1.5);
        EXPECT_DOUBLE_EQ(member(value, "exponent").asDouble(), 1000.0);
        EXPECT_DOUBLE_EQ(member(value, "negative").asDouble(), -0.25);
        EXPECT_EQ(member(value, "flag").asBool(), true);
        EXPECT_EQ(member(value, "off_switch").asBool(), false);
        EXPECT_EQ(member(value, "yes_switch").asBool(), true);
        EXPECT_TRUE(member(value, "empty").isNull());
        EXPECT_TRUE(member(value, "tilde").isNull());
        EXPECT_TRUE(member(value, "explicit_null").isNull());
    }

    TEST(YamlParser, QuotedScalarsStayStrings)
    {
        // 带引号的数字样字符串必须保序为字符串，这是覆盖层写回的关键语义
        const ParserValue value = YamlParser::parse("port: \"8080\"\nratio: '1.5'\nflag: \"true\"\n");

        EXPECT_EQ(member(value, "port").type(), ParserValueType::String);
        EXPECT_EQ(member(value, "port").asString(), "8080");
        EXPECT_EQ(member(value, "ratio").asString(), "1.5");
        EXPECT_EQ(member(value, "flag").asString(), "true");
    }

    TEST(YamlParser, DecodesDoubleQuotedEscapes)
    {
        const std::string source = "path: \"C:\\\\temp\"\nquote: \"say \\\"hi\\\"\"\nnewline: \"a\\nb\"\nrocket: \"\\uD83D\\uDE80\"\n";

        const ParserValue value = YamlParser::parse(source);

        EXPECT_EQ(member(value, "path").asString(), "C:\\temp");
        EXPECT_EQ(member(value, "quote").asString(), "say \"hi\"");
        EXPECT_EQ(member(value, "newline").asString(), "a\nb");
        EXPECT_EQ(member(value, "rocket").asString(), "\xF0\x9F\x9A\x80");
    }

    TEST(YamlParser, TreatsDoubledSingleQuoteAsOneQuote)
    {
        const ParserValue value = YamlParser::parse("name: 'it''s ok'\n");

        EXPECT_EQ(member(value, "name").asString(), "it's ok");
    }

    // ============================================================================
    // 块结构
    // ============================================================================

    TEST(YamlParser, ParsesNestedMappings)
    {
        const ParserValue value = YamlParser::parse(
                "server:\n"
                "  host: 0.0.0.0\n"
                "  tls:\n"
                "    enabled: true\n"
                "    certificate: server.crt\n"
                "logging:\n"
                "  level: info\n");

        const ParserValue &server = member(value, "server");
        EXPECT_EQ(member(server, "host").asString(), "0.0.0.0");

        const ParserValue &tls = member(server, "tls");
        EXPECT_EQ(member(tls, "enabled").asBool(), true);
        EXPECT_EQ(member(tls, "certificate").asString(), "server.crt");
        EXPECT_EQ(member(member(value, "logging"), "level").asString(), "info");
    }

    TEST(YamlParser, ParsesBlockSequences)
    {
        const ParserValue value = YamlParser::parse(
                "ports:\n"
                "  - 80\n"
                "  - 443\n"
                "  - 8080\n"
                "names:\n"
                "  - alpha\n");

        const ParserValueArray &ports = member(value, "ports").asArray();
        ASSERT_EQ(ports.size(), 3U);
        EXPECT_EQ(ports[0].asInt(), 80);
        EXPECT_EQ(ports[2].asInt(), 8080);
        EXPECT_EQ(member(value, "names").asArray()[0].asString(), "alpha");
    }

    TEST(YamlParser, ParsesSequenceOfMappings)
    {
        const ParserValue value = YamlParser::parse(
                "sinks:\n"
                "  - type: console\n"
                "    color: true\n"
                "  - type: file\n"
                "    path: logs/app.log\n");

        const ParserValueArray &sinks = member(value, "sinks").asArray();
        ASSERT_EQ(sinks.size(), 2U);

        EXPECT_EQ(member(sinks[0], "type").asString(), "console");
        EXPECT_EQ(member(sinks[0], "color").asBool(), true);
        EXPECT_EQ(member(sinks[1], "type").asString(), "file");
        EXPECT_EQ(member(sinks[1], "path").asString(), "logs/app.log");
    }

    TEST(YamlParser, ParsesSequenceOfSequences)
    {
        const ParserValue value = YamlParser::parse("matrix:\n  - - 1\n    - 2\n  - - 3\n");

        const ParserValueArray &matrix = member(value, "matrix").asArray();
        ASSERT_EQ(matrix.size(), 2U);
        EXPECT_EQ(matrix[0].asArray().size(), 2U);
        EXPECT_EQ(matrix[1].asArray().size(), 1U);
    }

    TEST(YamlParser, KeepsDottedKeysVerbatim)
    {
        const ParserValue value = YamlParser::parse("app.name: dashboard\n");

        EXPECT_EQ(member(value, "app.name").asString(), "dashboard");
        EXPECT_EQ(value.asObject().size(), 1U);
    }

    TEST(YamlParser, AcceptsQuotedKeys)
    {
        const ParserValue value = YamlParser::parse("\"with space\": 1\n\"8080\": two\n");

        EXPECT_EQ(member(value, "with space").asInt(), 1);
        EXPECT_EQ(member(value, "8080").asString(), "two");
    }

    TEST(YamlParser, ParsesTopLevelSequence)
    {
        const ParserValue value = YamlParser::parse("- one\n- 2\n");

        ASSERT_EQ(value.type(), ParserValueType::Array);
        EXPECT_EQ(value.asArray()[0].asString(), "one");
        EXPECT_EQ(value.asArray()[1].asInt(), 2);
    }

    // ============================================================================
    // 注释与流式风格
    // ============================================================================

    TEST(YamlParser, IgnoresWholeLineAndTrailingComments)
    {
        const ParserValue value = YamlParser::parse(
                "# 顶部注释\n"
                "port: 8080 # 行尾注释\n"
                "\n"
                "  # 缩进的注释行\n"
                "host: localhost\n");

        EXPECT_EQ(member(value, "port").asInt(), 8080);
        EXPECT_EQ(member(value, "host").asString(), "localhost");
        EXPECT_EQ(value.asObject().size(), 2U);
    }

    TEST(YamlParser, KeepsHashInsideQuotedScalar)
    {
        const ParserValue value = YamlParser::parse("color: \"#1A2B3C\"\nplain: a#b\n");

        EXPECT_EQ(member(value, "color").asString(), "#1A2B3C");
        EXPECT_EQ(member(value, "plain").asString(), "a#b");
    }

    TEST(YamlParser, ParsesFlowStyleCollections)
    {
        const ParserValue value = YamlParser::parse(
                "ports: [80, 443, 8080]\n"
                "labels: {region: cn, tier: \"1\"}\n"
                "empty_list: []\n"
                "empty_map: {}\n"
                "nested: [{a: 1}, [2, 3]]\n");

        EXPECT_EQ(member(value, "ports").asArray().size(), 3U);
        EXPECT_EQ(member(value, "ports").asArray()[1].asInt(), 443);
        EXPECT_EQ(member(member(value, "labels"), "region").asString(), "cn");
        EXPECT_EQ(member(member(value, "labels"), "tier").asString(), "1");
        EXPECT_TRUE(member(value, "empty_list").asArray().empty());
        EXPECT_TRUE(member(value, "empty_map").asObject().empty());

        const ParserValueArray &nested = member(value, "nested").asArray();
        ASSERT_EQ(nested.size(), 2U);
        EXPECT_EQ(member(nested[0], "a").asInt(), 1);
        EXPECT_EQ(nested[1].asArray()[0].asInt(), 2);
    }

    TEST(YamlParser, TrimsWhitespaceAroundValues)
    {
        const ParserValue value = YamlParser::parse("key:    spaced value   \n");

        EXPECT_EQ(member(value, "key").asString(), "spaced value");
    }

    // ============================================================================
    // 错误路径与定位
    // ============================================================================

    TEST(YamlParser, RejectsTabIndentation)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("root:\n\tchild: 1\n");
        });

        EXPECT_NE(std::string(error.what()).find("制表符"), std::string::npos);
        EXPECT_EQ(error.position().lineNumber, 2U);
    }

    TEST(YamlParser, RejectsDuplicateKeysOnTheSameLevel)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("a: 1\nb: 2\na: 3\n");
        });

        EXPECT_NE(std::string(error.what()).find("重复的键：a"), std::string::npos);
        EXPECT_EQ(error.position().lineNumber, 3U);
    }

    TEST(YamlParser, AllowsSameKeyInDifferentBlocks)
    {
        const ParserValue value = YamlParser::parse("first:\n  name: a\nsecond:\n  name: b\n");

        EXPECT_EQ(member(member(value, "first"), "name").asString(), "a");
        EXPECT_EQ(member(member(value, "second"), "name").asString(), "b");
    }

    TEST(YamlParser, RejectsLinesWithoutKeyValueSeparator)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("just a bare line\n");
        });

        EXPECT_NE(std::string(error.what()).find("应为 'key: value'"), std::string::npos);
    }

    TEST(YamlParser, RejectsInconsistentIndentation)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("a: 1\n  b: 2\n");
        });

        EXPECT_NE(std::string(error.what()).find("缩进不一致"), std::string::npos);
    }

    TEST(YamlParser, RejectsSequenceMixedIntoMappingLevel)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("a: 1\n- 2\n");
        });

        EXPECT_NE(std::string(error.what()).find("序列条目"), std::string::npos);
    }

    TEST(YamlParser, RejectsMultipleDocumentMarkers)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("---\na: 1\n");
        });

        EXPECT_NE(std::string(error.what()).find("多文档"), std::string::npos);
    }

    TEST(YamlParser, RejectsAnchorsAndAliases)
    {
        const ParserError anchorError = catchParserError([]
        {
            return YamlParser::parse("base: &anchor\n");
        });
        const ParserError aliasError = catchParserError([]
        {
            return YamlParser::parse("copy: *anchor\n");
        });

        EXPECT_NE(std::string(anchorError.what()).find("锚点与别名"), std::string::npos);
        EXPECT_NE(std::string(aliasError.what()).find("锚点与别名"), std::string::npos);
    }

    TEST(YamlParser, RejectsBlockScalars)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("script: |\n  echo hi\n");
        });

        EXPECT_NE(std::string(error.what()).find("块标量"), std::string::npos);
    }

    TEST(YamlParser, RejectsExplicitTypeTags)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("count: !!int 5\n");
        });

        EXPECT_NE(std::string(error.what()).find("显式类型标签"), std::string::npos);
    }

    TEST(YamlParser, RejectsDirectiveLines)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("%YAML 1.2\na: 1\n");
        });

        EXPECT_NE(std::string(error.what()).find("指令行"), std::string::npos);
    }

    TEST(YamlParser, RejectsUnterminatedQuotes)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("text: \"unclosed\n");
        });

        EXPECT_NE(std::string(error.what()).find("未闭合"), std::string::npos);
    }

    TEST(YamlParser, RejectsUnterminatedFlowCollections)
    {
        const ParserError sequenceError = catchParserError([]
        {
            return YamlParser::parse("ports: [80, 443\n");
        });
        const ParserError mappingError = catchParserError([]
        {
            return YamlParser::parse("labels: {a: 1\n");
        });

        EXPECT_NE(std::string(sequenceError.what()).find("流式序列未闭合"), std::string::npos);
        EXPECT_NE(std::string(mappingError.what()).find("流式映射未闭合"), std::string::npos);
    }

    TEST(YamlParser, RejectsFlowMappingEntryWithoutColon)
    {
        const ParserError error = catchParserError([]
        {
            return YamlParser::parse("labels: {region}\n");
        });

        EXPECT_NE(std::string(error.what()).find("key: value"), std::string::npos);
    }

    TEST(YamlParser, EnforcesNestingDepthLimit)
    {
        std::string source;
        for (int level = 0; level < 40; ++level)
        {
            source += std::string(static_cast<std::size_t>(level), ' ') + "nest:\n";
        }
        source += std::string(40, ' ') + "leaf: 1\n";

        const ParserError error = catchParserError([&source]
        {
            return YamlParser::parse(source);
        });

        EXPECT_NE(std::string(error.what()).find("嵌套深度"), std::string::npos);
    }
} // namespace AsynGyanis::Base
