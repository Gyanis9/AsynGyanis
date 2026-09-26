// HttpCookie 单元测试：Set-Cookie 的渲染与解析、属性校验、请求侧 Cookie 头解析，
// 以及 HttpRequest::cookies() 与 HttpResponse::setCookie() 这两处接线。
#include "Net/Http/HttpCookie.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 取一个可复现的时刻（RFC 9110 的样例时刻）
         * @return 1994-11-06T08:49:37Z
         */
        [[nodiscard]] std::chrono::system_clock::time_point sampleMoment()
        {
            const std::optional<std::chrono::system_clock::time_point> parsed = parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
            EXPECT_TRUE(parsed.has_value());
            return parsed.value_or(std::chrono::system_clock::time_point{});
        }
    } // namespace

    /**
     * @brief 只输出被显式设过的属性：没设就别替调用方编一个
     * @details 「没设 Path 就补 Path=/」这类好心会静默改变 Cookie 的作用域，
     *          而作用域正是 Cookie 最容易被放宽出事的地方。
     */
    TEST(HttpCookie, RendersOnlyExplicitlySetAttributes)
    {
        const HttpCookie bare("session", "abc");
        EXPECT_EQ(bare.renderAsSetCookie(), "session=abc");

        HttpCookie full("session", "abc");
        full.setPath("/app");
        full.setDomain("example.com");
        full.setMaxAgeSeconds(600);
        full.setExpiresAt(sampleMoment());
        full.setSecure();
        full.setHttpOnly();
        full.setSameSite(CookieSameSitePolicy::Lax);
        EXPECT_EQ(full.renderAsSetCookie(), "session=abc; Path=/app; Domain=example.com; Max-Age=600; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Secure; HttpOnly; SameSite=Lax");
    }

    /**
     * @brief 名字与取值的非法字符在构造与 setValue 两处都拒
     * @details 这是程序侧写错（往往来自未净化的用户输入），当场抛出比发出一条会破坏头部结构的
     *          Set-Cookie 好：后者等于给响应拆分留口子。
     */
    TEST(HttpCookie, RejectsInvalidNameAndValueAtConstruction)
    {
        EXPECT_THROW(HttpCookie("bad name", "v"), Base::InvalidArgumentException);
        EXPECT_THROW(HttpCookie("", "v"), Base::InvalidArgumentException);
        EXPECT_THROW(HttpCookie("n", "va;lue"), Base::InvalidArgumentException);
        EXPECT_THROW(HttpCookie("n", "value with space"), Base::InvalidArgumentException);

        HttpCookie cookie("n", "ok");
        EXPECT_NO_THROW(cookie.setValue(""));
        EXPECT_EQ(cookie.value(), "") << "空取值是 RFC 6265 允许的";
        EXPECT_THROW(cookie.setValue("bad\r\nvalue"), Base::InvalidArgumentException);
    }

    /**
     * @brief Path 与 Domain 各自的校验：不替调用方补斜杠、不收会破坏结构的字符
     */
    TEST(HttpCookie, ValidatesPathAndDomainAttributes)
    {
        HttpCookie cookie("n", "v");
        EXPECT_THROW(cookie.setPath("app"), Base::InvalidArgumentException);
        EXPECT_THROW(cookie.setPath(""), Base::InvalidArgumentException);
        EXPECT_THROW(cookie.setDomain(""), Base::InvalidArgumentException);
        EXPECT_THROW(cookie.setDomain("a b.com"), Base::InvalidArgumentException);
        EXPECT_THROW(cookie.setDomain("a;b.com"), Base::InvalidArgumentException);
        EXPECT_NO_THROW(cookie.setPath("/"));
        EXPECT_NO_THROW(cookie.setDomain(".example.com")) << "前导点是 RFC 6265 认可的写法，保留原样交出去";
    }

    /**
     * @brief 解析 Set-Cookie：属性名大小写不敏感、未知属性跳过而不判整条失败
     * @details RFC 6265 §5.2 明确「不认的属性名直接跳过」。服务端将来会加新属性（Partitioned 就是
     *          一个），判失败等于让每一次属性扩展都打断一次登录。
     */
    TEST(HttpCookie, ParsesSetCookieAttributesCaseInsensitivelyAndSkipsUnknown)
    {
        const std::optional<HttpCookie> parsed = HttpCookie::parseSetCookie("sid=42; PATH=/api; DOMAIN=Example.COM; max-age=+60; HTTPONLY; Future=1; SameSite=strict");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->name(), "sid");
        EXPECT_EQ(parsed->value(), "42");
        ASSERT_TRUE(parsed->path().has_value());
        EXPECT_EQ(*parsed->path(), "/api");
        ASSERT_TRUE(parsed->domain().has_value());
        EXPECT_EQ(*parsed->domain(), "Example.COM") << "域名大小写原样保留，归一化不是这一层的事";
        ASSERT_TRUE(parsed->maxAgeSeconds().has_value());
        EXPECT_EQ(*parsed->maxAgeSeconds(), 60) << "Max-Age 允许前导 '+'";
        EXPECT_TRUE(parsed->isHttpOnly());
        EXPECT_FALSE(parsed->isSecure());
        ASSERT_TRUE(parsed->sameSite().has_value());
        EXPECT_EQ(*parsed->sameSite(), CookieSameSitePolicy::Strict);
    }

    /**
     * @brief 渲染与解析互为逆运算：一条 Set-Cookie 绕一圈字段不变
     * @details 客户端 jar 要做「收到 → 存 → 下次发出」，两侧不闭合就会出现悄悄丢属性的 Cookie
     */
    TEST(HttpCookie, RoundTripsThroughRenderAndParse)
    {
        HttpCookie original("sid", "s3cr3t");
        original.setPath("/api");
        original.setDomain("example.com");
        original.setMaxAgeSeconds(-1);
        original.setExpiresAt(sampleMoment());
        original.setSecure();
        original.setHttpOnly();
        original.setSameSite(CookieSameSitePolicy::None);

        const std::optional<HttpCookie> parsed = HttpCookie::parseSetCookie(original.renderAsSetCookie());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->renderAsSetCookie(), original.renderAsSetCookie());
        ASSERT_TRUE(parsed->expiresAt().has_value());
        EXPECT_EQ(parsed->expiresAt()->time_since_epoch(), original.expiresAt()->time_since_epoch());
        ASSERT_TRUE(parsed->maxAgeSeconds().has_value());
        EXPECT_EQ(*parsed->maxAgeSeconds(), -1) << "负 Max-Age 是「删除」的合法表达，不能吞掉";
    }

    /**
     * @brief 首段畸形的 Set-Cookie 判不可解析，而不是造出一条无名 Cookie
     */
    TEST(HttpCookie, RejectsMalformedSetCookieFirstSegment)
    {
        EXPECT_FALSE(HttpCookie::parseSetCookie("just-a-name").has_value());
        EXPECT_FALSE(HttpCookie::parseSetCookie("=novalue").has_value());
        EXPECT_FALSE(HttpCookie::parseSetCookie("bad name=v").has_value());
        EXPECT_FALSE(HttpCookie::parseSetCookie("").has_value());
    }

    /**
     * @brief 请求侧 Cookie 头：按顺序取全部项，单项畸形只跳自己不跳整条头
     * @details 浏览器与代理拼出的 Cookie 头里混一个怪项是常见的（早年跨版本 Cookie 尤其如此），
     *          判整条失败会让其余会话信息一起读不到，症状是「登录态莫名掉了」。
     */
    TEST(HttpCookie, ParsesCookieHeaderAndSkipsMalformedItems)
    {
        const std::vector<HttpCookie> cookies = HttpCookie::parseCookieHeader("a=1; b=2; noequals; c=3; =4; d=5");

        ASSERT_EQ(cookies.size(), 4U) << "畸形项只该被跳过，其余四条都要在";
        EXPECT_EQ(cookies[0].name(), "a");
        EXPECT_EQ(cookies[1].name(), "b");
        EXPECT_EQ(cookies[2].name(), "c");
        EXPECT_EQ(cookies[3].name(), "d");
        EXPECT_FALSE(cookies[0].domain().has_value()) << "请求侧没有属性可言";
    }

    /**
     * @brief 接线检查：多条 Cookie 头合并成一项不落的列表
     */
    TEST(HttpCookie, RequestCookiesMergeEveryCookieHeader)
    {
        HttpRequest request;
        request.addHeader("cookie", "sid=1; theme=dark");
        request.addHeader("Cookie", "sid=2");

        const std::vector<HttpCookie> cookies = request.cookies();
        ASSERT_EQ(cookies.size(), 3U) << "只读了第一条 Cookie 头";
        EXPECT_EQ(cookies[0].name(), "sid");
        EXPECT_EQ(cookies[0].value(), "1");
        EXPECT_EQ(cookies[1].name(), "theme");
        EXPECT_EQ(cookies[2].name(), "sid");
        EXPECT_EQ(cookies[2].value(), "2") << "同名多条要按到达顺序都留着，覆盖式收拢会把会话信息抹掉";
    }

    /**
     * @brief 接线检查：响应侧 setCookie 走可重复头部通道，先设先发
     */
    TEST(HttpCookie, ResponseSetCookieAppendsInOrder)
    {
        HttpResponse response;
        response.setCookie(HttpCookie("first", "1"));
        HttpCookie second("second", "2");
        second.setHttpOnly();
        response.setCookie(second);

        const std::vector<std::string> values = response.headerValues("set-cookie");
        ASSERT_EQ(values.size(), 2U) << "两条 Set-Cookie 合并成一条，第二条的属性就把第一条顶掉了";
        EXPECT_EQ(values[0], "first=1");
        EXPECT_EQ(values[1], "second=2; HttpOnly");
    }
} // namespace AsynGyanis::Net
