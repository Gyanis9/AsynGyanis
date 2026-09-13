// TestHttpParserLimits.cpp —— HttpParserLimits 的覆盖：把各维度的限额调小后验证
//   一. 边界：恰好等于上限放行、超 1 字节按正确类别判错（URI / 头部名 / 头部值 / 头部条数 /
//       头部块总长 / 正文 / 请求行 / 分块块大小行逐项一组对照）；
//   二. 拒绝面：0 表示关闭该项保护（不设上限），不是「不允许任何长度」；
//   三. 出厂默认值：八个数字本身就是对外契约，钉在用例里防止实现漂移。
// 默认上限的用例保留在 TestHttpParser.cpp，两侧不重复；报文拼接的辅助函数与那边同口径。

#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpParseErrorKind.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

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
        constexpr std::string_view kChunkedHeaderBlock =
                "POST /chunked HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n";

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
    // 出厂默认值：运维文档与既有用例都引用这八个数字，改动必须让本用例一起失败
    // ============================================================================

    /**
     * @brief 钉住 HttpParserLimits 的默认字段值
     */
    TEST(HttpParserLimits, DefaultsKeepTheShippedValues)
    {
        const HttpParserLimits limits;

        EXPECT_EQ(limits.maximumRequestLineLength, 8u * 1024u + 32u + 16u);
        EXPECT_EQ(limits.maximumUriLength, 8u * 1024u);
        EXPECT_EQ(limits.maximumHeaderFieldNameLength, 256u);
        EXPECT_EQ(limits.maximumHeaderFieldValueLength, 8u * 1024u);
        EXPECT_EQ(limits.maximumHeaderCount, 100u);
        EXPECT_EQ(limits.maximumHeaderBlockLength, 64u * 1024u);
        EXPECT_EQ(limits.maximumBodySize, 8u * 1024u * 1024u);
        EXPECT_EQ(limits.maximumChunkSizeLineLength, 1024u);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = "GET /" + std::string(7, 'a') + " HTTP/1.1\r\n\r\n";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().uri().size(), 8u);
        EXPECT_FALSE(atLimitParser.hasError());

        HttpParser aboveLimitParser(limits);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({std::string(8, 'x') + ": v"});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader(std::string(8, 'x')).value_or(""), "v");

        HttpParser aboveLimitParser(limits);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({"x-big: " + std::string(16, 'v')});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader("x-big").value_or("").size(), 16u);

        HttpParser aboveLimitParser(limits);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders(makeHeaderLines(3, 1));
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().headers().size(), 3u);

        HttpParser aboveLimitParser(limits);
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
        HttpParser atLimitParser(limits);
        const std::string atLimit = makeRequestTextWithHeaders({"x-b: " + std::string(29, 'v')});
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().getHeader("x-b").value_or("").size(), 29u);

        HttpParser aboveLimitParser(limits);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = "POST /submit HTTP/1.1\r\nContent-Length: 8\r\n\r\n12345678";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().body(), "12345678");

        // 声明超 1 字节：正文字节一个都不必到就要判错，不能等收满再判
        HttpParser aboveLimitParser(limits);
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

        HttpParser atLimitParser(limits);
        const std::string atLimit = std::string(kChunkedHeaderBlock) + "4\r\nabcd\r\n4\r\nefgh\r\n0\r\n\r\n";
        ASSERT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::Done);
        EXPECT_EQ(atLimitParser.request().body(), "abcdefgh");

        // 单块都没超上限，解码后合计 10 字节：按累计量判错，而不是等正文无限收下去
        HttpParser aboveLimitParser(limits);
        const std::string aboveLimit = std::string(kChunkedHeaderBlock) + "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n";
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::BodyTooLarge, "分块解码后的请求体超出上限 8 字节");
    }

    /**
     * @brief 请求行上限：只在「一行始终不结束」时生效（整行长度）——等于上限仍要更多字节、超 1 字节判 431
     */
    TEST(HttpParserLimits, SmallRequestLineLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumRequestLineLength = 16;

        // 16 字节且没有 CRLF 的半行：没有收齐一行，只能停 NeedMore，不该判错
        HttpParser atLimitParser(limits);
        const std::string atLimit = "GET /aaaaaaaaaaa";
        ASSERT_EQ(atLimit.size(), 16u);
        EXPECT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(atLimitParser.hasError());

        HttpParser aboveLimitParser(limits);
        const std::string aboveLimit = "GET /aaaaaaaaaaaa";
        ASSERT_EQ(aboveLimit.size(), 17u);
        ASSERT_EQ(aboveLimitParser.parse(aboveLimit.data(), aboveLimit.size()), ParseStatus::Error);
        expectFailedWithKind(aboveLimitParser, HttpParseErrorKind::HeaderTooLarge, "请求行");
    }

    /**
     * @brief 分块块大小行上限：等于上限仍要更多字节、超 1 字节按正文过大判 413
     */
    TEST(HttpParserLimits, SmallChunkSizeLineLimitAcceptsAtLimitAndRejectsOneByteAbove)
    {
        HttpParserLimits limits;
        limits.maximumChunkSizeLineLength = 16;

        // 头部块之后是一行没有 CRLF 的块大小行（"5;" 加扩展）：长度 16 时只能停 NeedMore
        HttpParser atLimitParser(limits);
        const std::string atLimit = std::string(kChunkedHeaderBlock) + "5;" + std::string(14, 'a');
        ASSERT_EQ(atLimit.substr(kChunkedHeaderBlock.size()).size(), 16u);
        EXPECT_EQ(atLimitParser.parse(atLimit.data(), atLimit.size()), ParseStatus::NeedMore);
        EXPECT_FALSE(atLimitParser.hasError());

        HttpParser aboveLimitParser(limits);
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
        const std::vector<std::string> headerLines{
                "x-a: 1", "x-b: 2", "x-c: 3", "x-d: 4", "x-blob: " + std::string(9000, 'v')};

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
