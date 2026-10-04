// HttpCookieJar 单元测试：RFC 6265 的域/路径/Secure 匹配、替换与删除语义、有界存储与发送次序。
#include "Net/Http/Client/HttpCookieJar.h"
#include "Net/Http/HttpCookie.h"

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
     * @brief `__Host-` 的三条硬要求逐格判：Secure、不得带 Domain、Path 要明确写成 `/`
     *
     * @details 前缀是写进名字的授权声明（RFC 6265bis §4.1.2.6）：站点用它替代「自己去查这条 Cookie
     *          从哪来」，因为浏览器会把不合格式的整条丢掉。罐子不判这一格，收下的正是那种浏览器
     *          会丢的形状——带 `Domain` 的一条还会按域匹配发往同主域下的每个兄弟子域。
     *          四格都走加密连接，好让「不收」只可能来自前缀判据而不是 Secure 那条通用规则
     */
    TEST(HttpCookieJar, RejectsHostPrefixedCookieMissingSecure)
    {
        HttpCookieJar jar;
        storeOne(jar, "__Host-sid=1; Path=/", "example.com", true);
        EXPECT_EQ(jar.cookieCount(), 0U) << "没带 Secure 的 __Host- 不收";
    }

    TEST(HttpCookieJar, RejectsHostPrefixedCookieWithDomainAttribute)
    {
        HttpCookieJar jar;
        storeOne(jar, "__Host-sid=1; Secure; Path=/; Domain=example.com", "shop.example.com", true);
        EXPECT_EQ(jar.cookieCount(), 0U) << "带 Domain 的 __Host- 正是这个前缀明令不许的那一型";
        EXPECT_FALSE(headerFor(jar, "api.example.com", true).has_value()) << "它更不能被发给兄弟子域";
    }

    TEST(HttpCookieJar, RejectsHostPrefixedCookieWithNonRootPath)
    {
        HttpCookieJar jar;
        storeOne(jar, "__Host-sid=1; Secure; Path=/admin", "example.com", true);
        EXPECT_EQ(jar.cookieCount(), 0U) << "Path 必须是 /";

        HttpCookieJar implicitPathJar;
        storeOne(implicitPathJar, "__Host-sid=1; Secure", "example.com", true);
        EXPECT_EQ(implicitPathJar.cookieCount(), 0U) << "由请求路径推出来的根路径不算「明确写了 Path=/」——规范只看属性文本";
    }

    /**
     * @brief 合规格的 `__Host-` 照收，并且仍是 host-only
     * @details 收紧判据的反向一格：闸门不能宽到把合规的也拒了。它只回到种下自己的那台主机，
     *          主域与兄弟子域都拿不到——这正是这个前缀买到的东西
     */
    TEST(HttpCookieJar, AcceptsWellFormedHostPrefixedCookieAndKeepsItHostOnly)
    {
        HttpCookieJar jar;
        storeOne(jar, "__Host-sid=1; Secure; Path=/", "shop.example.com", true);
        ASSERT_EQ(jar.cookieCount(), 1U) << "合规格的 __Host- 必须收";

        EXPECT_FALSE(headerFor(jar, "example.com", true).has_value()) << "host-only 的 __Host- 不该发给主域";
        EXPECT_FALSE(headerFor(jar, "api.example.com", true).has_value()) << "也不该发给兄弟子域";
        ASSERT_TRUE(headerFor(jar, "shop.example.com", true).has_value());
        EXPECT_EQ(*headerFor(jar, "shop.example.com", true), "__Host-sid=1");
    }

    /**
     * @brief `__Secure-` 只多要一条 Secure
     */
    TEST(HttpCookieJar, SecurePrefixedCookieNeedsTheSecureAttribute)
    {
        HttpCookieJar withoutAttribute;
        storeOne(withoutAttribute, "__Secure-sid=1", "example.com", true);
        EXPECT_EQ(withoutAttribute.cookieCount(), 0U) << "名字说 __Secure- 而属性没写 Secure，浏览器会整条丢掉";

        HttpCookieJar withAttribute;
        storeOne(withAttribute, "__Secure-sid=2; Secure", "example.com", true);
        EXPECT_EQ(withAttribute.cookieCount(), 1U) << "同前缀而带 Secure 的照收";
    }

    /**
     * @brief 前缀的比较大小写敏感：Cookie 名字本就区分大小写
     */
    TEST(HttpCookieJar, PrefixComparisonIsCaseSensitiveLikeCookieNames)
    {
        HttpCookieJar jar;
        storeOne(jar, "__host-sid=1", "example.com", true);
        EXPECT_EQ(jar.cookieCount(), 1U) << "小写的 __host- 不是那个前缀，不该被当成不合规而丢掉";
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
    /**
     * @brief 钉住：`Domain=com` 这种单标签域一律不收
     * @details 语法上它罩得住 `example.com`，收下就等于把这条 Cookie 发给 `.com` 下的每一个站点。
     *          规范给的答案是公共后缀表，本框架不内置那份数据，因此用「必须含点」这条保守判据：
     *          它挡得住最恶性的一类，代价是 `co.uk` 这类两段公共后缀仍收得下。
     */
    TEST(HttpCookieJar, RejectsSingleLabelCookieDomain)
    {
        HttpCookieJar jar;

        storeOne(jar, "a=1; Domain=com");
        EXPECT_EQ(jar.cookieCount(), 0U) << "单标签域被收下，等于让这条 Cookie 跟着发往 .com 下所有站点";

        storeOne(jar, "b=2; Domain=example.com");
        EXPECT_EQ(jar.cookieCount(), 1U) << "正常的两段域要照常收下，别把判据做过头";
    }

    /**
     * @brief 钉住：解析侧的 Domain 字符集与写侧同一条判据
     * @details 此前解析侧只挡空格、控制符与 `;` `"`，`=` 与 `/` 能进罐子——而 `setDomain` 从不接受
     *          它们，罐子里因此留着本框架永远写不出来的形态，还继续参与域名匹配。
     *          合法的处理是「当这条属性没给」（RFC 6265 §5.2.3），即按主机独占收下而不是整条丢掉。
     */
    TEST(HttpCookieJar, RejectsCookieDomainWithReservedCharacters)
    {
        HttpCookieJar jar;

        storeOne(jar, R"(a=1; Domain=exa/mple.com)");
        ASSERT_EQ(jar.cookieCount(), 1U) << "无效域名的处置是忽略该属性，Cookie 本身仍按主机独占收下";
        EXPECT_FALSE(HttpCookie::parseSetCookie("a=1; Domain=ex=a").value().domain().has_value()) << "含 = 的域名不该被当作域名";

        // 主机独占的条目不跟着子域发：这正是「无效域名被放宽成子域通配」会造成外泄的那条路
        EXPECT_FALSE(headerFor(jar, "sub.example.com").has_value()) << "被忽略的 Domain 属性不能变成子域可发送";
        EXPECT_TRUE(headerFor(jar, "example.com").has_value());
    }

    /**
     * @brief 钉住：对端给的 Max-Age 大得换算不过来时按上限收，而不是溢出成「过去」
     * @details delta-seconds 是对端可控的 int64，而 system_clock 的周期是秒的十亿/百亿分之一：
     *          「一千年」这种写在真实站点出现过的取值，换算之后就已经越过 int64 上界。
     *          有符号溢出是 UB，而这里的落法是**到期时刻跑到过去**——刚存进去的 Cookie 当场被当成
     *          过期摘掉，症状是登录态莫名其妙存不住，且只在特定时钟取值下复现。
     *          判据用「存进去之后还在账上、且能被发送路径读出」这条正向断言：溢出成过去的实现
     *          会当场把它摘掉。
     */
    TEST(HttpCookieJar, SaturatesAbsurdMaxAgeInsteadOfOverflowingToThePast)
    {
        HttpCookieJar jar;

        // 「一千年」：真实世界写过的取值，换算 tick 已经溢出
        storeOne(jar, "old=1; Max-Age=31536000000");
        ASSERT_EQ(jar.cookieCount(), 1U) << "一千年的寿命被溢出成了过去";
        ASSERT_TRUE(headerFor(jar).has_value()) << "溢出后的到期时刻落在过去，发送路径把它过滤掉了";
        EXPECT_NE(headerFor(jar)->find("old=1"), std::string::npos);

        // int64 上界：最坏的一档同样要落在「永不过期」而不是 UB
        HttpCookieJar worstCase;
        storeOne(worstCase, "big=2; Max-Age=9223372036854775807");
        ASSERT_EQ(worstCase.cookieCount(), 1U) << "int64 上界的 Max-Age 溢出成了过去";
        EXPECT_TRUE(headerFor(worstCase).has_value());

        // 对照：负值是删除语义，不该被这套饱和逻辑接走
        HttpCookieJar deleted;
        storeOne(deleted, "gone=3; Max-Age=-1");
        EXPECT_FALSE(headerFor(deleted).has_value()) << "Max-Age=-1 是「立即过期」，不是永不过期";
    }

    /**
     * @brief 钉住：存入时真正摘掉已过期的条目
     * @details 发送路径只**过滤**已过期的（那是 const 查询，不能因有人来查就改账），此前没有任何
     *          地方真正删除它们。僵尸条目一直占着 `maximumTotalCookies` 与单域配额，一个爱发短命
     *          Cookie 的站点因此能把真正要用的会话 Cookie 挤出去——表现为静默登录失效。
     */
    TEST(HttpCookieJar, PrunesExpiredEntriesOnStore)
    {
        HttpCookieJar jar;

        storeOne(jar, "early=1; Max-Age=600");
        ASSERT_EQ(jar.cookieCount(), 1U) << "刚存入的未过期条目应在账上";

        // 换到一小时后再存一条：早先那条此刻已过期，应当被真正摘掉。
        // 新那条给十年而不是「一千年」：寿命长到能活过发送路径按真实时钟的过滤，
        // 又留在本时钟可表示的范围内（超出范围的形态由下面那条饱和用例专门钉）
        jar.storeFromResponse("example.com", false, "/", {"late=2; Max-Age=315360000"}, kReceivedAt + std::chrono::hours{1});
        EXPECT_EQ(jar.cookieCount(), 1U) << "过期条目没被摘掉，会继续占着 maximumTotalCookies 与单域配额";
        const std::optional<std::string> header = headerFor(jar);
        ASSERT_TRUE(header.has_value());
        EXPECT_NE(header->find("late=2"), std::string::npos);
        EXPECT_EQ(header->find("early=1"), std::string::npos);
    }

} // namespace AsynGyanis::Net
