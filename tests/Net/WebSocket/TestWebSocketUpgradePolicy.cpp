// 本文件直测 WebSocket 升级握手的两个公开判据：Origin 准入与子协议协商。
// 期望值全部手算写死，不调用被测实现里的任何归一化函数——同源判断的口径本身就是一条断言，
// 拿实现算期望等于把实现钉成「对」。
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace
{
    using AsynGyanis::Net::WebSocketOriginMode;
    using AsynGyanis::Net::WebSocketSubprotocolMismatch;
    using AsynGyanis::Net::WebSocketUpgradePolicy;

    /// 本连接自己的源：一台跑在 TLS 上的 8443 端口服务
    constexpr std::string_view kSameOrigin = "https://example.com:8443";

    WebSocketUpgradePolicy makeSameOriginOnlyPolicy()
    {
        WebSocketUpgradePolicy policy;
        policy.originMode = WebSocketOriginMode::SameOriginOnly;
        return policy;
    }
} // namespace

/**
 * @brief 钉住同源那一档：写法不同但指的是同一个源的要放行，源不同的必须拒
 * @details 同源是 (scheme, host, port) 三元组全等（RFC 6454 §5），端口缺省时补该协议的默认端口
 *          （§2.3 第 6 步）。这里每一格都手算了三元组：`http://example.com:80` 与
 *          `http://example.com` 同一个源，而 `https://example.com:80` 与 `https://example.com` 不是。
 */
TEST(WebSocketUpgradePolicy, SameOriginOnlyAcceptsIdenticalTripleAndRejectsOthers)
{
    const WebSocketUpgradePolicy policy = makeSameOriginOnlyPolicy();

    struct OriginCase
    {
        std::string_view origin;     ///< 对端发来的 Origin 原文
        std::string_view sameOrigin; ///< 本连接的源
        bool             isExpectedAllowed;
    };

    const std::vector<OriginCase> kCases{
            {"https://example.com:8443", kSameOrigin, true},          ///< 逐字相同
            {"HTTPS://EXAMPLE.COM:8443", kSameOrigin, true},          ///< scheme 与 host 的大小写不参与比较
            {"https://example.com:08443", kSameOrigin, true},         ///< 前导零是同一个端口的另一种写法
            {"https://example.com.:8443", kSameOrigin, true},         ///< 结尾根点不参与（与选站同一份归一化）
            {"https://example.com:8444", kSameOrigin, false},         ///< 端口不同就是另一个源
            {"http://example.com:8443", kSameOrigin, false},          ///< 协议不同就是另一个源
            {"https://other.com:8443", kSameOrigin, false},           ///< 主机不同就是另一个源
            {"http://example.com", "http://example.com:80", true},    ///< 缺省端口补成 80：同一个源
            {"http://example.com:80", "http://example.com", true},    ///< 反方向同理
            {"https://example.com", "https://example.com:443", true}, ///< https 的默认端口是 443
            {"https://example.com:80", "https://example.com", false}, ///< 显式 80 不是 https 的默认端口
            {"https://[::1]:8443", "https://[::1]:8443", true},       ///< IPv6 字面量带方括号
            {"https://[::1]:8443", "https://::1:8443", false},        ///< 没打方括号的 IPv6 读不出主机与端口
    };

    for (const OriginCase &testCase: kCases)
    {
        EXPECT_EQ(AsynGyanis::Net::isWebSocketOriginAllowed(testCase.origin, testCase.sameOrigin, policy), testCase.isExpectedAllowed)
                << "Origin=" << testCase.origin << " 本连接=" << testCase.sameOrigin << " 期望=" << (testCase.isExpectedAllowed ? "放行" : "拒绝");
    }
}

/**
 * @brief 钉住 Origin 缺席按放行、形状不合按拒绝，以及拒绝文案带着可操作的替代做法
 * @details RFC 6455 §4.1 第 8 条把 Origin 必发的义务只派给浏览器客户端，非浏览器客户端「MAY include」；
 *          把它判死会连自家 WebSocketClient 与 Autobahn 一起挡掉。形状读不出来时不猜——猜就是按
 *          本端的假设替对端改写了一个它没发出来的源。
 */
