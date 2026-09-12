/**
 * @file TestYamlWriter.cpp
 * @brief YamlWriter 单元测试：各类型输出、引号与块标量策略、选项两态与解析回环一致性
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlWriter.h"

#include "Base/Format/FormatError.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/Yaml/YamlParser.h"

#include <gtest/gtest.h>

#include <cmath>
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
         * @brief 把值写成 YAML 再按默认选项解析回来
         * @param value 待回环的配置值
         * @return FormatValue 解析结果
         */
        FormatValue roundTrip(const FormatValue &value)
        {
            return YamlParser::parse(YamlWriter::write(value));
        }

        /**
         * @brief 把值按指定选项写成 YAML 再解析回来
         * @param value 待回环的配置值
         * @param options 序列化选项
         * @return FormatValue 解析结果
         */
        FormatValue roundTrip(const FormatValue &value, const YamlWriteOptions &options)
        {
            return YamlParser::parse(YamlWriter::write(value, options));
        }

        /**
         * @brief 手工构造指定层数的嵌套数组
         * @details 绕过解析器的深度上限，用于验证序列化侧的深度守护；最外层数组是第 1 层。
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

    // ============================================================================
    // 标量：规范写法与 round-trip
    // ============================================================================

    TEST(YamlWriter, WritesScalarsInCanonicalForm)
    {
        EXPECT_EQ(YamlWriter::write(FormatValue(nullptr)), "null\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(true)), "true\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(false)), "false\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::int64_t(42))), "42\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::int64_t(-7))), "-7\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::uint64_t(7))), "7\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("text"))), "text\n");
    }

    TEST(YamlWriter, KeepsFloatingPointShapeParseable)
    {
        // 整数值浮点必须带小数点，否则回读会变成 int64（核心 schema 只有一种数字走整数路径）
        EXPECT_EQ(YamlWriter::write(FormatValue(3.0)), "3.0\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(3.5)), "3.5\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(-0.25)), "-0.25\n");
        EXPECT_EQ(roundTrip(FormatValue(3.0)).type(), FormatValueType::Double);
        EXPECT_EQ(roundTrip(FormatValue(3.0)).asDouble(), 3.0);
    }

    TEST(YamlWriter, PreservesFullDoublePrecision)
    {
        const FormatValue value(0.1234567890123);

        // ostream 默认精度只会写到 0.123457，这里必须能无损回读（含类型）
        EXPECT_TRUE(roundTrip(value) == value) << YamlWriter::write(value);
        EXPECT_DOUBLE_EQ(roundTrip(value).asDouble(), 0.1234567890123);
        EXPECT_DOUBLE_EQ(roundTrip(FormatValue(1.0e300)).asDouble(), 1.0e300);
        EXPECT_DOUBLE_EQ(roundTrip(FormatValue(1.0e-30)).asDouble(), 1.0e-30);
    }

    TEST(YamlWriter, WritesSpecialFloatingPointForms)
    {
        const double notANumber = std::numeric_limits<double>::quiet_NaN();
        const double infinity   = std::numeric_limits<double>::infinity();

        // §10.2.1.3：核心 schema 的特殊浮点必须带前导点
        EXPECT_EQ(YamlWriter::write(FormatValue(notANumber)), ".nan\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(infinity)), ".inf\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(-infinity)), "-.inf\n");

        // NaN 与自身不相等，回环只能按位判定
        EXPECT_TRUE(std::isnan(roundTrip(FormatValue(notANumber)).asDouble()));
        EXPECT_EQ(roundTrip(FormatValue(infinity)).asDouble(), infinity);
        EXPECT_EQ(roundTrip(FormatValue(-infinity)).asDouble(), -infinity);
    }

    TEST(YamlWriter, WritesUnsignedIntegersInDecimalForm)
    {
        EXPECT_EQ(YamlWriter::write(FormatValue(std::uint64_t(0))), "0\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::uint64_t(42))), "42\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::numeric_limits<std::uint64_t>::max())), "18446744073709551615\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::numeric_limits<std::int64_t>::min())), "-9223372036854775808\n");
    }

    TEST(YamlWriter, RoundTripsIntegersOutsideTheInt64RangeAsUInt)
    {
        const FormatValue huge(std::numeric_limits<std::uint64_t>::max());

        // 超出 INT64_MAX 的整数只能落地为 UInt，因此这一类是无损回环的
        EXPECT_EQ(roundTrip(huge).type(), FormatValueType::UInt);
        EXPECT_TRUE(roundTrip(huge) == huge);

        const FormatValue minimum(std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(roundTrip(minimum).type(), FormatValueType::Int);
        EXPECT_TRUE(roundTrip(minimum) == minimum);
    }

    TEST(YamlWriter, RoundTripsEveryScalarKindInOneDocument)
    {
        FormatValueObject members;
        members.emplace("null-value", FormatValue(nullptr));
        members.emplace("bool-value", FormatValue(true));
        members.emplace("int-value", FormatValue(std::int64_t(-9)));
        members.emplace("uint-value", FormatValue(std::numeric_limits<std::uint64_t>::max()));
        members.emplace("double-value", FormatValue(1.5));
        members.emplace("string-value", FormatValue(std::string("汉字与 text")));
        members.emplace("empty-string", FormatValue(std::string()));
        const FormatValue value(std::move(members));

        EXPECT_TRUE(roundTrip(value) == value) << YamlWriter::write(value);
    }

    // ============================================================================
    // 引号策略
    // ============================================================================

    TEST(YamlWriter, QuotesAmbiguousStringsByDefault)
    {
        // 不加引号会被核心 schema 判成 bool/null/int/float，字符串类型当场丢失
        const char *const ambiguous[] = {"true", "false", "null", "Null", "123", "-7", "1.5", "0x1F",
                                         "0o17", "0b101", "~", "yes", "no", "on", "off", "y", "n", ".inf", ".nan"};

        for (const char *const sample : ambiguous)
        {
            // 花括号初始化：`value(std::string(sample))` 会被解析成函数声明（most vexing parse）
            const FormatValue value{std::string(sample)};
            const std::string output = YamlWriter::write(value);

            EXPECT_EQ(roundTrip(value).type(), FormatValueType::String) << sample << " -> " << output;
            EXPECT_TRUE(roundTrip(value) == value) << sample << " -> " << output;
        }

        // 单引号是最省的选择：无需转义时不会退化成双引号
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("true"))), "'true'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("~"))), "'~'\n");
    }

    TEST(YamlWriter, LeavesAmbiguousStringsUnquotedWhenDisabled)
    {
        const YamlWriteOptions options{.quoteAmbiguousStrings = false};

        // 关闭后不再补引号：字符串 "true" 会被回读成 bool，这是该选项声明的有损取舍
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("true")), options), "true\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("123")), options), "123\n");
        EXPECT_EQ(roundTrip(FormatValue(std::string("true")), options).type(), FormatValueType::Bool);
        EXPECT_EQ(roundTrip(FormatValue(std::string("123")), options).type(), FormatValueType::Int);
    }

    TEST(YamlWriter, KeepsEmptyStringDistinctFromNull)
    {
        // 花括号初始化：`emptyString(std::string())` 会被解析成函数声明（most vexing parse）
        const FormatValue emptyString{std::string()};

        EXPECT_EQ(YamlWriter::write(emptyString), "''\n");
        EXPECT_EQ(roundTrip(emptyString).type(), FormatValueType::String);
        EXPECT_TRUE(roundTrip(emptyString) == emptyString);

        // 空串不是「歧义」而是「裸写必错」，因此 quoteAmbiguousStrings 关闭时照样加引号
        EXPECT_EQ(YamlWriter::write(emptyString, YamlWriteOptions{.quoteAmbiguousStrings = false}), "''\n");
    }

    TEST(YamlWriter, ChoosesPlainScalarWhenItIsSafe)
    {
        const char *const plainSamples[] = {"text", "plain-token", "100%", "a#b", "127.0.0.1",
                                            "file.tar.gz", "中文值", "a.b.c", "2026-09-12"};

        for (const char *const sample : plainSamples)
        {
            const FormatValue value{std::string(sample)};
            // 安全时保持裸标量：不加任何引号，且回环仍是同一个字符串
            EXPECT_EQ(YamlWriter::write(value), std::string(sample) + "\n") << sample;
            EXPECT_TRUE(roundTrip(value) == value) << sample;
        }
    }

    TEST(YamlWriter, QuotesStringsThatCannotBeWrittenPlain)
    {
        // ": " 是键值分隔、指示符开头会被结构语法抢先识别、首尾空白会被裁掉
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a: b"))), "'a: b'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("[tag]"))), "'[tag]'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("{k}"))), "'{k}'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("-x"))), "'-x'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string(" padded"))), "' padded'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("trailing "))), "'trailing '\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("it's"))), "'it''s'\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("say \"hi\""))), "'say \"hi\"'\n");
    }

    TEST(YamlWriter, UsesDoubleQuotesForControlCharacters)
    {
        // 制表符与回车必须转义为 \t / \r；反斜杠与双引号在双引号标量里也要转义（§7.3.2）
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("tab\there"))), "\"tab\\there\"\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("carriage\rreturn"))), "\"carriage\\rreturn\"\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\tb\\c"))), "\"a\\tb\\\\c\"\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("say \"hi\"\t"))), "\"say \\\"hi\\\"\\t\"\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string(1, '\x01'))), "\"\\x01\"\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string(1, '\x1F'))), "\"\\x1f\"\n");
    }

    TEST(YamlWriter, EscapesCommentLeadingHashesInsideQuotedScalars)
    {
        // 前置空白引导的 `#` 会被扫描器当成行尾注释砍掉整段内容，引号并不能保护它，
        // 因此这类 `#` 必须写成 `\x23`
        const FormatValue value(std::string("a #c"));
        EXPECT_EQ(YamlWriter::write(value), "\"a \\x23c\"\n");
        EXPECT_TRUE(roundTrip(value) == value);

        FormatValueObject members;
        members.emplace("#key", FormatValue(std::string("value #tail")));
        const FormatValue wrapped(std::move(members));
        EXPECT_NE(YamlWriter::write(wrapped).find("\\x23"), std::string::npos);
        EXPECT_TRUE(roundTrip(wrapped) == wrapped) << YamlWriter::write(wrapped);

        // 行首的 `#` 不受影响：引号本身就在它前面，单引号更省
        const FormatValue leading(std::string("#c"));
        EXPECT_EQ(YamlWriter::write(leading), "'#c'\n");
        EXPECT_TRUE(roundTrip(leading) == leading);
    }

    TEST(YamlWriter, RoundTripsStringsWithAwkwardCharacters)
    {
        const char *const samples[] = {"a: b",     "- x",     "[a]",   "{a}",     "#c",      "a #c",
                                       "'",        "\"",      "\\",    "x\ty",    "中文",    "100%",
                                       "-",        "?",       ":",     "---",     "...",     "%YAML",
                                       "@reserved", "`tick`", "&anchor", "*alias", "!tag",   "|pipe",
                                       ">gt",      ",comma",  "key:",  " trailing", "leading ", "true",
                                       "null",     "123",     "1.5",   "0x1F",    "~",       "yes"};

        for (const char *const sample : samples)
        {
            const FormatValue root{std::string(sample)};
            EXPECT_TRUE(roundTrip(root) == root) << "根值: " << sample;

            // 同一段文本作为映射值、序列条目与键时都必须原样保留
            FormatValueObject members;
            members.emplace(std::string(sample), FormatValue(std::int64_t(1)));
            const FormatValue wrapped(std::move(members));
            EXPECT_TRUE(roundTrip(wrapped) == wrapped) << "键: " << sample;

            FormatValueArray elements;
            elements.push_back(FormatValue(std::string(sample)));
            const FormatValue array(std::move(elements));
            EXPECT_TRUE(roundTrip(array) == array) << "条目: " << sample;
        }
    }

    // ============================================================================
    // 块标量
    // ============================================================================

    TEST(YamlWriter, UsesLiteralBlockScalarForMultiLineStrings)
    {
        const FormatValue value(std::string("line1\nline2\n"));

        // 显式缩进指示符（§8.1.1.1）；恰好 1 个尾随换行落在默认的 clip 上，不补空行（§8.1.1.2）
        EXPECT_EQ(YamlWriter::write(value), "|2\n  line1\n  line2\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, IndentsBlockScalarContentUnderItsKey)
    {
        FormatValueObject members;
        members.emplace("note", FormatValue(std::string("line1\nline2\n")));
        const FormatValue value(std::move(members));

        EXPECT_EQ(YamlWriter::write(value), "note: |2\n  line1\n  line2\n");
        EXPECT_TRUE(roundTrip(value) == value);

        // 缩进宽度可配置，块标量内容跟着走，缩进指示符同步变化
        const YamlWriteOptions options{.indentWidth = 4};
        EXPECT_EQ(YamlWriter::write(value, options), "note: |4\n    line1\n    line2\n");
        EXPECT_TRUE(roundTrip(value, options) == value);
    }

    TEST(YamlWriter, ChoosesChompingByTrailingLineBreakCount)
    {
        // 0 个尾随换行 → strip：正文末行写出的换行也被丢掉
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\nb"))), "|2-\n  a\n  b\n");
        // 1 个尾随换行 → 默认 clip：正文末行的换行正好保留一个，不需要补空行
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\nb\n"))), "|2\n  a\n  b\n");
        // 3 个尾随换行 → keep：正文末行的换行算 1 个，另补 2 个空行
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\nb\n\n\n"))), "|2+\n  a\n  b\n\n\n");

        const char *const texts[] = {"a\nb", "a\nb\n", "a\nb\n\n\n"};
        for (const char *const text : texts)
        {
            const FormatValue value{std::string(text)};
            EXPECT_TRUE(roundTrip(value) == value) << YamlWriter::write(value);
        }
    }

    TEST(YamlWriter, KeepsLeadingSpacesInsideBlockScalars)
    {
        // 自动探测会以首个非空行的缩进为准吃掉前导空格，显式缩进指示符才能保留
        const FormatValue value(std::string("  indented\nnext\n"));

        EXPECT_EQ(YamlWriter::write(value), "|2\n    indented\n  next\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, FallsBackToDoubleQuotesWhenBlockScalarsWouldBeLossy)
    {
        // 以换行开头：块标量的 chomping 只能按「有内容的末行」计数，整段全为换行更是无从表达
        // （§8.1.1.2），因此统一退回双引号转义
        const FormatValue leadingBreak(std::string("\nleading"));
        EXPECT_EQ(YamlWriter::write(leadingBreak), "\"\\nleading\"\n");
        EXPECT_TRUE(roundTrip(leadingBreak) == leadingBreak);

        // 解析侧确实按 §8.1.2 保留块标量的前导空行：上面的引号是输出器保守选择，并非不得不如此
        EXPECT_TRUE(YamlParser::parse("|2-\n\n  leading\n") == leadingBreak);

        // 仅由空格构成的行会被当成空行丢掉
        const FormatValue blankLine(std::string("a\n   \nb"));
        EXPECT_EQ(roundTrip(blankLine), blankLine);

        // 行首制表符会被判为非法缩进，同样不能走块标量
        const FormatValue tabIndented(std::string("a\n\tb"));
        EXPECT_TRUE(roundTrip(tabIndented) == tabIndented);
        EXPECT_EQ(YamlWriter::write(tabIndented), "\"a\\n\\tb\"\n");
    }

    TEST(YamlWriter, UsesFoldedStyleOnlyWhenItIsLossless)
    {
        const YamlWriteOptions folded{.multiLineStyle = ScalarStylePolicy::Folded};

        // 行内换行会被折叠成空格，因此含行内换行时自动回退为字面风格
        const FormatValue prose(std::string("alpha\nbeta\n"));
        EXPECT_EQ(YamlWriter::write(prose, folded), "|2\n  alpha\n  beta\n");
        EXPECT_TRUE(roundTrip(prose, folded) == prose);

        // 只有尾部换行时 `>` 与 `|` 等价，此时才真正用折叠风格
        const FormatValue trailing(std::string("alpha beta\n"));
        EXPECT_EQ(YamlWriter::write(trailing, folded), ">2\n  alpha beta\n");
        EXPECT_TRUE(roundTrip(trailing, folded) == trailing);
    }

    TEST(YamlWriter, QuotedMultiLineStyleAvoidsBlockScalars)
    {
        const YamlWriteOptions options{.multiLineStyle = ScalarStylePolicy::Quoted};
        const FormatValue value(std::string("line1\nline2\n"));

        EXPECT_EQ(YamlWriter::write(value, options), "\"line1\\nline2\\n\"\n");
        EXPECT_TRUE(roundTrip(value, options) == value);
    }

    // ============================================================================
    // 容器、flow 与空容器
    // ============================================================================

    TEST(YamlWriter, WritesEmptyContainersInFlowForm)
    {
        EXPECT_EQ(YamlWriter::write(FormatValue(FormatValueArray{})), "[]\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(FormatValueObject{})), "{}\n");

        FormatValueObject members;
        members.emplace("items", FormatValue(FormatValueArray{}));
        members.emplace("meta", FormatValue(FormatValueObject{}));
        const FormatValue value(std::move(members));

        EXPECT_EQ(YamlWriter::write(value), "items: []\nmeta: {}\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, EmptyContainerOptionFallsBackToImplicitNull)
    {
        FormatValueObject members;
        members.emplace("items", FormatValue(FormatValueArray{}));
        const FormatValue value(std::move(members));
        const YamlWriteOptions options{.useFlowForEmptyContainers = false};

        // 块风格没有空容器的写法，关闭该选项后只能留空（值为 null，属有损取舍）
        EXPECT_EQ(YamlWriter::write(value, options), "items:\n");
        EXPECT_EQ(roundTrip(value, options).asObject().at("items").type(), FormatValueType::Null);
        EXPECT_EQ(YamlWriter::write(FormatValue(FormatValueArray{}), options), "null\n");
    }

    TEST(YamlWriter, WritesBlockContainersWithNestedIndentation)
    {
        FormatValueArray ports;
        ports.push_back(FormatValue(std::int64_t(80)));
        ports.push_back(FormatValue(std::int64_t(443)));

        FormatValueObject server;
        server.emplace("host", FormatValue(std::string("127.0.0.1")));
        server.emplace("ports", FormatValue(std::move(ports)));

        FormatValueObject root;
        root.emplace("server", FormatValue(std::move(server)));
        const FormatValue value(std::move(root));

        EXPECT_EQ(YamlWriter::write(value), "server:\n  host: 127.0.0.1\n  ports:\n    - 80\n    - 443\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, WritesBlockSequenceItemsWithNestedContainers)
    {
        FormatValueArray items;
        FormatValueObject first;
        first.emplace("name", FormatValue(std::string("tls")));
        first.emplace("enabled", FormatValue(true));
        items.push_back(FormatValue(std::move(first)));
        items.push_back(FormatValue(std::int64_t(2)));
        const FormatValue value(items);

        // 键沿用容器自身的升序（enabled 在 name 之前）
        EXPECT_EQ(YamlWriter::write(value), "-\n  enabled: true\n  name: tls\n- 2\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, FlowThresholdSwitchesSmallAllScalarContainersToFlow)
    {
        FormatValueArray threeScalars;
        threeScalars.push_back(FormatValue(std::int64_t(1)));
        threeScalars.push_back(FormatValue(std::int64_t(2)));
        threeScalars.push_back(FormatValue(std::int64_t(3)));

        FormatValueArray fourScalars = threeScalars;
        fourScalars.push_back(FormatValue(std::int64_t(4)));

        FormatValueObject members;
        members.emplace("a", FormatValue(std::int64_t(1)));
        members.emplace("b", FormatValue(std::int64_t(2)));

        const FormatValue arrayValue(threeScalars);
        const FormatValue longArrayValue(fourScalars);
        const FormatValue objectValue(members);

        // 0 表示一律用块风格：根数组自身就是一层，元素直接落在 `- ` 行上
        EXPECT_EQ(YamlWriter::write(arrayValue), "- 1\n- 2\n- 3\n");
        EXPECT_EQ(YamlWriter::write(objectValue), "a: 1\nb: 2\n");

        const YamlWriteOptions options{.flowThreshold = 3};
        EXPECT_EQ(YamlWriter::write(arrayValue, options), "[1, 2, 3]\n");
        EXPECT_EQ(YamlWriter::write(objectValue, options), "{a: 1, b: 2}\n");
        EXPECT_EQ(YamlWriter::write(longArrayValue, options), "- 1\n- 2\n- 3\n- 4\n");

        EXPECT_TRUE(roundTrip(arrayValue, options) == arrayValue);
        EXPECT_TRUE(roundTrip(objectValue, options) == objectValue);
        EXPECT_TRUE(roundTrip(longArrayValue, options) == longArrayValue);
    }

    TEST(YamlWriter, FlowContainersStillProtectTheirScalars)
    {
        const YamlWriteOptions options{.flowThreshold = 4};

        FormatValueArray elements;
        elements.push_back(FormatValue(std::string("a,b")));
        elements.push_back(FormatValue(std::string("plain")));
        elements.push_back(FormatValue(std::string("true")));
        elements.push_back(FormatValue(std::string("a: b")));
        const FormatValue value(elements);

        // flow 内 `,` `[` `]` `{` `}` `:` 都是分隔符；裸 `true` 也会变成 bool
        EXPECT_EQ(YamlWriter::write(value, options), "['a,b', plain, 'true', 'a: b']\n");
        EXPECT_TRUE(roundTrip(value, options) == value);
    }

    TEST(YamlWriter, FlowContainersWithNestedValuesStayInBlockStyle)
    {
        FormatValueArray inner;
        inner.push_back(FormatValue(std::int64_t(1)));
        FormatValueArray outer;
        outer.push_back(FormatValue(inner));
        const FormatValue value(outer);

        // 外层含容器元素，即使元素数不超阈值也退回块风格；内层 [1] 全为标量且未超阈值，照常走 flow
        EXPECT_EQ(YamlWriter::write(value, YamlWriteOptions{.flowThreshold = 8}), "- [1]\n");
    }

    // ============================================================================
    // 选项：文档起始标记、键序、深度
    // ============================================================================

    TEST(YamlWriter, EmitDocumentStartPrefixesTheMarker)
    {
        const FormatValue value(std::int64_t(1));

        EXPECT_EQ(YamlWriter::write(value), "1\n");
        EXPECT_EQ(YamlWriter::write(value, YamlWriteOptions{.emitDocumentStart = true}), "---\n1\n");
        EXPECT_EQ(roundTrip(value, YamlWriteOptions{.emitDocumentStart = true}), value);

        FormatValueObject members;
        members.emplace("key", FormatValue(std::string("value")));
        const FormatValue wrapped(std::move(members));
        EXPECT_EQ(YamlWriter::write(wrapped, YamlWriteOptions{.emitDocumentStart = true}), "---\nkey: value\n");
        EXPECT_TRUE(roundTrip(wrapped, YamlWriteOptions{.emitDocumentStart = true}) == wrapped);

        // 文档起始标记与根块标量并存：`---` 独占一行，块标量内容仍缩进一层（§9.1.3、§8.1.1.1）
        const YamlWriteOptions documentStart{.emitDocumentStart = true};
        const FormatValue multiLine(std::string("line1\nline2\n"));
        EXPECT_EQ(YamlWriter::write(multiLine, documentStart), "---\n|2\n  line1\n  line2\n");
        EXPECT_TRUE(roundTrip(multiLine, documentStart) == multiLine);
    }

    TEST(YamlWriter, KeepsContainerOrderAndQuotesAwkwardKeys)
    {
        FormatValueObject members;
        members.emplace("zeta", FormatValue(std::int64_t(1)));
        members.emplace("alpha", FormatValue(std::int64_t(2)));
        members.emplace("true", FormatValue(std::int64_t(3)));
        members.emplace("a: b", FormatValue(std::int64_t(4)));
        members.emplace("<<", FormatValue(std::int64_t(5)));
        members.emplace("", FormatValue(std::int64_t(6)));
        const FormatValue value(std::move(members));

        // 键序沿用 FormatValueObject 自身的升序，不重排；需要引号的键一律引号
        EXPECT_EQ(YamlWriter::write(value), "'': 6\n'<<': 5\n'a: b': 4\nalpha: 2\n'true': 3\nzeta: 1\n");
        EXPECT_TRUE(roundTrip(value) == value);
    }

    TEST(YamlWriter, DepthGuardRejectsValuesDeeperThanTheConfiguredLimit)
    {
        const FormatValue nested(FormatValueArray{FormatValue(FormatValueArray{FormatValue(FormatValueArray{})})});

        EXPECT_EQ(YamlWriter::write(nested, YamlWriteOptions{.maximumDepth = 3}), "-\n  - []\n");

        try
        {
            static_cast<void>(YamlWriter::write(nested, YamlWriteOptions{.maximumDepth = 2}));
            FAIL() << "超过深度上限应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::DepthExceeded);
            EXPECT_NE(std::string(error.what()).find("嵌套深度"), std::string::npos);
        }
    }

    TEST(YamlWriter, DefaultDepthGuardStopsHandBuiltDeepValues)
    {
        const FormatValue deeplyNested = makeNestedArrays(300);

        // 默认上限 128 层：手搓的超深值会被拦下，而不是把调用栈写爆
        EXPECT_THROW(static_cast<void>(YamlWriter::write(deeplyNested)), FormatError);
        EXPECT_THROW(static_cast<void>(YamlWriter::write(deeplyNested, YamlWriteOptions{.maximumDepth = 128})), FormatError);

        // 上限置 0 表示不限制
        const std::string output = YamlWriter::write(deeplyNested, YamlWriteOptions{.maximumDepth = 0});
        EXPECT_EQ(output.front(), '-');
        EXPECT_EQ(output.compare(output.size() - 3, 3, "[]\n"), 0);
    }

    TEST(YamlWriter, ParserAcceptedNestingAlwaysSurvivesTheDefaultWriterGuard)
    {
        // 解析侧与序列化侧对「深度」的定义一致：默认选项下解析成功的值必然写得回去
        const std::string source = std::string(128, '[') + std::string(128, ']');
        const FormatValue value  = YamlParser::parse(source);

        const std::string serialized = YamlWriter::write(value);
        EXPECT_TRUE(YamlParser::parse(serialized) == value);
    }

    // ============================================================================
    // 锚点复用
    // ============================================================================

    TEST(YamlWriter, ReuseAnchorsEmitsAliasesForRepeatedSubtrees)
    {
        FormatValueObject shared;
        shared.emplace("x", FormatValue(std::int64_t(1)));
        shared.emplace("y", FormatValue(std::int64_t(2)));
        const FormatValue sharedValue(std::move(shared));

        FormatValueArray elements;
        elements.push_back(sharedValue);
        elements.push_back(sharedValue);
        const FormatValue value(elements);

        // 默认关闭：重复子树原样写两遍
        const std::string plain = YamlWriter::write(value);
        EXPECT_EQ(plain.find('*'), std::string::npos);

        const std::string anchored = YamlWriter::write(value, YamlWriteOptions{.reuseAnchors = true});
        // 等值子树按值归组：首份写 `&a1` 定义，后续副本写 `*a1`；
        // 序列指示符后必须保留分隔空白，键 `y` 因与 1.1 的 bool 写法同形而自带引号
        EXPECT_EQ(anchored, "- &a1\n  x: 1\n  'y': 2\n- *a1\n");

        // 解析侧按深拷贝展开别名，因此 round-trip 仍是相等的值
        EXPECT_TRUE(YamlParser::parse(anchored) == value);

        // 别名展开后两份子树等值，再写一遍仍应得到同一份文本（幂等）
        EXPECT_EQ(YamlWriter::write(YamlParser::parse(anchored), YamlWriteOptions{.reuseAnchors = true}), anchored);
    }

    TEST(YamlWriter, ReuseAnchorsKeepsDistinctSubTreesSeparate)
    {
        FormatValueObject first;
        first.emplace("x", FormatValue(std::int64_t(1)));
        FormatValueObject second;
        second.emplace("x", FormatValue(std::int64_t(2)));

        FormatValueArray elements;
        elements.push_back(FormatValue(std::move(first)));
        elements.push_back(FormatValue(std::move(second)));
        const FormatValue value(elements);

        const std::string output = YamlWriter::write(value, YamlWriteOptions{.reuseAnchors = true});

        // 结构不同就不该复用锚点
        EXPECT_EQ(output.find('*'), std::string::npos);
        EXPECT_TRUE(roundTrip(value) == value);
    }

    // ============================================================================
    // 折行与幂等
    // ============================================================================

    TEST(YamlWriter, FoldsLongPlainScalarsAtLineWidth)
    {
        const std::string sentence = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu";

        // lineWidth 为 0 表示不折行
        EXPECT_EQ(YamlWriter::write(FormatValue(sentence), YamlWriteOptions{.lineWidth = 0}), sentence + "\n");

        const std::string folded = YamlWriter::write(FormatValue(sentence), YamlWriteOptions{.lineWidth = 40});
        EXPECT_NE(folded.find('\n'), std::string::npos);
        EXPECT_EQ(roundTrip(FormatValue(sentence), YamlWriteOptions{.lineWidth = 40}).asString(), sentence);

        // 折行只落在单个空格上：折回的文本必须逐字相同
        FormatValueObject members;
        members.emplace("sentence", FormatValue(sentence));
        const FormatValue wrapped(std::move(members));
        EXPECT_TRUE(roundTrip(wrapped, YamlWriteOptions{.lineWidth = 40}) == wrapped);
    }

    TEST(YamlWriter, NeverFoldsTokensWithoutSafeBreakPoints)
    {
        const std::string token(120, 'x');

        // 没有空格就没有安全折点：整段照写，绝不硬切
        EXPECT_EQ(YamlWriter::write(FormatValue(token), YamlWriteOptions{.lineWidth = 40}), token + "\n");
    }

    TEST(YamlWriter, RoundTripsRichDocumentsAndStaysIdempotent)
    {
        const std::string source =
            "server:\n"
            "  host: 127.0.0.1\n"
            "  ports: [80, 443]\n"
            "  options:\n"
            "    - name: tls\n"
            "      enabled: true\n"
            "    - name: gzip\n"
            "      enabled: false\n"
            "  empty: {}\n"
            "  nothing: null\n"
            "  ratio: 0.5\n"
            "  note: \"第一行\\n第二行\\n\"\n"
            "  aliases: ['true', 'a: b', plain]\n"
            "top: [a, b, c]\n";

        const FormatValue value          = YamlParser::parse(source);
        const std::string firstPass      = YamlWriter::write(value);
        const FormatValue reparsed       = YamlParser::parse(firstPass);
        const std::string secondPass     = YamlWriter::write(reparsed);

        EXPECT_TRUE(reparsed == value) << firstPass;
        // 幂等：write(parse(write(v))) 与 write(v) 逐字节相同
        EXPECT_EQ(secondPass, firstPass) << firstPass;
    }

    TEST(YamlWriter, RoundTripsIndentedOutputBackToItself)
    {
        FormatValueObject members;
        members.emplace("ratio", FormatValue(1.5));
        members.emplace("count", FormatValue(std::int64_t(3)));
        members.emplace("note", FormatValue(std::string("多行\n文本\n")));
        const FormatValue value(std::move(members));

        const YamlWriteOptions options{.indentWidth = 4};
        const std::string firstPass   = YamlWriter::write(value, options);
        const std::string secondPass  = YamlWriter::write(YamlParser::parse(firstPass), options);

        EXPECT_EQ(firstPass, secondPass) << firstPass;
    }

    // ============================================================================
    // 块标量：空行、缩进与折行边界（与解析侧新行为自洽）
    // ============================================================================

    TEST(YamlWriter, KeepsBlankLinesInsideBlockScalars)
    {
        // 解析侧按 §8.1.2 把前导/中间空行算作内容（l-empty 位于内容产生式内部），
        // 输出器把空行写成零缩进空行，chomping 只作用于尾部（§8.1.1.2）
        const char *const samples[] = {"a\n\nb", "a\n\nb\n", "a\n\nb\n\n", "a\n\n\nb\n", "a\n#b\n", "a\n\n   \nb"};

        for (const char *const sample : samples)
        {
            const FormatValue value{std::string(sample)};
            const std::string firstPass = YamlWriter::write(value);

            EXPECT_TRUE(YamlParser::parse(firstPass) == value) << sample << " -> " << firstPass;
            EXPECT_EQ(YamlWriter::write(YamlParser::parse(firstPass)), firstPass) << sample;
        }

        // 中间空行原样保留，尾部空行按 chomping 计数，`#` 行是内容而不是注释
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\n\nb\n"))), "|2\n  a\n\n  b\n");
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("a\n#b\n"))), "|2\n  a\n  #b\n");

        // 行尾空白属于块内容，不能被裁掉
        const FormatValue trailingSpaces{std::string("a  \nb\n")};
        EXPECT_EQ(YamlWriter::write(trailingSpaces), "|2\n  a  \n  b\n");
        EXPECT_TRUE(YamlParser::parse(YamlWriter::write(trailingSpaces)) == trailingSpaces);
    }

    TEST(YamlWriter, RoundTripsBlockScalarsInSequenceEntries)
    {
        FormatValueArray elements;
        elements.push_back(FormatValue(std::string("first\nsecond\n")));
        elements.push_back(FormatValue(std::int64_t(1)));
        const FormatValue value(std::move(elements));

        // 序列条目的块标量：`-` 行即所在行，内容缩进以序列自身缩进为基准（§8.1.1.1）
        EXPECT_EQ(YamlWriter::write(value), "- |2\n  first\n  second\n- 1\n");

        // 缩进宽度 0 按 1 处理；超过 9 只能在头部写 9，内容缩进同步钳到 9（§8.1.1.1 只接受 1~9）
        EXPECT_EQ(YamlWriter::write(value, YamlWriteOptions{.indentWidth = 0}), "- |1\n first\n second\n- 1\n");
        EXPECT_EQ(YamlWriter::write(value, YamlWriteOptions{.indentWidth = 12}), "- |9\n         first\n         second\n- 1\n");

        // 缩进宽度（含 0→1 与超过 9 被指示符钳到 9）下的 round-trip 与幂等
        const std::size_t indentWidths[] = {0, 1, 2, 4, 9, 12};
        for (const std::size_t indentWidth : indentWidths)
        {
            const YamlWriteOptions options{.indentWidth = indentWidth};
            const std::string firstPass = YamlWriter::write(value, options);

            EXPECT_TRUE(YamlParser::parse(firstPass) == value) << "indentWidth=" << indentWidth << "\n" << firstPass;
            EXPECT_EQ(YamlWriter::write(YamlParser::parse(firstPass), options), firstPass) << "indentWidth=" << indentWidth;
        }
    }

    TEST(YamlWriter, FoldedStyleKeepsTrailingBreaksLosslessly)
    {
        const YamlWriteOptions folded{.multiLineStyle = ScalarStylePolicy::Folded};

        // 只有尾部换行时 `>` 才无损（§8.1.3）：1 个走 clip，多个走 keep 并补 N-1 个空行
        const char *const samples[] = {"alpha beta\n", "alpha beta\n\n", "alpha beta\n\n\n", "alpha\n\nbeta\n"};
        for (const char *const sample : samples)
        {
            const FormatValue value{std::string(sample)};
            const std::string firstPass = YamlWriter::write(value, folded);

            EXPECT_TRUE(YamlParser::parse(firstPass) == value) << sample << " -> " << firstPass;
            EXPECT_EQ(YamlWriter::write(YamlParser::parse(firstPass), folded), firstPass) << sample;
        }

        // 含行内换行时必定回退为字面风格，绝不为让 `>` 生效而牺牲 round-trip
        EXPECT_EQ(YamlWriter::write(FormatValue(std::string("alpha\n\nbeta\n")), folded), "|2\n  alpha\n\n  beta\n");
    }

    TEST(YamlWriter, QuotesTextWhoseLinesLookLikeDocumentMarkers)
    {
        // `---` / `...` / `%` 开头的行在扫描器眼里分别是文档边界与指令（§9.1.3、§6.2），
        // 块标量一旦照写就会被当场截断，只能退回引号标量
        const char *const samples[] = {"a\n---\n", "a\n...\n", "a\n%b\n", "a\n ---\n", "---\nb"};

        for (const char *const sample : samples)
        {
            const FormatValue value{std::string(sample)};
            const std::string firstPass = YamlWriter::write(value);

            EXPECT_TRUE(YamlParser::parse(firstPass) == value) << sample << " -> " << firstPass;
            EXPECT_EQ(YamlWriter::write(YamlParser::parse(firstPass)), firstPass) << sample;
        }
    }

    TEST(YamlWriter, FoldsPlainScalarsInsideNestedPositions)
    {
        const std::string sentence = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu";

        FormatValueArray elements;
        elements.push_back(FormatValue(sentence));
        FormatValueObject members;
        members.emplace("list", FormatValue(std::move(elements)));
        const FormatValue value(std::move(members));

        // 折行续行的缩进以所在行缩进加一层缩进为基准：窄缩进下也必须能折回原文
        const std::size_t indentWidths[] = {0, 1, 2, 4};
        for (const std::size_t indentWidth : indentWidths)
        {
            const YamlWriteOptions options{.indentWidth = indentWidth, .lineWidth = 40};
            const std::string firstPass = YamlWriter::write(value, options);

            EXPECT_TRUE(YamlParser::parse(firstPass) == value) << "indentWidth=" << indentWidth << "\n" << firstPass;
            EXPECT_EQ(YamlWriter::write(YamlParser::parse(firstPass), options), firstPass) << "indentWidth=" << indentWidth;
        }
    }
} // namespace AsynGyanis::Base
