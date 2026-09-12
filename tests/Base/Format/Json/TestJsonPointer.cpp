/**
 * @file TestJsonPointer.cpp
 * @brief JsonPointer 单元测试：转义、求值、数组下标校验与可写定位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonPointer.h"

#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Value/FormatValue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 构造测试文档
         * @details 内容为
         *          `{"a":{"b":[10,20,{"c":"deep"}]},"":{"empty":true},"m~n":1,"x/y":2}`，
         *          其中刻意包含空键（空 token 用例）与需要转义的键（`~` 与 `/`）。
         * @return FormatValue 文档
         */
        FormatValue makeDocument()
        {
            return JsonParser::parse(R"({"a":{"b":[10,20,{"c":"deep"}]},"":{"empty":true},"m~n":1,"x/y":2})");
        }

        /**
         * @brief 按 UTF-8 字符串比较 token 序列，避免依赖 vector 的打印形式
         * @param pointer 指针
         * @return std::string 以 '/' 连接的 token 原文
         */
        std::string joinTokens(const JsonPointer &pointer)
        {
            std::string joined;
            bool        isFirst = true;
            for (const std::string &token: pointer.tokens())
            {
                if (!isFirst)
                {
                    joined += '|';
                }
                isFirst = false;
                joined += token;
            }
            return joined;
        }

        /**
         * @brief 捕获一次调用抛出的 FormatError
         * @details 与 TestJsonParser.cpp 的同名辅助保持一致，便于直接断言 kind()。
         * @param action 待执行的调用
         * @return FormatError 捕获到的错误；未抛出时返回带说明的占位错误
         */
        FormatError catchFormatError(const std::function<void()> &action)
        {
            try
            {
                action();
            } catch (const FormatError &error)
            {
                return error;
            }
            catch (...)
            {
                ADD_FAILURE() << "预期抛出 FormatError，实际抛出了其他异常";
                return FormatError("wrong exception type", TextPosition{});
            }

            ADD_FAILURE() << "预期抛出 FormatError，但调用成功";
            return FormatError("no exception thrown", TextPosition{});
        }
    } // namespace

    // ============================================================================
    // 语法与转义（RFC 6901 §3）
    // ============================================================================

    TEST(JsonPointer, EmptyPointerRefersToWholeDocument)
    {
        const FormatValue document = makeDocument();

        const JsonPointer defaultPointer;
        EXPECT_TRUE(defaultPointer.empty());
        EXPECT_EQ(defaultPointer.size(), 0U);
        EXPECT_EQ(defaultPointer.toString(), "");

        const JsonPointer parsed = JsonPointer::parse("");
        EXPECT_EQ(parsed, defaultPointer);
        ASSERT_NE(parsed.evaluate(document), nullptr);
        EXPECT_TRUE(parsed.evaluate(document)->isObject());
    }

    TEST(JsonPointer, ParsesReferenceTokensInOrder)
    {
        const JsonPointer pointer = JsonPointer::parse("/a/b/0");

        EXPECT_EQ(pointer.size(), 3U);
        EXPECT_EQ(joinTokens(pointer), "a|b|0");
        EXPECT_EQ(pointer.toString(), "/a/b/0");
    }

    TEST(JsonPointer, UnescapesTildeSlashInOfficialOrder)
    {
        // RFC 6901 §3：~1 → '/'、~0 → '~'，且必须先还原 ~1 再还原 ~0
        const JsonPointer escapedSlash = JsonPointer::parse("/a~1b");
        ASSERT_EQ(escapedSlash.size(), 1U);
        EXPECT_EQ(escapedSlash.tokens()[0], "a/b");

        const JsonPointer escapedTilde = JsonPointer::parse("/m~0n");
        ASSERT_EQ(escapedTilde.size(), 1U);
        EXPECT_EQ(escapedTilde.tokens()[0], "m~n");

        // 经典边界："~01" 应解码为 "~1" 而不是 "/"
        const JsonPointer tildeZeroOne = JsonPointer::parse("/~01");
        ASSERT_EQ(tildeZeroOne.size(), 1U);
        EXPECT_EQ(tildeZeroOne.tokens()[0], "~1");
    }

    TEST(JsonPointer, EscapesTokensWhenSerializing)
    {
        EXPECT_EQ(JsonPointer::escapeToken("a/b~c"), "a~1b~0c");
        EXPECT_EQ(JsonPointer::escapeToken(""), "");
        EXPECT_EQ(JsonPointer::escapeToken("plain"), "plain");

        // 编码与解码互逆
        const std::string original = "x/y~z";
        EXPECT_EQ(JsonPointer::unescapeToken(JsonPointer::escapeToken(original)), original);

        const JsonPointer pointer = JsonPointer::parse("/a~1b/m~0n");
        EXPECT_EQ(pointer.toString(), "/a~1b/m~0n");
    }

    TEST(JsonPointer, RejectsPointerWithoutLeadingSlash)
    {
        // 指针文本本身不合法，属于语法错误：分类必须是 InvalidPointer 而不是字节级错误
        const FormatError error = catchFormatError([]
        {
            static_cast<void>(JsonPointer::parse("a/b"));
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidPointer);
        EXPECT_NE(std::string(error.what()).find("必须以 '/' 开头"), std::string::npos);
    }

    TEST(JsonPointer, RejectsInvalidEscapeSequence)
    {
        // 孤立的 '~'
        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/a~")), FormatError);
        // '~' 后跟非 0/1 字符
        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/a~2b")), FormatError);

        // 报错位置应落在非法的 '~' 上（列号从 1 起）
        const FormatError error = catchFormatError([]
        {
            static_cast<void>(JsonPointer::parse("/ab~2c"));
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidPointer);
        EXPECT_EQ(error.position().columnNumber, 4U);

        // 孤立 '~' 与非 0/1 转义都属于同一类语法错误
        EXPECT_EQ(catchFormatError([] { static_cast<void>(JsonPointer::parse("/a~")); }).kind(), FormatErrorKind::InvalidPointer);
        EXPECT_EQ(catchFormatError([] { static_cast<void>(JsonPointer::parse("/a~2b")); }).kind(), FormatErrorKind::InvalidPointer);
    }

    // ============================================================================
    // 求值（RFC 6901 §4）
    // ============================================================================

    TEST(JsonPointer, EvaluatesNestedMembersAndArrayIndices)
    {
        const FormatValue document = makeDocument();

        const FormatValue *first = JsonPointer::parse("/a/b/0").evaluate(document);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->asInt(), 10);

        const FormatValue *deep = JsonPointer::parse("/a/b/2/c").evaluate(document);
        ASSERT_NE(deep, nullptr);
        EXPECT_EQ(deep->asString(), "deep");
    }

    TEST(JsonPointer, EvaluatesEscapedKeysAgainstRealDocument)
    {
        const FormatValue document = makeDocument();

        // key 中真实含 '~' 与 '/'，必须用 ~0 / ~1 才能定位
        ASSERT_NE(JsonPointer::parse("/m~0n").evaluate(document), nullptr);
        EXPECT_EQ(JsonPointer::parse("/m~0n").evaluate(document)->asInt(), 1);
        ASSERT_NE(JsonPointer::parse("/x~1y").evaluate(document), nullptr);
        EXPECT_EQ(JsonPointer::parse("/x~1y").evaluate(document)->asInt(), 2);
    }

    TEST(JsonPointer, EmptyTokenAddressesEmptyKey)
    {
        const FormatValue document = makeDocument();

        // "/" 是一个空 token，指向键为 "" 的成员
        const JsonPointer emptyKey = JsonPointer::parse("/");
        ASSERT_EQ(emptyKey.size(), 1U);
        EXPECT_EQ(emptyKey.tokens()[0], "");

        const FormatValue *member = emptyKey.evaluate(document);
        ASSERT_NE(member, nullptr);
        EXPECT_TRUE(member->isObject());

        // 空键下的普通成员：路径里出现连续两个 '/'
        const FormatValue *flag = JsonPointer::parse("//empty").evaluate(document);
        ASSERT_NE(flag, nullptr);
        EXPECT_TRUE(flag->asBool());
    }

    TEST(JsonPointer, ReturnsNullptrForOutOfRangeArrayIndex)
    {
        const FormatValue document = makeDocument();

        EXPECT_EQ(JsonPointer::parse("/a/b/3").evaluate(document), nullptr);
        EXPECT_FALSE(JsonPointer::parse("/a/b/3").tryEvaluate(document).has_value());

        // 指针语法完全合法，只是定位不到值：属于语义错误 PatchTargetMissing
        const FormatError error = catchFormatError([&document]
        {
            static_cast<void>(JsonPointer::parse("/a/b/3").resolve(document));
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::PatchTargetMissing);
        EXPECT_NE(std::string(error.what()).find("JSON Pointer"), std::string::npos);
    }

    TEST(JsonPointer, SeparatesSyntaxFailuresFromResolutionMisses)
    {
        const FormatValue document = makeDocument();

        // 语法错：parse() 阶段即抛 InvalidPointer，与文档内容无关
        EXPECT_EQ(catchFormatError([] { static_cast<void>(JsonPointer::parse("a")); }).kind(), FormatErrorKind::InvalidPointer);
        EXPECT_EQ(catchFormatError([] { static_cast<void>(JsonPointer::parse("/a~3")); }).kind(), FormatErrorKind::InvalidPointer);

        // 语义错：指针本身合法，resolve() 未命中抛 PatchTargetMissing
        EXPECT_EQ(catchFormatError([&document] { static_cast<void>(JsonPointer::parse("/nope").resolve(document)); }).kind(),
                  FormatErrorKind::PatchTargetMissing);
        // 数组下标前导零在前端是「不定位任何元素」的求值失败，仍属语义错而非语法错
        EXPECT_EQ(catchFormatError([&document] { static_cast<void>(JsonPointer::parse("/a/b/01").resolve(document)); }).kind(),
                  FormatErrorKind::PatchTargetMissing);

        // 两类错误的分类必须互不相同，且都不再借用字节级分类
        EXPECT_NE(FormatErrorKind::InvalidPointer, FormatErrorKind::PatchTargetMissing);
        EXPECT_NE(FormatErrorKind::InvalidPointer, FormatErrorKind::UnexpectedByte);
        EXPECT_NE(FormatErrorKind::PatchTargetMissing, FormatErrorKind::UnexpectedByte);
        EXPECT_STREQ(errorKindName(FormatErrorKind::InvalidPointer), "invalid-pointer");
        EXPECT_STREQ(errorKindName(FormatErrorKind::PatchTargetMissing), "patch-target-missing");
    }

    TEST(JsonPointer, TreatsTypeMismatchAsMiss)
    {
        const FormatValue document = makeDocument();

        // 数组上使用非下标 token
        EXPECT_EQ(JsonPointer::parse("/a/b/c").evaluate(document), nullptr);
        // 标量上继续取成员（RFC 6901 §4 定义为求值失败）
        EXPECT_EQ(JsonPointer::parse("/a/b/0/x").evaluate(document), nullptr);
        // 对象上使用数字 token：对象成员名恰好不是 "0"
        EXPECT_EQ(JsonPointer::parse("/a/0").evaluate(document), nullptr);
    }

    TEST(JsonPointer, RejectsLeadingZeroArrayIndex)
    {
        const FormatValue document = makeDocument();

        // RFC 6901 §4：数组下标不允许前导零
        EXPECT_EQ(JsonPointer::parse("/a/b/01").evaluate(document), nullptr);
        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/a/b/01").resolve(document)), FormatError);
        // "0" 本身合法
        EXPECT_NE(JsonPointer::parse("/a/b/0").evaluate(document), nullptr);
    }

    TEST(JsonPointer, TreatsDashAsMissInEvaluation)
    {
        const FormatValue document = makeDocument();

        // '-' 代表「末尾之后」这个不存在的元素，求值中视为未命中
        EXPECT_EQ(JsonPointer::parse("/a/b/-").evaluate(document), nullptr);
        EXPECT_FALSE(JsonPointer::parse("/a/b/-").tryEvaluate(document).has_value());
    }

    TEST(JsonPointer, ResolveThrowsOnMissingPath)
    {
        const FormatValue document = makeDocument();

        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/nope").resolve(document)), FormatError);
    }

    TEST(JsonPointer, EvaluatesDeepPaths)
    {
        // 10 层嵌套：验证逐 token 下钻不丢层级
        const FormatValue document = JsonParser::parse(
                R"({"l1":{"l2":{"l3":{"l4":{"l5":{"l6":{"l7":{"l8":{"l9":{"l10":"bottom"}}}}}}}}}})");

        const FormatValue *bottom = JsonPointer::parse("/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10").evaluate(document);
        ASSERT_NE(bottom, nullptr);
        EXPECT_EQ(bottom->asString(), "bottom");

        // 少一层命中的是「承载 l10 的那个对象」，而不是最终的标量
        const FormatValue *wrapper = JsonPointer::parse("/l1/l2/l3/l4/l5/l6/l7/l8/l9").evaluate(document);
        ASSERT_NE(wrapper, nullptr);
        EXPECT_TRUE(wrapper->isObject());

        // 标量之上继续取成员属于求值失败（RFC 6901 §4），因此再深一层即未命中
        EXPECT_EQ(JsonPointer::parse("/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10/extra").evaluate(document), nullptr);
    }

    TEST(JsonPointer, EvaluateForWriteAllowsInPlaceMutation)
    {
        FormatValue document = makeDocument();

        FormatValue *writable = JsonPointer::parse("/a/b/1").evaluateForWrite(document);
        ASSERT_NE(writable, nullptr);
        EXPECT_EQ(writable->asInt(), 20);

        // 就地改写标量
        *writable = FormatValue(std::int64_t(99));
        EXPECT_EQ(JsonPointer::parse("/a/b/1").resolve(document).asInt(), 99);

        // 就地新增对象成员（值模型本身按键有序）
        FormatValue *object = JsonPointer::parse("/a/b/2").evaluateForWrite(document);
        ASSERT_NE(object, nullptr);
        object->set("extra", FormatValue(std::string("added")));
        EXPECT_EQ(JsonPointer::parse("/a/b/2/extra").resolve(document).asString(), "added");

        // 未命中的路径返回 nullptr
        EXPECT_EQ(JsonPointer::parse("/a/nope").evaluateForWrite(document), nullptr);
    }

    // ============================================================================
    // 前缀与相等判定
    // ============================================================================

    TEST(JsonPointer, IsProperPrefixOfDetectsAncestorOnly)
    {
        const JsonPointer ancestor = JsonPointer::parse("/a/b");
        const JsonPointer descendant = JsonPointer::parse("/a/b/c");
        const JsonPointer sibling = JsonPointer::parse("/a/x");
        const JsonPointer same = JsonPointer::parse("/a/b");

        EXPECT_TRUE(ancestor.isProperPrefixOf(descendant));
        EXPECT_FALSE(descendant.isProperPrefixOf(ancestor));
        EXPECT_FALSE(ancestor.isProperPrefixOf(sibling));
        // 长度相等时恒为 false（真前缀要求更短）
        EXPECT_FALSE(ancestor.isProperPrefixOf(same));

        // 根指针是任何非空指针的真前缀
        EXPECT_TRUE(JsonPointer().isProperPrefixOf(descendant));
        EXPECT_FALSE(JsonPointer().isProperPrefixOf(JsonPointer()));
    }

    TEST(JsonPointer, ComparesPointersByDecodedTokens)
    {
        EXPECT_EQ(JsonPointer::parse(""), JsonPointer(std::vector<std::string>{}));
        EXPECT_EQ(JsonPointer::parse("/a"), JsonPointer(std::vector<std::string>{"a"}));
        EXPECT_EQ(JsonPointer::parse("/a~01"), JsonPointer(std::vector<std::string>{"a~1"}));

        // "~01" 解码为 "~1"，"~1" 解码为 "/"，二者不等
        EXPECT_NE(JsonPointer::parse("/a~01"), JsonPointer::parse("/a~1"));
        EXPECT_NE(JsonPointer::parse("/a"), JsonPointer::parse("/b"));
    }
} // namespace AsynGyanis::Base
