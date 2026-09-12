/**
 * @file TestHttpResponse.cpp
 * @brief HttpResponse 单元测试：头部写入校验、可重复头部模型、序列化顺序与自动补齐
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpResponse.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// UTF-8 编码的「中」字，3 字节：用来钉住 content-length 算的是字节数而非字符数
        static constexpr std::string_view kThreeByteUtf8Character{"\xe4\xb8\xad", 3};

        /**
         * @brief 判断文本里是否出现指定子串
         * @param haystack 待搜索文本
         * @param needle   目标子串
         * @return true 命中
         */
        bool containsText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 取子串在文本中的位置，便于断言序列化先后顺序
         * @param haystack 待搜索文本
         * @param needle   目标子串
         * @return std::size_t 首次出现位置；未命中返回 std::string::npos
         */
        std::size_t positionOfText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle);
        }
    } // namespace

    // ============================================================================
    // 状态行与默认值
    // ============================================================================

    TEST(HttpResponse, DefaultsToOkayResponseWithEmptyBody)
    {
        const HttpResponse response;

        EXPECT_EQ(response.status(), 200);
        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(response.headers().empty());
        // 空正文仍要交代边界：整条报文只有状态行与一条自动补齐的 content-length
        EXPECT_EQ(response.toString(), "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n");
    }

    TEST(HttpResponse, WritesStandardReasonPhraseForKnownStatus)
    {
        HttpResponse response;
        response.setStatus(404);

        const std::string output = response.toString();

        EXPECT_TRUE(output.starts_with("HTTP/1.1 404 Not Found\r\n"));
    }

    TEST(HttpResponse, WritesEmptyReasonPhraseForUnknownStatus)
    {
        HttpResponse response;
        response.setHttpVersion("HTTP/1.0");
        response.setStatus(799);

        const std::string output = response.toString();

        // 未收录状态码给空原因短语，RFC 9110 §3.1.2 允许状态行以「空格 + CRLF」收尾
        EXPECT_TRUE(output.starts_with("HTTP/1.0 799 \r\n"));
    }

    // ============================================================================
    // 头部写入校验：非法值一律拒写且不动已有状态
    // ============================================================================

    TEST(HttpResponse, AcceptsValidHeaderAndStoresNameLowercased)
    {
        HttpResponse response;

        EXPECT_TRUE(response.setHeader("Content-Type", "text/html"));

        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/html");
        EXPECT_EQ(response.getHeader("CONTENT-TYPE").value_or(""), "text/html");
        EXPECT_EQ(response.headers().count("Content-Type"), 0U);
        EXPECT_TRUE(containsText(response.toString(), "content-type: text/html\r\n"));
    }

    TEST(HttpResponse, RejectsInjectedHeaderLineInsideHeaderValue)
    {
        HttpResponse response;
        ASSERT_TRUE(response.setHeader("Location", "https://example.test/target"));
        const std::string beforeAttack = response.toString();

        // 响应拆分攻击串：值里的 CRLF 会让调用方自己提前结束头部块
        EXPECT_FALSE(response.setHeader("Location", "x\r\nX-Injected: 1"));

        EXPECT_EQ(response.getHeader("location").value_or(""), "https://example.test/target");
        EXPECT_FALSE(containsText(response.toString(), "X-Injected"));
        EXPECT_EQ(response.toString(), beforeAttack);
    }

    TEST(HttpResponse, RejectsBareLineFeedInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", "a\nb"));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
        EXPECT_TRUE(response.headers().empty());
    }

    TEST(HttpResponse, RejectsBareCarriageReturnInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", "a\rb"));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
    }

    TEST(HttpResponse, RejectsFullHeaderTerminatorInsideHeaderValue)
    {
        HttpResponse response;
        ASSERT_TRUE(response.setHeader("X-Trace", "origin"));

        // 最狠的一种：值里带完整的「结束头部块 + 伪造正文」
        EXPECT_FALSE(response.setHeader("X-Trace", "c\r\nd: e\r\n\r\nbody"));

        EXPECT_EQ(response.getHeader("x-trace").value_or(""), "origin");
        const std::string output = response.toString();
        EXPECT_FALSE(containsText(output, "d: e"));
        EXPECT_FALSE(containsText(output, "\r\n\r\nbody"));
        // 正文与状态码由本类自行序列化，没被攻击串污染
        EXPECT_TRUE(containsText(output, "content-length: 0"));
    }

    TEST(HttpResponse, RejectsNulByteInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", std::string("a\0b", 3)));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
        EXPECT_TRUE(response.headers().empty());
    }

    TEST(HttpResponse, RejectsOtherControlCharactersInsideHeaderValue)
    {
        HttpResponse response;

        // 垂直制表同样是控制字符：它会撕裂报文行，不在放行清单里
        EXPECT_FALSE(response.setHeader("X-Note", std::string("a\vb", 3)));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
    }

    TEST(HttpResponse, AcceptsTabInsideHeaderValue)
    {
        HttpResponse response;

        // 水平制表符是 RFC 允许出现在 OWS 里的空白，属于显式放行项
        EXPECT_TRUE(response.setHeader("X-Note", "a\tb"));

        EXPECT_EQ(response.getHeader("x-note").value_or(""), "a\tb");
    }

    TEST(HttpResponse, RejectsMalformedHeaderNames)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("", "value"));
        EXPECT_FALSE(response.setHeader("X Note", "value"));
        EXPECT_FALSE(response.setHeader("X:Note", "value"));
        EXPECT_FALSE(response.setHeader("X\tNote", "value"));
        EXPECT_FALSE(response.setHeader(std::string("X\0Note", 6), "value"));

        EXPECT_TRUE(response.headers().empty());
        EXPECT_TRUE(response.headerValues("x note").empty());
    }

    TEST(HttpResponse, AcceptsLegalTokenCharactersInHeaderName)
    {
        HttpResponse response;

        // tchar 集合里的符号（含 x-amz- 这类私有前缀常用的连字符）必须放行，否则误伤真实业务
        EXPECT_TRUE(response.setHeader("x-aMz.meta#1", "value"));

        EXPECT_EQ(response.getHeader("x-amz.meta#1").value_or(""), "value");
    }

    // ============================================================================
    // 可重复头部与顺序
    // ============================================================================

    TEST(HttpResponse, AppendsEverySetCookieCallAsIndependentHeader)
    {
        HttpResponse response;

        EXPECT_TRUE(response.setHeader("Set-Cookie", "sid=1; Path=/"));
        EXPECT_TRUE(response.setHeader("set-cookie", "theme=dark"));
        EXPECT_TRUE(response.setHeader("SET-COOKIE", "lang=zh"));

        const std::vector<std::string> values = response.headerValues("Set-Cookie");
        ASSERT_EQ(values.size(), 3U);
        EXPECT_EQ(values[0], "sid=1; Path=/");
        EXPECT_EQ(values[1], "theme=dark");
        EXPECT_EQ(values[2], "lang=zh");
    }

    TEST(HttpResponse, SerializesEverySetCookieLineInSettingOrder)
    {
        HttpResponse response;
        response.setHeader("Set-Cookie", "first=1");
        response.setHeader("Set-Cookie", "second=2");

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "set-cookie: first=1\r\n");
        const std::size_t secondPosition = positionOfText(output, "set-cookie: second=2\r\n");

        EXPECT_NE(firstPosition, std::string::npos);
        EXPECT_NE(secondPosition, std::string::npos);
        EXPECT_LT(firstPosition, secondPosition);
        // 旧的 set-cookie_1 伪键会原样发到线上，已彻底移除
        EXPECT_FALSE(containsText(output, "set-cookie_1"));
        EXPECT_EQ(response.headers().count("set-cookie_1"), 0U);
    }

    TEST(HttpResponse, KeepsFirstSetCookieInSingleValueView)
    {
        HttpResponse response;
        response.setHeader("Set-Cookie", "first=1");
        response.setHeader("Set-Cookie", "second=2");

        // headers()/getHeader() 的「一名一值」契约不变：可重复头部留首条
        EXPECT_EQ(response.getHeader("set-cookie").value_or(""), "first=1");
        EXPECT_EQ(response.headers().at("set-cookie"), "first=1");
    }

    TEST(HttpResponse, SerializesHeadersInSettingOrder)
    {
        HttpResponse response;
        response.setHeader("x-bsecond", "2");
        response.setHeader("x-alast", "1");
        response.setHeader("x-cmiddle", "3");

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "x-bsecond: 2\r\n");
        const std::size_t secondPosition = positionOfText(output, "x-alast: 1\r\n");
        const std::size_t thirdPosition = positionOfText(output, "x-cmiddle: 3\r\n");

        EXPECT_NE(firstPosition, std::string::npos);
        EXPECT_LT(firstPosition, secondPosition);
        EXPECT_LT(secondPosition, thirdPosition);
        // 自动补齐的头部排在自设头部之后
        EXPECT_LT(thirdPosition, positionOfText(output, "content-length:"));
    }

    TEST(HttpResponse, OverwritesOrdinaryHeaderInPlaceWithoutMovingIt)
    {
        HttpResponse response;
        response.setHeader("x-first", "1");
        response.setHeader("x-second", "old");
        response.setHeader("x-third", "3");

        ASSERT_TRUE(response.setHeader("x-second", "new"));

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "x-first: 1\r\n");
        const std::size_t rewrittenPosition = positionOfText(output, "x-second: new\r\n");
        const std::size_t thirdPosition = positionOfText(output, "x-third: 3\r\n");

        EXPECT_NE(rewrittenPosition, std::string::npos);
        EXPECT_LT(firstPosition, rewrittenPosition);
        EXPECT_LT(rewrittenPosition, thirdPosition);
        EXPECT_FALSE(containsText(output, "old"));
        EXPECT_EQ(response.headerValues("x-second").size(), 1U);
    }

    // ============================================================================
    // 自动补齐：content-length 与 content-type
    // ============================================================================

    TEST(HttpResponse, AddsContentLengthMatchingBodyByteCount)
    {
        HttpResponse response;
        response.setBody("hello");

        EXPECT_TRUE(containsText(response.toString(), "content-length: 5\r\n"));
    }

    TEST(HttpResponse, CountsContentLengthInBytesNotCharacters)
    {
        HttpResponse response;
        // 两个「中」字：长度为 2，UTF-8 编码后实占 6 字节。收端按字节定界，写错一个字节整条连接就错位
        response.setBody(std::string(kThreeByteUtf8Character) + std::string(kThreeByteUtf8Character));

        EXPECT_TRUE(containsText(response.toString(), "content-length: 6\r\n"));
        EXPECT_FALSE(containsText(response.toString(), "content-length: 2"));
    }

    TEST(HttpResponse, KeepsExplicitContentLengthUntouched)
    {
        HttpResponse response;
        response.setHeader("content-length", "999");
        response.setBody("hello");

        const std::string output = response.toString();

        EXPECT_TRUE(containsText(output, "content-length: 999\r\n"));
        EXPECT_FALSE(containsText(output, "content-length: 5"));
    }

    TEST(HttpResponse, OmitsAutoContentLengthForNoContentResponse)
    {
        HttpResponse response;
        response.setStatus(204);

        // 204 不带正文，自动补 content-length 等于对收端多做一个承诺
        EXPECT_EQ(response.toString(), "HTTP/1.1 204 No Content\r\n\r\n");
    }

    TEST(HttpResponse, OmitsAutoContentLengthForInformationalResponses)
    {
        HttpResponse continueResponse;
        continueResponse.setStatus(100);
        EXPECT_EQ(continueResponse.toString(), "HTTP/1.1 100 Continue\r\n\r\n");

        HttpResponse switchingResponse;
        switchingResponse.setStatus(199);
        switchingResponse.setHeader("x-custom", "1");
        const std::string output = switchingResponse.toString();

        EXPECT_TRUE(containsText(output, "x-custom: 1\r\n"));
        EXPECT_FALSE(containsText(output, "content-length"));
    }

    TEST(HttpResponse, KeepsExplicitContentLengthOnNoContentResponse)
    {
        HttpResponse response;
        response.setStatus(204);
        ASSERT_TRUE(response.setHeader("content-length", "0"));

        // 调用方显式设置的不会被序列化时抹掉
        EXPECT_TRUE(containsText(response.toString(), "content-length: 0\r\n"));
    }

    TEST(HttpResponse, AddsTextPlainContentTypeOnlyWhenBodyIsPresent)
    {
        HttpResponse withBody;
        withBody.setBody("plain text");
        EXPECT_TRUE(containsText(withBody.toString(), "content-type: text/plain\r\n"));

        HttpResponse withoutBody;
        EXPECT_FALSE(containsText(withoutBody.toString(), "content-type"));
    }

    TEST(HttpResponse, DoesNotOverrideExplicitContentType)
    {
        HttpResponse response;
        response.setHeader("Content-Type", "application/json");
        response.setBody("{}");

        const std::string output = response.toString();

        EXPECT_TRUE(containsText(output, "content-type: application/json\r\n"));
        EXPECT_FALSE(containsText(output, "text/plain"));
        EXPECT_EQ(response.headerValues("content-type").size(), 1U);
    }

    TEST(HttpResponse, PlacesAutoHeadersAfterStatusLineAndCustomHeaders)
    {
        HttpResponse response;
        response.setBody("abc");
        response.setHeader("x-trace", "1");

        const std::string output = response.toString();
        const std::size_t bodySeparator = positionOfText(output, "\r\n\r\n");

        EXPECT_NE(bodySeparator, std::string::npos);
        EXPECT_LT(positionOfText(output, "x-trace: 1\r\n"), positionOfText(output, "content-type: text/plain\r\n"));
        EXPECT_LT(positionOfText(output, "content-type: text/plain\r\n"), positionOfText(output, "content-length: 3\r\n"));
        EXPECT_EQ(output.substr(bodySeparator + 4), "abc");
    }

    // ============================================================================
    // 正文与工厂方法、复位
    // ============================================================================

    TEST(HttpResponse, ReplacesStoredBodyOnEachSet)
    {
        HttpResponse response;
        response.setBody("first");
        response.setBody("second");

        EXPECT_EQ(response.body(), "second");
        response.setBody(std::string_view{});
        EXPECT_TRUE(response.body().empty());
    }

    TEST(HttpResponse, BuildsOkayResponseFromFactory)
    {
        const HttpResponse response = HttpResponse::ok("hi");

        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "hi");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/plain");
        EXPECT_TRUE(containsText(response.toString(), "content-length: 2\r\n"));
    }

    TEST(HttpResponse, BuildsNotFoundResponseFromFactory)
    {
        const HttpResponse response = HttpResponse::notFound();

        EXPECT_EQ(response.status(), 404);
        EXPECT_EQ(response.body(), "Not Found");
        EXPECT_TRUE(containsText(response.toString(), "HTTP/1.1 404 Not Found\r\n"));
        EXPECT_TRUE(containsText(response.toString(), "content-length: 9\r\n"));
    }

    TEST(HttpResponse, BuildsServerErrorResponseFromMessage)
    {
        const HttpResponse withMessage = HttpResponse::serverError("boom");
        EXPECT_EQ(withMessage.status(), 500);
        EXPECT_EQ(withMessage.body(), "boom");

        const HttpResponse withoutMessage = HttpResponse::serverError();
        EXPECT_EQ(withoutMessage.status(), 500);
        EXPECT_TRUE(withoutMessage.body().empty());
        EXPECT_TRUE(containsText(withoutMessage.toString(), "content-length: 0\r\n"));
    }

    TEST(HttpResponse, ResetClearsStatusVersionHeadersAndBody)
    {
        HttpResponse response;
        response.setStatus(503);
        response.setHttpVersion("HTTP/1.0");
        response.setHeader("Retry-After", "5");
        response.setHeader("Set-Cookie", "sid=1");
        response.setHeader("Set-Cookie", "theme=dark");
        response.setBody("stale");

        response.reset();

        EXPECT_EQ(response.status(), 200);
        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(response.headers().empty());
        // 两份存储一起清：视图清空了但权威记录残留，会照样发到线上
        EXPECT_TRUE(response.headerValues("set-cookie").empty());
        EXPECT_FALSE(response.getHeader("retry-after").has_value());
        EXPECT_TRUE(containsText(response.toString(), "HTTP/1.1 200 OK\r\n"));
    }
} // namespace AsynGyanis::Net
