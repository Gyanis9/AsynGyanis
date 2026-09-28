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
#include <string_view>
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
     * @brief 名字字符集逐字节钉成 RFC 7230 的 tchar 全集：多一个少一个都算错
     * @details 这两个方向都会出错而且都不报错：分隔符被放行等于给响应留拆分口（空格让名字与属性段
     *          粘连、'"' 与 ';' 直接截断），而合法字符被误拒会让真实存在的 Cookie 名存不进来。
     *          逐字节过一遍比三五个样本强——进制边界（0x21、0x22、0x2C）就是这么被抓出来的。
     * @note 证伪：往实现里的特殊字符表加 ';'，本条红在「非 token 字符被放了进来」那句
     */
    TEST(HttpCookie, NameCharsetIsExactlyTheRfc7230TokenSet)
    {
        static constexpr std::string_view kTokenSpecials{"!#$%&'*+-.^_`|~"};
        std::string                       rejectedLegalNames;
        std::string                       acceptedIllegalNames;
        for (int code = 0; code < 0x80; ++code)
        {
            const char character     = static_cast<char>(code);
            const bool isTokenChar   = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                                       kTokenSpecials.find(character) != std::string_view::npos;
            const bool reportedValid = HttpCookie::isValidName(std::string(1, character));
            if (reportedValid != isTokenChar)
            {
                (isTokenChar ? rejectedLegalNames : acceptedIllegalNames) += character;
            }
        }
        EXPECT_TRUE(rejectedLegalNames.empty()) << "这些合法的 token 字符被拒了：" << rejectedLegalNames;
        EXPECT_TRUE(acceptedIllegalNames.empty()) << "这些非 token 字符被当成了合法名字（会破坏头部结构）：" << acceptedIllegalNames;

        EXPECT_FALSE(HttpCookie::isValidName("")) << "空名字必须拒：RFC 6265 要求名字至少一个字符";
        EXPECT_FALSE(HttpCookie::isValidName("名")) << "非 ASCII 的每一个字节都不算 token";
    }

    /**
     * @brief 取值字符集：把每个会破坏头部结构的字符挡在外面，并记下一处刻意的放宽
     * @details 必须挡的是 CR/LF（头部注入）、';'（截断属性段）、'"'（提前闭合 quoted-string）、
     *          ','（多值头部粘连）、空格（名字与取值粘连）、控制符、DEL 与 0x80 以上。
     *          '=' 放行是规范的：RFC 6265 §4.1.1 的 cookie-octet 覆盖 0x2D..0x3A，而两条解析入口都只按
     *          **第一个** '=' 切分，取值里再出现的 '=' 原样保留。
     * @note 反斜杠是一处刻意的放宽：RFC 的 cookie-octet 不含 0x5C，本层放行它——它不破坏任何头部结构，
     *       而挡下来只会把一条本来能用的 Cookie 变成存不进来（真实取值里确实会出现它）
     * @note 证伪：把 0x5C 排掉、或把任何一段范围放宽到含 ';'/'"/','/0x7F，本条红
     */
    TEST(HttpCookie, ValueCharsetBlocksEveryHeaderBreakingCharacter)
    {
        EXPECT_TRUE(HttpCookie::isValidValue("")) << "空取值合法（RFC 6265 允许）";

        for (int code = 0x20; code <= 0x7E; ++code)
        {
            // RFC 6265 §4.1.1 的 cookie-octet 四段，外加本层刻意放行的 0x5C
            const bool shouldBeAllowed = code == 0x21 || (code >= 0x23 && code <= 0x2B) || (code >= 0x2D && code <= 0x3A) || (code >= 0x3C && code <= 0x7E);
            EXPECT_EQ(HttpCookie::isValidValue(std::string(1, static_cast<char>(code))), shouldBeAllowed) << "可打印字节 0x" << std::hex << code;
        }
        for (int code = 0x7F; code < 0x100; code += 1)
        {
            EXPECT_FALSE(HttpCookie::isValidValue(std::string(1, static_cast<char>(code)))) << "0x7F 以上不算取值字符：0x" << std::hex << code;
        }

        // 报错文案承诺的那几个分隔符逐个点名，别让文案与判据各说一套
        for (const char breaker: {' ', '\t', '"', ',', ';', '\r', '\n', '\x01'})
        {
            EXPECT_FALSE(HttpCookie::isValidValue(std::string(1, breaker))) << "这个字符必须挡在取值外：" << static_cast<int>(breaker);
        }
        // 带结构的整串也要拒：一条含 CRLF 的取值就是响应拆分
        EXPECT_FALSE(HttpCookie::isValidValue("a\r\nSet-Cookie: evil=1"));
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

        // 取值里的 '=' 原样保留（只按第一个 '=' 切分），而中间带空格的那一条按 isValidValue 判不合格、
        // 只丢它自己：Cookie 头是各级代理拼出来的，混一条怪的不该让其余的读不到
        const std::vector<HttpCookie> mixed = HttpCookie::parseCookieHeader("tok=abc=def; sp=a b; q=1");
        ASSERT_EQ(mixed.size(), 2U) << "带空格的取值该只丢它自己，其余两条都要在";
        EXPECT_EQ(mixed[0].name(), "tok");
        EXPECT_EQ(mixed[0].value(), "abc=def") << "取值里后续的 '=' 被切掉了：那是按第一个 '=' 切分的语义";
        EXPECT_EQ(mixed[1].name(), "q");
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