TEST(WebSocketUpgradePolicy, AbsentOriginIsAllowedWhileMalformedOriginIsRejected)
{
    const WebSocketUpgradePolicy policy = makeSameOriginOnlyPolicy();

    // 缺席放行：这一格是「非浏览器客户端」的形状，不是漏洞
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("", kSameOrigin, policy));
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("   ", kSameOrigin, policy));

    // 无从判定本连接同源时判否：判据拿不准的一侧宁可拒
    EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginAllowed("https://example.com:8443", "", policy));

    for (const std::string_view malformed: {std::string_view("https://example.com/path"), std::string_view("https://example.com?q=1"), std::string_view("example.com:8443"),
                                            std::string_view("ftp://example.com"), std::string_view("https://"), std::string_view("https://example.com:notaport")})
    {
        EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginAllowed(malformed, kSameOrigin, policy)) << "非法形状本该拒：" << malformed;
        EXPECT_FALSE(AsynGyanis::Net::describeWebSocketOriginRejection(malformed, kSameOrigin, policy).empty()) << "拒了却没给出原因：" << malformed;
    }

    // 跨源被拒时，文案要同时说出「哪一格不符」与「怎么放开」——只报「origin 不合法」的文案帮不上现场
    const std::string rejection = AsynGyanis::Net::describeWebSocketOriginRejection("https://evil.example", kSameOrigin, policy);
    EXPECT_NE(rejection.find("https://evil.example"), std::string::npos) << rejection;
    EXPECT_NE(rejection.find("AllowList"), std::string::npos) << rejection;
    // 放行时交回空串：调用方据此区分「没有拒绝」与「拒绝理由是空」
    EXPECT_TRUE(AsynGyanis::Net::describeWebSocketOriginRejection(kSameOrigin, kSameOrigin, policy).empty());
}

/**
 * @brief 钉住 AllowList 与 AllowAny 两档：显式列出才放行，显式选 AllowAny 才全放
 */
TEST(WebSocketUpgradePolicy, AllowListAndAllowAnyBehaveAsConfigured)
{
    WebSocketUpgradePolicy listPolicy;
    listPolicy.originMode     = WebSocketOriginMode::AllowList;
    listPolicy.allowedOrigins = {"https://admin.example:8443", "http://localhost"};

    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("https://admin.example:8443", kSameOrigin, listPolicy));
    // 表项里写的是缺省端口，对端显式带出 80 也算同一个源
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("http://localhost:80", kSameOrigin, listPolicy));
    // 同源仍然放行（放行表是「额外」而不是「取而代之」：漏了这一格会把自家页面的请求打成 403）
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed(kSameOrigin, kSameOrigin, listPolicy));
    // 表里没有、又不是同源：拒
    EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginAllowed("https://other.example:8443", kSameOrigin, listPolicy));

    const WebSocketUpgradePolicy anyPolicy = []
    {
        WebSocketUpgradePolicy policy;
        policy.originMode = WebSocketOriginMode::AllowAny;
        return policy;
    }();
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("https://anywhere.example", "", anyPolicy));
    // AllowAny 也不豁免形状检查以外的东西：它判的就是「不校验来源」，跨源照样过
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginAllowed("https://evil.example", kSameOrigin, anyPolicy));
}

/**
 * @brief 钉住本连接源文本的拼法：协议名由传输层决定，不由对端自报
 * @details 拿 `:scheme` 或请求行里的版本拼同源，等于让对端说自己是哪个源；这里只认会话给来的
 *          「这条连接是否跑在 TLS 上」。
 */
TEST(WebSocketUpgradePolicy, SameOriginTextComesFromTransportAndAuthority)
{
    EXPECT_EQ(AsynGyanis::Net::makeWebSocketSameOrigin(true, "example.com:8443"), "https://example.com:8443");
    EXPECT_EQ(AsynGyanis::Net::makeWebSocketSameOrigin(false, "example.com"), "http://example.com");
    // 权威段两侧 OWS 不算内容
    EXPECT_EQ(AsynGyanis::Net::makeWebSocketSameOrigin(true, " example.com "), "https://example.com");
    // 拿不到权威段时交回空串，调用方据此知道「本连接无从判定同源」
    EXPECT_TRUE(AsynGyanis::Net::makeWebSocketSameOrigin(true, "").empty());
}

/**
 * @brief 钉住子协议协商按对端次序取第一个本端有的，且比较逐字进行
 * @details RFC 6455 §4.1 第 10 条把对端的列表写成「ordered by preference」，所以本端的候选表顺序
 *          不构成优先级；反过来，把 `chat` 与 `Chat` 当同一个名字会回显一条对端没提过的写法，
 *          而那正是客户端必须判失败的形状（§4.2.2 步骤 6）。
 */
TEST(WebSocketUpgradePolicy, SubprotocolSelectionFollowsClientOrderAndIsCaseSensitive)
{
    WebSocketUpgradePolicy policy;
    // 本端候选表把 superchat 排在前面——它不构成优先级，要按对端次序挑
    policy.subprotocolCandidates = {"superchat", "chat"};

    EXPECT_EQ(AsynGyanis::Net::selectWebSocketSubprotocol("chat, superchat", policy).value_or("<无>"), "chat");
    EXPECT_EQ(AsynGyanis::Net::selectWebSocketSubprotocol("unknown, superchat", policy).value_or("<无>"), "superchat");
    // 段首尾 OWS 不算内容
    EXPECT_EQ(AsynGyanis::Net::selectWebSocketSubprotocol("  superchat ", policy).value_or("<无>"), "superchat");
    // 大小写不同是两个名字
    EXPECT_FALSE(AsynGyanis::Net::selectWebSocketSubprotocol("Chat", policy).has_value());
    // 前缀匹配不算命中
    EXPECT_FALSE(AsynGyanis::Net::selectWebSocketSubprotocol("chatv2", policy).has_value());
    // 一条都没谈成
    EXPECT_FALSE(AsynGyanis::Net::selectWebSocketSubprotocol("a, b", policy).has_value());
    // 候选表为空 = 不参与协商，对端发了也不回显
    const WebSocketUpgradePolicy noCandidates = makeSameOriginOnlyPolicy();
    EXPECT_FALSE(AsynGyanis::Net::selectWebSocketSubprotocol("chat", noCandidates).has_value());
    // 对端没发这条头部
    EXPECT_FALSE(AsynGyanis::Net::selectWebSocketSubprotocol("", policy).has_value());
}

