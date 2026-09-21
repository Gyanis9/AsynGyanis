// 响应压缩中间件的用例：协商、阈值、已编码内容、ETag 降级与响应往返 断言一律把 gzip 正文解回原字节再比较（只比大小发现不了「解不开」），
// 并且每条用例都同时钉住「不该压的时候确实没压」——压缩这类改写正文的中间件， 最危险的失败是「悄悄改了不该改的响应」。
#include "Net/Http/Middleware.h"

#include "Net/Http/HttpServer.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
                router.get("/declared-length",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               // 刻意声明一个错的长度再设正文：与静态文件对 HEAD 的
                               // 「先声明长度、不读正文」是同一条路径，压缩中间件必须把它清掉重算
                               response.setHeader("content-length", "999999");
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

        /// 外置用例的大正文大小：4 MiB 让 gzip 至少占住线程几十毫秒，够把「循环被堵住」量出来
        constexpr std::size_t kWordyBodyBytes = 4U * 1024U * 1024U;

        /// 压缩期间第二条请求允许的最长等待：明显小于大正文的压缩耗时，又容得下建连与调度
        constexpr auto kLoopMustStayFreeBudget = std::chrono::milliseconds{50};

        /**
         * @brief 构造一条词序被打乱的大正文，全进程只造一次
         * @details 周期重复的词表会被压缩器当成一次超长匹配，每字节成本低报好几倍，
         *          那样这条用例量的「压缩占住循环多久」就不成立；LCG 打乱后接近真实文本
         * @return const std::string & 正文本体
         */
        const std::string &wordyLargeBody()
        {
            static const std::string body = []
            {
                static constexpr std::string_view vocabulary[] = {
                    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
                    "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
                };
                std::uint32_t randomState = 0x2545F491U;
                std::string text;
                text.reserve(kWordyBodyBytes + 1);
                while (text.size() < kWordyBodyBytes)
                {
                    randomState = randomState * 1664525U + 1013904223U;
                    text += vocabulary[(randomState >> 16) % std::size(vocabulary)];
                    text += ' ';
                }
                return text;
            }();
            return body;
        }

        /**
         * @brief 造一台只有「大正文 + 小正文」两条路由的服务器，压缩中间件由调用方按循环现造
         * @details 两种执行位置共用同一份路由与同一份正文，唯一的变量就是压缩落在哪个线程上
         * @param makeMiddleware 拿到本服务器的循环、造出要挂的压缩中间件
         * @param isHugeHandled 输入输出：大正文路由被调用过就置真，调用方据此确定重叠窗口
         * @return std::unique_ptr<RunningHttpServerFixture> 已在监听的服务器
         */
        std::unique_ptr<RunningHttpServerFixture> makeTwoRouteCompressionFixture(
                const std::function<MiddlewareFunc(Core::EventLoop &)> &makeMiddleware, std::atomic<bool> &isHugeHandled)
        {
            const RouteRegistrar registerRoutes = [&](Router &router, Core::EventLoop &loop)
            {
                router.get("/huge", [&isHugeHandled](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setHeader("content-type", "text/plain; charset=utf-8");
                    response.setBody(wordyLargeBody());
                    // 交回中间件之前先亮旗：调用方据此知道「服务端正要开始压」，
                    // 重叠是自己构造出来的，不靠睡眠去猜调度
                    isHugeHandled.store(true);
                    co_return;
                });
                router.get("/quick", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setHeader("content-type", "text/plain; charset=utf-8");
                    response.setBody("ok");
                    co_return;
                });
                router.addMiddleware(makeMiddleware(loop));
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                                                       registerRoutes, HttpParserLimits{}, [](TestHttpServer &) {});
            EXPECT_TRUE(fixture->awaitRunning(kCompressionTestTimeout));
            return fixture;
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
                sendAndReadResponse(port, makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "响应没有声明 gzip 编码：\n" << response->headers;
        EXPECT_TRUE(hasHeaderLine(response->headers, "vary: accept-encoding")) << "压缩改变了表示，必须告诉缓存按 Accept-Encoding 分桶";
        EXPECT_LT(response->body.size(), kLargeBody.size()) << "正文没有变小，压缩可能没真正生效";

        const std::optional<std::string> restored = gunzip(response->body);
        ASSERT_TRUE(restored.has_value()) << "压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }

    /**
     * @brief 业务显式声明过的 content-length 描述的是未压缩正文：压完必须按新正体重算
     * @details 旧长度若残留，线上就是「头部说 999999 字节、实际只有几千」——本用例的读取器
     *          严格按声明长度收正文，读到不齐会超时返回空值，因此这条断言同时也是线上报文
     *          边界是否正确的证明（对 keep-alive，错位会让余下字节被当成下一条响应）
     */
    TEST(CompressionMiddleware, RecomputesContentLengthDeclaredByHandler)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);
        const std::uint16_t                             port    = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        const std::optional<ParsedResponse> response = sendAndReadResponse(
                port, makeRequestText("GET /declared-length HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应：content-length 可能仍是压缩前的旧值";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: gzip")) << "响应没有声明 gzip 编码：\n" << response->headers;
        EXPECT_EQ(parseContentLength(response->headers), response->body.size()) << "content-length 与压缩后的正文长度不符";
        EXPECT_EQ(parseContentLength(response->headers) == 999999U, false) << "旧长度被原样发上线";

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

        const std::optional<ParsedResponse> response = sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1"), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip;q=0"}), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /preencoded HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /image HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /etagged HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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
                fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, deflate, br, zstd"}), kCompressionTestTimeout);
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
                fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, br, zstd;q=0"}), kCompressionTestTimeout);
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
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: *"}), kCompressionTestTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(hasHeaderLine(response->headers, "content-encoding: zstd")) << response->headers;
        const std::optional<std::string> restored = unzstd(response->body, kLargeBody.size());
        ASSERT_TRUE(restored.has_value()) << "zstd 压出来的正文解不开";
        EXPECT_EQ(*restored, kLargeBody);
    }

    /**
     * @brief 外置到工作线程的压缩，产物必须与就地版逐字节相同
     * @details 两条路径共用同一份实现体，这条用例钉住「换执行位置不换输出」：同一份正文、
     *          同一个档位下 gzip 的输出是确定的，因此可以直接比字节，而不是只比「都能解开」
     */
    TEST(CompressionMiddleware, OffloadedCompressionProducesTheSameBytesAsInLoop)
    {
        Core::AsyncExecutor executor{1};
        std::atomic<bool>   isHugeHandled{false};

        const std::unique_ptr<RunningHttpServerFixture> inLoopFixture = makeTwoRouteCompressionFixture(
                [](Core::EventLoop &)
                {
                    return compressionMiddleware({.minimumBodySize = kTestThresholdBytes});
                },
                isHugeHandled);
        const std::unique_ptr<RunningHttpServerFixture> offloadedFixture = makeTwoRouteCompressionFixture(
                [&executor](Core::EventLoop &loop)
                {
                    return compressionMiddleware(loop, executor, {.minimumBodySize = kTestThresholdBytes});
                },
                isHugeHandled);
        ASSERT_NE(inLoopFixture->listeningPort(), 0U);
        ASSERT_NE(offloadedFixture->listeningPort(), 0U);

        const std::string requestText = makeRequestText("GET /huge HTTP/1.1", {"accept-encoding: gzip"});
        const std::optional<ParsedResponse> inLoopResponse =
                sendAndReadResponse(inLoopFixture->listeningPort(), requestText, kCompressionTestTimeout);
        const std::optional<ParsedResponse> offloadedResponse =
                sendAndReadResponse(offloadedFixture->listeningPort(), requestText, kCompressionTestTimeout);
        ASSERT_TRUE(inLoopResponse.has_value()) << "就地压缩那条没读到完整响应";
        ASSERT_TRUE(offloadedResponse.has_value()) << "外置压缩那条没读到完整响应";

        ASSERT_TRUE(hasHeaderLine(offloadedResponse->headers, "content-encoding: gzip")) << offloadedResponse->headers;
        EXPECT_TRUE(hasHeaderLine(offloadedResponse->headers, "vary: accept-encoding"));
        EXPECT_EQ(offloadedResponse->body, inLoopResponse->body) << "换执行位置换了输出字节";

        const std::optional<std::string> restored = gunzip(offloadedResponse->body);
        ASSERT_TRUE(restored.has_value()) << "外置压出来的正文解不开";
        EXPECT_EQ(*restored, wordyLargeBody());
    }

    /**
     * @brief 压缩在干活时，同一条循环必须还能服务别的连接
     * @details 这是外置的全部目的，因此判据要能报红：把中间件换成就地版，第二条请求就得等完
     *          整段压缩（4 MiB 打乱词序的正文至少几十毫秒），必然越过 50ms 这条线。
     *          重叠由 /huge 路由自己亮旗构造，不靠睡眠时间猜调度
     */
    TEST(CompressionMiddleware, OffloadedCompressionLeavesTheLoopFreeWhileCompressing)
    {
        Core::AsyncExecutor executor{2};
        std::atomic<bool>   isHugeHandled{false};
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeTwoRouteCompressionFixture(
                [&executor](Core::EventLoop &loop)
                {
                    return compressionMiddleware(loop, executor, {.minimumBodySize = kTestThresholdBytes});
                },
                isHugeHandled);
        const std::uint16_t port = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        std::optional<ParsedResponse> hugeResponse;
        const std::string             hugeRequestText = makeRequestText("GET /huge HTTP/1.1", {"accept-encoding: gzip"});
        std::thread                   hugeReader(
                [&]
                {
                    hugeResponse = sendAndReadResponse(port, hugeRequestText, kCompressionTestTimeout);
                });

        const auto deadline = std::chrono::steady_clock::now() + kCompressionTestTimeout;
        while (!isHugeHandled.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        ASSERT_TRUE(isHugeHandled.load(std::memory_order_acquire)) << "大正文请求没有在时限内进到服务端";

        const auto quickBegin = std::chrono::steady_clock::now();
        const std::optional<ParsedResponse> quickResponse =
                sendAndReadResponse(port, makeRequestText("GET /quick HTTP/1.1"), kCompressionTestTimeout);
        const auto quickElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - quickBegin);

        hugeReader.join();
        ASSERT_TRUE(quickResponse.has_value()) << "压缩期间第二条请求没读到响应";
        EXPECT_EQ(quickResponse->body, "ok");
        EXPECT_LT(quickElapsed, kLoopMustStayFreeBudget)
                << "压缩期间同一条循环上的其他请求被堵了 " << quickElapsed.count() << "ms：压缩没真的挪出循环线程";

        ASSERT_TRUE(hugeResponse.has_value()) << "大正文那条没读到完整响应";
        const std::optional<std::string> restored = gunzip(hugeResponse->body);
        ASSERT_TRUE(restored.has_value()) << "大正文压出来的响应解不开";
        EXPECT_EQ(*restored, wordyLargeBody());
    }
} // namespace AsynGyanis::Net
