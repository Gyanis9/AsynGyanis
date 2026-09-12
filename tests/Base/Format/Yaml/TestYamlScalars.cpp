/**
 * @file TestYamlScalars.cpp
 * @brief YamlParser 标量与类型解析测试：1.2 核心 schema、引号转义、块标量、标签
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlParser.h"

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/TextPosition.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

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
         * @brief 解析单键映射并取回该键的值
         * @param source YAML 文本
         * @param key 键名
         * @return const FormatValue& 对应值
         */
        const FormatValue &single(const std::string &source, const std::string &key)
        {
            static FormatValue holder(nullptr);
            holder = YamlParser::parse(source);
            const auto iterator = holder.asObject().find(key);
            EXPECT_NE(iterator, holder.asObject().end()) << "缺少键: " << key;
            static const FormatValue missingValue(nullptr);
            return iterator == holder.asObject().end() ? missingValue : iterator->second;
        }
    } // namespace

    // ============================================================================
    // 1.2 核心 schema 类型解析
    // ============================================================================

    TEST(YamlScalars, ResolvesNullForms)
    {
        const FormatValue value = YamlParser::parse("a: ~\nb: null\nc: Null\nd: NULL\ne:\n");

        EXPECT_TRUE(single("a: ~\n", "a").isNull());
        EXPECT_TRUE(single("b: null\n", "b").isNull());
        EXPECT_TRUE(single("c: Null\n", "c").isNull());
        EXPECT_TRUE(single("d: NULL\n", "d").isNull());
        EXPECT_TRUE(single("e:\n", "e").isNull());
        EXPECT_EQ(value.asObject().size(), 5U);
    }

    TEST(YamlScalars, ResolvesBooleanForms)
    {
        EXPECT_TRUE(single("a: true\n", "a").asBool());
        EXPECT_TRUE(single("a: True\n", "a").asBool());
        EXPECT_TRUE(single("a: TRUE\n", "a").asBool());
        EXPECT_FALSE(single("a: false\n", "a").asBool());
        EXPECT_FALSE(single("a: False\n", "a").asBool());
        EXPECT_FALSE(single("a: FALSE\n", "a").asBool());
    }

    TEST(YamlScalars, TreatsOneOneBooleansAsStrings)
    {
        // 1.2 核心 schema 只认 true/false；yes/no/on/off/y/n 一律是字符串
        EXPECT_EQ(single("a: yes\n", "a").type(), FormatValueType::String);
        EXPECT_EQ(single("a: Yes\n", "a").asString(), "Yes");
        EXPECT_EQ(single("a: no\n", "a").asString(), "no");
        EXPECT_EQ(single("a: on\n", "a").asString(), "on");
        EXPECT_EQ(single("a: off\n", "a").asString(), "off");
        EXPECT_EQ(single("a: y\n", "a").asString(), "y");
        EXPECT_EQ(single("a: n\n", "a").asString(), "n");
    }

    TEST(YamlScalars, ResolvesDecimalIntegers)
    {
        EXPECT_EQ(single("a: 0\n", "a").type(), FormatValueType::Int);
        EXPECT_EQ(single("a: +12\n", "a").asInt(), 12);
        EXPECT_EQ(single("a: -34\n", "a").asInt(), -34);
        EXPECT_EQ(single("a: 9223372036854775807\n", "a").asInt(), std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(single("a: -9223372036854775808\n", "a").asInt(), std::numeric_limits<std::int64_t>::min());
    }

    TEST(YamlScalars, HandlesIntegerOverflowByWideningThenFloating)
    {
        // 超出 int64 落到 UInt，再超出 uint64 才降级为 double
        EXPECT_EQ(single("a: 9223372036854775808\n", "a").type(), FormatValueType::UInt);
        EXPECT_EQ(single("a: 9223372036854775808\n", "a").asUInt(), 9223372036854775808ULL);
        EXPECT_EQ(single("a: 18446744073709551615\n", "a").type(), FormatValueType::UInt);
        EXPECT_EQ(single("a: 18446744073709551616\n", "a").type(), FormatValueType::Double);
    }

    TEST(YamlScalars, ResolvesHexOctalAndBinaryPrefixes)
    {
        EXPECT_EQ(single("a: 0x1F\n", "a").asInt(), 31);
        EXPECT_EQ(single("a: 0o755\n", "a").asInt(), 493);
        // 0b 是 1.1 扩展，本解析器按扩展支持
        EXPECT_EQ(single("a: 0b1010\n", "a").asInt(), 10);
        // 0xFFFFFFFFFFFFFFFF 超出 int64 落到 UInt
        EXPECT_EQ(single("a: 0xFFFFFFFFFFFFFFFF\n", "a").type(), FormatValueType::UInt);
    }

    TEST(YamlScalars, TreatsLeadingZeroAsDecimal)
    {
        // 1.2 与 1.1 的关键差异：前导 0 不再是八进制
        EXPECT_EQ(single("a: 0755\n", "a").asInt(), 755);
        EXPECT_EQ(single("a: 08\n", "a").asInt(), 8);
    }

    TEST(YamlScalars, ResolvesFloatingPointForms)
    {
        EXPECT_DOUBLE_EQ(single("a: 1.5\n", "a").asDouble(), 1.5);
        EXPECT_DOUBLE_EQ(single("a: .5\n", "a").asDouble(), 0.5);
        EXPECT_DOUBLE_EQ(single("a: -0.25\n", "a").asDouble(), -0.25);
        EXPECT_DOUBLE_EQ(single("a: 1.\n", "a").asDouble(), 1.0);
        EXPECT_DOUBLE_EQ(single("a: 1e3\n", "a").asDouble(), 1000.0);
        EXPECT_DOUBLE_EQ(single("a: 1.5e-2\n", "a").asDouble(), 0.015);

        const FormatValue infinite = single("a: .inf\n", "a");
        EXPECT_EQ(infinite.type(), FormatValueType::Double);
        EXPECT_TRUE(std::isinf(infinite.asDouble()));
        EXPECT_TRUE(infinite.asDouble() > 0);

        EXPECT_TRUE(std::isinf(single("a: -.inf\n", "a").asDouble()));
        EXPECT_TRUE(std::isnan(single("a: .nan\n", "a").asDouble()));
    }

    TEST(YamlScalars, KeepsSexagesimalLikeTextAsString)
    {
        // 1.2 取消了 1.1 的 60 进制数字，`1:30` 是字符串
        EXPECT_EQ(single("a: 1:30\n", "a").type(), FormatValueType::String);
        EXPECT_EQ(single("a: 1:30\n", "a").asString(), "1:30");
        EXPECT_EQ(single("a: 1.2.3\n", "a").asString(), "1.2.3");
    }

    TEST(YamlScalars, QuotedScalarsAreAlwaysStrings)
    {
        EXPECT_EQ(single("a: \"8080\"\n", "a").type(), FormatValueType::String);
        EXPECT_EQ(single("a: '1.5'\n", "a").asString(), "1.5");
        EXPECT_EQ(single("a: \"true\"\n", "a").asString(), "true");
        EXPECT_EQ(single("a: \"\"\n", "a").type(), FormatValueType::String);
        EXPECT_TRUE(single("a: \"\"\n", "a").asString().empty());
    }

    TEST(YamlScalars, TreatsYesInsideScalarTextAsPlainString)
    {
        // `wow!!really` 曾经被旧实现对 `!!` 的粗筛误伤，现在必须原样是字符串
        EXPECT_EQ(single("a: wow!!really\n", "a").asString(), "wow!!really");
        EXPECT_EQ(single("a: a#b\n", "a").asString(), "a#b");
    }

    // ============================================================================
    // 引号标量
    // ============================================================================

    TEST(YamlScalars, DecodesAllDoubleQuotedEscapes)
    {
        const FormatValue value = YamlParser::parse(
                "control: \"\\0\\a\\b\\t\\n\\v\\f\\r\\e\"\n"
                "quotes: \"\\\"\\/\\\\\"\n"
                "unicode: \"\\N\\_\\L\\P\"\n"
                "hex: \"\\x41\\u0042\\U00000043\"\n");

        std::string expectedControl;
        expectedControl += '\0';
        expectedControl += '\x07';
        expectedControl += '\b';
        expectedControl += '\t';
        expectedControl += '\n';
        expectedControl += '\v';
        expectedControl += '\f';
        expectedControl += '\r';
        expectedControl += '\x1B';
        EXPECT_EQ(single("control: \"\\0\\a\\b\\t\\n\\v\\f\\r\\e\"\n", "control").asString(), expectedControl);
        EXPECT_EQ(single("quotes: \"\\\"\\/\\\\\"\n", "quotes").asString(), "\"/\\");

        // \N→U+0085、\_→U+00A0、\L→U+2028、\P→U+2029
        const std::string expectedUnicode = "\xC2\x85\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9";
        EXPECT_EQ(single("unicode: \"\\N\\_\\L\\P\"\n", "unicode").asString(), expectedUnicode);

        EXPECT_EQ(single("hex: \"\\x41\\u0042\\U00000043\"\n", "hex").asString(), "ABC");
        static_cast<void>(value);
    }

    TEST(YamlScalars, DecodesSurrogatePairInDoubleQuotes)
    {
        EXPECT_EQ(single("a: \"\\uD83D\\uDE80\"\n", "a").asString(), "\xF0\x9F\x9A\x80");
    }

    TEST(YamlScalars, RejectsLoneSurrogateAndUnknownEscape)
    {
        const FormatError loneHigh = catchFormatError([]
        {
            return YamlParser::parse("a: \"\\uD83D\"\n");
        });
        EXPECT_EQ(loneHigh.kind(), FormatErrorKind::SurrogatePairError);

        const FormatError unknown = catchFormatError([]
        {
            return YamlParser::parse("a: \"\\q\"\n");
        });
        EXPECT_EQ(unknown.kind(), FormatErrorKind::InvalidEscape);
    }

    TEST(YamlScalars, DecodesDoubledSingleQuoteAsOneQuote)
    {
        EXPECT_EQ(single("a: 'it''s ok'\n", "a").asString(), "it's ok");
    }

    TEST(YamlScalars, FoldsLineBreaksInsideQuotedScalars)
    {
        // 引号标量内单换行折叠为空格，空行保留为换行
        EXPECT_EQ(single("a: \"one\n  two\"\n", "a").asString(), "one two");
        EXPECT_EQ(single("a: 'one\n  two'\n", "a").asString(), "one two");
        EXPECT_EQ(single("a: \"one\n\n  two\"\n", "a").asString(), "one\ntwo");
    }

    TEST(YamlScalars, EscapedLineBreakProducesNoSeparator)
    {
        // 反斜杠换行是转义换行：不产生空格，续行缩进被跳过
        EXPECT_EQ(single("a: \"one\\\n  two\"\n", "a").asString(), "onetwo");
    }

    TEST(YamlScalars, RejectsUnterminatedQuotedScalar)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("a: \"unclosed\n");
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedString);
    }

    // ============================================================================
    // 裸标量跨行折叠
    // ============================================================================

    TEST(YamlScalars, FoldsMultiLinePlainScalar)
    {
        const FormatValue value = YamlParser::parse(
                "a: this is a long\n"
                "  description continued\n"
                "b: second\n  line\n\n  after blank\n");

        EXPECT_EQ(single("a: this is a long\n  description continued\n", "a").asString(),
                  "this is a long description continued");
        EXPECT_EQ(single("b: second\n  line\n\n  after blank\n", "b").asString(),
                  "second line\nafter blank");
        EXPECT_EQ(value.asObject().size(), 2U);

        // 续行只看缩进（§7.3.3）：缩进到父块之内的 `---` / `...` 行是标量正文，
        // 只有列 0 的标记才结束标量（§9.1.3）
        EXPECT_EQ(single("a: first\n  ---\n  second\n", "a").asString(), "first --- second");
    }

    // ============================================================================
    // 块标量
    // ============================================================================

    TEST(YamlScalars, ParsesLiteralBlockScalarWithClip)
    {
        EXPECT_EQ(single("a: |\n  echo hi\n", "a").asString(), "echo hi\n");
        EXPECT_EQ(single("a: |\n  one\n  two\n", "a").asString(), "one\ntwo\n");
        EXPECT_EQ(single("a: |\n  one\n\n  two\n", "a").asString(), "one\n\ntwo\n");
    }

    TEST(YamlScalars, AppliesLiteralChompingIndicators)
    {
        EXPECT_EQ(single("a: |-\n  echo hi\n", "a").asString(), "echo hi");
        EXPECT_EQ(single("a: |-\n  one\n  two\n", "a").asString(), "one\ntwo");
        EXPECT_EQ(single("a: |+\n  echo hi\n\n", "a").asString(), "echo hi\n\n");
    }

    TEST(YamlScalars, ParsesFoldedBlockScalar)
    {
        EXPECT_EQ(single("a: >\n  one\n  two\n", "a").asString(), "one two\n");
        EXPECT_EQ(single("a: >\n  one\n\n  two\n", "a").asString(), "one\ntwo\n");
        EXPECT_EQ(single("a: >-\n  one\n  two\n", "a").asString(), "one two");
    }

    TEST(YamlScalars, KeepsMoreIndentedFoldedLinesUnfolded)
    {
        const FormatValue value = YamlParser::parse("a: >\n  one\n    deeper\n  two\n");
        EXPECT_EQ(single("a: >\n  one\n    deeper\n  two\n", "a").asString(), "one\n  deeper\ntwo\n");
        static_cast<void>(value);
    }

    TEST(YamlScalars, HonorsExplicitIndentationIndicator)
    {
        // 显式缩进 2 表示内容相对块缩进去掉 2 个空格，多余空格保留
        EXPECT_EQ(single("a: |2\n    kept\n", "a").asString(), "  kept\n");
        EXPECT_EQ(single("a: |-2\n    kept\n", "a").asString(), "  kept");
    }

    TEST(YamlScalars, RejectsBlockScalarWithDuplicateIndicators)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("a: |2-2\n  x\n");
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
    }

    TEST(YamlScalars, ParsesBlockScalarInsideSequenceItem)
    {
        const FormatValue value = YamlParser::parse("script:\n  - |\n    line one\n    line two\n");
        const FormatValueArray &scripts = value.asObject().at("script").asArray();
        ASSERT_EQ(scripts.size(), 1U);
        EXPECT_EQ(scripts[0].asString(), "line one\nline two\n");
    }

    TEST(YamlScalars, KeepsColumnZeroMarkersInsideLiteralBlockScalarAsContent)
    {
        // §8.1.2 的 l-nb-literal-text：块标量内部没有注释、指令与文档标记语法，
        // 缩进到内容列的 `#` / `---` / `...` / `%YAML` 统统是内容；
        // 只有列 0 的 `---` / `...` 才是文档标记（§9.1.3）、列 0 的 `%` 才是指令（§6.8）
        const std::string source =
                "script: |\n"
                "  #!/bin/sh\n"
                "  ---\n"
                "  ...\n"
                "  %YAML 1.2\n"
                "  echo hi\n";

        EXPECT_EQ(single(source, "script").asString(), "#!/bin/sh\n---\n...\n%YAML 1.2\necho hi\n");

        // 根位置的块标量同理：内容列的 `---` 不得被当成文档标记而分出新文档
        EXPECT_EQ(YamlParser::parse("|\n  ---\n  echo hi\n").asString(), "---\necho hi\n");
    }

    TEST(YamlScalars, KeepsColumnZeroMarkersInsideFoldedBlockScalarAsContent)
    {
        // 折叠风格下这些行同缩进、不属于 more-indented，行间单个换行折叠为空格（§8.1.3）
        const std::string source =
                "note: >\n"
                "  ---\n"
                "  ...\n"
                "  %YAML 1.2\n";

        EXPECT_EQ(single(source, "note").asString(), "--- ... %YAML 1.2\n");
    }

    TEST(YamlScalars, KeepsBarePercentLineInsideBlockScalarAsContent)
    {
        // 内容行 `%` 独立成行且无参数：它是内容而不是指令，不得报「指令缺少参数」
        EXPECT_EQ(single("a: |\n  %\n  x\n", "a").asString(), "%\nx\n");
    }

    // ============================================================================
    // 显式标签
    // ============================================================================

    TEST(YamlScalars, AppliesCoreSchemaTags)
    {
        EXPECT_EQ(single("a: !!int 5\n", "a").asInt(), 5);
        EXPECT_EQ(single("a: !!str 5\n", "a").asString(), "5");
        EXPECT_TRUE(single("a: !!bool true\n", "a").asBool());
        EXPECT_TRUE(single("a: !!null whatever\n", "a").isNull());
        EXPECT_DOUBLE_EQ(single("a: !!float 1\n", "a").asDouble(), 1.0);
        EXPECT_DOUBLE_EQ(single("a: !!float 1.5\n", "a").asDouble(), 1.5);
        EXPECT_EQ(single("a: !!int 0x10\n", "a").asInt(), 16);
    }

    TEST(YamlScalars, ResolvesBinaryTagAsDecodedBytes)
    {
        EXPECT_EQ(single("a: !!binary aGVsbG8=\n", "a").asString(), "hello");
    }

    TEST(YamlScalars, KeepsLocalAndUnknownTagsAsStrings)
    {
        EXPECT_EQ(single("a: !foo bar\n", "a").asString(), "bar");
        EXPECT_EQ(single("a: !<tag:example.com,2000:thing> value\n", "a").asString(), "value");
    }

    TEST(YamlScalars, RejectsTagTextMismatch)
    {
        const FormatError integerError = catchFormatError([]
        {
            return YamlParser::parse("a: !!int not-a-number\n");
        });
        EXPECT_EQ(integerError.kind(), FormatErrorKind::InvalidNumber);

        const FormatError booleanError = catchFormatError([]
        {
            return YamlParser::parse("a: !!bool yes\n");
        });
        EXPECT_EQ(booleanError.kind(), FormatErrorKind::InvalidKeyword);
    }

    TEST(YamlScalars, NonSpecificTagForcesString)
    {
        // `!` 是非特定标签，对裸标量按规范解析为 !!str
        EXPECT_EQ(single("a: ! 123\n", "a").type(), FormatValueType::String);
    }
} // namespace AsynGyanis::Base