/**
 * @brief 钉住三通道共用的那份判定结论：谁先拒、给哪个码、回显哪一个
 * @details 判定顺序是有意的：来源不可接受（403）要先于子协议谈不成（400）。反过来会让对端从状态码
 *          差异里试探出「哪些源其实被放行」，那是白送的一格信息。
 */
TEST(WebSocketUpgradePolicy, DecisionPicksRejectionOrderAndEchoValue)
{
    WebSocketUpgradePolicy policy;
    policy.originMode            = WebSocketOriginMode::SameOriginOnly;
    policy.subprotocolCandidates = {"superchat", "chat"};

    // 同源 + 谈得成：放行，回显按对端次序挑中的那一个
    const auto accepted = AsynGyanis::Net::evaluateWebSocketUpgrade(kSameOrigin, "chat, superchat", kSameOrigin, policy);
    EXPECT_TRUE(accepted.isAccepted);
    ASSERT_TRUE(accepted.selectedSubprotocol.has_value());
    EXPECT_EQ(*accepted.selectedSubprotocol, "chat");
    EXPECT_TRUE(accepted.rejectionReason.empty());
    EXPECT_EQ(accepted.rejectionStatus, 0);

    // 同源 + 一条都没谈成 + 默认档：仍然放行，但不回显
    const auto proceed = AsynGyanis::Net::evaluateWebSocketUpgrade(kSameOrigin, "nope", kSameOrigin, policy);
    EXPECT_TRUE(proceed.isAccepted);
    EXPECT_FALSE(proceed.selectedSubprotocol.has_value());

    // 同一份输入换成「谈不成即拒」：400，原因里点得出对端的名字
    policy.subprotocolMismatch  = WebSocketSubprotocolMismatch::RejectHandshake;
    const auto rejectedProtocol = AsynGyanis::Net::evaluateWebSocketUpgrade(kSameOrigin, "nope", kSameOrigin, policy);
    EXPECT_FALSE(rejectedProtocol.isAccepted);
    EXPECT_EQ(rejectedProtocol.rejectionStatus, 400);
    EXPECT_NE(rejectedProtocol.rejectionReason.find("nope"), std::string::npos) << rejectedProtocol.rejectionReason;

    // 跨源优先于子协议：两个都违时只能看到 403，不能通过对端名字反推源的结论
    const auto rejectedOrigin = AsynGyanis::Net::evaluateWebSocketUpgrade("https://evil.example", "nope", kSameOrigin, policy);
    EXPECT_FALSE(rejectedOrigin.isAccepted);
    EXPECT_EQ(rejectedOrigin.rejectionStatus, 403) << "来源不对时必须先按 403 收口";
    EXPECT_NE(rejectedOrigin.rejectionReason.find("Origin"), std::string::npos) << rejectedOrigin.rejectionReason;
    EXPECT_FALSE(rejectedOrigin.selectedSubprotocol.has_value()) << "被拒的握手里不该已经选好子协议";

    // 不参与协商（候选表空）时，任何 Origin 合法的请求都放行且不回显
    WebSocketUpgradePolicy noCandidates = policy;
    noCandidates.subprotocolCandidates.clear();
    noCandidates.subprotocolMismatch = WebSocketSubprotocolMismatch::RejectHandshake;
    const auto withoutNegotiation    = AsynGyanis::Net::evaluateWebSocketUpgrade(kSameOrigin, "chat", kSameOrigin, noCandidates);
    EXPECT_TRUE(withoutNegotiation.isAccepted) << "没登记候选表时不该因为「谈不成」拒掉请求";
    EXPECT_FALSE(withoutNegotiation.selectedSubprotocol.has_value());
}

/**
 * @brief 钉住登记处的形状校验：写歪的表项当场说清楚，而不是等到握手时悄悄落空
 */
TEST(WebSocketUpgradePolicy, OriginTextValidityIsCheckableAtRegistrationTime)
{
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginTextValid("https://example.com:8443"));
    EXPECT_TRUE(AsynGyanis::Net::isWebSocketOriginTextValid("http://localhost"));
    EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginTextValid("https://example.com/path"));
    EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginTextValid("example.com"));
    EXPECT_FALSE(AsynGyanis::Net::isWebSocketOriginTextValid(""));
}
