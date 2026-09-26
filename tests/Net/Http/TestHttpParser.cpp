// HttpParser 单元测试：增量解析、报文定界、头部存储模型与资源上限
#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include "Net/Http/HttpHeaderRules.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        // HttpParser 的上限常量是私有成员，测试侧按同一口径复述一份数值：
        // 这些数字本身就是对外的 DoS 防护契约，实现改动必须让测试一起失败
        constexpr std::size_t kUriLengthLimitInBytes   = 8 * 1024;  ///< 请求 URI 上限
        constexpr std::size_t kHeaderNameLimitInBytes  = 256;       ///< 单个头部名上限
        constexpr std::size_t kHeaderValueLimitInBytes = 8 * 1024;  ///< 单个头部值上限
        constexpr std::size_t kHeaderCountLimit        = 100;       ///< 头部条数上限
        constexpr std::size_t kHeaderBlockLimitInBytes = 64 * 1024; ///< 头部块总长上限

        /// 上限探测用的「方法原文 + 期望枚举」配对
        struct MethodProbe
        {
            std::string_view methodText;     ///< 请求行里的方法原文
            HttpMethod       expectedMethod; ///< 期望映射到的枚举值
        };

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 用给定的头部行拼出一条完整 GET 报文（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeRequestTextWithHeaders;

        /**
         * @brief 生成指定条数的头部行，名与值各自都控制在上限之内
         * @param headerCount 需要的头部条数
         * @param valueLength 每条头部的值长度
         * @return std::vector<std::string> 头部行列表，名字互不相同以免在单值视图里合并
         */
        std::vector<std::string> makeHeaderLines(const std::size_t headerCount, const std::size_t valueLength)
        {
            std::vector<std::string> headerLines;
            headerLines.reserve(headerCount);
            for (std::size_t index = 0; index < headerCount; ++index)
            {
                headerLines.push_back("x-count-" + std::to_string(index) + ": " + std::string(valueLength, 'v'));
            }
            return headerLines;
        }
    } // namespace

    // ============================================================================
    // 报文结构与增量语义
    // ============================================================================

    TEST(HttpParser, ParsesRequestLineAndHeaders)
    {
        HttpParser parser;

        const std::string message = "GET /index.html HTTP/1.1\r\nHost: example.test\r\nAccept: */*\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        const HttpRequest &request = parser.request();
        EXPECT_EQ(request.method(), HttpMethod::GET);
        EXPECT_EQ(request.uri(), "/index.html");
        EXPECT_EQ(request.httpVersion(), "HTTP/1.1");
        EXPECT_EQ(request.getHeader("host").value_or(""), "example.test");
        EXPECT_TRUE(request.body().empty());
        EXPECT_FALSE(parser.hasError());
        EXPECT_TRUE(parser.errorMessage().empty());
    }

    TEST(HttpParser, ParsesBodyDelimitedByContentLength)
    {
        HttpParser parser;

        const std::string message = "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_EQ(parser.request().method(), HttpMethod::POST);
        EXPECT_EQ(parser.request().body(), "hello");
    }

    TEST(HttpParser, AsksForMoreDataUntilWholeMessageArrives)
    {
        HttpParser parser;

        // 按任意字节边界切开喂入：行中间、CRLF 中间、正文中间都要能续上
        static constexpr std::array<std::string_view, 4> kChunks{"POST /upload HTTP/1.1\r", "\nContent-Length: 11\r", "\n\r\nhel", "lo world"};

        for (std::size_t index = 0; index + 1 < kChunks.size(); ++index)
        {
            EXPECT_EQ(parser.parse(kChunks[index].data(), kChunks[index].size()), ParseStatus::NeedMore) << "分片下标 " << index;
        }
        EXPECT_EQ(parser.parse(kChunks.back().data(), kChunks.back().size()), ParseStatus::Done);

        EXPECT_EQ(parser.request().uri(), "/upload");
        EXPECT_EQ(parser.request().body(), "hello world");
    }

    TEST(HttpParser, DoesNotExposeHalfBuiltRequestAsFinalResult)
    {
        HttpParser parser;

        // 请求行与头部都已到达，只差收尾空行：状态必须是 NeedMore
        const std::string partial = "GET /half-built HTTP/1.1\r\nX-Stage: headers\r\n";
        EXPECT_EQ(parser.parse(partial.data(), partial.size()), ParseStatus::NeedMore);

        // 方法、URI、版本要等完成回调才定稿，半成品阶段绝不能读出一个「看起来完整」的请求
        const HttpRequest &partialRequest = parser.request();
        EXPECT_EQ(partialRequest.method(), HttpMethod::UNKNOWN);
        EXPECT_TRUE(partialRequest.uri().empty());
        EXPECT_TRUE(partialRequest.httpVersion().empty());
        // 已到达但尚未收尾的头部同样读不到：解析结果先落在内部暂存上，只有一条报文收齐
        // 才整体搬进对外对象，因此半成品阶段的请求对象整体是空的，
        // 上层连「过程数据」都无从误用——比「可读但不许放行」更强
        EXPECT_FALSE(partialRequest.getHeader("x-stage").has_value());
        EXPECT_FALSE(parser.hasError());

        const std::string terminator = "\r\n";
        EXPECT_EQ(parser.parse(terminator.data(), terminator.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().method(), HttpMethod::GET);
        EXPECT_EQ(parser.request().uri(), "/half-built");
    }

    /**
     * @brief 钉住：流式派发的「头部块已收齐」门闩只在收尾空行之后放行，且提前搬进来的头部不被最终提交抹掉
     * @details 会话在头部收齐、正文还在路上这一刻就把 request() 交给处理器跑（HttpSession 的流式分支），
     *          靠的就是这两条：门闩不早放，最终提交只补正文。
     */
    TEST(HttpParser, CommitsStreamingHeadersOnlyAfterTheHeaderBlockEnds)
    {
        HttpParser parser;

        const std::string head = "PUT /stream HTTP/1.1\r\nContent-Length: 5\r\nX-Token: abc\r\n";
        EXPECT_EQ(parser.parse(head.data(), head.size()), ParseStatus::NeedMore);
        // 收尾空行还没到：头部块没收齐，既不许提交也不许读出来（与「半成品是空壳」同一条契约）
        EXPECT_FALSE(parser.isHeaderBlockComplete());
        EXPECT_FALSE(parser.commitHeadersForStreaming());
        EXPECT_TRUE(parser.request().uri().empty());
        EXPECT_FALSE(parser.request().getHeader("x-token").has_value());

        const std::string blankLine = "\r\n";
        EXPECT_EQ(parser.parse(blankLine.data(), blankLine.size()), ParseStatus::NeedMore);
        EXPECT_TRUE(parser.isHeaderBlockComplete()) << "定长正文阶段：头部块早已收齐，流式派发该放行";
        ASSERT_TRUE(parser.commitHeadersForStreaming());
        EXPECT_EQ(parser.request().uri(), "/stream");
        EXPECT_EQ(parser.request().getHeader("x-token").value_or("<缺失>"), "abc");
        EXPECT_TRUE(parser.request().body().empty()) << "正文还没到，提交头部不该造出正文来";

        // 同一轮里再要一次：必须幂等，且不许把已经搬进来的头部重置掉——会话手里正拿着这份对象在跑
        ASSERT_TRUE(parser.commitHeadersForStreaming());
        EXPECT_EQ(parser.request().getHeader("x-token").value_or("<缺失>"), "abc");

        const std::string body = "hello";
        EXPECT_EQ(parser.parse(body.data(), body.size()), ParseStatus::Done);
        // 最终提交只补正文：方法/URI/头部要原样还在。这里若重跑一遍搬运，处理器跑完看到的就成了空壳
        EXPECT_EQ(parser.request().uri(), "/stream");
        EXPECT_EQ(parser.request().getHeader("x-token").value_or("<缺失>"), "abc");
        EXPECT_EQ(parser.request().body(), "hello");
    }

    /**
     * @brief 钉住：提前提交的门闩随报文复位，第二条报文不会沿用第一条的头部
     */
    TEST(HttpParser, ResetsTheStreamingCommitLatchPerMessage)
    {
        HttpParser parser;

        const std::string firstHead = "PUT /first HTTP/1.1\r\nContent-Length: 2\r\nX-Only-First: 1\r\n\r\n";
        EXPECT_EQ(parser.parse(firstHead.data(), firstHead.size()), ParseStatus::NeedMore);
        ASSERT_TRUE(parser.commitHeadersForStreaming());
        const std::string firstBody = "ab";
        EXPECT_EQ(parser.parse(firstBody.data(), firstBody.size()), ParseStatus::Done);

        parser.reset();
        const std::string secondHead = "PUT /second HTTP/1.1\r\nContent-Length: 2\r\nX-Only-Second: 2\r\n\r\n";
        EXPECT_EQ(parser.parse(secondHead.data(), secondHead.size()), ParseStatus::NeedMore);
        // 复位没做对时这一句会走「已提交」那条早退分支：直接返回 true 却不再搬运，请求对象还挂着第一条的
        // URI 与头部——处理器会把请求发到上一条报文的路由上，且现场看不出来
        ASSERT_TRUE(parser.commitHeadersForStreaming());
        EXPECT_EQ(parser.request().uri(), "/second");
        EXPECT_TRUE(parser.request().getHeader("x-only-second").has_value());
        EXPECT_FALSE(parser.request().getHeader("x-only-first").has_value()) << "门闩跨报文残留：上一条的头部被沿用了";
        const std::string secondBody = "cd";
        EXPECT_EQ(parser.parse(secondBody.data(), secondBody.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().body(), "cd") << "复位之后最终提交没补上正文";
    }

    /**
     * @brief 钉住：100-continue 只在正文还在路上时给一次；trailer 阶段头部块算收齐但不再欠对端表态
     */
    TEST(HttpParser, OffersContinueOnlyWhileTheChunkedBodyIsStillIncoming)
    {
        const std::string head = "POST /up HTTP/1.1\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n";

        // 一条都没表态的解析器：走到 trailer 段（最后一个块已到）之后正文已经收完，再回 100 没有意义
        HttpParser lateParser;
        EXPECT_EQ(lateParser.parse(head.data(), head.size()), ParseStatus::NeedMore);
        const std::string oneChunk = "3\r\nabc\r\n";
        EXPECT_EQ(lateParser.parse(oneChunk.data(), oneChunk.size()), ParseStatus::NeedMore);
        const std::string lastChunk = "0\r\n";
        EXPECT_EQ(lateParser.parse(lastChunk.data(), lastChunk.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(lateParser.takeContinueRequest()) << "trailer 阶段正文已收完，不该再回 100";
        // 同一阶段的另一条判据却是「放行」：头部块早收齐了，流式派发不必等 trailer 段结束
        EXPECT_TRUE(lateParser.isHeaderBlockComplete());

        // 另一条解析器在块边界上表态：给一次，之后再要一律不给（同一条报文只回一个 100）
        HttpParser earlyParser;
        EXPECT_EQ(earlyParser.parse(head.data(), head.size()), ParseStatus::NeedMore);
        EXPECT_TRUE(earlyParser.takeContinueRequest()) << "正文还在路上且带 Expect，应当给出 100 的时机";
        EXPECT_FALSE(earlyParser.takeContinueRequest()) << "同一条报文只许回一次 100";
    }

    /**
     * @brief 钉住：100-continue 的两枚门闩都随报文一起复位，keep-alive 上每条报文各拿自己的时机
     * @details 两个方向的失效各有各的难看：「对端在等 100」不清 → 下一条不带 Expect 的报文被多回一个
     *          100，正文还没到就先冒出一句状态行；「这条已经取过」不清 → 后面真带 Expect 的报文再也
     *          要不到时机，客户端停在头部等表态，直到它自己的 Expect 超时（默认 1 秒）才发正文。
     *          同一条连接上复用同一份解析器状态时才现形，因此必须用三条报文把两侧夹住。
     */
    TEST(HttpParser, ResetsContinueLatchesPerMessage)
    {
        HttpParser parser;

        // 第一条：带 Expect。给一次时机，取过之后同一条不再给
        const std::string expectingHead = "POST /first HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n";
        EXPECT_EQ(parser.parse(expectingHead.data(), expectingHead.size()), ParseStatus::NeedMore);
        EXPECT_TRUE(parser.takeContinueRequest());
        EXPECT_FALSE(parser.takeContinueRequest());
        const std::string firstBody = "abc";
        EXPECT_EQ(parser.parse(firstBody.data(), firstBody.size()), ParseStatus::Done);

        // 第二条：不带 Expect。上一条「对端在等」若留着，这里会平白多回一次时机
        parser.reset();
        const std::string plainHead = "POST /second HTTP/1.1\r\nContent-Length: 3\r\n\r\n";
        EXPECT_EQ(parser.parse(plainHead.data(), plainHead.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(parser.takeContinueRequest()) << "门闩跨报文残留：不带 Expect 的报文被上一条的「在等 100」带走了";
        const std::string secondBody = "def";
        EXPECT_EQ(parser.parse(secondBody.data(), secondBody.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().uri(), "/second");

        // 第三条：又带 Expect。「这条已取过」若留着，对端等到自己的超时也等不到 100
        parser.reset();
        const std::string thirdHead = "POST /third HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n";
        EXPECT_EQ(parser.parse(thirdHead.data(), thirdHead.size()), ParseStatus::NeedMore);
        EXPECT_TRUE(parser.takeContinueRequest()) << "「已取过 100」没随报文复位：这条报文再也拿不到回 100 的时机";
        EXPECT_FALSE(parser.takeContinueRequest()) << "复位顺带把「同一条只给一次」也放开了";
        const std::string thirdBody = "ghi";
        EXPECT_EQ(parser.parse(thirdBody.data(), thirdBody.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().body(), "ghi");
    }

    TEST(HttpParser, RefusesToConsumeBytesAfterMessageCompleted)
    {
        HttpParser parser;

        const std::string firstMessage = "GET /first HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(firstMessage.data(), firstMessage.size()), ParseStatus::Done);

        // 完成之后再喂任何东西都直接返回 Done，且一字节不吃：否则会把上一条已定稿的结果改坏
        const std::string extraMessage = "GET /second HTTP/1.1\r\nX-Late: 1\r\n\r\n";
        EXPECT_EQ(parser.parse(extraMessage.data(), extraMessage.size()), ParseStatus::Done);

        EXPECT_EQ(parser.request().uri(), "/first");
        EXPECT_FALSE(parser.request().getHeader("x-late").has_value());
    }

    TEST(HttpParser, KeepsPipelinedMessagesApartInsteadOfMergingThem)
    {
        HttpParser parser;

        // 流水线：两条报文挤在同一次调用里。第一条必须完整落地，第二条被守卫拦下而不是串进来
        const std::string pipelined = "GET /first HTTP/1.1\r\nX-Only-First: 1\r\n\r\nGET /second HTTP/1.1\r\nX-Only-Second: 2\r\n\r\n";
        EXPECT_EQ(parser.parse(pipelined.data(), pipelined.size()), ParseStatus::Done);
        EXPECT_FALSE(parser.hasError());

        EXPECT_EQ(parser.request().uri(), "/first");
        EXPECT_TRUE(parser.request().getHeader("x-only-first").has_value());
        EXPECT_FALSE(parser.request().getHeader("x-only-second").has_value());

        // 上层 reset() 之后单独再喂第二条，才是正常的续读路径
        parser.reset();
        const std::string secondMessage = "GET /second HTTP/1.1\r\nX-Only-Second: 2\r\n\r\n";
        EXPECT_EQ(parser.parse(secondMessage.data(), secondMessage.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().uri(), "/second");
        EXPECT_FALSE(parser.request().getHeader("x-only-first").has_value());
    }

    TEST(HttpParser, MapsRecognizedMethodsAndFoldsOthersToUnknown)
    {
        static constexpr std::array<MethodProbe, 8> kProbes{{
                {"GET", HttpMethod::GET},
                {"POST", HttpMethod::POST},
                {"PUT", HttpMethod::PUT},
                {"DELETE", HttpMethod::DELETE},
                {"PATCH", HttpMethod::PATCH},
                {"HEAD", HttpMethod::HEAD},
                {"OPTIONS", HttpMethod::OPTIONS},
                {"TRACE", HttpMethod::UNKNOWN},
        }};

        for (const MethodProbe &probe: kProbes)
        {
            HttpParser        parser;
            const std::string message = std::string(probe.methodText) + " /probe HTTP/1.1\r\n\r\n";
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done) << probe.methodText;
            EXPECT_EQ(parser.request().method(), probe.expectedMethod) << probe.methodText;
        }
    }

    TEST(HttpParser, RecordsHttpVersionExactlyAsReceived)
    {
        HttpParser parser;

        const std::string message = "GET /old HTTP/1.0\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_EQ(parser.request().httpVersion(), "HTTP/1.0");
    }

    // ============================================================================
    // 头部存储模型
    // ============================================================================

    TEST(HttpParser, NormalizesHeaderNamesToLowercase)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({"Content-Type: application/json", "X-TRACE-ID: abc"});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        const HttpRequest &request = parser.request();
        EXPECT_EQ(request.getHeader("content-type").value_or(""), "application/json");
        EXPECT_EQ(request.getHeader("X-Trace-Id").value_or(""), "abc");
        EXPECT_EQ(request.headers().count("Content-Type"), 0U);
    }

    TEST(HttpParser, MergesRepeatedOrdinaryHeaderValuesInSingleView)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({"Cookie: a=1", "Cookie: b=2"});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        const HttpRequest &request = parser.request();
        EXPECT_EQ(request.getHeader("cookie").value_or(""), "a=1, b=2");

        const std::vector<std::string> values = request.headerValues("cookie");
        ASSERT_EQ(values.size(), 2U);
        EXPECT_EQ(values[0], "a=1");
        EXPECT_EQ(values[1], "b=2");
    }

    TEST(HttpParser, KeepsRepeatedSetCookieValuesSeparate)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({"Set-Cookie: sid=1", "Set-Cookie: theme=dark"});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        const HttpRequest             &request = parser.request();
        const std::vector<std::string> values  = request.headerValues("set-cookie");
        ASSERT_EQ(values.size(), 2U);
        EXPECT_EQ(values[0], "sid=1");
        EXPECT_EQ(values[1], "theme=dark");

        // 单值视图只留首条，且绝不允许再出现 set-cookie_1 这类伪键
        EXPECT_EQ(request.getHeader("set-cookie").value_or(""), "sid=1");
        EXPECT_EQ(request.headers().count("set-cookie_1"), 0U);
        EXPECT_EQ(request.headers().count("set-cookie_2"), 0U);
    }

    // ============================================================================
    // 错误与粘滞态
    // ============================================================================

    TEST(HttpParser, ReportsMalformedContentLengthAsPlainParseError)
    {
        HttpParser parser;

        const std::string message = "POST /x HTTP/1.1\r\nContent-Length: not-a-number\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.hasError());
        // 协议级非法不是超限，上层据此回 400 而不是 431/413
        EXPECT_FALSE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        // 中文外壳 + 保留 llhttp 的英文原因，只断言关键子串
        EXPECT_TRUE(containsText(errorMessage, "解析失败"));
        EXPECT_TRUE(containsText(errorMessage, "Content-Length"));
    }

    TEST(HttpParser, StaysInStickyErrorStateUntilReset)
    {
        HttpParser parser;

        const std::string malformed = "POST /x HTTP/1.1\r\nContent-Length: nope\r\n\r\n";
        EXPECT_EQ(parser.parse(malformed.data(), malformed.size()), ParseStatus::Error);

        // 错误粘滞：接着喂合法数据也只会拿回同一个错误，逼上层要么 reset 要么断开
        const std::string valid = "GET /after-error HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(valid.data(), valid.size()), ParseStatus::Error);

        parser.reset();
        EXPECT_FALSE(parser.hasError());
        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_TRUE(parser.errorMessage().empty());

        EXPECT_EQ(parser.parse(valid.data(), valid.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().uri(), "/after-error");
    }

    TEST(HttpParser, HandsOverCleanRequestForNextKeepAliveMessage)
    {
        HttpParser parser;

        const std::string firstMessage = "POST /first HTTP/1.1\r\nContent-Length: 2\r\nX-First: 1\r\n\r\nhi";
        ASSERT_EQ(parser.parse(firstMessage.data(), firstMessage.size()), ParseStatus::Done);
        parser.request().setParam("leftover", "yes");

        parser.reset();
        const std::string secondMessage = "GET /second HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(secondMessage.data(), secondMessage.size()), ParseStatus::Done);

        const HttpRequest &request = parser.request();
        EXPECT_EQ(request.uri(), "/second");
        EXPECT_TRUE(request.body().empty());
        EXPECT_FALSE(request.getHeader("x-first").has_value());
        EXPECT_FALSE(request.param("leftover").has_value());
    }

    // ============================================================================
    // 资源上限：达标放行、超限判错，并且标出「超限」这一子类
    // ============================================================================

    TEST(HttpParser, AcceptsRequestUriExactlyAtLengthLimit)
    {
        HttpParser parser;

        const std::string message = "GET /" + std::string(kUriLengthLimitInBytes - 1, 'a') + " HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_EQ(parser.request().uri().size(), kUriLengthLimitInBytes);
    }

    TEST(HttpParser, RejectsRequestUriAboveLengthLimit)
    {
        HttpParser parser;

        const std::string message = "GET /" + std::string(kUriLengthLimitInBytes + 1, 'a') + " HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        EXPECT_TRUE(containsText(errorMessage, "URI"));
        EXPECT_TRUE(containsText(errorMessage, "上限"));
    }

    TEST(HttpParser, AcceptsHeaderFieldNameExactlyAtLengthLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({std::string(kHeaderNameLimitInBytes, 'x') + ": v"});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_EQ(parser.request().getHeader(std::string(kHeaderNameLimitInBytes, 'x')).value_or(""), "v");
    }

    TEST(HttpParser, RejectsHeaderFieldNameAboveLengthLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({std::string(kHeaderNameLimitInBytes + 1, 'x') + ": v"});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        EXPECT_TRUE(containsText(errorMessage, "头部名"));
        EXPECT_TRUE(containsText(errorMessage, "上限"));
    }

    TEST(HttpParser, AcceptsHeaderValueExactlyAtLengthLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({"x-big: " + std::string(kHeaderValueLimitInBytes, 'v')});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_EQ(parser.request().getHeader("x-big").value_or("").size(), kHeaderValueLimitInBytes);
    }

    TEST(HttpParser, RejectsHeaderValueAboveLengthLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders({"x-big: " + std::string(kHeaderValueLimitInBytes + 1, 'v')});
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        EXPECT_TRUE(containsText(errorMessage, "头部值"));
        EXPECT_TRUE(containsText(errorMessage, "上限"));
    }

    TEST(HttpParser, AcceptsHeaderCountExactlyAtLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders(makeHeaderLines(kHeaderCountLimit, 1));
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);

        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_EQ(parser.request().headers().size(), kHeaderCountLimit);
    }

    TEST(HttpParser, RejectsHeaderCountAboveLimit)
    {
        HttpParser parser;

        const std::string message = makeRequestTextWithHeaders(makeHeaderLines(kHeaderCountLimit + 1, 1));
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        EXPECT_TRUE(containsText(errorMessage, "条数"));
        EXPECT_TRUE(containsText(errorMessage, "上限"));
    }

    TEST(HttpParser, RejectsHeaderBlockTotalAboveLimitEvenWithModestFields)
    {
        HttpParser parser;

        // 单条名、单条值、条数三道闸各自都拦不住，靠总长兜住：
        // 每条「9 字节名 + 7300 字节值」，第 9 条累计突破 64 KiB
        const std::string message = makeRequestTextWithHeaders(makeHeaderLines(10, 7300));
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.isLimitExceeded());
        const std::string errorMessage = parser.errorMessage();
        EXPECT_FALSE(errorMessage.empty());
        EXPECT_TRUE(containsText(errorMessage, "总长"));
        EXPECT_TRUE(containsText(errorMessage, "上限"));
    }

    TEST(HttpParser, ClearsLimitFlagsAfterReset)
    {
        HttpParser parser;

        const std::string overLongName = makeRequestTextWithHeaders({std::string(kHeaderNameLimitInBytes + 1, 'x') + ": v"});
        ASSERT_EQ(parser.parse(overLongName.data(), overLongName.size()), ParseStatus::Error);
        ASSERT_TRUE(parser.isLimitExceeded());

        parser.reset();
        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_FALSE(parser.hasError());
        EXPECT_TRUE(parser.errorMessage().empty());

        const std::string normal = "GET /recovered HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.parse(normal.data(), normal.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().uri(), "/recovered");
    }

    // ============================================================================
    // 手写状态机的严格性：过时语法与「猜长度」一律拒绝，绝不宽容处理
    // ============================================================================

    /**
     * @brief 折行（obs-fold）头部明确拒绝
     *
     * @details 以空白开头的续行已被 RFC 9112 判为过时。宽容拼接会让同一个头部名出现
     *          两种解释（转发链两侧各按己方理解取值），是请求走私的经典入口。
     */
    TEST(HttpParser, RejectsObsoleteLineFolding)
    {
        HttpParser parser;

        const std::string message = "GET /folded HTTP/1.1\r\nX-Note: first\r\n second\r\n\r\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.hasError());
        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_TRUE(containsText(parser.errorMessage(), "折行"));
    }

    /**
     * @brief 行尾只认 CRLF：单独出现的 LF 不是行结束
     */
    TEST(HttpParser, RejectsBareLineFeedAsLineTerminator)
    {
        HttpParser parser;

        const std::string message = "GET /bare-lf HTTP/1.1\nHost: example.test\n\n";
        EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_TRUE(parser.hasError());
        EXPECT_FALSE(parser.isLimitExceeded());
        EXPECT_TRUE(containsText(parser.errorMessage(), "CRLF"));
    }

    /**
     * @brief 重复的 Content-Length 只在取值一致时放行，取值不一致当场判错
     *
     * @details 取值不一致等于「同一份报文有两个长度解释」，收发两侧各自按己方理解切包
     *          正是请求走私的温床。这一口径与 HttpSession 的定界器保持一致。
     */
    TEST(HttpParser, AcceptsIdenticalRepeatedContentLengthButRejectsConflictingOnes)
    {
        HttpParser        identicalParser;
        const std::string sameValue = "POST /dup HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
        EXPECT_EQ(identicalParser.parse(sameValue.data(), sameValue.size()), ParseStatus::Done);
        EXPECT_EQ(identicalParser.request().body(), "hello");

        HttpParser        conflictingParser;
        const std::string differentValue = "POST /dup HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello";
        EXPECT_EQ(conflictingParser.parse(differentValue.data(), differentValue.size()), ParseStatus::Error);
        EXPECT_FALSE(conflictingParser.isLimitExceeded());
        EXPECT_TRUE(containsText(conflictingParser.errorMessage(), "Content-Length"));
    }

    /**
     * @brief 请求行严格校验：目标里混进空格、版本位数不对都要判错
     */
    TEST(HttpParser, RejectsMalformedRequestLine)
    {
        static constexpr std::array<std::string_view, 5> kMalformedMessages{
                "GET /two words HTTP/1.1\r\n\r\n", ///< 目标里混进空格
                "GET  /x HTTP/1.1\r\n\r\n",        ///< 多一个分隔空格（目标以空格开头）
                "GET /x HTTP/11\r\n\r\n",          ///< 版本缺少「主.次」结构
                "GET /x HTTP/1.11\r\n\r\n",        ///< 版本次版本号位数超出
                "GET /x HTTP/9.9\r\n\r\n",         ///< 主版本不是 0/1：那是另一套协议，不该按文本解析
        };

        for (const std::string_view message: kMalformedMessages)
        {
            HttpParser parser;
            EXPECT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error) << message;
            EXPECT_TRUE(parser.hasError()) << message;
            EXPECT_TRUE(containsText(parser.errorMessage(), "解析失败")) << message;
        }
    }

    // ============================================================================
    // 交出的边界与失败分类：会话据此省掉自己那一遍定界扫描
    // ============================================================================

    /**
     * @brief Done 之后交回本条报文的长度，缓冲区里排在后面的字节属于下一条报文
     */
    TEST(HttpParser, ReportsConsumedLengthSoTheNextMessageBoundaryIsKnown)
    {
        HttpParser parser;

        const std::string first  = "POST /first HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
        const std::string second = "GET /second HTTP/1.1\r\n\r\n";

        // 两条报文挤在同一次调用里：第一条的长度必须精确交回，第二条一个字节都不吃
        const std::string pipelined = first + second;
        ASSERT_EQ(parser.parse(pipelined.data(), pipelined.size()), ParseStatus::Done);
        EXPECT_EQ(parser.consumedByteCount(), first.size()) << "交回的消费量不是第一条报文的长度";
        EXPECT_EQ(parser.request().uri(), "/first");
        EXPECT_EQ(parser.request().body(), "hello");

        // 剩下的字节正是下一条报文：reset 之后从那个位置重新喂，一条不差
        parser.reset();
        EXPECT_EQ(parser.parse(pipelined.data() + first.size(), second.size()), ParseStatus::Done);
        EXPECT_EQ(parser.consumedByteCount(), second.size());
        EXPECT_EQ(parser.request().uri(), "/second");
    }

    /**
     * @brief 分片喂入时按每次调用分别交回消费量
     */
    TEST(HttpParser, ReportsConsumedLengthForEachSlice)
    {
        HttpParser parser;

        const std::string message    = "GET /sliced HTTP/1.1\r\nHost: x\r\n\r\n";
        const std::size_t firstSlice = 7;

        EXPECT_EQ(parser.parse(message.data(), firstSlice), ParseStatus::NeedMore);
        EXPECT_EQ(parser.consumedByteCount(), firstSlice) << "NeedMore 时喂进去的字节应当全部被消费";

        EXPECT_EQ(parser.parse(message.data() + firstSlice, message.size() - firstSlice), ParseStatus::Done);
        EXPECT_EQ(parser.consumedByteCount(), message.size() - firstSlice);
    }

    /**
     * @brief 失败的类别与会话的状态码一一对应，上层不必去匹配错误文案
     */
    TEST(HttpParser, ReportsTypedFailureKinds)
    {
        {
            HttpParser        parser;
            const std::string message = makeRequestTextWithHeaders({std::string(kHeaderNameLimitInBytes + 1, 'x') + ": v"});
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);
            EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::HeaderTooLarge);
        }
        {
            HttpParser        parser;
            const std::string message = "POST /huge HTTP/1.1\r\nContent-Length: 9000000000\r\n\r\n";
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);
            EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::BodyTooLarge);
        }
        {
            // Transfer-Encoding 取值不是 chunked：按协议级非法处理（400），不能悄悄按 identity 收下。
            // 分块请求体本身的解码与拒绝面另见 TestHttpParserChunked.cpp
            HttpParser        parser;
            const std::string message = "POST /chunked HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);
            EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::Malformed);
        }
        {
            HttpParser        parser;
            const std::string message = "GET /x HTTP/9.9\r\n\r\n";
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);
            EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::Malformed);
        }

        EXPECT_EQ(HttpParser().errorKind(), HttpParseErrorKind::None);
    }

    /**
     * @brief 声明的 Content-Length 超限时，正文字节一个都还没到就要判错
     *
     * @details 等到收满 8 MiB 才判，等于按对端的声明替它预留内存——声明一个天文数字就能把
     *          缓冲区耗光。因此这条判定落在解析头部的那一刻。
     */
    TEST(HttpParser, DeclaredContentLengthAboveLimitIsRejectedBeforeBodyArrives)
    {
        HttpParser parser;

        const std::string message = "POST /big HTTP/1.1\r\nContent-Length: 9000000000\r\n\r\n";
        ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);

        EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::BodyTooLarge);
        EXPECT_TRUE(parser.isLimitExceeded());
        EXPECT_TRUE(containsText(parser.errorMessage(), "上限"));
    }
    /**
     * @brief Content-Length 的取值必须是不带符号、无空格、无后缀的纯十进制
     * @details 这条共享规则是请求走私面的第一道闸：`12abc`（前缀解析会读成 12）、`+5`、
     *          `" 5 "`（两侧空白）、超过 19 位（溢出）都必须拒。h1 与 h2 两侧都走它，
     *          因此这里直接钉规则本身——此前只有「声明与实收不符」一类用例，规则改宽松了
     *          一条用例都不会红
     */
    TEST(HttpHeaderRules, RejectsMalformedContentLengthValues)
    {
        std::size_t parsed = 0;

        EXPECT_FALSE(parseContentLengthValue("12abc", parsed)) << "带后缀的取值被当成了 12";
        EXPECT_FALSE(parseContentLengthValue("+5", parsed));
        EXPECT_FALSE(parseContentLengthValue("-5", parsed));
        EXPECT_FALSE(parseContentLengthValue("5 5", parsed)) << "中间夹空白不是十进制数";
        EXPECT_FALSE(parseContentLengthValue("", parsed));
        EXPECT_FALSE(parseContentLengthValue("99999999999999999999", parsed)) << "20 位数字应判越界而不是回绕";

        EXPECT_TRUE(parseContentLengthValue("0", parsed)) << "0 是合法的正文长度（空正文）";
        EXPECT_EQ(parsed, 0U);
        EXPECT_TRUE(parseContentLengthValue("8192", parsed));
        EXPECT_EQ(parsed, 8192U);
        // 首尾 OWS 本就不属于字段值（RFC 9110 §5.5），规则先裁掉再判——两侧带空白的取值因此合法
        EXPECT_TRUE(parseContentLengthValue(" 5 ", parsed)) << "字段值允许首尾 OWS";
        EXPECT_EQ(parsed, 5U);
    }

} // namespace AsynGyanis::Net
