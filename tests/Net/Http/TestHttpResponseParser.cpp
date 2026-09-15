/**
 * @file TestHttpResponseParser.cpp
 * @brief HttpResponseParser 单元测试：正文定界（chunked / content-length / 连接关闭）、
 *        跨馈送分段与拒绝面
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/Client/HttpResponseParser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一次性喂完整段报文，返回是否解析完成
        bool feedAll(HttpResponseParser &parser, const std::string_view message)
        {
            parser.feed(message);
            return parser.isComplete();
        }

        /// 按 1 字节步长喂入（钉住「跨馈送调用得到同一结果」）
        bool feedOneByteAtATime(HttpResponseParser &parser, const std::string_view message)
        {
            for (const char character: message)
            {
                parser.feed(std::string_view(&character, 1));
                // 失败即刻收手：继续喂只会反复走 Failed 分支，掩盖真实阶段
                if (parser.hasFailed())
                {
                    return false;
                }
            }
            return parser.isComplete();
        }
    } // namespace

    /**
     * @brief 钉住 Content-Length 定界：收满声明长度即完成，多出的字节不吞
     */
    TEST(HttpResponseParser, DecodesContentLengthBody)
    {
        const std::string message = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(message), message.size() - 5);
        EXPECT_TRUE(parser.isComplete());
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_EQ(parser.result().body, "hello");
    }

    /**
     * @brief 钉住 chunked 定界：0 块 + 空行之后必须收尾完成（缺陷回归——此前的实现
     *        永远停在正文阶段，客户端会丢弃整条响应）
     */
    TEST(HttpResponseParser, DecodesChunkedBodyAndCompletes)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
        EXPECT_EQ(parser.result().body, "hello world");
    }

    /**
     * @brief 钉住 chunked 的跨馈送分段：一字节一字节喂入与整段喂入结果一致
     */
    TEST(HttpResponseParser, DecodesChunkedOneByteAtATime)
    {
        const std::string message = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        HttpResponseParser parser;
        EXPECT_TRUE(feedOneByteAtATime(parser, message));
        EXPECT_EQ(parser.result().body, "hello world");
    }

    /**
     * @brief 钉住 chunked 的 trailer 段：非空 trailer 行被接受，空行收尾
     */
    TEST(HttpResponseParser, AcceptsChunkedTrailerSection)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-Trailer: v\r\n\r\n"));
        EXPECT_EQ(parser.result().body, "hello");
    }

    /**
     * @brief 钉住 Transfer-Encoding 的大小写不敏感：Chunked 变体同样识别
     */
    TEST(HttpResponseParser, AcceptsChunkedSpellingRegardlessOfCase)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));
        EXPECT_EQ(parser.result().body, "abc");
    }

    /**
     * @brief 钉住「Transfer-Encoding 优先于 Content-Length」的拒绝面：两者并存按非法处理，
     *        不能挑一个信（响应走私的入口）
     */
    TEST(HttpResponseParser, RejectsContentLengthWithTransferEncoding)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        EXPECT_TRUE(parser.hasFailed());
        EXPECT_FALSE(parser.isComplete());
    }

    /**
     * @brief 钉住 Content-Length 取值的严格性：带后缀的数字与重复且冲突的取值都按非法拒绝
     */
    TEST(HttpResponseParser, RejectsMalformedContentLength)
    {
        HttpResponseParser withSuffix;
        withSuffix.feed("HTTP/1.1 200 OK\r\nContent-Length: 12abc\r\n\r\n");
        EXPECT_TRUE(withSuffix.hasFailed());

        HttpResponseParser conflicting;
        conflicting.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
        EXPECT_TRUE(conflicting.hasFailed());
    }

    /**
     * @brief 钉住重复 Content-Length 的接受面：取值完全一致时按合法处理
     */
    TEST(HttpResponseParser, AcceptsDuplicateIdenticalContentLength)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello"));
        EXPECT_EQ(parser.result().body, "hello");
    }

    /**
     * @brief 钉住块大小行的严格性：非十六进制字符按非法拒绝，不静默当成 0
     */
    TEST(HttpResponseParser, RejectsMalformedChunkSizeLine)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\nhello\r\n0\r\n\r\n");
        EXPECT_TRUE(parser.hasFailed());
    }

    /**
     * @brief 钉住 close-delimited 定界：Connection: close 且无长度头时收到连接关闭才算完成
     */
    TEST(HttpResponseParser, CompletesCloseDelimitedOnlyAtEndOfStream)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial");
        EXPECT_FALSE(parser.isComplete());
        EXPECT_FALSE(parser.hasFailed());
        parser.endOfStream();
        EXPECT_TRUE(parser.isComplete());
        EXPECT_EQ(parser.result().body, "partial");
    }

    /**
     * @brief 钉住 HTTP/1.0 的默认行为：无长度头时按连接关闭定界，不当作「无正文」
     */
    TEST(HttpResponseParser, TreatsHttp10WithoutLengthAsCloseDelimited)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.0 200 OK\r\n\r\nbody-by-close");
        EXPECT_FALSE(parser.isComplete());
        parser.endOfStream();
        EXPECT_TRUE(parser.isComplete());
        EXPECT_EQ(parser.result().body, "body-by-close");
    }

    /**
     * @brief 钉住无正文状态码：1xx / 204 / 304 即便带 Content-Length 也立即完成、不等待正文
     */
    TEST(HttpResponseParser, CompletesNoBodyStatusWithoutWaiting)
    {
        HttpResponseParser notModified;
        EXPECT_TRUE(feedAll(notModified, "HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n"));
        EXPECT_TRUE(notModified.result().body.empty());

        HttpResponseParser noContent;
        EXPECT_TRUE(feedAll(noContent, "HTTP/1.1 204 No Content\r\n\r\n"));
        EXPECT_TRUE(noContent.result().body.empty());
    }

    /**
     * @brief 钉住不完整正文的失败面：chunked 未收尾时连接关闭 → 失败，而不是产出一条半截响应
     */
    TEST(HttpResponseParser, FailsWhenChunkedIncompleteAtEndOfStream)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel");
        EXPECT_FALSE(parser.isComplete());
        parser.endOfStream();
        EXPECT_TRUE(parser.hasFailed());
    }

    /**
     * @brief 钉住重置语义：reset() 之后可以复用同一对象解析下一条响应
     */
    TEST(HttpResponseParser, ResetAllowsReuse)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
        parser.reset();
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));
        EXPECT_EQ(parser.result().body, "abc");
    }
} // namespace AsynGyanis::Net
