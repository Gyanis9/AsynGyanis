/**
 * @file TestHttpParser.cpp
 * @brief HttpParser 单元测试：增量解析、报文定界、头部存储模型与资源上限
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/ParseStatus.h"

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
        constexpr std::size_t kUriLengthLimitInBytes = 8 * 1024;              ///< 请求 URI 上限
        constexpr std::size_t kHeaderNameLimitInBytes = 256;                  ///< 单个头部名上限
        constexpr std::size_t kHeaderValueLimitInBytes = 8 * 1024;            ///< 单个头部值上限
        constexpr std::size_t kHeaderCountLimit = 100;                        ///< 头部条数上限
        constexpr std::size_t kHeaderBlockLimitInBytes = 64 * 1024;           ///< 头部块总长上限

        /// 上限探测用的「方法原文 + 期望枚举」配对
        struct MethodProbe
        {
            std::string_view methodText;   ///< 请求行里的方法原文
            HttpMethod expectedMethod;     ///< 期望映射到的枚举值
        };

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
         * @brief 用给定的头部行拼出一条完整 GET 报文
         * @details 只负责固定请求行与收尾空行，头部行完全由调用方给，
         *          这样「一共几条头部」这类计数断言才是可精确推算的。
         * @param headerLines 头部行原文，不含行尾 CRLF
         * @return std::string 可直接喂给 parse() 的报文
         */
        std::string makeRequestTextWithHeaders(const std::vector<std::string> &headerLines)
        {
            std::string message = "GET /indexed HTTP/1.1\r\n";
            for (const std::string &headerLine: headerLines)
            {
                message.append(headerLine);
                message.append("\r\n");
            }
            message.append("\r\n");
            return message;
        }

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
        static constexpr std::array<std::string_view, 4> kChunks{
                "POST /upload HTTP/1.1\r",
                "\nContent-Length: 11\r",
                "\n\r\nhel",
                "lo world"};

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
        // 已到达但尚未收尾的头部同样读不到：llhttp 要看到下一行的首字节才会发出
        // on_header_value_complete，因此半成品阶段的请求对象整体是空的，
        // 上层连「过程数据」都无从误用——比「可读但不许放行」更强
        EXPECT_FALSE(partialRequest.getHeader("x-stage").has_value());
        EXPECT_FALSE(parser.hasError());

        const std::string terminator = "\r\n";
        EXPECT_EQ(parser.parse(terminator.data(), terminator.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().method(), HttpMethod::GET);
        EXPECT_EQ(parser.request().uri(), "/half-built");
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
            HttpParser parser;
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

        const HttpRequest &request = parser.request();
        const std::vector<std::string> values = request.headerValues("set-cookie");
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
} // namespace AsynGyanis::Net
