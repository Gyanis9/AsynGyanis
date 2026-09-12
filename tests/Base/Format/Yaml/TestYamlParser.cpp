/**
 * @file TestYamlParser.cpp
 * @brief YamlParser 单元测试：块结构、流式风格、多文档、指令与错误定位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlParser.h"

#include "Base/Format/Yaml/YamlParseOptions.h"
#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/TextPosition.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 执行一段应当失败的解析并取回解析错误
         * @param action 触发解析的动作
         * @return FormatError 捕获到的错误对象副本
         */
        template<typename Action>
        FormatError catchFormatError(const Action &action)
        {
            try
            {
                static_cast<void>(action());
            } catch (const FormatError &error)
            {
                return error;
            }
            catch (...)
            {
                ADD_FAILURE() << "预期抛出 FormatError，实际抛出了其他异常";
                return FormatError("wrong exception type", TextPosition{});
            }

            ADD_FAILURE() << "预期抛出 FormatError，但解析成功";
            return FormatError("no exception thrown", TextPosition{});
        }

        /**
         * @brief 取根对象中指定键的值
         * @param value 根值，必须是对象
         * @param key 键名
         * @return const FormatValue& 对应值
         */
        const FormatValue &member(const FormatValue &value, const std::string &key)
        {
            const FormatValueObject &members = value.asObject();
            const auto               iterator = members.find(key);
            EXPECT_NE(iterator, members.end()) << "缺少键: " << key;
            static const FormatValue missingValue(nullptr);
            return iterator == members.end() ? missingValue : iterator->second;
        }
    } // namespace

    // ============================================================================
    // 空文档与多文档
    // ============================================================================

    TEST(YamlParser, EmptyOrCommentOnlyInputIsNull)
    {
        // 规范：空文档是 null，而不是空映射（YAML 1.2 §9.1.1）
        EXPECT_TRUE(YamlParser::parse("").isNull());
        EXPECT_TRUE(YamlParser::parse("   \n\n\t\n").isNull());
        EXPECT_TRUE(YamlParser::parse("# 只有注释\n").isNull());
        EXPECT_TRUE(YamlParser::parse("---\n").isNull());
    }

    TEST(YamlParser, ParsesMultipleDocumentsAndReturnsFirstByDefault)
    {
        const std::string source =
                "%YAML 1.2\n"
                "---\n"
                "a: 1\n"
                "...\n"
                "---\n"
                "b: 2\n";

        const FormatValue first = YamlParser::parse(source);
        EXPECT_EQ(member(first, "a").asInt(), 1);
        EXPECT_FALSE(first.asObject().contains("b"));

        const std::vector<FormatValue> documents = YamlParser::parseAll(source);
        ASSERT_EQ(documents.size(), 2U);
        EXPECT_EQ(member(documents[0], "a").asInt(), 1);
        EXPECT_EQ(member(documents[1], "b").asInt(), 2);
    }

    TEST(YamlParser, ParseAllReturnsNoDocumentForEmptyStream)
    {
        EXPECT_TRUE(YamlParser::parseAll("").empty());
        EXPECT_TRUE(YamlParser::parseAll("# 只有注释\n").empty());
    }

    // ============================================================================
    // 块标量内的列 0 标记：只有列 0 才具备结构含义（§9.1.3、§6.8）
    // ============================================================================

    TEST(YamlParser, KeepsColumnZeroDocumentStartMarkerAfterBlockScalar)
    {
        // 块标量结束于列 0 的 `---`（其缩进必然小于内容缩进），该行仍是文档起始标记
        const std::vector<FormatValue> documents = YamlParser::parseAll(
                "script: |\n"
                "  echo hi\n"
                "---\n"
                "name: second\n");

        ASSERT_EQ(documents.size(), 2U);
        EXPECT_EQ(member(documents[0], "script").asString(), "echo hi\n");
        EXPECT_EQ(member(documents[1], "name").asString(), "second");
    }

    TEST(YamlParser, KeepsColumnZeroDirectiveAfterBlockScalar)
    {
        // 列 0 的 `%YAML` 仍走指令路径：非法版本报 InvalidKeyword，而不是退化成标量内容
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parseAll("a: |\n  text\n%YAML 2.0\n---\nb: 1\n");
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
        EXPECT_NE(std::string(error.what()).find("不支持的 YAML 版本"), std::string::npos);
    }

    TEST(YamlParser, EndsBlockScalarAtUnderIndentedMarkerLikeLineHandledOutside)
    {
        // 形如指令但缩进不足（低于内容缩进）的行不属于块内容（§8.1.1.1 的内容缩进）：
        // 块在此结束，列 0 的该行由外层按指令处理，随后 `---` 开启第二份文档
        const std::vector<FormatValue> documents = YamlParser::parseAll(
                "a: |\n"
                "  text\n"
                "%YAML 1.2\n"
                "---\n"
                "b: 1\n");

        ASSERT_EQ(documents.size(), 2U);
        EXPECT_EQ(member(documents[0], "a").asString(), "text\n");
        EXPECT_EQ(member(documents[1], "b").asInt(), 1);
    }

    TEST(YamlParser, RejectsUnderIndentedMarkerLikeLineInsideBlockScalar)
    {
        // `  %YAML 1.2` 只到第 2 列，低于内容缩进 4 列：它既不是内容也不是指令（§9.1.3），
        // 块结束后由外层映射按「同级键必须对齐」拒绝（§8.2.2）
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("script: |\n    echo hi\n  %YAML 1.2\n");
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("缩进不一致"), std::string::npos);
    }

    TEST(YamlParser, RejectsMissingDocumentSeparator)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("first\nsecond\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::TrailingContent);
        EXPECT_NE(std::string(error.what()).find("分隔符"), std::string::npos);
    }

    // ============================================================================
    // 块结构
    // ============================================================================

    TEST(YamlParser, ParsesNestedMappings)
    {
        const FormatValue value = YamlParser::parse(
                "server:\n"
                "  host: 0.0.0.0\n"
                "  tls:\n"
                "    enabled: true\n"
                "    certificate: server.crt\n"
                "logging:\n"
                "  level: info\n");

        const FormatValue &server = member(value, "server");
        EXPECT_EQ(member(server, "host").asString(), "0.0.0.0");

        const FormatValue &tls = member(server, "tls");
        EXPECT_EQ(member(tls, "enabled").asBool(), true);
        EXPECT_EQ(member(tls, "certificate").asString(), "server.crt");
        EXPECT_EQ(member(member(value, "logging"), "level").asString(), "info");
    }

    TEST(YamlParser, ParsesBlockSequences)
    {
        const FormatValue value = YamlParser::parse(
                "ports:\n"
                "  - 80\n"
                "  - 443\n"
                "  - 8080\n"
                "names:\n"
                "  - alpha\n");

        const FormatValueArray &ports = member(value, "ports").asArray();
        ASSERT_EQ(ports.size(), 3U);
        EXPECT_EQ(ports[0].asInt(), 80);
        EXPECT_EQ(ports[2].asInt(), 8080);
        EXPECT_EQ(member(value, "names").asArray()[0].asString(), "alpha");
    }

    TEST(YamlParser, SupportsZeroIndentSequenceUnderKey)
    {
        // 最常见的块序列写法：`ports:` 换行后 `- 80` 与键同缩进（§8.2.1）
        const FormatValue value = YamlParser::parse(
                "ports:\n"
                "- 80\n"
                "- 443\n"
                "name: web\n");

        const FormatValueArray &ports = member(value, "ports").asArray();
        ASSERT_EQ(ports.size(), 2U);
        EXPECT_EQ(ports[0].asInt(), 80);
        EXPECT_EQ(ports[1].asInt(), 443);
        EXPECT_EQ(member(value, "name").asString(), "web");
    }

    TEST(YamlParser, ParsesSequenceOfMappings)
    {
        const FormatValue value = YamlParser::parse(
                "sinks:\n"
                "  - type: console\n"
                "    color: true\n"
                "  - type: file\n"
                "    path: logs/app.log\n");

        const FormatValueArray &sinks = member(value, "sinks").asArray();
        ASSERT_EQ(sinks.size(), 2U);

        EXPECT_EQ(member(sinks[0], "type").asString(), "console");
        EXPECT_EQ(member(sinks[0], "color").asBool(), true);
        EXPECT_EQ(member(sinks[1], "type").asString(), "file");
        EXPECT_EQ(member(sinks[1], "path").asString(), "logs/app.log");
    }

    TEST(YamlParser, ParsesSequenceOfSequences)
    {
        const FormatValue value = YamlParser::parse("matrix:\n  - - 1\n    - 2\n  - - 3\n");

        const FormatValueArray &matrix = member(value, "matrix").asArray();
        ASSERT_EQ(matrix.size(), 2U);
        ASSERT_EQ(matrix[0].asArray().size(), 2U);
        EXPECT_EQ(matrix[0].asArray()[0].asInt(), 1);
        EXPECT_EQ(matrix[1].asArray().size(), 1U);
    }

    TEST(YamlParser, ParsesTopLevelSequence)
    {
        const FormatValue value = YamlParser::parse("- one\n- 2\n");

        ASSERT_EQ(value.type(), FormatValueType::Array);
        EXPECT_EQ(value.asArray()[0].asString(), "one");
        EXPECT_EQ(value.asArray()[1].asInt(), 2);
    }

    TEST(YamlParser, KeepsDottedKeysVerbatim)
    {
        const FormatValue value = YamlParser::parse("app.name: dashboard\n");

        EXPECT_EQ(member(value, "app.name").asString(), "dashboard");
        EXPECT_EQ(value.asObject().size(), 1U);
    }

    TEST(YamlParser, AcceptsQuotedKeys)
    {
        const FormatValue value = YamlParser::parse("\"with space\": 1\n\"8080\": two\n");

        EXPECT_EQ(member(value, "with space").asInt(), 1);
        EXPECT_EQ(member(value, "8080").asString(), "two");
    }

    TEST(YamlParser, ParsesExplicitComplexKey)
    {
        const FormatValue value = YamlParser::parse(
                "? key\n"
                ": value\n"
                "? \"quoted key\"\n"
                ": 2\n");

        EXPECT_EQ(member(value, "key").asString(), "value");
        EXPECT_EQ(member(value, "quoted key").asInt(), 2);
    }

    TEST(YamlParser, ExplicitKeyWithoutValueIsNull)
    {
        const FormatValue value = YamlParser::parse("? key\n");
        EXPECT_TRUE(member(value, "key").isNull());
    }

    TEST(YamlParser, ParsesBareScalarDocument)
    {
        // 裸标量文档是合法 YAML：整体是一个字符串
        EXPECT_EQ(YamlParser::parse("just a bare line\n").asString(), "just a bare line");
        // 冒号后无空白不构成键值分隔
        EXPECT_EQ(YamlParser::parse("a:b\n").asString(), "a:b");
    }

    // ============================================================================
    // 注释与流式风格
    // ============================================================================

    TEST(YamlParser, IgnoresWholeLineAndTrailingComments)
    {
        const FormatValue value = YamlParser::parse(
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
        const FormatValue value = YamlParser::parse("color: \"#1A2B3C\"\nplain: a#b\n");

        EXPECT_EQ(member(value, "color").asString(), "#1A2B3C");
        EXPECT_EQ(member(value, "plain").asString(), "a#b");
    }

    TEST(YamlParser, ParsesFlowStyleCollections)
    {
        const FormatValue value = YamlParser::parse(
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

        const FormatValueArray &nested = member(value, "nested").asArray();
        ASSERT_EQ(nested.size(), 2U);
        EXPECT_EQ(member(nested[0], "a").asInt(), 1);
        EXPECT_EQ(nested[1].asArray()[0].asInt(), 2);
    }

    TEST(YamlParser, ParsesFlowCollectionsSpanningLines)
    {
        const FormatValue value = YamlParser::parse(
                "ports: [80,\n"
                "  443]\n"
                "labels: {a: 1,\n"
                "  b: 2}\n");

        ASSERT_EQ(member(value, "ports").asArray().size(), 2U);
        EXPECT_EQ(member(value, "ports").asArray()[1].asInt(), 443);

        const FormatValue &labels = member(value, "labels");
        EXPECT_EQ(member(labels, "a").asInt(), 1);
        EXPECT_EQ(member(labels, "b").asInt(), 2);
    }

    TEST(YamlParser, IgnoresCommentsInsideMultilineFlow)
    {
        const FormatValue value = YamlParser::parse(
                "ports: [80, # 行内注释\n"
                "  443]\n");

        ASSERT_EQ(member(value, "ports").asArray().size(), 2U);
        EXPECT_EQ(member(value, "ports").asArray()[0].asInt(), 80);
        EXPECT_EQ(member(value, "ports").asArray()[1].asInt(), 443);
    }

    TEST(YamlParser, TrimsWhitespaceAroundValues)
    {
        const FormatValue value = YamlParser::parse("key:    spaced value   \n");

        EXPECT_EQ(member(value, "key").asString(), "spaced value");
    }

    // ============================================================================
    // 解析选项
    // ============================================================================

    TEST(YamlParser, AllowsDuplicateKeysWhenConfigured)
    {
        YamlParseOptions options;
        options.allowDuplicateKeys = true;

        const FormatValue value = YamlParser::parse("a: 1\na: 2\n", options);
        EXPECT_EQ(member(value, "a").asInt(), 2);
    }

    TEST(YamlParser, EnforcesConfiguredLimits)
    {
        YamlParseOptions options;
        options.maximumInputLength = 4;
        EXPECT_EQ(catchFormatError([&options]
                    {
                        return YamlParser::parse("a: 1\n", options);
                    })
                          .kind(),
                  FormatErrorKind::SizeExceeded);

        YamlParseOptions scalarOptions;
        scalarOptions.maximumScalarLength = 4;
        EXPECT_EQ(catchFormatError([&scalarOptions]
                    {
                        return YamlParser::parse("a: abcdefgh\n", scalarOptions);
                    })
                          .kind(),
                  FormatErrorKind::SizeExceeded);

        YamlParseOptions documentOptions;
        documentOptions.maximumDocumentCount = 2;
        EXPECT_EQ(catchFormatError([&documentOptions]
                    {
                        return YamlParser::parseAll("---\n1\n---\n2\n---\n3\n", documentOptions);
                    })
                          .kind(),
                  FormatErrorKind::SizeExceeded);
    }

    TEST(YamlParser, EnforcesConfiguredNestingDepth)
    {
        YamlParseOptions options;
        options.maximumDepth = 4;

        std::string source;
        for (int level = 0; level < 8; ++level)
        {
            source += std::string(static_cast<std::size_t>(level) * 2, ' ') + "nest:\n";
        }
        source += std::string(16, ' ') + "leaf: 1\n";

        const FormatError error = catchFormatError([&source, &options]
        {
            return YamlParser::parse(source, options);
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::DepthExceeded);
    }

    TEST(YamlParser, DefaultNestingDepthIsGenerous)
    {
        // 默认上限 128 远高于普通配置的嵌套深度
        std::string source;
        for (int level = 0; level < 20; ++level)
        {
            source += std::string(static_cast<std::size_t>(level) * 2, ' ') + "nest:\n";
        }
        source += std::string(40, ' ') + "leaf: 1\n";

        EXPECT_NO_THROW(static_cast<void>(YamlParser::parse(source)));
    }

    // ============================================================================
    // 错误路径与定位
    // ============================================================================

    TEST(YamlParser, RejectsTabIndentation)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("root:\n\tchild: 1\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("制表符"), std::string::npos);
        EXPECT_EQ(error.position().lineNumber, 2U);
        EXPECT_EQ(error.position().columnNumber, 1U);
    }

    TEST(YamlParser, RejectsDuplicateKeysOnTheSameLevel)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("a: 1\nb: 2\na: 3\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::DuplicateKey);
        EXPECT_NE(std::string(error.what()).find("重复的键：a"), std::string::npos);
        EXPECT_EQ(error.position().lineNumber, 3U);
    }

    TEST(YamlParser, AllowsSameKeyInDifferentBlocks)
    {
        const FormatValue value = YamlParser::parse("first:\n  name: a\nsecond:\n  name: b\n");

        EXPECT_EQ(member(member(value, "first"), "name").asString(), "a");
        EXPECT_EQ(member(member(value, "second"), "name").asString(), "b");
    }

    TEST(YamlParser, RejectsInconsistentIndentation)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("a: 1\n  b: 2\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("缩进不一致"), std::string::npos);
    }

    TEST(YamlParser, RejectsSequenceMixedIntoMappingLevel)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("a: 1\n- 2\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("序列条目"), std::string::npos);
    }

    TEST(YamlParser, RejectsUnterminatedFlowCollections)
    {
        const FormatError sequenceError = catchFormatError([]
        {
            return YamlParser::parse("ports: [80, 443\n");
        });
        const FormatError mappingError = catchFormatError([]
        {
            return YamlParser::parse("labels: {a: 1\n");
        });

        EXPECT_EQ(sequenceError.kind(), FormatErrorKind::UnterminatedContainer);
        EXPECT_NE(std::string(sequenceError.what()).find("流式序列未闭合"), std::string::npos);
        EXPECT_EQ(mappingError.kind(), FormatErrorKind::UnterminatedContainer);
        EXPECT_NE(std::string(mappingError.what()).find("流式映射未闭合"), std::string::npos);
    }

    TEST(YamlParser, RejectsFlowMappingEntryWithoutColon)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("labels: {region}\n");
        });

        EXPECT_NE(std::string(error.what()).find("key: value"), std::string::npos);
    }
} // namespace AsynGyanis::Base
