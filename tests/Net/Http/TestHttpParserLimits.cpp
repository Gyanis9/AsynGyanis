// TestHttpParserLimits.cpp —— HttpParserLimits 的覆盖：把各维度的限额调小后验证
//   一. 边界：恰好等于上限放行、超 1 字节按正确类别判错（URI / 头部名 / 头部值 / 头部条数 /
//       头部块总长 / 正文 / 请求行 / 分块块大小行逐项一组对照）；
//   二. 请求行的整行上限由 URI 上限推出（URI + 方法名与版本串的固定余量）：既钉住推导公式本身，
//       也钉住「只放宽 URI 一项即可放行更长的整行」；
//   三. 拒绝面：0 表示关闭该项保护（不设上限），不是「不允许任何长度」；
//   四. 出厂默认值：七个字段与推导出的请求行上限本身就是对外契约，钉在用例里防止实现漂移；
//   五. 跨连接正文预算的唯一读数 bufferedBodyByteCount()：随喂入增长、分块按解码后计、收齐后归零。
// 默认上限的用例保留在 TestHttpParser.cpp，两侧不重复；报文拼接的辅助函数与那边同口径。

#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpParseErrorKind.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 分块请求的固定头部块，与 TestHttpParserChunked.cpp 同口径
        constexpr std::string_view kChunkedHeaderBlock = "POST /chunked HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n";

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 用给定的头部行拼出一条完整 GET 报文（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeRequestTextWithHeaders;

        /**
         * @brief 生成指定条数的头部行，名互不相同
         * @param headerCount 需要的头部条数
         * @param valueLength 每条头部的值长度
         * @return std::vector<std::string> 头部行列表
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

        /**
         * @brief 断言解析以指定类别判错，且文案里出现给定的定位子串
         * @param parser 已经解析失败的解析器
         * @param expectedKind 期望的失败类别（决定上层回 400/431/413）
         * @param expectedText 期望出现在错误文案里的子串
         */
        void expectFailedWithKind(const HttpParser &parser, const HttpParseErrorKind expectedKind, const std::string_view expectedText)
        {
            EXPECT_EQ(parser.errorKind(), expectedKind) << "错误文案：" << parser.errorMessage();
            EXPECT_TRUE(parser.isLimitExceeded()) << "错误文案：" << parser.errorMessage();
            EXPECT_TRUE(containsText(parser.errorMessage(), expectedText)) << "错误文案：" << parser.errorMessage();
        }
    } // namespace

    // ============================================================================
    // 出厂默认值：运维文档与既有用例都引用这几个数字，改动必须让本用例一起失败
    // ============================================================================

    /**
     * @brief 钉住 HttpParserLimits 的默认字段值与由 URI 上限推出的请求行上限
     */
    TEST(HttpParserLimits, DefaultsKeepTheShippedValues)
    {
        const HttpParserLimits limits;

        EXPECT_EQ(limits.maximumUriLength, 8u * 1024u);
        EXPECT_EQ(limits.maximumHeaderFieldNameLength, 256u);
        EXPECT_EQ(limits.maximumHeaderFieldValueLength, 8u * 1024u);
        EXPECT_EQ(limits.maximumHeaderCount, 100u);
        EXPECT_EQ(limits.maximumHeaderBlockLength, 64u * 1024u);
        EXPECT_EQ(limits.maximumBodySize, 8u * 1024u * 1024u);
        EXPECT_EQ(limits.maximumChunkSizeLineLength, 1024u);

        // 请求行整行上限不是字段而是推导值：默认 = URI 8 KiB + 固定余量，与出厂契约的 8240 一致
        EXPECT_EQ(limits.requestLineLengthLimit(), 8u * 1024u + kRequestLineFixedOverheadBytes);
        EXPECT_EQ(limits.requestLineLengthLimit(), 8240u) << "出厂整行上限变了：运维文档与容量评估都引用这个数字";
    }

    // ============================================================================
    // 边界对照：恰好等于上限放行、超 1 字节判错，两组都断言，缺一半就说明不了边界
    // ============================================================================

    /**
     * @brief URI 上限：等于上限通过、超 1 字节判 431 类别
     */
    TEST(HttpParserLimits, SmallUriLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumUriLength = 8;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = "GET /" + std::string(7, 'a') + " HTTP/1.1\r\n\r\n";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().uri().size(), 8u);
        EXPECT_FALSE(atLimitParser.hasError());

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = "GET /" + std::string(8, 'a') + " HTTP/1.1\r\n\r\n";
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "URI");
    }

    /**
     * @brief 单个头部名上限：等于上限通过、超 1 字节判 431 类别
     */
    TEST(HttpParserLimits, SmallHeaderNameLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumHeaderFieldNameLength = 8;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({std::string(8, 'x') + ": v"});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader(std::string(8, 'x')).value_or(""), "v");

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = makeRequestTextWithHeaders({std::string(9, 'x') + ": v"});
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "头部名");
    }

    /**
     * @brief 单个头部值上限：等于上限通过、超 1 字节判 431 类别，文案里带的是配置值
     */
    TEST(HttpParserLimits, SmallHeaderValueLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumHeaderFieldValueLength = 16;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({"x-big: " + std::string(16, 'v')});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader("x-big").value_or("").size(), 16u);

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = makeRequestTextWithHeaders({"x-big: " + std::string(17, 'v')});
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "头部值");
        // 判错文案里的数字必须是这份配置的值，而不是某处的出厂常量
        EXPECT_TRUE(containsText(aboveLimitParser.errorMessage(), "上限 16 字节")) << aboveLimitParser.errorMessage();
    }

    /**
     * @brief 头部条数上限：等于上限通过、多 1 条判 431 类别
     */
    TEST(HttpParserLimits, SmallHeaderCountLimitAcceptsAtLimitAndRejectsOneAbove)
    {
        HttpParserLimits limits;
        limits.maximumHeaderCount = 3;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders(makeHeaderLines(3, 1));
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().headers().size(), 3u);

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = makeRequestTextWithHeaders(makeHeaderLines(4, 1));
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "条数");
    }

    /**
     * @brief 头部块总长上限：名与值净字节之和等于上限通过、超 1 字节判 431 类别
     */
    TEST(HttpParserLimits, SmallHeaderBlockLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumHeaderBlockLength = 32;

        // 名 "x-b" 占 3 字节，值给 29 字节时净字节正好 32
        HttpParser        atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({"x-b: " + std::string(29, 'v')});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader("x-b").value_or("").size(), 29u);

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = makeRequestTextWithHeaders({"x-b: " + std::string(30, 'v')});
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "总长");
    }

    /**
     * @brief 正文上限（Content-Length 定界）：等于上限通过、超 1 字节按声明长度当场判 413 类别
     */
    TEST(HttpParserLimits, SmallBodyLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumBodySize = 8;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = "POST /submit HTTP/1.1\r\nContent-Length: 8\r\n\r\n12345678";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().body(), "12345678");

        // 声明超 1 字节：正文字节一个都不必到就要判错，不能等收满再判
        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = "POST /submit HTTP/1.1\r\nContent-Length: 9\r\n\r\n";
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::BodyTooLarge, "上限 8 字节");
    }

    /**
     * @brief 正文上限（分块解码后累计）：等于上限通过、多 1 字节判 413 类别
     */
    TEST(HttpParserLimits, SmallBodyLimitAppliesToDecodedChunkedBody)
    {
        HttpParserLimits limits;
        limits.maximumBodySize = 8;

        HttpParser        atLimitParser(limits);
        const std::string atLimit = std::string(kChunkedHeaderBlock) + "4\r\nabcd\r\n4\r\nefgh\r\n0\r\n\r\n";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().body(), "abcdefgh");

        // 单块都没超上限，解码后合计 10 字节：按累计量判错，而不是等正文无限收下去
        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = std::string(kChunkedHeaderBlock) + "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n";
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::BodyTooLarge, "分块解码后的请求体超出上限 8 字节");
    }

    /**
     * @brief 预算读数随喂入增长、收齐后归零：它是跨连接正文预算的唯一输入
     * @details request() 要等 Done 才拿到正文，所以「收到一半时占了多少内存」只能靠这个读数；
     *          读数若在中途恒为 0，慢速正文洪水就绕过了全局预算（等看见时内存已经占住）；
     *          若 Done 之后不归零，流水线里的下一条请求会背上上一条的残留而被误拒。
     */
    TEST(HttpParserLimits, BufferedBodyReadingGrowsWhileFeedingAndZeroesAfterDone)
    {
        HttpParser parser;

        const std::string firstPart = "POST /submit HTTP/1.1\r\nContent-Length: 8\r\n\r\n1234";
        ASSERT_EQ(parser.parse(firstPart.data(), firstPart.size()), ParseStatus::NeedMore);
        EXPECT_EQ(parser.bufferedBodyByteCount(), 4U) << "已喂进来的 4 字节正文没被读数算进去，预算挡不住半截正文";

        const std::string secondPart = "5678";
        ASSERT_EQ(parser.parse(secondPart.data(), secondPart.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().body().size(), 8U);
        // 正文已移交请求对象，读数交回 0；此时该由 request().body().size() 接着记账
        EXPECT_EQ(parser.bufferedBodyByteCount(), 0U) << "收齐后读数没归零：读数与请求对象里那份正文会被重复计一次";
    }

    /**
     * @brief 分块请求的预算读数按解码后字节计，不含长度行与 CRLF 这些帧开销
     * @details 「分块按解码后的字节数计」这条口径此前只在拒绝面（超上限判 413）上钉过，读数本身没钉。
     *          按线上字节计会让「小块多帧」的写法把额度虚报掉好几倍，同一份预算实际能收的正文反而变小。
     */
    TEST(HttpParserLimits, BufferedBodyReadingCountsDecodedChunkedBytesNotWireFraming)
    {
        HttpParser parser;

        // 14 个线上字节里只有 2 字节是正文，其余是分块长度行与 CRLF
        const std::string chunkedFrames = "1\r\na\r\n1\r\nb\r\n";
        const std::string firstPart     = std::string(kChunkedHeaderBlock) + chunkedFrames;
        ASSERT_EQ(parser.parse(firstPart.data(), firstPart.size()), ParseStatus::NeedMore);
        EXPECT_EQ(parser.bufferedBodyByteCount(), 2U) << "读数把分块的帧开销也算进了正文（线上 " << chunkedFrames.size() << " 字节 / 解码后 2 字节）";

        const std::string secondPart = "1\r\nc\r\n0\r\n\r\n";
        ASSERT_EQ(parser.parse(secondPart.data(), secondPart.size()), ParseStatus::Done);
        EXPECT_EQ(parser.request().body(), "abc");
        EXPECT_EQ(parser.bufferedBodyByteCount(), 0U);
    }

    /**
     * @brief 请求行上限随 URI 上限推出：等于上限仍要更多字节、超 1 字节判 431
     * @details 整行上限只在「一行始终不结束」时生效（整行长度）。这里只把 URI 上限调小到 16，
     *          整行上限即 16 + 固定余量；用例按该推导值构造边界输入，因此同时钉住「推导公式」与
     *          「解析器确实按推导值判错」两件事。
     */
    TEST(HttpParserLimits, RequestLineLimitFollowsUriLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumUriLength = 16;

        const std::size_t requestLineLimit = 16u + kRequestLineFixedOverheadBytes;
        ASSERT_EQ(limits.requestLineLengthLimit(), requestLineLimit) << "整行上限应等于 URI 上限加固定余量";

        // 16 字节 URI 上限对应的整行上限，且没有 CRLF 的半行：没有收齐一行，只能停 NeedMore
        HttpParser        atLimitParser(limits);
        const std::string atLimit = "GET /" + std::string(requestLineLimit - 5, 'a');
        ASSERT_EQ(atLimit.size(), requestLineLimit);
        EXPECT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(atLimitParser.hasError());

        // 只多一个字节：整行上限是硬边界，多 1 字节即按 431 类别判错
        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = atLimit + "a";
        ASSERT_EQ(aboveLimit.size(), requestLineLimit + 1);
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "请求行");
    }

    /**
     * @brief 只放宽 URI 上限一项即可放行更长的请求行：整行闸门随 URI 上限自动放宽
     * @details 同一份 12 KiB 的半行输入，在出厂限额下撞整行上限被判 431，在只放宽 maximumUriLength
     *          的限额下则只是「还没收齐」。两侧差异只来自这一项配置，因此这条用例就是「整行上限
     *          随 URI 上限自动放宽」的判据。
     */
    TEST(HttpParserLimits, RaisingUriLimitAlsoRaisesTheRequestLineBound)
    {
        const std::string halfRequestLine = "GET /" + std::string(12u * 1024u, 'a');

        // 出厂限额：URI 档 8 KiB，整行上限 8 KiB + 固定余量，12 KiB 的半行先撞整行闸门
        HttpParser defaultParser;
        ASSERT_EQ(defaultParser.parse(halfRequestLine.data(), halfRequestLine.size()), ParseStatus::Error);
        expectFailedWithKind(defaultParser, HttpParseErrorKind::HeaderTooLarge, "请求行");

        HttpParserLimits relaxedLimits;
        relaxedLimits.maximumUriLength = 16u * 1024u; // 只放宽这一项
        HttpParser relaxedParser(relaxedLimits);
        EXPECT_EQ(relaxedParser.parse(halfRequestLine.data(), halfRequestLine.size()), ParseStatus::NeedMore) << relaxedParser.errorMessage();
        EXPECT_FALSE(relaxedParser.hasError()) << relaxedParser.errorMessage();
    }

    /**
     * @brief 分块块大小行上限：等于上限仍要更多字节、超 1 字节按正文过大判 413
     */
    TEST(HttpParserLimits, SmallChunkSizeLineLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumChunkSizeLineLength = 16;

        // 头部块之后是一行没有 CRLF 的块大小行（"5;" 加扩展）：长度 16 时只能停 NeedMore
        HttpParser        atLimitParser(limits);
        const std::string atLimit = std::string(kChunkedHeaderBlock) + "5;" + std::string(14, 'a');
        ASSERT_EQ(atLimit.substr(kChunkedHeaderBlock.size()).size(), 16u);
        EXPECT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(atLimitParser.hasError());

        HttpParser        aboveLimitParser(limits);
        const std::string aboveLimit = std::string(kChunkedHeaderBlock) + "5;" + std::string(15, 'a');
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::BodyTooLarge, "分块块大小行");
    }

    // ============================================================================
    // 拒绝面：0 的语义是「关闭该项保护」，不是「什么都不允许」
    // ============================================================================

    /**
     * @brief 把条数 / 头部值 / 正文三项设为 0 后，超出各默认档口的报文照样能收完
     *
     * @details 反向对照是同一份报文配一台小限额解析器：它是必被拒的。两侧差异只来自配置，
     *          因此「0 = 关闭该项保护」与「0 = 不允许任何长度」两种语义在这里是可区分的。
     */
    TEST(HttpParserLimits, ZeroDisablesTheProtectionInsteadOfRejectingEverything)
    {
        HttpParserLimits relaxed;
        relaxed.maximumHeaderCount            = 0; // 头部条数不限
        relaxed.maximumHeaderFieldValueLength = 0; // 单个头部值不限
        relaxed.maximumBodySize               = 0; // 正文不限

        HttpParserLimits strict;
        strict.maximumHeaderCount            = 3;
        strict.maximumHeaderFieldValueLength = 16;
        strict.maximumBodySize               = 8;

        // 5 条头部（多于 3 条档口）、其中一条的值 9000 字节（远超 8 KiB 出厂档口）、正文 64 字节
        const std::vector<std::string> headerLines{"x-a: 1", "x-b: 2", "x-c: 3", "x-d: 4", "x-blob: " + std::string(9000, 'v')};

        std::string message = "POST /indexed HTTP/1.1\r\n";
        for (const std::string &headerLine: headerLines)
        {
            message.append(headerLine);
            message.append("\r\n");
        }
        message.append("Content-Length: 64\r\n\r\n");
        message.append(64, 'b');

        HttpParser relaxedParser(relaxed);
        ASSERT_EQ(relaxedParser.parse(message.data(), message.size()), ParseStatus::Done) << relaxedParser.errorMessage();
        // 五条探测头部加一条 Content-Length 声明：它们在受限解析器上会撞不同的闸口
        EXPECT_EQ(relaxedParser.request().headers().size(), 6u);
        EXPECT_EQ(relaxedParser.request().getHeader("x-blob").value_or("").size(), 9000u);
        EXPECT_EQ(relaxedParser.request().body().size(), 64u);
        EXPECT_FALSE(relaxedParser.isLimitExceeded());

        HttpParser strictParser(strict);
        ASSERT_EQ(strictParser.parse(message.data(), message.size()), ParseStatus::Error);
        EXPECT_TRUE(strictParser.isLimitExceeded());
        EXPECT_TRUE(containsText(strictParser.errorMessage(), "上限")) << strictParser.errorMessage();
    }
} // namespace AsynGyanis::Net
