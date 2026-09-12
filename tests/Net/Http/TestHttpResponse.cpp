/**
 * @file TestHttpResponse.cpp
 * @brief HttpResponse 单元测试：头部写入校验、可重复头部模型、序列化顺序、自动补齐（date/content-type/content-length）
 *        与映射正文（整份与区间）的字节精确性
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpResponse.h"

#include "Platform/IO/MemoryMappedFile.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// UTF-8 编码的「中」字，3 字节：用来钉住 content-length 算的是字节数而非字符数
        static constexpr std::string_view kThreeByteUtf8Character{"\xe4\xb8\xad", 3};

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
         * @brief 取子串在文本中的位置，便于断言序列化先后顺序
         * @param haystack 待搜索文本
         * @param needle   目标子串
         * @return std::size_t 首次出现位置；未命中返回 std::string::npos
         */
        std::size_t positionOfText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle);
        }

        /**
         * @brief 摘掉报文里的自动 date 行，便于对其余部分做逐字节比对
         *
         * @details date 头取自当前时刻，断言里写不死；用例关心的是报文其余部分的字节形态，
         *          因此先把这一行移除再比对。报文里没有 date 行时原样返回。
         * @param message 完整报文文本
         * @return 移除 date 行之后的文本
         */
        std::string withoutDateHeaderLine(const std::string &message)
        {
            const std::size_t datePosition = message.find("\r\ndate: ");
            if (datePosition == std::string::npos)
            {
                return message;
            }
            const std::size_t lineStart = datePosition + 2;
            const std::size_t lineEnd = message.find("\r\n", lineStart);
            if (lineEnd == std::string::npos)
            {
                return message;
            }
            std::string stripped = message;
            stripped.erase(lineStart, lineEnd + 2 - lineStart);
            return stripped;
        }

        /**
         * @brief 临时文件夹具：给「映射正文」的用例提供一份磁盘上的真实文件
         *
         * @details 文件内容按二进制写入（文本模式会在 Windows 上把换行翻译成 CRLF，
         *          正文长度随之失真）；析构时递归删除整个临时目录，失败退出也能清理干净。
         */
        class TemporaryFile
        {
        public:
            /**
             * @brief 创建临时目录并写入待映射的文件（文件名为 body.bin）
             * @param namePrefix 便于调试的用途前缀
             * @param content 文件内容
             */
            TemporaryFile(const std::string &namePrefix, const std::string_view content)
            {
                static std::atomic<unsigned int> sequenceCounter{0};

                const std::string salt = std::to_string(
                                                 std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                         std::to_string(sequenceCounter.fetch_add(1));
                m_directory = std::filesystem::temp_directory_path() / ("AsynGyanis_HttpResponse_" + namePrefix + "_" + salt);

                std::error_code error;
                std::filesystem::create_directories(m_directory, error);
                m_filePath = m_directory / "body.bin";

                std::ofstream file(m_filePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (file.is_open())
                {
                    file.write(content.data(), static_cast<std::streamsize>(content.size()));
                }
            }

            ~TemporaryFile()
            {
                std::error_code error;
                std::filesystem::remove_all(m_directory, error);
            }

            TemporaryFile(const TemporaryFile &) = delete;

            TemporaryFile &operator=(const TemporaryFile &) = delete;

            /**
             * @brief 文件路径
             * @return const std::filesystem::path& 映射用的文件绝对路径
             */
            [[nodiscard]] const std::filesystem::path &path() const noexcept
            {
                return m_filePath;
            }

        private:
            std::filesystem::path m_directory; ///< 本次用例独占的临时目录
            std::filesystem::path m_filePath;  ///< 目录内待映射的文件
        };
    } // namespace

    // ============================================================================
    // 状态行与默认值
    // ============================================================================

    TEST(HttpResponse, DefaultsToOkayResponseWithEmptyBody)
    {
        const HttpResponse response;

        EXPECT_EQ(response.status(), 200);
        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(response.headers().empty());

        const std::string output = response.toString();
        // 空正文仍要交代边界：状态行 + 自动补的 content-length + 自动补的 date
        EXPECT_TRUE(containsText(output, "content-length: 0\r\n"));
        EXPECT_TRUE(containsText(output, "\r\ndate: "));
        // 摘掉取自当前时刻的 date 行后，其余部分逐字节恒定
        EXPECT_EQ(withoutDateHeaderLine(output), "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n");
    }

    TEST(HttpResponse, WritesStandardReasonPhraseForKnownStatus)
    {
        HttpResponse response;
        response.setStatus(404);

        const std::string output = response.toString();

        EXPECT_TRUE(output.starts_with("HTTP/1.1 404 Not Found\r\n"));
    }

    TEST(HttpResponse, WritesEmptyReasonPhraseForUnknownStatus)
    {
        HttpResponse response;
        response.setHttpVersion("HTTP/1.0");
        response.setStatus(799);

        const std::string output = response.toString();

        // 未收录状态码给空原因短语，RFC 9110 §3.1.2 允许状态行以「空格 + CRLF」收尾
        EXPECT_TRUE(output.starts_with("HTTP/1.0 799 \r\n"));
    }

    // ============================================================================
    // 头部写入校验：非法值一律拒写且不动已有状态
    // ============================================================================

    TEST(HttpResponse, AcceptsValidHeaderAndStoresNameLowercased)
    {
        HttpResponse response;

        EXPECT_TRUE(response.setHeader("Content-Type", "text/html"));

        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/html");
        EXPECT_EQ(response.getHeader("CONTENT-TYPE").value_or(""), "text/html");
        EXPECT_EQ(response.headers().count("Content-Type"), 0U);
        EXPECT_TRUE(containsText(response.toString(), "content-type: text/html\r\n"));
    }

    TEST(HttpResponse, RejectsInjectedHeaderLineInsideHeaderValue)
    {
        HttpResponse response;
        ASSERT_TRUE(response.setHeader("Location", "https://example.test/target"));
        const std::string beforeAttack = response.toString();

        // 响应拆分攻击串：值里的 CRLF 会让调用方自己提前结束头部块
        EXPECT_FALSE(response.setHeader("Location", "x\r\nX-Injected: 1"));

        EXPECT_EQ(response.getHeader("location").value_or(""), "https://example.test/target");
        EXPECT_FALSE(containsText(response.toString(), "X-Injected"));
        EXPECT_EQ(response.toString(), beforeAttack);
    }

    TEST(HttpResponse, RejectsBareLineFeedInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", "a\nb"));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
        EXPECT_TRUE(response.headers().empty());
    }

    TEST(HttpResponse, RejectsBareCarriageReturnInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", "a\rb"));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
    }

    TEST(HttpResponse, RejectsFullHeaderTerminatorInsideHeaderValue)
    {
        HttpResponse response;
        ASSERT_TRUE(response.setHeader("X-Trace", "origin"));

        // 最狠的一种：值里带完整的「结束头部块 + 伪造正文」
        EXPECT_FALSE(response.setHeader("X-Trace", "c\r\nd: e\r\n\r\nbody"));

        EXPECT_EQ(response.getHeader("x-trace").value_or(""), "origin");
        const std::string output = response.toString();
        EXPECT_FALSE(containsText(output, "d: e"));
        EXPECT_FALSE(containsText(output, "\r\n\r\nbody"));
        // 正文与状态码由本类自行序列化，没被攻击串污染
        EXPECT_TRUE(containsText(output, "content-length: 0"));
    }

    TEST(HttpResponse, RejectsNulByteInsideHeaderValue)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("X-Note", std::string("a\0b", 3)));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
        EXPECT_TRUE(response.headers().empty());
    }

    TEST(HttpResponse, RejectsOtherControlCharactersInsideHeaderValue)
    {
        HttpResponse response;

        // 垂直制表同样是控制字符：它会撕裂报文行，不在放行清单里
        EXPECT_FALSE(response.setHeader("X-Note", std::string("a\vb", 3)));

        EXPECT_FALSE(response.getHeader("x-note").has_value());
    }

    TEST(HttpResponse, AcceptsTabInsideHeaderValue)
    {
        HttpResponse response;

        // 水平制表符是 RFC 允许出现在 OWS 里的空白，属于显式放行项
        EXPECT_TRUE(response.setHeader("X-Note", "a\tb"));

        EXPECT_EQ(response.getHeader("x-note").value_or(""), "a\tb");
    }

    TEST(HttpResponse, RejectsMalformedHeaderNames)
    {
        HttpResponse response;

        EXPECT_FALSE(response.setHeader("", "value"));
        EXPECT_FALSE(response.setHeader("X Note", "value"));
        EXPECT_FALSE(response.setHeader("X:Note", "value"));
        EXPECT_FALSE(response.setHeader("X\tNote", "value"));
        EXPECT_FALSE(response.setHeader(std::string("X\0Note", 6), "value"));

        EXPECT_TRUE(response.headers().empty());
        EXPECT_TRUE(response.headerValues("x note").empty());
    }

    TEST(HttpResponse, AcceptsLegalTokenCharactersInHeaderName)
    {
        HttpResponse response;

        // tchar 集合里的符号（含 x-amz- 这类私有前缀常用的连字符）必须放行，否则误伤真实业务
        EXPECT_TRUE(response.setHeader("x-aMz.meta#1", "value"));

        EXPECT_EQ(response.getHeader("x-amz.meta#1").value_or(""), "value");
    }

    // ============================================================================
    // 可重复头部与顺序
    // ============================================================================

    TEST(HttpResponse, AppendsEverySetCookieCallAsIndependentHeader)
    {
        HttpResponse response;

        EXPECT_TRUE(response.setHeader("Set-Cookie", "sid=1; Path=/"));
        EXPECT_TRUE(response.setHeader("set-cookie", "theme=dark"));
        EXPECT_TRUE(response.setHeader("SET-COOKIE", "lang=zh"));

        const std::vector<std::string> values = response.headerValues("Set-Cookie");
        ASSERT_EQ(values.size(), 3U);
        EXPECT_EQ(values[0], "sid=1; Path=/");
        EXPECT_EQ(values[1], "theme=dark");
        EXPECT_EQ(values[2], "lang=zh");
    }

    TEST(HttpResponse, SerializesEverySetCookieLineInSettingOrder)
    {
        HttpResponse response;
        response.setHeader("Set-Cookie", "first=1");
        response.setHeader("Set-Cookie", "second=2");

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "set-cookie: first=1\r\n");
        const std::size_t secondPosition = positionOfText(output, "set-cookie: second=2\r\n");

        EXPECT_NE(firstPosition, std::string::npos);
        EXPECT_NE(secondPosition, std::string::npos);
        EXPECT_LT(firstPosition, secondPosition);
        // set-cookie_1 这类伪键不得出现在输出里：它会原样发到线上
        EXPECT_FALSE(containsText(output, "set-cookie_1"));
        EXPECT_EQ(response.headers().count("set-cookie_1"), 0U);
    }

    TEST(HttpResponse, KeepsFirstSetCookieInSingleValueView)
    {
        HttpResponse response;
        response.setHeader("Set-Cookie", "first=1");
        response.setHeader("Set-Cookie", "second=2");

        // headers()/getHeader() 的「一名一值」契约不变：可重复头部留首条
        EXPECT_EQ(response.getHeader("set-cookie").value_or(""), "first=1");
        EXPECT_EQ(response.headers().at("set-cookie"), "first=1");
    }

    TEST(HttpResponse, SerializesHeadersInSettingOrder)
    {
        HttpResponse response;
        response.setHeader("x-bsecond", "2");
        response.setHeader("x-alast", "1");
        response.setHeader("x-cmiddle", "3");

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "x-bsecond: 2\r\n");
        const std::size_t secondPosition = positionOfText(output, "x-alast: 1\r\n");
        const std::size_t thirdPosition = positionOfText(output, "x-cmiddle: 3\r\n");

        EXPECT_NE(firstPosition, std::string::npos);
        EXPECT_LT(firstPosition, secondPosition);
        EXPECT_LT(secondPosition, thirdPosition);
        // 自动补齐的头部排在自设头部之后
        EXPECT_LT(thirdPosition, positionOfText(output, "content-length:"));
    }

    TEST(HttpResponse, OverwritesOrdinaryHeaderInPlaceWithoutMovingIt)
    {
        HttpResponse response;
        response.setHeader("x-first", "1");
        response.setHeader("x-second", "old");
        response.setHeader("x-third", "3");

        ASSERT_TRUE(response.setHeader("x-second", "new"));

        const std::string output = response.toString();
        const std::size_t firstPosition = positionOfText(output, "x-first: 1\r\n");
        const std::size_t rewrittenPosition = positionOfText(output, "x-second: new\r\n");
        const std::size_t thirdPosition = positionOfText(output, "x-third: 3\r\n");

        EXPECT_NE(rewrittenPosition, std::string::npos);
        EXPECT_LT(firstPosition, rewrittenPosition);
        EXPECT_LT(rewrittenPosition, thirdPosition);
        EXPECT_FALSE(containsText(output, "old"));
        EXPECT_EQ(response.headerValues("x-second").size(), 1U);
    }

    // ============================================================================
    // 自动补齐：content-length 与 content-type
    // ============================================================================

    TEST(HttpResponse, AddsContentLengthMatchingBodyByteCount)
    {
        HttpResponse response;
        response.setBody("hello");

        EXPECT_TRUE(containsText(response.toString(), "content-length: 5\r\n"));
    }

    TEST(HttpResponse, CountsContentLengthInBytesNotCharacters)
    {
        HttpResponse response;
        // 两个「中」字：长度为 2，UTF-8 编码后实占 6 字节。收端按字节定界，写错一个字节整条连接就错位
        response.setBody(std::string(kThreeByteUtf8Character) + std::string(kThreeByteUtf8Character));

        EXPECT_TRUE(containsText(response.toString(), "content-length: 6\r\n"));
        EXPECT_FALSE(containsText(response.toString(), "content-length: 2"));
    }

    TEST(HttpResponse, KeepsExplicitContentLengthUntouched)
    {
        HttpResponse response;
        response.setHeader("content-length", "999");
        response.setBody("hello");

        const std::string output = response.toString();

        EXPECT_TRUE(containsText(output, "content-length: 999\r\n"));
        EXPECT_FALSE(containsText(output, "content-length: 5"));
    }

    TEST(HttpResponse, OmitsAutoContentLengthForNoContentResponse)
    {
        HttpResponse response;
        response.setStatus(204);

        // 204 不带正文，自动补 content-length 等于对收端多做一个承诺（date 仍应补上）
        const std::string output = response.toString();
        EXPECT_FALSE(containsText(output, "content-length"));
        EXPECT_EQ(withoutDateHeaderLine(output), "HTTP/1.1 204 No Content\r\n\r\n");
    }

    TEST(HttpResponse, OmitsAutoContentLengthForInformationalResponses)
    {
        HttpResponse continueResponse;
        continueResponse.setStatus(100);
        EXPECT_EQ(withoutDateHeaderLine(continueResponse.toString()), "HTTP/1.1 100 Continue\r\n\r\n");

        HttpResponse switchingResponse;
        switchingResponse.setStatus(199);
        switchingResponse.setHeader("x-custom", "1");
        const std::string output = switchingResponse.toString();

        EXPECT_TRUE(containsText(output, "x-custom: 1\r\n"));
        EXPECT_FALSE(containsText(output, "content-length"));
    }

    TEST(HttpResponse, KeepsExplicitContentLengthOnNoContentResponse)
    {
        HttpResponse response;
        response.setStatus(204);
        ASSERT_TRUE(response.setHeader("content-length", "0"));

        // 调用方显式设置的不会被序列化时抹掉
        EXPECT_TRUE(containsText(response.toString(), "content-length: 0\r\n"));
    }

    /**
     * @brief 304 允许携带 content-length：自动补缺规则必须只排除 1xx 与 204
     *
     * @details 这条与「不允许带正文」是两套判据：304 不许有正文，却明确允许声明长度。
     *          两者混成一条会让 304 响应丢掉 content-length，收端对报文边界的判断随之失去依据。
     */
    TEST(HttpResponse, KeepsAutoContentLengthOnNotModifiedResponse)
    {
        HttpResponse response;
        response.setStatus(304);

        const std::string output = response.toString();
        EXPECT_TRUE(containsText(output, "content-length: 0\r\n")) << "304 被误当成「不得声明长度」的一类";
        EXPECT_EQ(output.ends_with("\r\n\r\n"), true) << "304 不该带上正文";
    }

    /**
     * @brief serializeHead() 不含正文，且与 body() 拼接后和 toString() 逐字节相同
     *
     * @details 发送路径据此把「头部块 + 正文」作为两段一次提交，正文因而不必再拷一份。
     *          用例用远大于头部的正文把这个前提钉死：头部串里不允许出现正文内容。
     */
    TEST(HttpResponse, SerializeHeadExcludesBodySoItCanBeSentSeparately)
    {
        const std::string largeBody(64u * 1024u, 'x');

        HttpResponse response;
        ASSERT_TRUE(response.setHeader("x-trace", "1"));
        response.setBody(largeBody);

        const std::string head = response.serializeHead();
        EXPECT_LT(head.size(), largeBody.size()) << "头部串里混进了正文：分段发送省不掉拷贝";
        EXPECT_FALSE(containsText(head, largeBody)) << "头部串里混进了正文：分段发送省不掉拷贝";
        EXPECT_TRUE(containsText(head, "content-length: 65536\r\n"));
        EXPECT_TRUE(head.ends_with("\r\n\r\n"));

        // 两段拼接 == 整块序列化，逐字节相同：分段只是省拷贝，不改变线上字节流
        EXPECT_EQ(head + std::string(response.body()), response.toString());
    }

    TEST(HttpResponse, AddsTextPlainContentTypeOnlyWhenBodyIsPresent)
    {
        HttpResponse withBody;
        withBody.setBody("plain text");
        EXPECT_TRUE(containsText(withBody.toString(), "content-type: text/plain\r\n"));

        HttpResponse withoutBody;
        EXPECT_FALSE(containsText(withoutBody.toString(), "content-type"));
    }

    TEST(HttpResponse, DoesNotOverrideExplicitContentType)
    {
        HttpResponse response;
        response.setHeader("Content-Type", "application/json");
        response.setBody("{}");

        const std::string output = response.toString();

        EXPECT_TRUE(containsText(output, "content-type: application/json\r\n"));
        EXPECT_FALSE(containsText(output, "text/plain"));
        EXPECT_EQ(response.headerValues("content-type").size(), 1U);
    }

    TEST(HttpResponse, PlacesAutoHeadersAfterStatusLineAndCustomHeaders)
    {
        HttpResponse response;
        response.setBody("abc");
        response.setHeader("x-trace", "1");

        const std::string output = response.toString();
        const std::size_t bodySeparator = positionOfText(output, "\r\n\r\n");

        EXPECT_NE(bodySeparator, std::string::npos);
        EXPECT_LT(positionOfText(output, "x-trace: 1\r\n"), positionOfText(output, "content-type: text/plain\r\n"));
        EXPECT_LT(positionOfText(output, "content-type: text/plain\r\n"), positionOfText(output, "content-length: 3\r\n"));
        EXPECT_EQ(output.substr(bodySeparator + 4), "abc");
    }

    /**
     * @brief 未显式设 date 时按当前时刻补一条 IMF-fixdate，位置排在其它自动头部之后
     */
    TEST(HttpResponse, AddsCurrentDateHeaderWhenNotSetExplicitly)
    {
        HttpResponse response;
        response.setBody("abc");

        // 头部形态的断言要看 serializeHead()：toString() 后面还跟着正文，不以 GMT 收尾
        const std::string serializedHead = response.serializeHead();
        const std::size_t datePosition = positionOfText(serializedHead, "\r\ndate: ");

        ASSERT_NE(datePosition, std::string::npos);
        // date 值必须完整落在头部块内，并以 " GMT" 收尾（IMF-fixdate 固定 GMT 时区）
        EXPECT_TRUE(serializedHead.ends_with(" GMT\r\n\r\n")) << "date 值不是 IMF-fixdate 形态";
        // 与 content-type/content-length 一样，自动补出的 date 只在序列化时落笔
        EXPECT_FALSE(response.getHeader("date").has_value());
        EXPECT_LT(positionOfText(serializedHead, "content-length: 3\r\n"), datePosition);
    }

    /**
     * @brief 调用方显式设过 date 时不覆盖、也不追加第二条
     */
    TEST(HttpResponse, KeepsExplicitDateHeaderUntouched)
    {
        HttpResponse response;
        ASSERT_TRUE(response.setHeader("Date", "Sun, 06 Nov 1994 08:49:37 GMT"));

        const std::string output = response.toString();
        const std::size_t firstDatePosition = positionOfText(output, "date: Sun, 06 Nov 1994 08:49:37 GMT\r\n");

        EXPECT_NE(firstDatePosition, std::string::npos);
        EXPECT_EQ(response.headerValues("date").size(), 1U);
        // 只此一条 date，不得再自动补一条
        EXPECT_EQ(output.find("date: ", firstDatePosition + 1), std::string::npos);
    }

    /**
     * @brief 同一响应多次序列化给出逐字一致的 date：分段发送不会把两段拼成两种日期
     */
    TEST(HttpResponse, ReusesSameAutoDateAcrossSerializations)
    {
        HttpResponse response;
        response.setBody("stable");

        EXPECT_EQ(response.serializeHead(), response.serializeHead());
        EXPECT_EQ(withoutDateHeaderLine(response.serializeHead()), "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-length: 6\r\n\r\n");
    }

    // ============================================================================
    // 正文与工厂方法、复位
    // ============================================================================

    TEST(HttpResponse, ReplacesStoredBodyOnEachSet)
    {
        HttpResponse response;
        response.setBody("first");
        response.setBody("second");

        EXPECT_EQ(response.body(), "second");
        response.setBody(std::string_view{});
        EXPECT_TRUE(response.body().empty());
    }

    TEST(HttpResponse, BuildsOkayResponseFromFactory)
    {
        const HttpResponse response = HttpResponse::ok("hi");

        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "hi");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/plain");
        EXPECT_TRUE(containsText(response.toString(), "content-length: 2\r\n"));
    }

    TEST(HttpResponse, BuildsNotFoundResponseFromFactory)
    {
        const HttpResponse response = HttpResponse::notFound();

        EXPECT_EQ(response.status(), 404);
        EXPECT_EQ(response.body(), "Not Found");
        EXPECT_TRUE(containsText(response.toString(), "HTTP/1.1 404 Not Found\r\n"));
        EXPECT_TRUE(containsText(response.toString(), "content-length: 9\r\n"));
    }

    TEST(HttpResponse, BuildsServerErrorResponseFromMessage)
    {
        const HttpResponse withMessage = HttpResponse::serverError("boom");
        EXPECT_EQ(withMessage.status(), 500);
        EXPECT_EQ(withMessage.body(), "boom");

        const HttpResponse withoutMessage = HttpResponse::serverError();
        EXPECT_EQ(withoutMessage.status(), 500);
        EXPECT_TRUE(withoutMessage.body().empty());
        EXPECT_TRUE(containsText(withoutMessage.toString(), "content-length: 0\r\n"));
    }

    TEST(HttpResponse, ResetClearsStatusVersionHeadersAndBody)
    {
        HttpResponse response;
        response.setStatus(503);
        response.setHttpVersion("HTTP/1.0");
        response.setHeader("Retry-After", "5");
        response.setHeader("Set-Cookie", "sid=1");
        response.setHeader("Set-Cookie", "theme=dark");
        response.setBody("stale");

        response.reset();

        EXPECT_EQ(response.status(), 200);
        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(response.headers().empty());
        // 两份存储一起清：视图清空了但权威记录残留，会照样发到线上
        EXPECT_TRUE(response.headerValues("set-cookie").empty());
        EXPECT_FALSE(response.getHeader("retry-after").has_value());
        EXPECT_TRUE(containsText(response.toString(), "HTTP/1.1 200 OK\r\n"));
    }

    // ============================================================================
    // 映射正文（静态文件路径）
    // ============================================================================

    /**
     * @brief 映射正文与堆正文在序列化上等价：内容逐字节一致，content-length 按映射长度补齐
     */
    TEST(HttpResponse, ServesMappedFileAsBodyWithByteAccurateContentLength)
    {
        // UTF-8 三字节字符混在 ASCII 里：映射长度若误按字符数计算，content-length 会当场失真
        const std::string content = "mapped-body-" + std::string(kThreeByteUtf8Character);
        const TemporaryFile temporaryFile("MappedBody", content);

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setStatus(200);
        response.setMappedBody(std::move(mappedFile));

        // 正文视图直接指向文件页，不经堆缓冲
        EXPECT_EQ(response.body(), content);
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: " + std::to_string(content.size()) + "\r\n"));
        // toString() 与「头部 + 正文」拼接逐字节相同：映射只改正文来源，不改报文形态
        EXPECT_EQ(response.toString(), response.serializeHead() + content);
    }

    /**
     * @brief 映射正文被 setBody 取代后不再参与序列化：两条正文存储互斥
     */
    TEST(HttpResponse, HeapBodyReplacesMappedBody)
    {
        const TemporaryFile temporaryFile("HeapReplaces", "file-content-that-must-disappear");

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setMappedBody(std::move(mappedFile));
        response.setBody("stale");

        EXPECT_EQ(response.body(), "stale");
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 5\r\n"));
        EXPECT_EQ(response.toString().find("file-content"), std::string::npos);
    }

    /**
     * @brief 映射正文被 setMappedBody 取代后不再参与序列化：反向的互斥同样成立
     */
    TEST(HttpResponse, MappedBodyReplacesHeapBody)
    {
        const std::string content = "mapped-takes-over";
        const TemporaryFile temporaryFile("MappedReplaces", content);

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setBody("heap-body-that-must-disappear");
        response.setMappedBody(std::move(mappedFile));

        EXPECT_EQ(response.body(), content);
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: " + std::to_string(content.size()) + "\r\n"));
        EXPECT_EQ(response.toString().find("heap-body"), std::string::npos);
    }

    /**
     * @brief 复位清掉映射正文：复用响应对象时上一轮的文件不会跟着下一条报文发出去
     */
    TEST(HttpResponse, ResetClearsMappedBody)
    {
        const TemporaryFile temporaryFile("ResetMapped", "file-content-that-must-disappear");

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setMappedBody(std::move(mappedFile));
        response.reset();

        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 0\r\n"));
        EXPECT_EQ(response.toString().find("file-content"), std::string::npos);
    }

    /**
     * @brief 空文件映射是「有效但零字节」：正文为空，响应仍给出 content-length: 0
     */
    TEST(HttpResponse, EmptyMappedFileCountsAsEmptyBody)
    {
        const TemporaryFile temporaryFile("EmptyMapped", "");

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setMappedBody(std::move(mappedFile));

        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 0\r\n"));
    }

    /**
     * @brief 传入无效映射当正文：按空正文处理，不抛异常也不产出脏字节
     */
    TEST(HttpResponse, InvalidMappedFileCountsAsEmptyBody)
    {
        const TemporaryFile temporaryFile("InvalidMapped", "unused");
        const std::filesystem::path missingPath = temporaryFile.path().parent_path() / "no-such-file.bin";

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(missingPath);
        ASSERT_FALSE(mappedFile.isValid());

        HttpResponse response;
        response.setMappedBody(std::move(mappedFile));

        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 0\r\n"));
    }

    /**
     * @brief 映射正文的区间重载：body() 只暴露子区间，content-length 按区间长度补齐
     */
    TEST(HttpResponse, ServesMappedFileRangeAsBody)
    {
        const std::string content = "0123456789";
        const TemporaryFile temporaryFile("MappedRange", content);

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setStatus(206);
        response.setMappedBody(std::move(mappedFile), 2, 5);

        // 子区间视图直接指向文件页：既不整份发出，也不为切片拷一份堆内存
        EXPECT_EQ(response.body(), "23456");
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 5\r\n"));
        EXPECT_EQ(response.toString(), response.serializeHead() + "23456");
    }

    /**
     * @brief 区间长度为 0 是空正文，而不是「读到映射开头」
     */
    TEST(HttpResponse, MappedFileRangeTreatsZeroLengthAsEmptyBody)
    {
        const std::string content = "abcdef";
        const TemporaryFile temporaryFile("MappedRangeEmpty", content);

        Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(temporaryFile.path());
        ASSERT_TRUE(mappedFile.isValid());

        HttpResponse response;
        response.setMappedBody(std::move(mappedFile), 3, 0);

        EXPECT_TRUE(response.body().empty());
        EXPECT_TRUE(containsText(response.serializeHead(), "content-length: 0\r\n"));
    }

    /**
     * @brief 区间越界必须报错而不是静默钳制：否则 content-length 会与实际字节数悄悄不一致
     */
    TEST(HttpResponse, RejectsMappedBodyRangeBeyondMapping)
    {
        const std::string content = "0123456789";
        const TemporaryFile temporaryFile("MappedRangeOverflow", content);

        HttpResponse response;

        // offset 越界、以及 offset + length 越过末尾，两种都要拦住
        EXPECT_THROW(response.setMappedBody(Platform::MemoryMappedFile::open(temporaryFile.path()), 10, 1), Base::InvalidArgumentException);
        EXPECT_THROW(response.setMappedBody(Platform::MemoryMappedFile::open(temporaryFile.path()), 8, 3), Base::InvalidArgumentException);
        // 用法错误据此可归入 std::invalid_argument 分支：调用方能一次捕获所有「自己用错了」
        EXPECT_THROW(response.setMappedBody(Platform::MemoryMappedFile::open(temporaryFile.path()), 11, 0), std::invalid_argument);

        // 恰好贴住边界不算越界：整段与零长度都要放行
        EXPECT_NO_THROW(response.setMappedBody(Platform::MemoryMappedFile::open(temporaryFile.path()), 10, 0));
        EXPECT_EQ(response.body(), "");
        EXPECT_NO_THROW(response.setMappedBody(Platform::MemoryMappedFile::open(temporaryFile.path()), 0, 10));
        EXPECT_EQ(response.body(), content);
    }
} // namespace AsynGyanis::Net
