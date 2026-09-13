// TestHttpParserChunked.cpp —— 手写解析器的分块请求体解码（RFC 9112 §7.1）：
//   一. 快乐路径：单块、带块扩展、多块加 trailer 段、大写十六进制、终止块后直接收尾；
//   二. 增量语义：逐字节喂入与一次喂入等价、任意前缀都不提前判完成；
//   三. 拒绝面：Content-Length 与 Transfer-Encoding 并存、非 chunked 编码、块大小非法、
//       块大小行与块数据后缺 CRLF、trailer 行畸形、单块大小与解码后总长超上限。
// HttpParser 的上限常量是私有成员，测试侧按同一口径复述一份数值。

#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpParseErrorKind.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 请求体上限，与 HttpParser 的正文上限同值：超限判定必须落在同一个数字上
        constexpr std::size_t kBodyLimitInBytes = 8 * 1024 * 1024;

        /// 分块请求的头部块（不含正文），固定声明 Transfer-Encoding: chunked
        constexpr std::string_view kChunkedHeaderBlock =
                "POST /chunked HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n";

        /**
         * @brief 拼出一条分块报文
         * @param chunkedPayload 头部块之后的原始分块编码字节
         * @return std::string 可直接喂给 parse() 的报文
         */
        std::string makeChunkedMessage(const std::string_view chunkedPayload)
        {
            std::string message(kChunkedHeaderBlock);
            message.append(chunkedPayload);
            return message;
        }

        /**
         * @brief 断言整条报文一次喂入即解析成功，且解码后的正文与消费量符合预期
         * @param message 完整报文
         * @param expectedBody 期望的解码后正文
         * @param caseLabel 失败信息里的用例标识
         */
        void expectDecodedBody(const std::string &message, const std::string &expectedBody, const std::string &caseLabel)
        {
            HttpParser parser;
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done) << caseLabel;
            EXPECT_FALSE(parser.hasError()) << caseLabel;
            EXPECT_EQ(parser.request().body(), expectedBody) << caseLabel;
            // 消费量必须正好是报文长度：Done 之后多吃的字节属于流水线里的下一条报文
            EXPECT_EQ(parser.consumedByteCount(), message.size()) << caseLabel;
        }

        /**
         * @brief 断言整条报文被拒绝，且失败类别与「是否超限」都符合预期
         * @param message 完整报文
         * @param expectedKind 期望的失败类别
         * @param expectedLimitExceeded 期望 isLimitExceeded() 的取值
         * @param caseLabel 失败信息里的用例标识
         */
        void expectRejected(const std::string &message, const HttpParseErrorKind expectedKind, const bool expectedLimitExceeded,
                            const std::string &caseLabel)
        {
            HttpParser parser;
            ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error) << caseLabel;
            EXPECT_EQ(parser.errorKind(), expectedKind) << caseLabel << "，错误文案：" << parser.errorMessage();
            EXPECT_EQ(parser.isLimitExceeded(), expectedLimitExceeded) << caseLabel;
            EXPECT_FALSE(parser.errorMessage().empty()) << caseLabel;
        }
    } // namespace

    // ============================================================================
    // 快乐路径：按 RFC 9112 §7.1 解码
    // ============================================================================

    /**
     * @brief 单个分块被解码进正文，终止块与收尾 CRLF 之后整条报文完成
     */
    TEST(HttpParserChunked, DecodesSimpleChunkedBody)
    {
        const std::string message = makeChunkedMessage("5\r\nhello\r\n0\r\n\r\n");

        HttpParser parser;
        ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().method(), HttpMethod::POST);
        EXPECT_EQ(parser.request().uri(), "/chunked");
        EXPECT_EQ(parser.request().body(), "hello");
        EXPECT_FALSE(parser.hasError());
        EXPECT_EQ(parser.consumedByteCount(), message.size());
    }

    /**
     * @brief 块扩展只校验语法、不参与解码，块与终止块都可以带扩展
     */
    TEST(HttpParserChunked, AcceptsWellFormedChunkExtensions)
    {
        expectDecodedBody(makeChunkedMessage("5;foo=bar\r\nhello\r\n0;last\r\n\r\n"), "hello", "带 token 值的扩展");
        expectDecodedBody(makeChunkedMessage("5;a=\"quoted value\";b\r\nhello\r\n0\r\n\r\n"), "hello", "带引号字符串的扩展值");
    }

    /**
     * @brief 扩展语法非法（空扩展名、等号后没有值、段之间缺分号）判错，而不是当成未知扩展忽略
     */
    TEST(HttpParserChunked, RejectsMalformedChunkExtensions)
    {
        static constexpr std::array<std::string_view, 3> kPayloads{
                "5;=bar\r\nhello\r\n0\r\n\r\n",   ///< 扩展名为空
                "5;foo=\r\nhello\r\n0\r\n\r\n",    ///< 等号之后没有值
                "5;foo bar\r\nhello\r\n0\r\n\r\n", ///< 段之间缺少分号
        };

        for (const std::string_view payload: kPayloads)
        {
            expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::Malformed, false, std::string(payload));
        }
    }

    /**
     * @brief 多块按序拼接，trailer 段只做语法校验、不并入请求头部
     *
     * @details trailer 内容有意不落进 request.headers()：trailer 里的 content-length 之类若被上层
     *          当成头部读到，就会与解析器实际使用的定界方式形成两种解释（请求走私面）。
     */
    TEST(HttpParserChunked, ConcatenatesChunksAndValidatesTrailerSection)
    {
        const std::string message = makeChunkedMessage("5\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: v\r\nX-Count: 2\r\n\r\n");

        HttpParser parser;
        ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().body(), "hello world");
        EXPECT_TRUE(parser.request().getHeader("transfer-encoding").has_value());
        EXPECT_FALSE(parser.request().getHeader("x-trailer").has_value());
        EXPECT_FALSE(parser.request().getHeader("x-count").has_value());
    }

    /**
     * @brief 块大小是十六进制：大写字母同样合法
     */
    TEST(HttpParserChunked, AcceptsUppercaseHexadecimalChunkSize)
    {
        expectDecodedBody(makeChunkedMessage("A\r\n0123456789\r\n0\r\n\r\n"), "0123456789", "大写十六进制块大小");
    }

    /**
     * @brief 终止块之后直接是收尾 CRLF：正文为空，且不吃属于下一条报文的字节
     */
    TEST(HttpParserChunked, AcceptsEmptyBodyTerminatedWithoutTrailer)
    {
        const std::string message   = makeChunkedMessage("0\r\n\r\n");
        const std::string pipelined = message + "GET /next HTTP/1.1\r\n\r\n";

        HttpParser parser;
        ASSERT_EQ(parser.parse(pipelined.data(), pipelined.size()), ParseStatus::Done);
        EXPECT_TRUE(parser.request().body().empty());
        EXPECT_EQ(parser.consumedByteCount(), message.size()) << "交回的消费量不是分块报文的长度";
    }

    // ============================================================================
    // 增量语义：任何字节边界都能切开续上
    // ============================================================================

    /**
     * @brief trailer 段的收尾 CRLF 到达之前只能是 NeedMore，绝不提前宣布完成
     */
    TEST(HttpParserChunked, AsksForMoreUntilTrailerSectionCloses)
    {
        const std::string message   = makeChunkedMessage("5\r\nhello\r\n0\r\nX-Trailer: v\r\n\r\n");
        const std::string truncated = message.substr(0, message.size() - 2);

        HttpParser parser;
        EXPECT_EQ(parser.parse(truncated.data(), truncated.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(parser.hasError());

        EXPECT_EQ(parser.parse(message.data() + truncated.size(), 2), ParseStatus::Done);
        EXPECT_EQ(parser.request().body(), "hello");
        EXPECT_EQ(parser.consumedByteCount(), 2U);
    }

    /**
     * @brief 逐字节喂入与一次喂入逐字段等价，且中途每一步都不许判完成或判错
     */
    TEST(HttpParserChunked, DecodesIdenticallyWhenFedOneByteAtATime)
    {
        const std::string message = makeChunkedMessage("A;ext=1\r\n0123456789\r\n3\r\nabc\r\n0\r\nX-Trailer: v\r\n\r\n");

        HttpParser wholeParser;
        ASSERT_EQ(wholeParser.parse(message.data(), message.size()), ParseStatus::Done);

        HttpParser slicedParser;
        for (std::size_t index = 0; index < message.size(); ++index)
        {
            const ParseStatus status = slicedParser.parse(message.data() + index, 1);
            if (index + 1 < message.size())
            {
                // 少一个字节就不是完整报文：中途任何一步判 Done 都意味着上层会放行半条请求
                ASSERT_EQ(status, ParseStatus::NeedMore) << "第 " << index << " 字节喂入后状态异常";
                ASSERT_FALSE(slicedParser.hasError()) << "第 " << index << " 字节喂入后判错：" << slicedParser.errorMessage();
            } else
            {
                ASSERT_EQ(status, ParseStatus::Done) << "末字节喂入后未判完成";
            }
            EXPECT_EQ(slicedParser.consumedByteCount(), 1U) << "第 " << index << " 字节未被消费";
        }

        EXPECT_EQ(slicedParser.request().body(), wholeParser.request().body());
        EXPECT_EQ(slicedParser.request().uri(), wholeParser.request().uri());
        EXPECT_EQ(slicedParser.request().httpVersion(), wholeParser.request().httpVersion());
        EXPECT_EQ(slicedParser.request().headers(), wholeParser.request().headers());
        EXPECT_FALSE(slicedParser.isLimitExceeded());
    }

    /**
     * @brief 分块报文的每个前缀都只能是 NeedMore 或 Error，绝不可能是 Done
     */
    TEST(HttpParserChunked, EveryTruncatedPrefixStaysIncomplete)
    {
        const std::string message = makeChunkedMessage("A;ext=1\r\n0123456789\r\n5\r\nhello\r\n0\r\nX-Trailer: v\r\n\r\n");

        for (std::size_t prefixLength = 0; prefixLength < message.size(); ++prefixLength)
        {
            HttpParser parser;
            const ParseStatus status = parser.parse(message.data(), prefixLength);
            EXPECT_NE(status, ParseStatus::Done) << "前缀长度 " << prefixLength << " 被当成完整报文";
            EXPECT_LE(parser.consumedByteCount(), prefixLength) << "前缀长度 " << prefixLength << " 消费了超出喂入的字节";
        }
    }

    // ============================================================================
    // 拒绝面：定界头的组合、语法与上限
    // ============================================================================

    /**
     * @brief Content-Length 与 Transfer-Encoding 并存判错
     *
     * @details 两者并存时「正文到哪里结束」有两个互相矛盾的解释，两侧各按己方理解切包正是
     *          请求走私的经典面。RFC 9112 §6.3 允许拒绝或只认一条，这里选拒绝。
     */
    TEST(HttpParserChunked, RejectsContentLengthTogetherWithTransferEncoding)
    {
        const std::string message =
                "POST /smuggle HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";

        expectRejected(message, HttpParseErrorKind::Malformed, false, "CL 与 TE 并存");

        HttpParser parser;
        ASSERT_EQ(parser.parse(message.data(), message.size()), ParseStatus::Error);
        EXPECT_NE(parser.errorMessage().find("Content-Length"), std::string::npos);
        EXPECT_NE(parser.errorMessage().find("Transfer-Encoding"), std::string::npos);
    }

    /**
     * @brief 只接受唯一的 chunked：gzip 等其它编码、chunked 不在末尾、重复声明、空值都判错
     */
    TEST(HttpParserChunked, RejectsTransferEncodingOtherThanSingleChunked)
    {
        static constexpr std::array<std::string_view, 5> kEncodingValues{
                "gzip",             ///< 不接受的传输编码
                "chunked, gzip",    ///< chunked 不在编码链末尾
                "gzip, chunked",    ///< 前面还挂着别的编码
                "chunked, chunked", ///< chunked 重复出现
                "",                 ///< 空值：不能当成「没有编码」
        };

        for (const std::string_view encodingValue: kEncodingValues)
        {
            const std::string message =
                    "POST /enc HTTP/1.1\r\nTransfer-Encoding: " + std::string(encodingValue) + "\r\n\r\n0\r\n\r\n";
            expectRejected(message, HttpParseErrorKind::Malformed, false, "Transfer-Encoding: " + std::string(encodingValue));
        }

        // 同一个头名出现两次同样要看见全部取值后一起判，而不是只认第一条
        const std::string duplicated =
                "POST /enc HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n";
        expectRejected(duplicated, HttpParseErrorKind::Malformed, false, "Transfer-Encoding 出现两次");
    }

    /**
     * @brief 块大小含非法十六进制位判错
     */
    TEST(HttpParserChunked, RejectsChunkSizeWithNonHexadecimalCharacter)
    {
        static constexpr std::array<std::string_view, 2> kPayloads{
                "g\r\nhello\r\n0\r\n\r\n",  ///< 非法十六进制位
                "5x\r\nhello\r\n0\r\n\r\n", ///< 十六进制数字后紧跟其它字节
        };

        for (const std::string_view payload: kPayloads)
        {
            expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::Malformed, false, std::string(payload));
        }
    }

    /**
     * @brief 块大小行与块数据后缺 CRLF 都判错，不把裸 LF 当作行结束
     */
    TEST(HttpParserChunked, RejectsMissingCrlfAroundChunkData)
    {
        static constexpr std::array<std::string_view, 3> kPayloads{
                "5\nhello\r\n0\r\n\r\n",      ///< 块大小行以裸 LF 结尾
                "5\r\nhelloXX\r\n0\r\n\r\n",  ///< 块数据之后不是 CRLF
                "5\r\nhello\n0\r\n\r\n",      ///< 块数据之后只有裸 LF
        };

        for (const std::string_view payload: kPayloads)
        {
            expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::Malformed, false, std::string(payload));
        }
    }

    /**
     * @brief trailer 行必须也是「名: 值」字段，畸形行判错
     */
    TEST(HttpParserChunked, RejectsMalformedTrailerLine)
    {
        static constexpr std::array<std::string_view, 3> kPayloads{
                "0\r\nBad-Trailer\r\n\r\n",            ///< 缺冒号
                "0\r\n: v\r\n\r\n",                    ///< 名为空
                "0\r\nX-Trailer: 1\r\nBad Line: 2\r\n\r\n", ///< 名字里含空格（非 token）
        };

        for (const std::string_view payload: kPayloads)
        {
            expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::Malformed, false, std::string(payload));
        }
    }

    /**
     * @brief 单块声明的块大小超上限：一个正文字节都不必到就判 413
     *
     * @details 提前判定而不是等正文到齐，否则等于按对端的声明替它预留内存；十六进制位手工累加，
     *          超长数字串在这里只会判错，不会让长度静默回绕。
     */
    TEST(HttpParserChunked, RejectsSingleChunkSizeAboveLimit)
    {
        static constexpr std::array<std::string_view, 2> kPayloads{
                "4000001\r\nexpected-body-not-needed\r\n", ///< 0x4000001 = 64 MiB + 1
                "FFFFFFFFFFFFFFFF\r\n",                    ///< 超长数字串：只能判错，不能回绕
        };

        for (const std::string_view payload: kPayloads)
        {
            expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::BodyTooLarge, true, std::string(payload));
        }
    }

    /**
     * @brief 单个块都不超上限，但解码后的正文总量超上限同样判 413
     *
     * @details 上限必须按解码后的字节数判：按编码后的体积判，多块拼出超大正文就能溜过去。
     */
    TEST(HttpParserChunked, RejectsDecodedBodyAboveLimit)
    {
        constexpr std::size_t kHalfLimitInBytes = kBodyLimitInBytes / 2; ///< 0x400000，正好是上限的一半

        std::string payload;
        payload.append("400000\r\n");
        payload.append(kHalfLimitInBytes, 'a');
        payload.append("\r\n400000\r\n");
        payload.append(kHalfLimitInBytes, 'b');
        // 前两块正好填到上限（等于上限仍放行），第三块再给 1 字节即越界
        payload.append("\r\n1\r\nc\r\n0\r\n\r\n");

        expectRejected(makeChunkedMessage(payload), HttpParseErrorKind::BodyTooLarge, true, "解码后正文超上限");
    }
} // namespace AsynGyanis::Net
