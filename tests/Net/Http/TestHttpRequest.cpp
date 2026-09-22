// HttpRequest 单元测试：方法映射、头部存储模型、URI 与正文的整块交接、路径与查询串解析、取消信号
#include "Net/Http/HttpRequest.h"

#include "Net/Http/HttpMethod.h"

#include <gtest/gtest.h>

#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 用查询串文本构造一条请求（percentDecode 是私有静态成员，只能经 queryParams() 观察）
         * @param queryText 不含前导 '?' 的查询串原文
         * @return HttpRequest URI 为 "/probe?<queryText>" 的请求对象
         */
        HttpRequest makeRequestWithQuery(const std::string_view queryText)
        {
            HttpRequest request;
            request.setMethod(HttpMethod::GET);
            request.setUri(std::string("/probe?") + std::string(queryText));
            return request;
        }
    } // namespace

    // ============================================================================
    // 方法映射与基本字段
    // ============================================================================

    TEST(HttpRequest, ConvertsRecognizedMethodNames)
    {
        EXPECT_EQ(HttpRequest::methodFromString("GET"), HttpMethod::GET);
        EXPECT_EQ(HttpRequest::methodFromString("POST"), HttpMethod::POST);
        EXPECT_EQ(HttpRequest::methodFromString("PUT"), HttpMethod::PUT);
        EXPECT_EQ(HttpRequest::methodFromString("DELETE"), HttpMethod::DELETE);
        EXPECT_EQ(HttpRequest::methodFromString("PATCH"), HttpMethod::PATCH);
        EXPECT_EQ(HttpRequest::methodFromString("HEAD"), HttpMethod::HEAD);
        EXPECT_EQ(HttpRequest::methodFromString("OPTIONS"), HttpMethod::OPTIONS);
    }

    TEST(HttpRequest, FoldsUnrecognizedMethodNamesToUnknown)
    {
        // 未收录方法与大小写不符的拼写一律 UNKNOWN：CONNECT / TRACE 这类方法要能被上层看见，而不是被误认成通配
        EXPECT_EQ(HttpRequest::methodFromString("CONNECT"), HttpMethod::UNKNOWN);
        EXPECT_EQ(HttpRequest::methodFromString("TRACE"), HttpMethod::UNKNOWN);
        EXPECT_EQ(HttpRequest::methodFromString("M-SEARCH"), HttpMethod::UNKNOWN);
        EXPECT_EQ(HttpRequest::methodFromString("get"), HttpMethod::UNKNOWN);
        EXPECT_EQ(HttpRequest::methodFromString(""), HttpMethod::UNKNOWN);
    }

    TEST(HttpRequest, StartsAsEmptyRequest)
    {
        const HttpRequest request;

        EXPECT_EQ(request.method(), HttpMethod::UNKNOWN);
        EXPECT_TRUE(request.uri().empty());
        EXPECT_TRUE(request.httpVersion().empty());
        EXPECT_TRUE(request.headers().empty());
        EXPECT_TRUE(request.body().empty());
        EXPECT_TRUE(request.path().empty());
        EXPECT_TRUE(request.queryParams().empty());
        EXPECT_FALSE(request.getHeader("host").has_value());
        EXPECT_FALSE(request.param("id").has_value());
    }

    TEST(HttpRequest, KeepsUriTextVerbatimIncludingQuery)
    {
        HttpRequest request;
        request.setUri("/user/42?tab=posts&flag");

        EXPECT_EQ(request.uri(), "/user/42?tab=posts&flag");
        // path() 只做裁剪，不做百分号解码：解码属于业务判断
        EXPECT_EQ(request.path(), "/user/42");
    }

    TEST(HttpRequest, StoresMethodAndVersionAsGiven)
    {
        HttpRequest request;
        request.setMethod(HttpMethod::DELETE);
        request.setHttpVersion("HTTP/1.0");

        EXPECT_EQ(request.method(), HttpMethod::DELETE);
        EXPECT_EQ(request.httpVersion(), "HTTP/1.0");
    }

    // ============================================================================
    // 头部存储模型
    // ============================================================================

    TEST(HttpRequest, NormalizesHeaderNameToLowercaseOnInsert)
    {
        HttpRequest request;
        request.addHeader("Content-Type", "application/json");

        EXPECT_EQ(request.getHeader("content-type").value_or(""), "application/json");
        EXPECT_EQ(request.getHeader("CONTENT-TYPE").value_or(""), "application/json");
        EXPECT_EQ(request.headers().count("content-type"), 1U);
        EXPECT_EQ(request.headers().count("Content-Type"), 0U);
    }

    TEST(HttpRequest, MergesRepeatedOrdinaryHeaderValuesWithComma)
    {
        HttpRequest request;
        request.addHeader("Accept-Encoding", "gzip");
        request.addHeader("accept-encoding", "br");

        // 单值视图按 RFC 7230 §3.2.2 的收件人规则合并，权威记录仍逐条留档
        EXPECT_EQ(request.getHeader("accept-encoding").value_or(""), "gzip, br");

        const std::vector<std::string> values = request.headerValues("Accept-Encoding");
        ASSERT_EQ(values.size(), 2U);
        EXPECT_EQ(values[0], "gzip");
        EXPECT_EQ(values[1], "br");
        EXPECT_EQ(request.headers().size(), 1U);
    }

    TEST(HttpRequest, KeepsRepeatedSetCookieValuesInArrivalOrder)
    {
        HttpRequest request;
        request.addHeader("Set-Cookie", "sid=1");
        request.addHeader("Set-Cookie", "theme=dark");

        const std::vector<std::string> values = request.headerValues("set-cookie");
        ASSERT_EQ(values.size(), 2U);
        EXPECT_EQ(values[0], "sid=1");
        EXPECT_EQ(values[1], "theme=dark");

        // 可重复头部不得被逗号合并：cookie 值本身可含逗号，合并后无法还原
        EXPECT_EQ(request.getHeader("set-cookie").value_or(""), "sid=1");
        EXPECT_EQ(request.headers().count("set-cookie_1"), 0U);
    }

    TEST(HttpRequest, ReturnsEmptyOptionalAndEmptyListForAbsentHeader)
    {
        HttpRequest request;
        request.addHeader("Host", "example.test");

        EXPECT_FALSE(request.getHeader("missing").has_value());
        EXPECT_TRUE(request.headerValues("missing").empty());
    }

    TEST(HttpRequest, ReplacesBodyOnSetAndAppendsChunksAfterwards)
    {
        HttpRequest request;
        request.setBody("first");
        request.setBody("second");
        static constexpr std::string_view kTail = "!tail";
        request.appendBody(kTail.data(), kTail.size());

        EXPECT_EQ(request.body(), "second!tail");
    }

    /**
     * @brief 交接 URI 与正文是两条缓冲整块交换，被覆盖的旧内容交给源而不是消失
     * @details 判据取「源侧接手到本请求原先那份内容且容量还在」：按值移动会把源清空、把目标的旧内容
     *          直接丢弃，从请求侧读结果两者一模一样，只有交换才在源侧留下可见的痕迹。
     *          稳态零分配由 TestHotPathAllocations 钉，这里钉的是它能成立的前提——源清零之前那些字节归源。
     */
    TEST(HttpRequest, AdoptSwapsWholeBuffersInsteadOfDiscardingTheOldOnes)
    {
        HttpRequest request;
        request.setUri("/first-with-a-long-enough-path");
        request.setBody(std::string(64, 'a'));

        std::string stagedUri  = "/second";
        std::string stagedBody = std::string(3, 'b');
        request.adoptStagedUri(stagedUri);
        request.adoptStagedBody(stagedBody);

        EXPECT_EQ(request.uri(), "/second");
        EXPECT_EQ(request.body(), "bbb");

        // 旧内容换到了源这一侧：复用路径上调用方必须把它清零，否则串进下一条报文
        EXPECT_EQ(stagedUri, "/first-with-a-long-enough-path");
        EXPECT_EQ(stagedBody, std::string(64, 'a'));
        stagedBody.clear();
        EXPECT_GE(stagedBody.capacity(), 64U) << "交换退化成移动时源侧接手不到这块缓冲，容量退回短串内联";

        request.reset();
        EXPECT_TRUE(request.uri().empty());
        EXPECT_TRUE(request.body().empty());
    }

    // ============================================================================
    // 路径与查询串
    // ============================================================================

    TEST(HttpRequest, SplitsPathAtFirstQuestionMark)
    {
        HttpRequest request;
        request.setUri("/a/b?c=1?d=2");

        EXPECT_EQ(request.path(), "/a/b");
    }

    TEST(HttpRequest, ReturnsWholeUriAsPathWhenQueryDelimiterAbsent)
    {
        HttpRequest request;
        request.setUri("/plain/path");

        EXPECT_EQ(request.path(), "/plain/path");
    }

    TEST(HttpRequest, ParsesSimpleQueryPairs)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("page=2&size=20").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("page"), "2");
        EXPECT_EQ(parameters.at("size"), "20");
    }

    TEST(HttpRequest, SplitsQueryOnAmpersandBeforeSplittingOnEquals)
    {
        // 先按 '&' 分键值对再找 '='：反过来会把 "a&b=c" 当成一个键
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("a&b=c").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("a"), "");
        EXPECT_EQ(parameters.at("b"), "c");
    }

    TEST(HttpRequest, TreatsPairWithoutEqualsAsKeyWithEmptyValue)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("flag&other=1").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("flag"), "");
        EXPECT_EQ(parameters.at("other"), "1");
    }

    TEST(HttpRequest, KeepsRemainingEqualsSignsInsideValue)
    {
        // 只按第一个 '=' 切分：值里的 '=' 是值的组成部分
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("q=a=b&sig=x==").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("q"), "a=b");
        EXPECT_EQ(parameters.at("sig"), "x==");
    }

    TEST(HttpRequest, LetsLaterDuplicateKeyOverrideEarlier)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("a=1&a=2").queryParams();

        ASSERT_EQ(parameters.size(), 1U);
        EXPECT_EQ(parameters.at("a"), "2");
    }

    TEST(HttpRequest, SkipsEmptyPairsAndPairsWithoutKey)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("&a=1&&=v&b=2&").queryParams();

        // 空分对与「只有值没有键」的分对都被丢弃，绝不允许冒出空键
        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("a"), "1");
        EXPECT_EQ(parameters.at("b"), "2");
        EXPECT_EQ(parameters.count(""), 0U);
    }

    TEST(HttpRequest, ReturnsEmptyMapWhenQueryIsAbsentOrEmpty)
    {
        HttpRequest withoutQuery;
        withoutQuery.setUri("/no/query");
        EXPECT_TRUE(withoutQuery.queryParams().empty());

        HttpRequest trailingDelimiter;
        trailingDelimiter.setUri("/no/query?");
        EXPECT_TRUE(trailingDelimiter.queryParams().empty());
    }

    TEST(HttpRequest, DecodesPercentEscapeAtEndOfValue)
    {
        // 越界判据必须按「剩余长度容得下一个完整序列」，否则以 %41 收尾的值会漏解
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("name=%41").queryParams();

        ASSERT_EQ(parameters.size(), 1U);
        EXPECT_EQ(parameters.at("name"), "A");
    }

    TEST(HttpRequest, DecodesPercentEscapesInKeysAsWellAsValues)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("%41=%42%3d%43").queryParams();

        ASSERT_EQ(parameters.size(), 1U);
        EXPECT_EQ(parameters.at("A"), "B=C");
    }

    TEST(HttpRequest, DecodesEscapedNulByteAndHighByteFromQueryValue)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("ctl=%00%e4%b8%ad").queryParams();

        ASSERT_EQ(parameters.size(), 1U);
        // 期望值：一个 NUL 字节加上「中」的 UTF-8 三字节，共 4 字节。用 string_view 定长构造避免数组退化
        static constexpr std::string_view kExpectedValue{"\0\xe4\xb8\xad", 4};
        EXPECT_EQ(parameters.at("ctl").length(), kExpectedValue.size());
        EXPECT_EQ(parameters.at("ctl"), std::string(kExpectedValue));
    }

    TEST(HttpRequest, KeepsTruncatedPercentSequenceVerbatim)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("v=%4&kept=1").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("v"), "%4");
        EXPECT_EQ(parameters.at("kept"), "1");
    }

    TEST(HttpRequest, KeepsDoublePercentSequenceVerbatim)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("v=%%&kept=1").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("v"), "%%");
        EXPECT_EQ(parameters.at("kept"), "1");
    }

    TEST(HttpRequest, KeepsNonHexadecimalPercentSequenceWithoutSwallowingCharacters)
    {
        // 非法序列既报错也不吞字符：'%4G' 三个字符按原文留下
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("v=%4G&kept=1").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("v"), "%4G");
        EXPECT_EQ(parameters.at("kept"), "1");
    }

    TEST(HttpRequest, DecodesPlusSignAsSpaceInQueryText)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("greeting=hello+world&a+b=1").queryParams();

        ASSERT_EQ(parameters.size(), 2U);
        EXPECT_EQ(parameters.at("greeting"), "hello world");
        EXPECT_EQ(parameters.at("a b"), "1");
    }

    TEST(HttpRequest, PreservesLiteralQuestionMarkInsideValue)
    {
        const std::unordered_map<std::string, std::string> parameters = makeRequestWithQuery("u=a?b").queryParams();

        ASSERT_EQ(parameters.size(), 1U);
        EXPECT_EQ(parameters.at("u"), "a?b");
    }

    // ============================================================================
    // 路由参数
    // ============================================================================

    TEST(HttpRequest, StoresRouteParamsAndOverwritesSameName)
    {
        HttpRequest request;
        request.setParam("id", "1");
        request.setParam("id", "2");
        request.setParam("slug", "hello");

        EXPECT_EQ(request.param("id").value_or(""), "2");
        EXPECT_EQ(request.param("slug").value_or(""), "hello");
    }

    TEST(HttpRequest, TreatsRouteParamNamesCaseSensitively)
    {
        HttpRequest request;
        request.setParam("id", "7");

        EXPECT_TRUE(request.param("id").has_value());
        EXPECT_FALSE(request.param("ID").has_value());
    }

    // ============================================================================
    // 取消信号与复位
    // ============================================================================

    TEST(HttpRequest, RequestsCancelOnlyOnce)
    {
        HttpRequest request;
        const std::stop_token token = request.cancelToken();

        EXPECT_FALSE(token.stop_requested());
        EXPECT_TRUE(request.requestCancel());
        EXPECT_TRUE(token.stop_requested());
        // 重复取消不再通知第二次
        EXPECT_FALSE(request.requestCancel());
    }

    TEST(HttpRequest, ExposesCancelSourceForExternalSignals)
    {
        HttpRequest request;

        request.cancelSource().request_stop();

        EXPECT_TRUE(request.cancelToken().stop_requested());
    }

    TEST(HttpRequest, ResetClearsEveryField)
    {
        HttpRequest request;
        request.setMethod(HttpMethod::POST);
        request.setUri("/old?keep=0");
        request.setHttpVersion("HTTP/1.1");
        request.addHeader("X-Old", "1");
        request.setBody("body");
        request.setParam("id", "9");

        request.reset();

        EXPECT_EQ(request.method(), HttpMethod::UNKNOWN);
        EXPECT_TRUE(request.uri().empty());
        EXPECT_TRUE(request.httpVersion().empty());
        EXPECT_TRUE(request.headers().empty());
        EXPECT_TRUE(request.body().empty());
        EXPECT_FALSE(request.param("id").has_value());
        EXPECT_FALSE(request.getHeader("x-old").has_value());
    }

    /**
     * @brief 单值视图按需重建：先查询、再追加同名头部，第二次查询必须看到合并后的结果
     *
     * @details 视图是**惰性**的（首次查询才由权威记录建出，见 rebuildSingleValueView），
     *          因此「查询 → 新增头部 → 再查询」这条时序必须把视图标脏并重建；
     *          漏掉标脏会让第二次查询拿到旧的合并结果，而这类错误只在特定调用顺序下出现。
     */
    TEST(HttpRequest, RebuildsSingleValueViewAfterLaterHeaderArrives)
    {
        HttpRequest request;
        request.addHeader("Cookie", "a=1");

        // 第一次查询：视图此刻才建出来
        EXPECT_EQ(request.getHeader("cookie").value_or(""), "a=1");
        EXPECT_EQ(request.headers().size(), 1U);

        request.addHeader("Cookie", "b=2");

        // 第二次查询：必须看到 ", " 合并后的结果，而不是第一次查询时的快照
        EXPECT_EQ(request.getHeader("cookie").value_or(""), "a=1, b=2");
        EXPECT_EQ(request.headers().at("cookie"), "a=1, b=2");
    }

    TEST(HttpRequest, FirstHeaderValueNormalizesTheKeyAndSkipsMerging)
    {
        HttpRequest request;
        request.addHeader("X-Request-Id", "trace-a");
        request.addHeader("x-request-id", "trace-b");

        // 包装层负责把名归一化（存储侧的 firstValue 只收小写名），调用方按线上写法传即可
        EXPECT_EQ(request.firstHeaderValue("X-Request-Id").value_or("<缺失>"), "trace-a");
        EXPECT_EQ(request.firstHeaderValue("x-request-id"), request.headerValues("X-Request-Id").front());
        EXPECT_FALSE(request.firstHeaderValue("X-Absent").has_value());
    }

    TEST(HttpRequest, HeaderValueTokenLooksAcrossRepeatedHeadersOfTheSameName)
    {
        HttpRequest request;
        // 对端把 Connection 拆成两条发（合法写法），token 判定必须跨条生效
        request.addHeader("Connection", "Keep-Alive");
        request.addHeader("connection", "Upgrade, TE");

        EXPECT_TRUE(request.hasHeaderValueToken("Connection", "keep-alive"));
        EXPECT_TRUE(request.hasHeaderValueToken("CONNECTION", "upgrade"));
        EXPECT_TRUE(request.hasHeaderValueToken("connection", "te"));
        EXPECT_FALSE(request.hasHeaderValueToken("connection", "close"));
    }

    TEST(HttpRequest, ResetHandsOverFreshCancelSource)
    {
        HttpRequest request;
        request.requestCancel();
        ASSERT_TRUE(request.cancelToken().stop_requested());

        request.reset();

        // 复用连接时下一条请求必须拿到未被取消的令牌
        EXPECT_FALSE(request.cancelToken().stop_requested());
        EXPECT_TRUE(request.requestCancel());
    }

    TEST(HttpRequest, ExposesPresenceAndZeroCopyViewsOfHeaderValues)
    {
        HttpRequest request;
        request.addHeader("Accept-Encoding", "gzip, deflate, br");
        request.addHeader("x-empty", "");

        // 存在性与视图都大小写不敏感，且与 owning 版同口径：会话与中间件改走这两条入口之后，
        // 口径不一致会让「同一份请求」在两条路上读出不同结果
        EXPECT_TRUE(request.hasHeader("accept-encoding"));
        EXPECT_TRUE(request.hasHeader("ACCEPT-ENCODING"));
        EXPECT_FALSE(request.hasHeader("cookie"));

        const auto encoding = request.firstHeaderValueView("accept-encoding");
        ASSERT_TRUE(encoding.has_value());
        EXPECT_EQ(*encoding, "gzip, deflate, br");
        EXPECT_EQ(request.firstHeaderValue("Accept-Encoding").value_or("<缺失>"), *encoding)
                << "两版取到的必须是同一个首值";

        // 空取值算「存在」：写 `Content-Length:` 与压根不写这条头部不是同一回事
        EXPECT_TRUE(request.hasHeader("x-empty"));
        const auto emptyValue = request.firstHeaderValueView("x-empty");
        ASSERT_TRUE(emptyValue.has_value());
        EXPECT_TRUE(emptyValue->empty());

        request.reset();
        EXPECT_FALSE(request.hasHeader("accept-encoding")) << "复用连接后不得看到上一条请求的头部";
        EXPECT_FALSE(request.firstHeaderValueView("accept-encoding").has_value());
    }
} // namespace AsynGyanis::Net
