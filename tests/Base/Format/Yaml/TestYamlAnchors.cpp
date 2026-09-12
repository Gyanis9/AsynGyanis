/**
 * @file TestYamlAnchors.cpp
 * @brief YamlParser 锚点、别名与合并键测试
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

#include <functional>
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

    TEST(YamlAnchors, ResolvesAliasToACopyOfAnchoredSubtree)
    {
        FormatValue root = YamlParser::parse(
                "base: &base\n"
                "  a: 1\n"
                "  b: 2\n"
                "copy: *base\n");

        EXPECT_EQ(member(root, "copy"), member(root, "base"));

        // 别名落地为深拷贝：改动副本不影响原锚点子树
        ASSERT_NE(root.find("copy"), nullptr);
        root.find("copy")->set("a", FormatValue(static_cast<std::int64_t>(99)));
        EXPECT_EQ(member(member(root, "base"), "a").asInt(), 1);
    }

    TEST(YamlAnchors, ResolvesAliasOnScalarsAndSequences)
    {
        const FormatValue root = YamlParser::parse(
                "scalar: &s hello\n"
                "scalarCopy: *s\n"
                "list: &l [1, 2]\n"
                "listCopy: *l\n");

        EXPECT_EQ(member(root, "scalarCopy").asString(), "hello");
        ASSERT_EQ(member(root, "listCopy").asArray().size(), 2U);
        EXPECT_EQ(member(root, "listCopy").asArray()[1].asInt(), 2);
    }

    TEST(YamlAnchors, RejectsUndefinedAlias)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("copy: *missing\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
        EXPECT_NE(std::string(error.what()).find("未定义的别名"), std::string::npos);
    }

    TEST(YamlAnchors, DuplicateAnchorNameUsesLatestDefinition)
    {
        const FormatValue root = YamlParser::parse(
                "first: &x 1\n"
                "second: &x 2\n"
                "third: *x\n");

        EXPECT_EQ(member(root, "third").asInt(), 2);
    }

    TEST(YamlAnchors, AnchorsAreScopedPerDocument)
    {
        // 第二份文档不得看到第一份文档的锚点
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parseAll("---\na: &x 1\n---\nb: *x\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
    }

    TEST(YamlAnchors, MergesSingleMappingWithExplicitKeysWinning)
    {
        const FormatValue root = YamlParser::parse(
                "base: &base\n"
                "  a: 1\n"
                "  b: 2\n"
                "child:\n"
                "  <<: *base\n"
                "  b: 3\n"
                "  c: 4\n");

        const FormatValue &child = member(root, "child");
        EXPECT_EQ(member(child, "a").asInt(), 1);
        EXPECT_EQ(member(child, "b").asInt(), 3); // 显式键覆盖合并结果
        EXPECT_EQ(member(child, "c").asInt(), 4);
        EXPECT_EQ(child.asObject().size(), 3U);
    }

    TEST(YamlAnchors, ExplicitKeysWinRegardlessOfMergePosition)
    {
        const FormatValue root = YamlParser::parse(
                "base: &base {a: 1}\n"
                "child:\n"
                "  b: 2\n"
                "  <<: *base\n"
                "  a: 3\n");

        const FormatValue &child = member(root, "child");
        EXPECT_EQ(member(child, "a").asInt(), 3);
        EXPECT_EQ(member(child, "b").asInt(), 2);
    }

    TEST(YamlAnchors, EarlierMergeSourceWinsOverLater)
    {
        const FormatValue root = YamlParser::parse(
                "first: &first {a: 1, b: 1}\n"
                "second: &second {b: 2, c: 2}\n"
                "merged:\n"
                "  <<: [*first, *second]\n");

        const FormatValue &merged = member(root, "merged");
        EXPECT_EQ(member(merged, "a").asInt(), 1);
        EXPECT_EQ(member(merged, "b").asInt(), 1); // 靠前的合并源优先
        EXPECT_EQ(member(merged, "c").asInt(), 2);
    }

    TEST(YamlAnchors, QuotedDoubleAngleBracketIsNotAMergeKey)
    {
        const FormatValue root = YamlParser::parse(
                "base: &base {a: 1}\n"
                "child:\n"
                "  \"<<\": *base\n");

        const FormatValue &child = member(root, "child");
        EXPECT_TRUE(child.asObject().contains("<<"));
        EXPECT_FALSE(child.asObject().contains("a"));
    }

    TEST(YamlAnchors, RejectsMergeKeyWithScalarValue)
    {
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("child:\n  <<: 5\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::InvalidKeyword);
        EXPECT_NE(std::string(error.what()).find("合并键"), std::string::npos);
    }

    TEST(YamlAnchors, EnforcesAliasExpansionBudget)
    {
        YamlParseOptions options;
        options.maximumAliasCount = 10;

        // 锚点子树含 6 个节点；两次引用累计 12，第二次必然越界
        const std::string source =
                "base: &b [1, 2, 3, 4, 5]\n"
                "first: *b\n"
                "second: *b\n";

        const FormatError error = catchFormatError([&source, &options]
        {
            return YamlParser::parse(source, options);
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::SizeExceeded);
    }

    TEST(YamlAnchors, SupportsScalarComplexKey)
    {
        const FormatValue root = YamlParser::parse(
                "? !!str 1\n"
                ": one\n");

        ASSERT_EQ(root.asObject().size(), 1U);
        EXPECT_EQ(root.asObject().at("1").asString(), "one");
    }

    TEST(YamlAnchors, RejectsCollectionKey)
    {
        // 非标量键无法无损落进字符串键的对象，明确报错而不是静默文本化
        const FormatError error = catchFormatError([]
        {
            return YamlParser::parse("? [a, b]\n: 1\n");
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::UnexpectedByte);
        EXPECT_NE(std::string(error.what()).find("映射键暂不支持集合类型"), std::string::npos);
    }
} // namespace AsynGyanis::Base
