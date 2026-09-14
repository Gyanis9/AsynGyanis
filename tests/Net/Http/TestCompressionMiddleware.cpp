/**
 * @file TestCompressionMiddleware.cpp
 * @brief 响应压缩中间件的用例：协商、阈值、已编码内容、ETag 降级与响应往返
 * @details 断言一律把 gzip 正文解回原字节再比较（只比大小发现不了「解不开」），
 *          并且每条用例都同时钉住「不该压的时候确实没压」——压缩这类改写正文的中间件，
 *          最危险的失败是「悄悄改了不该改的响应」。
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/Middleware.h"

#include "Net/Http/HttpServer.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 用例等待响应的上限
        constexpr auto kCompressionTestTimeout = std::chrono::seconds{5};

        /// 压缩阈值：用例里的正文都明显大于它
        constexpr std::size_t kTestThresholdBytes = 64;

        /// 够长且重复的正文：一定能压小，便于断言「确实压过」
        const std::string kLargeBody = []
        {
            std::string body;
            body.reserve(4096);
            for (int index = 0; index < 256; ++index)
            {
                body += "asyn-gyanis-compressible-payload/";
            }
            return body;
        }();

        /**
         * @brief 解开 gzip 容器（用例侧自检）
         * @param input gzip 字节
         * @return std::optional<std::string> 原始内容；解不开时为空
         */
        std::optional<std::string> gunzip(const std::string_view input)
        {
            z_stream stream{};
            if (::inflateInit2(&stream, 15 + 16) != Z_OK)
            {
                return std::nullopt;
            }

            std::string output;
            stream.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
            stream.avail_in = static_cast<uInt>(input.size());

            std::string chunk(4096, '\0');
            int         result = Z_OK;
            while (result != Z_STREAM_END)
            {
                stream.next_out  = reinterpret_cast<Bytef *>(chunk.data());
                stream.avail_out = static_cast<uInt>(chunk.size());
                result           = ::inflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
                {
                    ::inflateEnd(&stream);
                    return std::nullopt;
                }
                output.append(chunk.data(), chunk.size() - stream.avail_out);
                if (result == Z_BUF_ERROR)
                {
                    break;
                }
            }

            ::inflateEnd(&stream);
            return output;
        }

        /**
         * @brief 从头部块里取 content-length
         * @param headers 头部块
         * @return std::size_t 长度；缺失或非法时为 0
         */
        std::size_t parseContentLength(const std::string_view headers)
        {
            std::string lowered(headers);
            for (char &character: lowered)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }

            const std::size_t position = lowered.find("content-length: ");
            if (position == std::string::npos)
            {
                return 0;
            }

            std::size_t value = 0;
            for (std::size_t index = position + std::string_view("content-length: ").size(); index < lowered.size(); ++index)
            {
                const char character = lowered[index];
                if (character < '0' || character > '9')
                {
                    break;
                }
                value = value * 10 + static_cast<std::size_t>(character - '0');
            }
            return value;
        }

        /**
         * @brief 一条完整响应：头部块与正文分开
         */
        struct ParsedResponse
        {
            std::string headers; ///< 含状态行与各头部行（不含结尾空行）
            std::string body;    ///< 正文原始字节
        };

        /**
         * @brief 发一条请求并读回完整响应（按 content-length 读满正文）
         * @param port 监听端口
         * @param requestText 请求报文
         * @return std::optional<ParsedResponse> 解析结果；读不全时为空
         */
        std::optional<ParsedResponse> sendAndReadResponse(const std::uint16_t port, const std::string &requestText)
        {
            LoopbackClient client(port);
            if (!client.isValid())
            {
                return std::nullopt;
            }
            if (!client.sendText(requestText, kCompressionTestTimeout))
            {
                return std::nullopt;
            }

            std::string accumulated;
            const auto  deadline = std::chrono::steady_clock::now() + kCompressionTestTimeout;

            // 第一步：等头部块收尾
            while (accumulated.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                if (client.readOnce(accumulated) == ReadOutcome::PeerClosed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            const std::size_t headerEnd = accumulated.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                return std::nullopt;
            }

            // 第二步：从 content-length 算出正文长度，读满为止
            const std::size_t contentLength = parseContentLength(accumulated.substr(0, headerEnd));
            const std::size_t expectedTotal = headerEnd + 4 + contentLength;
            while (accumulated.size() < expectedTotal && std::chrono::steady_clock::now() < deadline)
            {
                if (client.readOnce(accumulated) == ReadOutcome::PeerClosed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (accumulated.size() < expectedTotal)
            {
                return std::nullopt;
            }

            ParsedResponse parsed;
            parsed.headers = accumulated.substr(0, headerEnd);
            parsed.body    = accumulated.substr(headerEnd + 4, contentLength);
            return parsed;
        }

        /// 头部块里是否出现某个（大小写不敏感的）头部行
        bool hasHeaderLine(const std::string_view headers, const std::string_view expectedLine)
        {
            std::string loweredHeaders(headers);
            std::string loweredLine(expectedLine);
            for (std::string *text: {&loweredHeaders, &loweredLine})
            {
                for (char &character: *text)
                {
                    if (character >= 'A' && character <= 'Z')
                    {
                        character = static_cast<char>(character - 'A' + 'a');
                    }
                }
            }
            return loweredHeaders.find(loweredLine) != std::string::npos;
        }

        /**
         * @brief 造一台挂了压缩中间件与一条大正文路由的服务器
         * @param minimumBodySize 压缩阈值
         * @return RunningHttpServerFixture 夹具
         */
        std::unique_ptr<RunningHttpServerFixture> makeCompressionFixture(const std::size_t minimumBodySize)
        {
            const RouteRegistrar registerRoutes = [](Router &router, Core::EventLoop &)
            {
                router.get("/large",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setBody(kLargeBody);
                               co_return;
                           });
                router.get("/etagged",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setHeader("etag", "\"strong-validator\"");
                               response.setBody(kLargeBody);
                               co_return;
                           });
                router.get("/preencoded",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setHeader("content-encoding", "br");
                               response.setBody(kLargeBody);
                               co_return;
                           });
                router.get("/image",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "image/png");
                               response.setBody(kLargeBody);
                               co_return;
                           });
            };

            const ServerConfigurator configureServer = [minimumBodySize](TestHttpServer &server)
            {
                server.router().addMiddleware(compressionMiddleware({.minimumBodySize = minimumBodySize}));
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                                                       registerRoutes, HttpParserLimits{}, configureServer);
            EXPECT_TRUE(fixture->awaitRunning(kCompressionTestTimeout));
            return fixture;
        }

        /**
         * @brief 解开 zstd 帧（用例侧的自检工具）
         * @param input zstd 字节
         * @param expectedSize 原始长度（调用方已知）
         * @return std::optional<std::string> 原始内容；解不开或长度不符时为空
         */
        std::optional<std::string> unzstd(const std::string_view input, const std::size_t expectedSize)
        {
            std::string output(expectedSize, '\0');
            const std::size_t writtenLength = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
            if (ZSTD_isError(writtenLength) != 0 || writtenLength != expectedSize)
            {
                return std::nullopt;
            }
            return output;
        }

        /**
         * @brief 解开 brotli 流（用例侧的自检工具）
         * @param input brotli 字节
         * @param expectedSize 原始长度（调用方已知）
         * @return std::optional<std::string> 原始内容；解不开或长度不符时为空
         */
        std::optional<std::string> unbrotli(const std::string_view input, const std::size_t expectedSize)
        {
            std::string output(expectedSize, '\0');
            std::size_t decodedLength = output.size();
            if (BrotliDecoderDecompress(input.size(), reinterpret_cast<const std::uint8_t *>(input.data()), &decodedLength,
                                        reinterpret_cast<std::uint8_t *>(output.data())) != BROTLI_DECODER_RESULT_SUCCESS ||
                decodedLength != expectedSize)
            {
                return std::nullopt;
            }
            return output;
        }
    } // namespace

    /**
     * @brief 对端接受 gzip 时压正文，并给出 content-encoding 与 vary
     */
    TEST(CompressionMiddleware, CompressesWhenClientAcceptsGzip)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);
        const std::uint16_t                             port    = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(port, makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip"}));
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "响应没有声明 gzip 编码：\n" << response->headers;
        EXPECT_TRUE(hasHeaderLine(response->headers, "vary: accept-encoding")) << "压缩改变了表示，必须告诉缓存按 Accept-Encoding 分桶";
        EXPECT_LT(response->body.size(), kLargeBody.size()) << "正文没有变小，压缩可能没真正生效";

        const std::optional<std::string> restored = gunzip(response->body);
        ASSERT_TRUE(restored.has_value()) << "压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }

    /**
     * @brief 对端没提 Accept-Encoding 时正文原样发送
     */
    TEST(CompressionMiddleware, LeavesBodyAloneWithoutAcceptEncoding)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response = sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1"));
        ASSERT_TRUE(response.has_value());

        EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding")) << "对端没要求压缩却压了";
        EXPECT_FALSE(hasHeaderLine(response->headers, "vary:")) << "没压缩就不该新增 vary";
        EXPECT_EQ(response->body, kLargeBody);
    }

    /**
     * @brief `gzip;q=0` 是明确拒绝：必须当成不接受，不能压
     */
    TEST(CompressionMiddleware, HonoursExplicitZeroQualityRefusal)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip;q=0"}));
        ASSERT_TRUE(response.has_value());

        EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "q=0 表示拒绝，仍然压了";
        EXPECT_EQ(response->body, kLargeBody);
    }

    /**
     * @brief 正文小于阈值时不压：小正文压缩后往往更大
     */
    TEST(CompressionMiddleware, SkipsBodiesBelowThreshold)
    {
        // 阈值设得比正文还大：这条路径同样走「协商通过但不值得压」
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kLargeBody.size() * 2 + 1);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip"}));
        ASSERT_TRUE(response.has_value());

        EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "正文没过阈值却压了";
        EXPECT_EQ(response->body, kLargeBody);
    }

    /**
     * @brief 已带 content-encoding 的响应不再压第二层
     */
    TEST(CompressionMiddleware, LeavesAlreadyEncodedResponsesAlone)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /preencoded HTTP/1.1", {"accept-encoding: gzip"}));
        ASSERT_TRUE(response.has_value());

        EXPECT_TRUE(hasHeaderLine(response->headers, "content-encoding: br")) << "上游的编码被改掉了";
        EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "压了第二层，对端解不开";
        EXPECT_EQ(response->body, kLargeBody);
    }

    /**
     * @brief 已压缩的媒体类型不再压
     */
    TEST(CompressionMiddleware, SkipsIncompressibleContentTypes)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /image HTTP/1.1", {"accept-encoding: gzip"}));
        ASSERT_TRUE(response.has_value());

        EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "图片类内容不该再压一遍";
        EXPECT_EQ(response->body, kLargeBody);
    }

    /**
     * @brief 压缩后强 ETag 降级为弱校验器：正文表示变了，强校验器不能再复用
     */
    TEST(CompressionMiddleware, WeakensStrongETagWhenCompressing)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /etagged HTTP/1.1", {"accept-encoding: gzip"}));
        ASSERT_TRUE(response.has_value());

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "前置条件不成立：这条响应没被压缩";
        EXPECT_TRUE(hasHeaderLine(response->headers, "etag: W/\"strong-validator\"")) << "压缩后 ETag 没有降级为弱校验器：\n" << response->headers;
    }

    /**
     * @brief 对端同时接受三种编码时按偏好选 zstd（压缩率与速度综合最好）
     */
    TEST(CompressionMiddleware, PrefersZstdWhenClientAdvertisesAllCodecs)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response = sendAndReadResponse(
                fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, deflate, br, zstd"}));
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: zstd")) << "偏好顺序没有选 zstd：\n" << response->headers;
        EXPECT_TRUE(hasHeaderLine(response->headers, "vary: accept-encoding"));

        const std::optional<std::string> restored = unzstd(response->body, kLargeBody.size());
        ASSERT_TRUE(restored.has_value()) << "zstd 压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }

    /**
     * @brief zstd 被 q=0 明确拒绝时退到下一个偏好（brotli）
     */
    TEST(CompressionMiddleware, FallsBackToBrotliWhenZstdIsRejected)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response = sendAndReadResponse(
                fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, br, zstd;q=0"}));
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: br")) << "zstd 被拒后没有退到 brotli：\n" << response->headers;
        const std::optional<std::string> restored = unbrotli(response->body, kLargeBody.size());
        ASSERT_TRUE(restored.has_value()) << "brotli 压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }

    /**
     * @brief 通配（*）视为全部接受，同样按偏好选 zstd
     */
    TEST(CompressionMiddleware, WildcardSelectsPreferredCodec)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: *"}));
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: zstd")) << response->headers;
        const std::optional<std::string> restored = unzstd(response->body, kLargeBody.size());
        ASSERT_TRUE(restored.has_value()) << "zstd 压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }
} // namespace AsynGyanis::Net
