/**
 * @file TestJsonParser.cpp
 * @brief JsonParser 单元测试：各类值、转义、数字格式与错误定位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonParser.h"

#include "Base/Format/Value/FormatValue.h"
#include "Base/Format/Value/FormatValueType.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/TextPosition.h"

#include <gtest/gtest.h>

#include <cstddef>
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
        FormatError catchFormatError(const std::function<FormatValue()> &action)
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
        EXPECT_EQ(JsonParser::parse("100").type(), FormatValueType::Int);
        EXPECT_EQ(JsonParser::parse("100.0").type(), FormatValueType::Double);
        EXPECT_EQ(JsonParser::parse("1e2").type(), FormatValueType::Double);
    }

    TEST(JsonParser, UsesUInt64ForPositiveIntegersBeyondInt64Range)
    {
        // int64 上界仍是 Int，数值逐位精确
        const FormatValue maximumSigned = JsonParser::parse("9223372036854775807");
        EXPECT_EQ(maximumSigned.type(), FormatValueType::Int);
        EXPECT_EQ(maximumSigned.asInt(), std::numeric_limits<std::int64_t>::max());

        // 越过 int64 上界后进 uint64（UInt），不再退化为浮点
        const FormatValue aboveInt64 = JsonParser::parse("9223372036854775808");
        EXPECT_EQ(aboveInt64.type(), FormatValueType::UInt);
        EXPECT_EQ(aboveInt64.asUInt(), static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U);

        const FormatValue maximumUnsigned = JsonParser::parse("18446744073709551615");
        EXPECT_EQ(maximumUnsigned.type(), FormatValueType::UInt);
        EXPECT_EQ(maximumUnsigned.asUInt(), std::numeric_limits<std::uint64_t>::max());
    }

    TEST(JsonParser, PromotesIntegersBeyondUInt64RangeToDouble)
    {
        // 超出 uint64 范围后退化为 double：量级保持，不凭空造数
        const FormatValue aboveUnsigned64 = JsonParser::parse("18446744073709551616");
        EXPECT_EQ(aboveUnsigned64.type(), FormatValueType::Double);
        EXPECT_DOUBLE_EQ(aboveUnsigned64.asDouble(), static_cast<double>(std::numeric_limits<std::uint64_t>::max()));

        const FormatValue farBeyond = JsonParser::parse("99999999999999999999999");
        EXPECT_EQ(farBeyond.type(), FormatValueType::Double);
        EXPECT_DOUBLE_EQ(farBeyond.asDouble(), 1e23);
    }

    TEST(JsonParser, PromotesNegativeIntegersBeyondInt64RangeToDouble)
    {
        const FormatValue minimumSigned = JsonParser::parse("-9223372036854775808");
        EXPECT_EQ(minimumSigned.type(), FormatValueType::Int);
        EXPECT_EQ(minimumSigned.asInt(), std::numeric_limits<std::int64_t>::min());

        // 负数没有无符号档位可退，只能落到 double
        const FormatValue beyondMinimum = JsonParser::parse("-9223372036854775809");
        EXPECT_EQ(beyondMinimum.type(), FormatValueType::Double);
        EXPECT_DOUBLE_EQ(beyondMinimum.asDouble(), static_cast<double>(std::numeric_limits<std::int64_t>::min()));
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
        const FormatValue value = JsonParser::parse("[1, \"two\", false, null]");

        const FormatValueArray &elements = value.asArray();
        ASSERT_EQ(elements.size(), 4U);
        EXPECT_EQ(elements[0].asInt(), 1);
        EXPECT_EQ(elements[1].asString(), "two");
        EXPECT_EQ(elements[2].asBool(), false);
        EXPECT_TRUE(elements[3].isNull());
    }

    TEST(JsonParser, ParsesNestedObjectsAndArrays)
    {
        const FormatValue value = JsonParser::parse(R"({"server":{"ports":[80,443],"tls":true}})");

        const FormatValue &server = value.asObject().at("server");
        EXPECT_EQ(server.asObject().at("tls").asBool(), true);

        const FormatValueArray &ports = server.asObject().at("ports").asArray();
        ASSERT_EQ(ports.size(), 2U);
        EXPECT_EQ(ports[0].asInt(), 80);
        EXPECT_EQ(ports[1].asInt(), 443);
    }

    TEST(JsonParser, AcceptsWhitespaceBetweenEveryToken)
    {
        const FormatValue value = JsonParser::parse("{\n  \"a\" : [ 1 ,\t2 ]\n}");

        EXPECT_EQ(value.asObject().at("a").asArray().size(), 2U);
    }

    // ============================================================================
    // 字符串与转义
    // ============================================================================

    TEST(JsonParser, DecodesAllSimpleEscapes)
    {
        const FormatValue value = JsonParser::parse(R"("q\"b\\f\fnn\r\t\/u")");

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

        const FormatValue value = JsonParser::parse(rocketEscape);

        // 火箭表情在 UTF-8 下占 4 字节
        EXPECT_EQ(value.asString(), "\xF0\x9F\x9A\x80");
        EXPECT_EQ(value.asString().size(), 4U);
    }

    TEST(JsonParser, KeepsNonAsciiUtf8TextIntact)
    {
        const FormatValue value = JsonParser::parse(R"("中文日志")");

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
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("");
        });

        EXPECT_NE(std::string(error.what()).find("为空"), std::string::npos);
    }

    TEST(JsonParser, RejectsSingleQuotedStrings)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("'single'");
        });

        EXPECT_NE(std::string(error.what()).find("应为 JSON 值"), std::string::npos);
    }

    TEST(JsonParser, RejectsComments)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("{ /* note */ }");
        });

        EXPECT_NE(std::string(error.what()).find("对象键"), std::string::npos);
    }

    TEST(JsonParser, RejectsTrailingCommaInArray)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("[1,2,]");
        });

        EXPECT_NE(std::string(error.what()).find("应为 JSON 值"), std::string::npos);
    }

    TEST(JsonParser, RejectsTrailingCommaInObject)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(R"({"a":1,})");
        });

        EXPECT_NE(std::string(error.what()).find("双引号"), std::string::npos);
    }

    TEST(JsonParser, RejectsLeadingZeros)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("01");
        });

        EXPECT_NE(std::string(error.what()).find("前导零"), std::string::npos);
    }

    TEST(JsonParser, RejectsIncompleteFractionAndExponent)
    {
        const FormatError fractionError = catchFormatError([]
        {
            return JsonParser::parse("1.");
        });
        const FormatError exponentError = catchFormatError([]
        {
            return JsonParser::parse("1e");
        });

        EXPECT_NE(std::string(fractionError.what()).find("小数点"), std::string::npos);
        EXPECT_NE(std::string(exponentError.what()).find("指数"), std::string::npos);
    }

    TEST(JsonParser, RejectsNonFiniteLiterals)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("NaN");
        });

        EXPECT_NE(std::string(error.what()).find("应为 JSON 值"), std::string::npos);
    }

    TEST(JsonParser, RejectsIncompleteKeyword)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("truely");
        });

        EXPECT_NE(std::string(error.what()).find("关键字格式错误"), std::string::npos);
    }

    TEST(JsonParser, RejectsDuplicateObjectKeys)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(R"({"a":1,"a":2})");
        });

        EXPECT_NE(std::string(error.what()).find("对象存在重复键：a"), std::string::npos);
    }

    TEST(JsonParser, RejectsUnterminatedString)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("\"unterminated");
        });

        EXPECT_NE(std::string(error.what()).find("字符串未闭合"), std::string::npos);
    }

    TEST(JsonParser, RejectsRawControlCharacterInsideString)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(std::string("\"line\0break\"", 12));
        });

        EXPECT_NE(std::string(error.what()).find("控制字符"), std::string::npos);
    }

    TEST(JsonParser, RejectsLoneHighSurrogate)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(R"("\uD800")");
        });

        EXPECT_NE(std::string(error.what()).find("代理项"), std::string::npos);
    }

    TEST(JsonParser, RejectsInvalidHexEscape)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(R"("\u00ZZ")");
        });

        EXPECT_NE(std::string(error.what()).find("十六进制"), std::string::npos);
    }

    TEST(JsonParser, RejectsUnknownEscapeLetter)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse(R"("\q")");
        });

        EXPECT_NE(std::string(error.what()).find("不支持的转义"), std::string::npos);
    }

    TEST(JsonParser, RejectsContentAfterTheDocument)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("1 2");
        });

        EXPECT_NE(std::string(error.what()).find("文档结束后"), std::string::npos);
    }

    TEST(JsonParser, ReportsLineAndColumnOfTheFailure)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("{\n  \"a\": @\n}");
        });

        EXPECT_EQ(error.position().lineNumber, 2U);
        EXPECT_EQ(error.position().columnNumber, 8U);
    }

    TEST(JsonParser, EnforcesNestingDepthLimit)
    {
        // 默认上限为 256 层，取 300 层确保越过
        const std::string deeplyNested(300, '[');
        const std::string closed = deeplyNested + std::string(300, ']');

        const FormatError error = catchFormatError([&closed]
        {
            return JsonParser::parse(closed);
        });

        EXPECT_NE(std::string(error.what()).find("嵌套深度"), std::string::npos);
        EXPECT_EQ(error.kind(), FormatErrorKind::DepthExceeded);

        // 上限之内的深层嵌套必须解析成功
        const std::string acceptableNested(200, '[');
        const std::string acceptableClosed = acceptableNested + std::string(200, ']');
        EXPECT_EQ(JsonParser::parse(acceptableClosed).type(), FormatValueType::Array);
    }

    // ============================================================================
    // UTF-8 BOM
    // ============================================================================

    TEST(JsonParser, SkipsUtf8BomByDefault)
    {
        const std::string withBom = std::string("\xEF\xBB\xBF") + R"({"a":1})";

        const FormatValue value = JsonParser::parse(withBom);

        ASSERT_TRUE(value.isObject());
        EXPECT_EQ(value["a"].asInt(), 1);
    }

    TEST(JsonParser, BomOnlyInputIsReportedAsEmpty)
    {
        const FormatError error = catchFormatError([]
        {
            return JsonParser::parse("\xEF\xBB\xBF");
        });

        EXPECT_NE(std::string(error.what()).find("为空"), std::string::npos);
        EXPECT_EQ(error.kind(), FormatErrorKind::EmptyInput);
    }

    TEST(JsonParser, RejectsBomWhenSkippingIsDisabled)
    {
        JsonParseOptions options;
        options.skipUtf8Bom = false;

        const std::string withBom = std::string("\xEF\xBB\xBF") + "1";

        const FormatError error = catchFormatError([&withBom, &options]
        {
            return JsonParser::parse(withBom, options);
        });

        EXPECT_NE(std::string(error.what()).find("应为 JSON 值"), std::string::npos);
        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
    }

    // ============================================================================
    // 三档上限
    // ============================================================================

    TEST(JsonParser, EnforcesMaximumInputLength)
    {
        JsonParseOptions options;
        options.maximumInputLength = 5;

        // 正好等于上限（[1,2] 为 5 字节）：照常解析出完整文档，绝不截断成「半个文档」
        const FormatValue atLimit = JsonParser::parse("[1,2]", options);
        ASSERT_TRUE(atLimit.isArray());
        EXPECT_EQ(atLimit.asArray().size(), 2U);

        // 超出一个字节（123456 为 6 字节）即整体拒绝，分类为 SizeExceeded 而非截断后照常解析
        const FormatError overflowError = catchFormatError([&options]
        {
            return JsonParser::parse("123456", options);
        });
        EXPECT_NE(std::string(overflowError.what()).find("输入长度"), std::string::npos);
        EXPECT_EQ(overflowError.kind(), FormatErrorKind::SizeExceeded);

        const FormatError error = catchFormatError([&options]
        {
            return JsonParser::parse("[1,2,3]", options);
        });

        EXPECT_NE(std::string(error.what()).find("输入长度"), std::string::npos);
        EXPECT_EQ(error.kind(), FormatErrorKind::SizeExceeded);
    }

    TEST(JsonParser, EnforcesMaximumStringLength)
    {
        JsonParseOptions options;
        options.maximumStringLength = 3;

        EXPECT_EQ(JsonParser::parse("\"abc\"", options).asString(), "abc");

        const FormatError valueError = catchFormatError([&options]
        {
            return JsonParser::parse("\"abcd\"", options);
        });
        EXPECT_NE(std::string(valueError.what()).find("字符串长度"), std::string::npos);
        EXPECT_EQ(valueError.kind(), FormatErrorKind::SizeExceeded);

        // 对象键走同一条字符串路径，同样受限
        const FormatError keyError = catchFormatError([&options]
        {
            return JsonParser::parse(R"({"abcd":1})", options);
        });
        EXPECT_EQ(keyError.kind(), FormatErrorKind::SizeExceeded);

        // 上限按原文（含转义序列）字节数计：短转义串也按原始长度计入
        const FormatError escapeError = catchFormatError([&options]
        {
            return JsonParser::parse(R"("\u0041\u0042")", options);
        });
        EXPECT_EQ(escapeError.kind(), FormatErrorKind::SizeExceeded);
    }

    TEST(JsonParser, EnforcesMaximumContainerElements)
    {
        JsonParseOptions options;
        options.maximumContainerElements = 2;

        EXPECT_EQ(JsonParser::parse("[1,2]", options).asArray().size(), 2U);
        EXPECT_EQ(JsonParser::parse(R"({"a":1,"b":2})", options).asObject().size(), 2U);

        const FormatError arrayError = catchFormatError([&options]
        {
            return JsonParser::parse("[1,2,3]", options);
        });
        EXPECT_NE(std::string(arrayError.what()).find("数组元素数量"), std::string::npos);
        EXPECT_EQ(arrayError.kind(), FormatErrorKind::SizeExceeded);

        const FormatError objectError = catchFormatError([&options]
        {
            return JsonParser::parse(R"({"a":1,"b":2,"c":3})", options);
        });
        EXPECT_NE(std::string(objectError.what()).find("对象成员数量"), std::string::npos);
        EXPECT_EQ(objectError.kind(), FormatErrorKind::SizeExceeded);
    }

    TEST(JsonParser, ZeroLimitsDisableTheCorrespondingProtections)
    {
        // 证明「0 = 不限制」的方式是**把限制调小**、而不是把输入调深：同一份 33 层的输入，
        // 在 maximumDepth = 32 时必须被拦、在 maximumDepth = 0 时必须通过，两个方向的差异
        // 只来自配置。递归下降解析器每层吃一个栈帧，几百层的输入在 AddressSanitizer 下会直接栈溢出
        const JsonParseOptions limitedOptions{.maximumDepth = 32};

        JsonParseOptions unlimitedOptions;
        unlimitedOptions.maximumInputLength       = 0;
        unlimitedOptions.maximumStringLength      = 0;
        unlimitedOptions.maximumContainerElements = 0;
        unlimitedOptions.maximumDepth             = 0;

        const std::string nested(33, '[');
        const std::string closed = nested + std::string(33, ']');

        // 有上限时同一份输入必须被拒，且原因分类是超深
        const FormatError error = catchFormatError([&closed, &limitedOptions]
        {
            return JsonParser::parse(closed, limitedOptions);
        });
        EXPECT_EQ(error.kind(), FormatErrorKind::DepthExceeded);

        // 上限置 0 后同一份输入必须解析成功：这才叫「0 表示不限制」
        EXPECT_EQ(JsonParser::parse(closed, unlimitedOptions).type(), FormatValueType::Array);
    }

    // ============================================================================
    // 错误分类（kind）
    // ============================================================================

    TEST(JsonParser, ClassifiesEveryDocumentedSyntaxFailure)
    {
        struct FailureSample
        {
            std::string     text;              // 非法输入
            FormatErrorKind expectedKind;      // 期望分类
            std::string     expectedSubstring; // 期望保留的中文子串
        };

        const FailureSample samples[] = {
                {"", FormatErrorKind::EmptyInput, "为空"},
                {"'x'", FormatErrorKind::UnexpectedByte, "应为 JSON 值"},
                {"@", FormatErrorKind::UnexpectedByte, "应为 JSON 值"},
                {R"({"a":1)", FormatErrorKind::UnterminatedContainer, "对象未闭合"},
                {"[1,2", FormatErrorKind::UnterminatedContainer, "数组未闭合"},
                {"\"abc", FormatErrorKind::UnterminatedString, "字符串未闭合"},
                {std::string("\"a\0b\"", 5), FormatErrorKind::ControlCharacter, "控制字符"},
                {"01", FormatErrorKind::InvalidNumber, "前导零"},
                {"1.", FormatErrorKind::InvalidNumber, "小数点"},
                {"1e", FormatErrorKind::InvalidNumber, "指数"},
                {"truely", FormatErrorKind::InvalidKeyword, "关键字格式错误"},
                {"tru", FormatErrorKind::InvalidKeyword, "关键字必须"},
                {R"({"a":1,"a":2})", FormatErrorKind::DuplicateKey, "对象存在重复键：a"},
                {"1 2", FormatErrorKind::TrailingContent, "文档结束后"},
        };

        for (const FailureSample &sample: samples)
        {
            const FormatError error = catchFormatError([&sample]
            {
                return JsonParser::parse(sample.text);
            });

            EXPECT_EQ(error.kind(), sample.expectedKind) << "input=" << sample.text;
            EXPECT_NE(std::string(error.what()).find(sample.expectedSubstring), std::string::npos) << "input=" << sample.text;
        }
    }

    TEST(JsonParser, ErrorKindNamesAndDefaultClassificationAreStable)
    {
        EXPECT_STREQ(errorKindName(FormatErrorKind::EmptyInput), "empty-input");
        EXPECT_STREQ(errorKindName(FormatErrorKind::InvalidUtf8), "invalid-utf8");
        EXPECT_STREQ(errorKindName(static_cast<FormatErrorKind>(200)), "unknown");

        // 分类与文案解耦：同一条解析路径只需要一个分类名即可被上层识别
        const FormatError parseError = catchFormatError([] { return JsonParser::parse(""); });
        EXPECT_STREQ(errorKindName(parseError.kind()), "empty-input");

        // 未走分类构造的路径（含跨格式共用原语）保持 None，文本断言不受影响
        const FormatError unclassified("原因", TextPosition{1, 1, 0});
        EXPECT_EQ(unclassified.kind(), FormatErrorKind::None);
        EXPECT_EQ(unclassified.reason(), "原因");
    }

    TEST(JsonParser, ClassifiesEscapeFailuresFromTheSharedTextPrimitives)
    {
        struct EscapeSample
        {
            std::string     text;         // 非法转义输入
            FormatErrorKind expectedKind; // 期望分类
        };

        const EscapeSample samples[] = {
                {R"("\q")", FormatErrorKind::InvalidEscape},
                {R"("\u00ZZ")", FormatErrorKind::InvalidEscape},
                {R"("\u12")", FormatErrorKind::InvalidEscape},
                {R"("\uD800")", FormatErrorKind::SurrogatePairError},
                {R"("\uDC00")", FormatErrorKind::SurrogatePairError},
                {R"("\uD83D\u0041")", FormatErrorKind::SurrogatePairError},
        };

        for (const EscapeSample &sample: samples)
        {
            const FormatError error = catchFormatError([&sample]
            {
                return JsonParser::parse(sample.text);
            });

            EXPECT_EQ(error.kind(), sample.expectedKind) << "input=" << sample.text;
        }
    }

    // ============================================================================
    // 宽松模式：注释、尾逗号、单引号字符串
    // ============================================================================

    TEST(JsonParser, LenientModeAcceptsComments)
    {
        JsonParseOptions options;
        options.allowComments = true;

        const FormatValue withLineComment = JsonParser::parse("{ // 行注释\n \"a\": 1 }", options);
        EXPECT_EQ(withLineComment["a"].asInt(), 1);

        const FormatValue withBlockComment = JsonParser::parse("[1, /* 块注释\n跨行 */ 2]", options);
        EXPECT_EQ(withBlockComment.asArray().size(), 2U);

        // 注释可以出现在任意空白位置，包括文档首尾
        EXPECT_EQ(JsonParser::parse("// 抬头\n[1]// 结尾", options).asArray().size(), 1U);

        // 关闭开关（默认）时必须仍然拒绝
        const FormatError strictError = catchFormatError([] { return JsonParser::parse("{ /* note */ }"); });
        EXPECT_EQ(strictError.kind(), FormatErrorKind::UnexpectedByte);
    }

    TEST(JsonParser, LenientModeRejectsUnterminatedBlockComment)
    {
        JsonParseOptions options;
        options.allowComments = true;

        const FormatError error = catchFormatError([&options]
        {
            return JsonParser::parse("[1, /* 未闭合", options);
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnterminatedComment);
        EXPECT_NE(std::string(error.what()).find("块注释未闭合"), std::string::npos);
    }

    TEST(JsonParser, LenientModeAcceptsTrailingCommas)
    {
        JsonParseOptions options;
        options.allowTrailingCommas = true;

        EXPECT_EQ(JsonParser::parse("[1,2,]", options).asArray().size(), 2U);
        EXPECT_EQ(JsonParser::parse(R"({"a":1,})", options).asObject().size(), 1U);
        EXPECT_EQ(JsonParser::parse("[[1,],]", options).asArray().size(), 1U);

        // 关闭开关（默认）时必须仍然拒绝，并保留原有中文文案
        const FormatError arrayError = catchFormatError([] { return JsonParser::parse("[1,2,]"); });
        EXPECT_NE(std::string(arrayError.what()).find("应为 JSON 值"), std::string::npos);

        const FormatError objectError = catchFormatError([] { return JsonParser::parse(R"({"a":1,})"); });
        EXPECT_NE(std::string(objectError.what()).find("双引号"), std::string::npos);
    }

    TEST(JsonParser, LenientModeAcceptsSingleQuotedStrings)
    {
        JsonParseOptions options;
        options.allowSingleQuotedStrings = true;

        EXPECT_EQ(JsonParser::parse("'text'", options).asString(), "text");
        EXPECT_EQ(JsonParser::parse(R"({'key':'value'})", options)["key"].asString(), "value");
        // 单引号字符串内的双引号无需转义，简单转义与双引号字符串共用同一套语义
        EXPECT_EQ(JsonParser::parse(R"('say "hi"')", options).asString(), "say \"hi\"");
        EXPECT_EQ(JsonParser::parse(R"('tab\there')", options).asString(), "tab\there");
        EXPECT_EQ(JsonParser::parse("\"text\"", options).asString(), "text");

        // 对象键的位置提示文案随模式变化，但语义仍是「必须是字符串」
        const FormatError keyError = catchFormatError([&options]
        {
            return JsonParser::parse("{ key: 1 }", options);
        });
        EXPECT_NE(std::string(keyError.what()).find("对象键必须是字符串"), std::string::npos);
        EXPECT_EQ(keyError.kind(), FormatErrorKind::UnexpectedByte);

        // 关闭开关（默认）时单引号仍是非法起始字符
        const FormatError strictError = catchFormatError([] { return JsonParser::parse("'single'"); });
        EXPECT_NE(std::string(strictError.what()).find("应为 JSON 值"), std::string::npos);
    }

    TEST(JsonParser, SingleQuotedStringsDoNotAddExtraEscapeForms)
    {
        JsonParseOptions options;
        options.allowSingleQuotedStrings = true;

        // JSON 语义没有 \' 转义，因此单引号字符串里无法书写单引号
        const FormatError quoteError = catchFormatError([&options]
        {
            return JsonParser::parse(R"('it\'s')", options);
        });
        EXPECT_EQ(quoteError.kind(), FormatErrorKind::InvalidEscape);
        EXPECT_NE(std::string(quoteError.what()).find("不支持的转义"), std::string::npos);

        // \xNN 之类的扩展转义同样不受支持
        const FormatError hexError = catchFormatError([&options]
        {
            return JsonParser::parse(R"('\x41')", options);
        });
        EXPECT_EQ(hexError.kind(), FormatErrorKind::InvalidEscape);
    }

    // ============================================================================
    // UTF-8 合法性校验
    // ============================================================================

    TEST(JsonParser, AcceptsWellFormedMultiByteUtf8Sequences)
    {
        EXPECT_EQ(JsonParser::parse(R"("中文日志")").asString(), "中文日志");
        EXPECT_EQ(JsonParser::parse("\"\xC2\xA9\"").asString(), "\xC2\xA9");                 // U+00A9 二字节
        EXPECT_EQ(JsonParser::parse("\"\xE2\x82\xAC\"").asString(), "\xE2\x82\xAC");         // U+20AC 三字节
        EXPECT_EQ(JsonParser::parse("\"\xF0\x9F\x9A\x80\"").asString(), "\xF0\x9F\x9A\x80"); // U+1F680 四字节
    }

    TEST(JsonParser, AcceptsRawDeleteCharacterBecauseRfcAllowsIt)
    {
        // RFC 8259 §7 只禁止 U+0000..U+001F 的裸控制字符，U+007F（DEL）合法
        const FormatValue value = JsonParser::parse(std::string("\"\x7F\"", 3));

        EXPECT_EQ(value.asString().size(), 1U);
        EXPECT_EQ(value.asString(), std::string("\x7F"));
    }

    TEST(JsonParser, RejectsMalformedUtf8SequencesInsideStrings)
    {
        const std::string samples[] = {
                std::string("\"\x80\"", 3),                  // 孤立的后续字节
                std::string("\"\xC0\xAF\"", 4),              // 二字节过长编码
                std::string("\"\xC1\xBF\"", 4),              // 二字节过长编码
                std::string("\"\xE0\x80\xAF\"", 5),          // 三字节过长编码
                std::string("\"\xE4\xB8\"", 4),              // 三字节序列被截断
                std::string("\"\xED\xA0\x80\"", 5),          // 以 UTF-8 编码的代理项
                // 长度须为 6（含首尾引号），写成 5 会切掉收尾引号而退化为「字符串未闭合」
                std::string("\"\xF5\x80\x80\x80\"", 6),      // 首字节越界（RFC 3629 §3：合法首字节止于 0xF4）
                std::string("\"\xF0\x9F\x9A\"", 5),          // 四字节序列被截断
        };

        for (const std::string &sample: samples)
        {
            const FormatError error = catchFormatError([&sample]
            {
                return JsonParser::parse(sample);
            });

            EXPECT_EQ(error.kind(), FormatErrorKind::InvalidUtf8);
        }
    }
} // namespace AsynGyanis::Base
