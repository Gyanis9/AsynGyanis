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
        EXPECT_TRUE(parsed.evaluate(document)->is<FormatValueObject>());
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
        try
        {
            static_cast<void>(JsonPointer::parse("a/b"));
            FAIL() << "缺少前导 '/' 的指针应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        }
    }

    TEST(JsonPointer, RejectsInvalidEscapeSequence)
    {
        // 孤立的 '~'
        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/a~")), FormatError);
        // '~' 后跟非 0/1 字符
        EXPECT_THROW(static_cast<void>(JsonPointer::parse("/a~2b")), FormatError);

        // 报错位置应落在非法的 '~' 上（列号从 1 起）
        try
        {
            static_cast<void>(JsonPointer::parse("/ab~2c"));
            FAIL() << "非法转义序列应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
            EXPECT_EQ(error.position().columnNumber, 4U);
        }
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
        EXPECT_TRUE(member->is<FormatValueObject>());

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

        try
        {
            static_cast<void>(JsonPointer::parse("/a/b/3").resolve(document));
            FAIL() << "越界下标在 resolve 中应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
            EXPECT_NE(std::string(error.what()).find("JSON Pointer"), std::string::npos);
        }
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
        EXPECT_TRUE(wrapper->is<FormatValueObject>());

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
