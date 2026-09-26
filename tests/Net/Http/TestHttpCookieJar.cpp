// HttpCookieJar 单元测试：RFC 6265 的域/路径/Secure 匹配、替换与删除语义、有界存储与发送次序。
#include "Net/Http/Client/HttpCookieJar.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 用例里固定使用的「收到响应」时刻，免得断言依赖墙钟
        constexpr std::chrono::system_clock::time_point kReceivedAt{std::chrono::seconds{1'700'000'000}};

        /**
         * @brief 收一条 Set-Cookie 的简写
         * @param jar 目标罐子
         * @param setCookieValue Set-Cookie 头部取值
         * @param requestHost 请求主机
         * @param isSecureConnection 连接是否加密
         * @param requestPath 请求路径
         */
        void storeOne(HttpCookieJar &jar, const std::string_view setCookieValue, const std::string_view requestHost = "example.com", const bool isSecureConnection = false,
                      const std::string_view requestPath = "/")
        {
            jar.storeFromResponse(requestHost, isSecureConnection, requestPath, {std::string(setCookieValue)}, kReceivedAt);
        }

        /**
         * @brief 取发往某目标的 Cookie 头
         * @return std::optional<std::string> 没有可发的返回空
         */
        [[nodiscard]] std::optional<std::string> headerFor(const HttpCookieJar &jar, const std::string_view requestHost = "example.com", const bool isSecureConnection = false,
                                                           const std::string_view requestPath = "/")
        {
            return jar.buildRequestHeader(requestHost, isSecureConnection, requestPath);
        }
    } // namespace

    /**
     * @brief 收下就能发回同一个主机
     */
    TEST(HttpCookieJar, StoresAndSendsBackOnSameHost)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=42");

        ASSERT_TRUE(headerFor(jar).has_value());
        EXPECT_EQ(*headerFor(jar), "sid=42");
        EXPECT_EQ(jar.cookieCount(), 1U);
    }

    /**
     * @brief 名字里的 HttpOnly/Secure 等属性不影响回送——HttpOnly 只限制脚本读
     */
    TEST(HttpCookieJar, HttpOnlyAndSessionCookiesStillGetSent)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=1; HttpOnly; SameSite=Lax");

        ASSERT_TRUE(headerFor(jar).has_value());
        EXPECT_EQ(*headerFor(jar), "sid=1") << "发出去的 Cookie 头只带名字与取值";
    }

    /**
     * @brief 不收别的域的 Cookie
     * @details 这是把 Set-Cookie 变成跨站跟踪的第一道闸：对端指哪个域就存哪个域，
     *          下一次请求就会带着会话飞到别人那里
     */
    TEST(HttpCookieJar, RejectsDomainAttributeThatDoesNotCoverRequestHost)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=1; Domain=evil.com", "example.com");

        EXPECT_EQ(jar.cookieCount(), 0U) << "域属性罩不住请求主机时整条必须丢";
        EXPECT_FALSE(headerFor(jar).has_value());
    }

    /**
     * @brief 主域 Cookie 对子域生效，而 host-only 的不会漏到子域
     */
    TEST(HttpCookieJar, DistinguishesDomainScopeFromHostOnly)
    {
        HttpCookieJar subdomainWide;
        storeOne(subdomainWide, "sid=1; Domain=example.com", "shop.example.com");
        EXPECT_TRUE(headerFor(subdomainWide, "www.example.com").has_value()) << "主域 Cookie 应当覆盖子域";
        EXPECT_TRUE(headerFor(subdomainWide, "example.com").has_value());
        EXPECT_FALSE(headerFor(subdomainWide, "example.com.evil.net").has_value()) << "后缀里含着这串字符的域名不该被罩住";

        HttpCookieJar hostOnly;
        storeOne(hostOnly, "sid=2", "example.com");
        EXPECT_TRUE(headerFor(hostOnly, "example.com").has_value());
        EXPECT_FALSE(headerFor(hostOnly, "shop.example.com").has_value()) << "没有 Domain 属性就是 host-only，不该外扩一格";
    }

    /**
     * @brief IP 字面量不接受 Domain 属性（RFC 6265 §5.2.3）
     */
    TEST(HttpCookieJar, RejectsDomainAttributeForIpHosts)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=1; Domain=.local", "127.0.0.1");
        EXPECT_EQ(jar.cookieCount(), 0U);

        HttpCookieJar ipv6Jar;
        storeOne(ipv6Jar, "sid=1; Domain=example.com", "::1");
        EXPECT_EQ(ipv6Jar.cookieCount(), 0U) << "IPv6 字面量同样没有子域关系可言";

        EXPECT_TRUE(HttpCookieJar::isIpAddressLiteral("127.0.0.1"));
        EXPECT_TRUE(HttpCookieJar::isIpAddressLiteral("::1"));
        EXPECT_FALSE(HttpCookieJar::isIpAddressLiteral("localhost"));
        EXPECT_FALSE(HttpCookieJar::isIpAddressLiteral("v1.8.example.com")) << "带字母的点分名字不是 IP";
        EXPECT_FALSE(HttpCookieJar::isIpAddressLiteral("999.999.999.999")) << "段长超过三位或含非法字符按非 IP 处理，交给主机名规则";
    }

    /**
     * @brief Secure 的 Cookie：明文连接既不收回也不发出
     * @details 收侧就丢比「存着但不用」要紧——留着就多一次被误发的机会（重定向、改配置、复用罐子）
     */
    TEST(HttpCookieJar, SecureCookiesNeedEncryptedConnection)
    {
        HttpCookieJar plainJar;
        storeOne(plainJar, "sid=1; Secure", "example.com", false);
        EXPECT_EQ(plainJar.cookieCount(), 0U);

        HttpCookieJar secureJar;
        storeOne(secureJar, "sid=2; Secure", "example.com", true);
        EXPECT_FALSE(headerFor(secureJar, "example.com", false).has_value()) << "明文连接不该带上 Secure 的 Cookie";
        ASSERT_TRUE(headerFor(secureJar, "example.com", true).has_value());
        EXPECT_EQ(*headerFor(secureJar, "example.com", true), "sid=2");
    }

    /**
     * @brief 缺省路径按 RFC 6265 §5.1.4 推：单个斜杠是根，多个斜杠去掉最右一段
     */
    TEST(HttpCookieJar, DefaultPathFollowsTheDocumentedRemovalRule)
    {
        HttpCookieJar rootJar;
        storeOne(rootJar, "sid=1", "example.com", false, "/orders");
        EXPECT_TRUE(headerFor(rootJar, "example.com", false, "/anything").has_value()) << "只有一个斜杠时缺省路径是 /";

        HttpCookieJar nestedJar;
        storeOne(nestedJar, "sid=2", "example.com", false, "/api/v1/orders");
        EXPECT_TRUE(headerFor(nestedJar, "example.com", false, "/api/v1/x").has_value());
        EXPECT_TRUE(headerFor(nestedJar, "example.com", false, "/api/v1").has_value()) << "路径逐字相等也算命中";
        EXPECT_FALSE(headerFor(nestedJar, "example.com", false, "/api/v2").has_value());
        EXPECT_FALSE(headerFor(nestedJar, "example.com", false, "/api/v1extra").has_value()) << "前缀后不接斜杠就是另一个资源，不该被罩住";
    }

    /**
     * @brief 显式 Path 覆盖缺省推导
     */
    TEST(HttpCookieJar, ExplicitPathAttributeWinsOverDefault)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=1; Path=/admin", "example.com", false, "/login");

        EXPECT_FALSE(headerFor(jar, "example.com", false, "/login").has_value());
        EXPECT_TRUE(headerFor(jar, "example.com", false, "/admin/users").has_value());
    }

    /**
     * @brief Max-Age 为非正数即删除；已过 Expires 的时刻同样删
     */
    TEST(HttpCookieJar, DeletionSemanticsRemoveStoredCookie)
    {
        HttpCookieJar jar;
        storeOne(jar, "keep=v1");
        storeOne(jar, "sid=42; Max-Age=3600");
        ASSERT_EQ(jar.cookieCount(), 2U);

        storeOne(jar, "sid=x; Max-Age=0");
        EXPECT_EQ(*headerFor(jar), "keep=v1") << "Max-Age=0 要精确删掉同作用域那条，而不是全清";

        storeOne(jar, "keep=v2; Expires=Wed, 09 Jun 2021 10:18:14 GMT");
        EXPECT_FALSE(headerFor(jar).has_value()) << "过期时刻早于收到的时刻即删除";
    }

    /**
     * @brief 发送次序按 RFC 6265 §5.4：路径长的在前（长度相同才比插入顺序）
     */
    TEST(HttpCookieJar, OrdersLongerPathsFirst)
    {
        HttpCookieJar jar;
        storeOne(jar, "root=2", "example.com", false, "/");
        storeOne(jar, "deep=1; Path=/api/v1", "example.com", false, "/");
        storeOne(jar, "other=3; Path=/api", "example.com", false, "/");

        const std::optional<std::string> header = headerFor(jar, "example.com", false, "/api/v1/orders");
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(*header, "deep=1; other=3; root=2") << "三条都命中，按路径长度降序：" << *header;
    }

    /**
     * @brief 同名同域同路径即替换，不累积成两条
     */
    TEST(HttpCookieJar, ReplacesSameScopeInsteadOfAccumulating)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=first");
        storeOne(jar, "sid=second");

        EXPECT_EQ(jar.cookieCount(), 1U);
        EXPECT_EQ(*headerFor(jar), "sid=second");
    }

    /**
     * @brief 有界存储：单域上限先挤掉本站最旧的，总上限再兜住全局
     * @details 不设上限时，一个愿意一直回 Set-Cookie 的对端就能把客户端内存吃光
     */
    TEST(HttpCookieJar, CapsEvictOldestEntries)
    {
        HttpCookieJar::Limits limits;
        limits.maximumCookiesPerDomain = 3;
        limits.maximumTotalCookies     = 5;
        HttpCookieJar jar(limits);

        for (int index = 0; index < 6; ++index)
        {
            storeOne(jar, "c" + std::to_string(index) + "=v", "example.com", false, "/p" + std::to_string(index));
        }
        EXPECT_EQ(jar.cookieCount(), 3U) << "同一个域存到上限就该停住";
        const std::string header = headerFor(jar).value_or("");
        EXPECT_NE(header.find("c5=v"), std::string::npos) << "留下的应当是最新的几条：" << header;
        EXPECT_EQ(header.find("c0=v"), std::string::npos) << "最旧的那条应当先被淘汰：" << header;

        for (int index = 0; index < 4; ++index)
        {
            storeOne(jar, "d" + std::to_string(index) + "=v", "other.example.org", false, "/q" + std::to_string(index));
        }
        EXPECT_EQ(jar.cookieCount(), 5U) << "总量上限必须兜住跨域的情况";
    }

    /**
     * @brief 主机名大小写不影响匹配
     */
    TEST(HttpCookieJar, HostComparisonIsAsciiCaseInsensitive)
    {
        HttpCookieJar jar;
        storeOne(jar, "SID=1; Domain=EXAMPLE.COM", "Shop.Example.COM");

        EXPECT_EQ(jar.cookieCount(), 1U);
        EXPECT_EQ(headerFor(jar, "shop.example.com").value_or(""), "SID=1");
    }

    /**
     * @brief clear() 之后什么都不发
     */
    TEST(HttpCookieJar, ClearEmptiesTheJar)
    {
        HttpCookieJar jar;
        storeOne(jar, "sid=1");
        ASSERT_EQ(jar.cookieCount(), 1U);

        jar.clear();
        EXPECT_EQ(jar.cookieCount(), 0U);
        EXPECT_FALSE(headerFor(jar).has_value());
    }
} // namespace AsynGyanis::Net
