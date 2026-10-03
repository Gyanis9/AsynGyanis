// HttpResponseParser 单元测试：正文定界（chunked / content-length / 连接关闭）、 跨馈送分段与拒绝面
#include "Net/Http/Client/HttpResponseParser.h"

#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http2/Http2ClientConnection.h"
#include "Net/Http3/Http3ClientConnection.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        const std::string  message = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
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
        const std::string  message = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
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
     * @brief 钉住：本端拆不掉的传输编码链按报文不合规拒绝，不再退回「读到连接关闭」
     * @details 本框架只实现 chunked 这一种传输编码。`gzip, chunked` 拆完块还留着一层没人拆的 gzip，
     *          `chunked, gzip` 更是连长度都定不了——两种旧行为都把一层原始字节当成正文交给调用方，
     *          且一条错都不报。入站侧（`HttpParser`）一直按「恰好一个 chunked」判死，
     *          现在两个方向共用那一份判据（`isSingleChunkedEncoding`）。
     *          正向对照由 `DecodesChunkedBodyAndCompletes` 与 `AcceptsChunkedSpellingRegardlessOfCase` 把着
     */
    TEST(HttpResponseParser, RejectsTransferEncodingChainItCannotUndo)
    {
        for (const std::string_view encodingBlock: {std::string_view{"Transfer-Encoding: gzip, chunked"}, std::string_view{"Transfer-Encoding: chunked, gzip"},
                                                    // 重复出现也算：这与入站对 `Transfer-Encoding: chunked` 写两次的判法一致
                                                    std::string_view{"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked"}})
        {
            HttpResponseParser parser;
            parser.feed("HTTP/1.1 200 OK\r\n" + std::string(encodingBlock) + "\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
            EXPECT_TRUE(parser.hasFailed()) << encodingBlock;
            EXPECT_FALSE(parser.isComplete()) << encodingBlock;
        }
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
     * @brief 钉住块扩展：语法要判，内容照旧忽略
     * @details 扩展不参与块边界计算，因此忽略它的内容是对的；但 RFC 9112 §7.1.1 要求每段写成
     *          「;名字」或「;名字=值」且名字是 token，接收方得先验语法再忽略。入站那一路一直是拒的，
     *          出站先前直接不看——一个坏服务器就能用非法扩展把本端一路按分块读下去（缓存投毒与
     *          请求分裂的入口）。两处现在共用同一份判据。
     */
    TEST(HttpResponseParser, ValidatesChunkExtensionSyntaxButIgnoresItsContent)
    {
        HttpResponseParser ok;
        EXPECT_TRUE(feedAll(ok, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;a=1\r\nhello\r\n0;b=\"x y\"\r\n\r\n")) << "合法扩展（token 值与带引号值）把报文判成了失败";
        EXPECT_EQ(ok.result().body, "hello") << "扩展的内容没被忽略，混进正文了";

        HttpResponseParser noName;
        feedAll(noName, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5; =1\r\nhello\r\n0\r\n\r\n");
        EXPECT_TRUE(noName.hasFailed()) << "扩展没有 token 名也照样放行";

        HttpResponseParser unclosedQuote;
        feedAll(unclosedQuote, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;a=\"1\r\nhello\r\n0\r\n\r\n");
        EXPECT_TRUE(unclosedQuote.hasFailed()) << "引号没闭合的扩展值也照样放行";
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
        const std::string  message = "HTTP/1.1 103 Early Hints\r\nTransfer-Encoding: chunked\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        EXPECT_TRUE(feedAll(parser, message));
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_EQ(parser.result().body, "ok");
    }

    /**
     * @brief 多字段 Transfer-Encoding 先按 RFC 9112 §6.1 合并，再作为一条编码链整体裁决
     * @details 「chunked 必须在链末尾」是对**合并后的列表**说的。逐条看，
     *          `Transfer-Encoding: chunked` + `Transfer-Encoding: gzip` 里的第一条会单独通过，
     *          正文就被按分块解读而错位——这条测试存在的理由一直是「先合并」。
     *          合并之后本端拆不掉这条链（只实现 chunked 这一种传输编码），因此现在判的是失败，
     *          不再是「按读到连接关闭收下原始字节」：旧断言要求把 gzip 套 chunked 的字节当正文交出，
     *          那与入站侧明写的「绝不悄悄按 identity 处理」相反（同一判据见 isSingleChunkedEncoding）。
     */
    TEST(HttpResponseParser, MergesTransferEncodingFieldsBeforeJudgingTheChain)
    {
        HttpResponseParser parser;
        parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\nraw-bytes");
        EXPECT_TRUE(parser.hasFailed()) << "合并后的链里有本端拆不掉的编码：既不能按分块解读，也不该把原始字节当正文交出";
        EXPECT_FALSE(parser.isComplete());
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
        std::string        bigHeader = "HTTP/1.1 200 OK\r\nx-big: " + std::string(HttpResponseParser::kDefaultMaximumHeaderBlockByteCount, 'v') + "\r\n\r\n";
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
        const std::string  message = "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
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
     * @brief 上限可以在建好之后改：池按自己的配置落在连接上，靠的就是这一句
     * @details 只验 setter 与两个读数是否自洽：`isBodyOverLimit()` 是失败原因的判据（「本端胃口有限」
     *          与「对端发了不合规范的报文」得能分开说），所以它必须跟着新上限走，而不是留在构造时
     *          那个默认值上。
     */
    TEST(HttpResponseParser, MaximumBodySizeIsConfigurableAfterConstruction)
    {
        HttpResponseParser parser;
        EXPECT_EQ(parser.maximumBodySize(), HttpResponseParser::kDefaultMaximumBodySize);
        EXPECT_FALSE(parser.isBodyOverLimit()) << "还没喂过字节就报越界";

        parser.setMaximumBodySize(8U);
        EXPECT_EQ(parser.maximumBodySize(), 8U);
        // 声明的 20 字节本身就越过 8：一条永远收不完的响应不该白分配缓冲，整条按失败收口
        EXPECT_FALSE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n01234567890123456789"));
        EXPECT_TRUE(parser.hasFailed());
        EXPECT_TRUE(parser.isBodyOverLimit()) << "改过的上限没生效，或越界与不合规范分不开";

        parser.reset();
        parser.setMaximumBodySize(0U);
        EXPECT_TRUE(feedAll(parser, "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n01234567890123456789"));
        EXPECT_FALSE(parser.isBodyOverLimit()) << "填 0 是不限，越界判据得跟着闭嘴";
    }

    /**
     * @brief 钉住：两条通路与池三处的「默认正文上限」是同一个数
     * @details 默认值写在三处（HTTP/1.1 的解析器、池的配置、h2 客户端的连接），而使用方只看到
     *          「HttpClient 愿意收多大的响应」这一件事：一旦三处分叉，同一个请求在明文 h1 上能成、
     *          到 h2 上就莫名失败（或反过来）。这条用例就是让那种分叉当场红，而不是留给人去对代码。
     */
    TEST(HttpResponseParser, ResponseBodyDefaultsAgreeAcrossBothTransports)
    {
        EXPECT_EQ(HttpOutboundConnectionPool::kDefaultMaximumResponseBodyBytes, HttpResponseParser::kDefaultMaximumBodySize) << "池与 h1 解析器的默认档分叉了";
        EXPECT_EQ(Http2ClientConnection::kDefaultMaximumResponseBodyBytes, HttpResponseParser::kDefaultMaximumBodySize) << "h2 与 h1 的默认档分叉了：换协议就换胃口";
        EXPECT_EQ(Http3ClientConnection::kDefaultMaximumResponseBodyBytes, HttpResponseParser::kDefaultMaximumBodySize) << "h3 与 h1 的默认档分叉了：三条通路共用一份胃口才对";
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

    /**
     * @brief 钉住出站方向也拒绝折行、非法头名与非法头值——过去这几条只有入站有
     * @details 旧实现缺冒号时把**整行**当成头名塞进结果，上层按名字取头就可能读到一个对端根本没发过的
     *          字段；折行（obs-fold）与冒号前带空白的名字同样照收。同一串字节在入站解析里是明确拒绝的，
     *          两个方向给出不同结论就是中转分歧的入口（缓存投毒、请求走私都从这里进去）。
     *          值这一半是同一处分歧的另一面：控制字符进了 headers 就再也发不出去（写头的闸门拒同一批
     *          字节），而下游日志会把它原样打出去。
     */
    TEST(HttpResponseParser, RejectsFoldedMalformedHeaderLinesAndBadFieldValues)
    {
        struct MalformedCase
        {
            const char *text;
            const char *why;
        };
        const MalformedCase cases[] = {
                {"HTTP/1.1 200 OK\r\nX-A: 1\r\n continuation\r\n\r\n", "折行（obs-fold）头部"},
                {"HTTP/1.1 200 OK\r\nno-colon-line\r\n\r\n", "缺冒号的头部行"},
                {"HTTP/1.1 200 OK\r\n: v\r\n\r\n", "冒号在首位（空名）"},
                {"HTTP/1.1 200 OK\r\nBad Name: v\r\n\r\n", "冒号前带空白"},
        };
        for (const MalformedCase &item: cases)
        {
            HttpResponseParser parser;
            feedAll(parser, item.text);
            EXPECT_TRUE(parser.hasFailed()) << item.why;
        }

        // 值里的控制字符：NUL 与 DEL 都是入站明确拒绝、出站原先照收的形态。裸 CR 单独一档——
        // 行是按 CRLF 切的，值里剩下的那个 CR 说明对端在行内塞了控制字符
        std::string nulInValue = "HTTP/1.1 200 OK\r\nX-A: a";
        nulInValue.push_back('\0'); // 按字节拼：字面量直接初始化 std::string 会在 NUL 处被 strlen 截掉，那条就白喂了
        nulInValue += "b\r\n\r\n";
        const std::string_view bareCrInValue = "HTTP/1.1 200 OK\r\nX-A: a\rb\r\n\r\n";
        const std::string_view delInValue    = "HTTP/1.1 200 OK\r\nX-A: a\x7f"
                                               "b\r\n\r\n";
        for (const std::string_view valueCase: {std::string_view(nulInValue), bareCrInValue, delInValue})
        {
            HttpResponseParser parser;
            feedAll(parser, valueCase);
            EXPECT_TRUE(parser.hasFailed()) << "值里的控制字符被照收（" << valueCase.size() << " 字节那条）";
        }

        // 反向对照：obs-text（RFC 9110 §5.5 放行 0x80-0xFF）不能被这道闸误挡——真实服务器会把
        // 未转码的 UTF-8 直接写进值里，挡下来就等于把好好的响应判成畸形
        HttpResponseParser obsText;
        EXPECT_TRUE(feedAll(obsText, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Obs: \xC3\xA9\r\n\r\n"));
        EXPECT_FALSE(obsText.hasFailed()) << "obs-text 被当成非法值挡了";

        // trailer 段与头部共用同一处判据：值里带控制字符的那条 trailer 也要拒
        HttpResponseParser badTrailer;
        feedAll(badTrailer, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-T: a\rb\r\n\r\n");
        EXPECT_TRUE(badTrailer.hasFailed()) << "trailer 段的值没走同一道闸";

        // 正向对照：合法的行照收，值的前导空白按 RFC 9112 去掉
        HttpResponseParser ok;
        EXPECT_TRUE(feedAll(ok, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-A:    spaced-value\r\n\r\n"));
        EXPECT_FALSE(ok.hasFailed());
        const bool found =
                std::any_of(ok.result().headers.begin(), ok.result().headers.end(), [](const auto &field) { return field.first == "X-A" && field.second == "spaced-value"; });
        EXPECT_TRUE(found) << "值的前导空白没被去掉，或头部没收到";
    }

    /**
     * @brief 钉住 trailer 段与头部共用同一套判据
     * @details trailer 在报文 framing 之内：折行或缺冒号的 trailer 行说明这条流已经错位，
     *          不能因为「反正要丢掉」就放过——放过等于让对端用一段 trailer 决定本端在哪里收口。
     */
    TEST(HttpResponseParser, RejectsMalformedTrailerLines)
    {
        HttpResponseParser folded;
        feedAll(folded, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n continuation\r\n\r\n");
        EXPECT_TRUE(folded.hasFailed()) << "trailer 段的折行被放过了";

        // 这一条才是「trailer 与头部共用判据」的钉子：它有冒号、也不是折行，旧判据（只看有没有冒号）
        // 会放过，收紧后必须拒——否则对端可以用一段非法头名的 trailer 影响本端的收口判断
        HttpResponseParser badName;
        feedAll(badName, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\nBad Name: v\r\n\r\n");
        EXPECT_TRUE(badName.hasFailed()) << "trailer 段没校验头名字符集，冒号前带空白的行被放过了";

        HttpResponseParser wellFormed;
        EXPECT_TRUE(feedAll(wellFormed, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\nX-Trailer: v\r\n\r\n"));
        EXPECT_TRUE(wellFormed.isComplete());
        EXPECT_EQ(wellFormed.result().body, "a") << "trailer 段之后的正文被改写了";
    }

    /**
     * @brief 状态行里的状态码必须是恰好三位数字，形状不合就整条响应判失败而不是折出一个数
     * @details 旧写法取前三字符喂 atoi：`HTTP/1.1 abc OK` 得到 0、`HTTP/1.1 2000 OK` 得到 2000、
     *          `HTTP/1.1 20 OK` 得到 20——上层按「2xx 才算成功」分支时，这些假号与真号无法区分，
     *          而报文里从来没写过它们。判据与 h2/h3 的 `:status` 共用 parseStatusCodeText 一处。
     *          正向对照两则：带原因短语的常规状态行，以及**没有原因短语**的状态行
     *          （RFC 9112 §9 里 Reason-Phrase 是可选的，收紧状态码不该把它一起挡掉）。
     */
    TEST(HttpResponseParser, RejectsStatusCodeTextThatIsNotThreeDigits)
    {
        HttpResponseParser parser;
        EXPECT_FALSE(feedAll(parser, "HTTP/1.1 abc OK\r\nContent-Length: 0\r\n\r\n")) << "非数字状态码被折成了一个数";
        parser.reset();
        EXPECT_FALSE(feedAll(parser, "HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n")) << "四位数字被截成 200 收下";
        parser.reset();
        EXPECT_FALSE(feedAll(parser, "HTTP/1.1 20 OK\r\nContent-Length: 0\r\n\r\n")) << "两位不是状态码";
        parser.reset();
        EXPECT_FALSE(feedAll(parser, "HTTP/1.1  200 OK\r\nContent-Length: 0\r\n\r\n")) << "状态码前多一个空格也不该被当作可忽略的空白";

        parser.reset();
        ASSERT_TRUE(feedAll(parser, "HTTP/1.1 204 No Content\r\n\r\n")) << "常规状态行被误拒";
        EXPECT_EQ(parser.result().statusCode, 204);

        parser.reset();
        ASSERT_TRUE(feedAll(parser, "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n")) << "原因短语是可选的（RFC 9112 §9）";
        EXPECT_EQ(parser.result().statusCode, 200);
        EXPECT_TRUE(parser.result().reasonPhrase.empty());
    }
    /**
     * @brief 钉住：chunked 的 trailer 段收进 `trailers` 而不是丢掉，并与头部共用同一道配额账
     * @details 过去这一支只校验形态就丢弃——正文之后才知道的结果（校验和、最终状态）调用方压根读不到，
     *          而那正是 chunked trailer 存在的理由（RFC 9112 §7.1.2）。收下来就要记账：条数与净字节两道
     *          闸门按**整条报文**累计判，与入站侧（h1 解析器、h2 会话、h3 会话）同一条口径；否则「把字段
     *          拆进 trailer 段」就成了本端这两道闸的绕过口。
     */
    TEST(HttpResponseParser, CapturesTrailerFieldsAndCountsThemIntoTheHeaderBudget)
    {
        {
            const std::string  message = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
                                         "3\r\nabc\r\n0\r\nx-checksum: 616263\r\nx-result: ok\r\n\r\n";
            HttpResponseParser parser;
            ASSERT_TRUE(feedAll(parser, message));
            EXPECT_EQ(parser.result().body, "abc");
            EXPECT_EQ(parser.result().headers.size(), 2U) << "trailer 段的字段不该混进响应头部";
            ASSERT_EQ(parser.result().trailers.size(), 2U) << "两条尾部字段都要收下来，而不是校验完就丢";
            EXPECT_EQ(parser.result().trailers[0].first, "x-checksum");
            EXPECT_EQ(parser.result().trailers[0].second, "616263") << "值按到达顺序原样留着（前导 OWS 去掉）";
            EXPECT_EQ(parser.result().trailers[1].first, "x-result") << "两条的到达顺序也要保持";
        }
        {
            // 配额按整条报文累计：`Transfer-Encoding` + 98 条附加头 = 99 条头部，出厂条数上限 100，
            // 于是第一条 trailer 恰好占满、第二条越限
            std::string message = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n";
            for (std::size_t index = 0; index < 98U; ++index)
            {
                message += "x-h" + std::to_string(index) + ": v\r\n";
            }
            message += "\r\n3\r\nabc\r\n0\r\nx-a: 1\r\nx-b: 2\r\n\r\n";

            HttpResponseParser parser;
            static_cast<void>(parser.feed(message));
            EXPECT_FALSE(parser.isComplete()) << "trailer 段与头部不共用条数闸门：这道闸能被拆块绕过";
            EXPECT_TRUE(parser.hasFailed()) << "越限要判这条流不对，而不是默默收下";
        }
    }
} // namespace AsynGyanis::Net
