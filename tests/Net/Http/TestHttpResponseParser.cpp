// HttpResponseParser 单元测试：正文定界（chunked / content-length / 连接关闭）、 跨馈送分段与拒绝面
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
     * @brief 钉住 chunked 定界：0 块 + 空行之后必须收尾完成（缺陷回归：此前的实现永远停在正文阶段，客户端会丢弃整条响应）
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
     * @brief 钉住「Transfer-Encoding 优先于 Content-Length」：两者并存按非法处理，不能挑一个信（响应走私的入口）
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
     * @brief HTTP/1.1 且既无 Transfer-Encoding 也无 Content-Length：正文按「读到连接关闭」定界
     * @details RFC 9112 §6.3 第 8 条对响应同样成立，不是因为带了 Connection: close 才如此。
     *          此前这种情况被当成「本响应没有正文」当场收尾，对端随后发来的正文被静默截成空串
     */
    TEST(HttpResponseParser, TreatsHttp11WithoutFramingHeadersAsCloseDelimited)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\n\r\nbody-by-close");
        EXPECT_FALSE(parser.isComplete()) << "无定界头的 HTTP/1.1 响应不该当场收尾";
        EXPECT_FALSE(parser.hasFailed());
        parser.endOfStream();
        EXPECT_TRUE(parser.isComplete());
        EXPECT_EQ(parser.result().body, "body-by-close") << "正文被静默截掉了";
    }

    /**
     * @brief HEAD 的应答在头块之后立即结束：不带定界头也不去等正文或关闭
     * @details RFC 9112 §6.3 第 1 条。解析器不知道请求方法，由调用方标记；不标记的话这条响应
     *          会被按「读到连接关闭」处理，keep-alive 连接上客户端永远等不到结果
     */
    TEST(HttpResponseParser, CompletesHeadResponseWithoutFramingHeadersImmediately)
    {
        HttpResponseParser parser;
        parser.markAsHeadResponse();
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"));
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_TRUE(parser.result().body.empty());
    }

    /**
     * @brief 过渡响应带的 Transfer-Encoding 不能影响随后那条真正响应的正文定界
     * @details 1xx 只是招呼，它的定界头随头部一起作废；标志若留在成员上，最终响应即便只给了
     *          Content-Length 也会被按分块解读，解析直接失败
     */
    TEST(HttpResponseParser, InterimResponseFramingHeadersDoNotLeakIntoFinalResponse)
    {
        HttpResponseParser parser;
        const std::string message = "HTTP/1.1 103 Early Hints\r\nTransfer-Encoding: chunked\r\n\r\n"
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        EXPECT_TRUE(feedAll(parser, message));
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_EQ(parser.result().body, "ok");
    }

    /**
     * @brief 多字段 Transfer-Encoding 按 RFC 9112 §6.1 合并后取最后一个编码：chunked, gzip 不是分块
     * @details 「chunked 必须在末尾」是对**合并后的列表**说的。逐条看最后一项，会把
     *          `Transfer-Encoding: chunked` + `Transfer-Encoding: gzip` 判成分块，正文随即错位
     */
    TEST(HttpResponseParser, MergesTransferEncodingFieldsBeforeCheckingChunkedIsLast)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\nraw-bytes");
        EXPECT_FALSE(parser.hasFailed()) << "合并后的末尾编码是 gzip，不该按分块解读";
        EXPECT_FALSE(parser.isComplete());
        parser.endOfStream();
        EXPECT_TRUE(parser.isComplete());
        EXPECT_EQ(parser.result().body, "raw-bytes");
    }


    /**
     * @brief 头部没有闸门就是让对端决定本端分配多少内存（正文早有 8 MiB 上限）
     * @details 三种都钉：条数、头块净字节、单行长度。客户端是「对端说了算」的那一侧，
     *          恶意或被接管的服务端可以用无限头部把客户端进程顶爆
     */
    TEST(HttpResponseParser, RejectsOversizeHeaderCountAndBlock)
    {
        HttpResponseParser parser;

        // 条数：默认上限 100 条，发 101 条即判失败
        std::string manyHeaders = "HTTP/1.1 200 OK\r\n";
        for (std::size_t index = 0; index <= HttpResponseParser::kDefaultMaximumHeaderCount; ++index)
        {
            manyHeaders += "x-" + std::to_string(index) + ": v\r\n";
        }
        manyHeaders += "\r\n";
        parser.feed(manyHeaders);
        EXPECT_TRUE(parser.hasFailed()) << "头部条数超过上限没有被拦下";

        // 头块净字节：单条头就把块长顶爆（值给到上限 + 1 字节）
        HttpResponseParser blockParser;
        std::string       bigHeader = "HTTP/1.1 200 OK\r\nx-big: " +
                                std::string(HttpResponseParser::kDefaultMaximumHeaderBlockByteCount, 'v') + "\r\n\r\n";
        blockParser.feed(bigHeader);
        EXPECT_TRUE(blockParser.hasFailed()) << "头部块总长超过上限没有被拦下";
    }

    /**
     * @brief 一行永不含 CRLF 的字节流不能把行缓冲撑到无界
     */
    TEST(HttpResponseParser, RejectsEndlessStatusLineWithoutCrlf)
    {
        HttpResponseParser parser;

        const std::string endlessLine(HttpResponseParser::kDefaultMaximumLineByteCount + 1, 'A');
        parser.feed(endlessLine);
        // 闸门在 feed() 入口看行缓冲：越界那一段落地后即刻判失败（最多多攒一次喂入的字节）
        parser.feed(endlessLine);
        EXPECT_TRUE(parser.hasFailed()) << "永不含 CRLF 的行没有被行长度闸门拦下";
    }

    /**
     * @brief 钉住无正文状态码：204 / 304 即便带 Content-Length 也立即完成、不等待正文
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
     * @brief 钉住过渡响应（1xx）的跳过语义：103 之后必须继续解析最终响应，不能拿着 103 当结果
     * @details RFC 9110 §15.2：1xx 只是最终响应之前的一声招呼，可能带自己的头部（Early Hints 就靠它
     *          捎带预加载提示）。此前的实现把它当「无正文的最终响应」当场收尾，真正的响应被整条丢弃。
     */
    TEST(HttpResponseParser, SkipsInterimResponseAndDeliversFinalOne)
    {
        HttpResponseParser parser;
        const std::string message = "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        EXPECT_TRUE(feedAll(parser, message));
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_EQ(parser.result().body, "ok");

        // 过渡响应的头部不得混进最终结果：上面那条 Link 属于 103
        for (const auto &[name, value]: parser.result().headers)
        {
            EXPECT_NE(name, "Link") << "过渡响应的头部被当成了最终响应的头部";
        }
    }

    /**
     * @brief 钉住过渡响应的跨馈送分段：1xx 与最终响应分两次喂入结果一致
     */
    TEST(HttpResponseParser, SkipsInterimResponseAcrossFeedBoundaries)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 100 Continue\r\n\r\n");
        EXPECT_FALSE(parser.isComplete());
        EXPECT_FALSE(parser.hasFailed());

        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"));
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_EQ(parser.result().body, "hello");
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
     * @brief 声明的正文长度超过上限：当场判失败，不为一条永远收不完的响应白分配缓冲
     */
    TEST(HttpResponseParser, FailsWhenDeclaredBodyExceedsLimit)
    {
        HttpResponseParser parser(16);
        parser.feed(std::string("HTTP/1.1 200 OK\r\nContent-Length: 17\r\n\r\n"));

        EXPECT_TRUE(parser.hasFailed()) << "声明长度超上限却继续等着收";
        EXPECT_FALSE(parser.isComplete());
    }

    /**
     * @brief 分块正文边收边判上限：一直喂块也不能把内存撑破
     */
    TEST(HttpResponseParser, FailsWhenChunkedBodyExceedsLimit)
    {
        HttpResponseParser parser(16);
        feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\n0123456789abcdef\r\n");

        // 第一块正好 16 字节，还没越界；再来一块就必须失败
        ASSERT_FALSE(parser.hasFailed()) << "恰好等于上限就被判失败，上限口径按「超过」算";
        parser.feed("1\r\nx\r\n");

        EXPECT_TRUE(parser.hasFailed()) << "分块正文越上限却继续累积";
    }

    /**
     * @brief 读到连接关闭定界的正文同样受上限约束：长度完全由对端决定
     */
    TEST(HttpResponseParser, FailsWhenCloseDelimitedBodyExceedsLimit)
    {
        HttpResponseParser parser(16);
        parser.feed("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n0123456789abcdef01234");

        EXPECT_TRUE(parser.hasFailed()) << "读到关闭的正文越上限却继续累积";
    }

    /**
     * @brief 上限为 0 表示不限：调用方明确要求收多大的正文都得收完
     */
    TEST(HttpResponseParser, ZeroLimitMeansUnlimited)
    {
        HttpResponseParser parser(0);
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n01234567890123456789"));
        EXPECT_EQ(parser.result().body.size(), 20u);
    }

    /**
     * @brief 钉住重置语义：reset() 之后可以复用同一对象解析下一条响应
     * @details 连「上一条是 HEAD」这一项状态也要一起清掉：keep-alive 上复用同一个解析器时，漏了它
     *          会让第二条响应也在头块之后收口——正文被静默丢掉，而状态码看着完全正常。
     */
    TEST(HttpResponseParser, ResetAllowsReuse)
    {
        HttpResponseParser parser;
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
        parser.reset();
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));
        EXPECT_EQ(parser.result().body, "abc");

        parser.reset();
        parser.markAsHeadResponse();
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n"));
        EXPECT_TRUE(parser.result().body.empty()) << "HEAD 的应答本就不该有正文";
        parser.reset();
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
        EXPECT_EQ(parser.result().body, "ok") << "reset() 没清掉 HEAD 标记：下一条带长度的响应被按 HEAD 收口了";
    }
} // namespace AsynGyanis::Net
