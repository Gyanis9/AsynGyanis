// OAuthSupport 的用例：OAuth 家族共用小工具的直接判据——不经 HTTP 往返，边界形状直接构造。
// 覆盖：
// - PercentEncodeFormComponentMatchesTheFormRules（未保留字符原样、保留字符 %XX、空格 %20 不用 '+'、UTF-8 按字节）
// - OptionalJsonStringDistinguishesAbsentFromWrongType（缺失为空串；字符串取回；数字与 null 交回原因；非对象按缺失兜底）
// - RequiredJsonStringRejectsMissingWrongTypedAndEmpty（必填口径：缺失、错类型、空串三者各点名拒）

#include "Net/Http/OAuthSupport.h"

#include <gtest/gtest.h>

#include <string>

namespace AsynGyanis::Net
{
    TEST(OAuthSupportTest, PercentEncodeFormComponentMatchesTheFormRules)
    {
        // 未保留字符原样（RFC 3986 §2.3 的那一组）
        EXPECT_EQ(percentEncodeFormComponent("AZaz09-._~"), "AZaz09-._~");
        // 空格编 %20：'+' 会在解码侧与真值里的加号混淆
        EXPECT_EQ(percentEncodeFormComponent("read write"), "read%20write");
        // 保留字符与加号本身一律 %XX（grant_type 里的冒号也在这条判据里）
        EXPECT_EQ(percentEncodeFormComponent("a+b/c:d=e"), "a%2Bb%2Fc%3Ad%3De");
        // 非 ASCII 按字节编（UTF-8 的「中」是 E4 B8 AD）
        EXPECT_EQ(percentEncodeFormComponent("中"), "%E4%B8%AD");
        EXPECT_EQ(percentEncodeFormComponent(""), "");
    }

    TEST(OAuthSupportTest, OptionalJsonStringDistinguishesAbsentFromWrongType)
    {
        const nlohmann::json document = nlohmann::json::parse(R"({"text":"值","number":42,"blank":null})", nullptr, false);
        ASSERT_FALSE(document.is_discarded());

        // 是字符串：取回原文
        const auto text = optionalJsonString(document, "text");
        ASSERT_TRUE(text.has_value());
        EXPECT_EQ(*text, "值");

        // 缺失：可选字段的常态，交回空串而不是失败
        const auto absent = optionalJsonString(document, "missing");
        ASSERT_TRUE(absent.has_value());
        EXPECT_TRUE(absent->empty());

        // 类型不符：交回原因并点名（不静默按缺省、不抛异常）；null 也是「不是字符串」
        for (const char *wrongTyped: {"number", "blank"})
        {
            const auto field = optionalJsonString(document, wrongTyped);
            EXPECT_FALSE(field.has_value()) << wrongTyped;
            if (!field.has_value())
            {
                EXPECT_NE(field.error().find(wrongTyped), std::string::npos) << field.error();
            }
        }

        // 非对象：按缺失兜底（find 对非对象是未定义行为，这条兜底不留给调用方记）
        const nlohmann::json arrayDocument = nlohmann::json::parse("[1,2]", nullptr, false);
        ASSERT_FALSE(arrayDocument.is_discarded());
        const auto notObject = optionalJsonString(arrayDocument, "text");
        ASSERT_TRUE(notObject.has_value());
        EXPECT_TRUE(notObject->empty());
    }

    TEST(OAuthSupportTest, RequiredJsonStringRejectsMissingWrongTypedAndEmpty)
    {
        const nlohmann::json document = nlohmann::json::parse(R"({"text":"值","number":42,"blank":""})", nullptr, false);
        ASSERT_FALSE(document.is_discarded());

        const auto text = requiredJsonString(document, "text");
        ASSERT_TRUE(text.has_value());
        EXPECT_EQ(*text, "值");

        // 缺失、类型不符、空串三者都不可用：各交回点名原因，不让任何一格静默借道
        for (const char *unusable: {"missing", "number", "blank"})
        {
            const auto field = requiredJsonString(document, unusable);
            EXPECT_FALSE(field.has_value()) << unusable;
            if (!field.has_value())
            {
                EXPECT_NE(field.error().find(unusable), std::string::npos) << field.error();
            }
        }
    }

} // namespace AsynGyanis::Net
