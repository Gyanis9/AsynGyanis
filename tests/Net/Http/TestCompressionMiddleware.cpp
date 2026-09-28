// 响应压缩中间件的用例：协商、阈值、收益判定、已编码内容、ETag 降级与响应往返 断言一律把 gzip 正文解回原字节再比较（只比大小发现不了「解不开」），
// 并且每条用例都同时钉住「不该压的时候确实没压」——压缩这类改写正文的中间件， 最危险的失败是「悄悄改了不该改的响应」。
#include "Net/Http/Middleware.h"

#include "Net/Http/Compression.h"
#include "Net/Http/Gzip.h"
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
         * @brief 一份接近随机的高熵正文：长度过阈值、内容类型也属于「可压」那一类，但三种压缩器都只会把它撑大
         * @details 用来把「正文够长」与「压了确实更短」这两件事分开测——只看前缀阈值的实现会在这一份上放行。
         *          xorshift 而不是 LCG 低位：后者低位周期短，压缩器能从里面找出重复
         * @return const std::string & 正文本体（全进程造一次）
         */
        const std::string &highEntropyBody()
        {
            static const std::string body = []
            {
                std::uint32_t randomState = 0x9E3779B9U;
                std::string   bytes;
                bytes.reserve(4096);
                while (bytes.size() < 4096)
                {
                    randomState ^= randomState << 13;
                    randomState ^= randomState >> 17;
                    randomState ^= randomState << 5;
                    bytes.push_back(static_cast<char>(randomState & 0xFFU));
                }
                return bytes;
            }();
            return body;
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
                router.get("/etagged-revalidated",
                           [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                           {
                               // 只验「304 上的验证器改写」这一件事：正文留空、也不声明长度
                               // （序列化层对 304 不自动补 content-length）
                               response.setHeader("etag", "\"strong-validator\"");
                               if (request.getHeader("if-none-match").value_or(std::string{}) == "\"strong-validator\"")
                               {
                                   response.setStatus(304);
                                   co_return;
                               }
                               response.setHeader("content-type", "text/plain; charset=utf-8");
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
                router.get("/high-entropy",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               // 内容类型属于「可压」、长度也过了阈值：唯一能挡住它的是实际收益
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setBody(highEntropyBody());
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
            { server.router().addMiddleware(compressionMiddleware({.minimumBodySize = minimumBodySize})); };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerRoutes, HttpParserLimits{},
                                                                      configureServer);
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
            std::string       output(expectedSize, '\0');
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
            if (BrotliDecoderDecompress(input.size(), reinterpret_cast<const std::uint8_t *>(input.data()), &decodedLength, reinterpret_cast<std::uint8_t *>(output.data())) !=
                        BROTLI_DECODER_RESULT_SUCCESS ||
                decodedLength != expectedSize)
            {
                return std::nullopt;
            }
            return output;
        }

        /// 外置用例的大正文大小：4 MiB 让 gzip 至少占住线程几十毫秒，够把「循环被堵住」量出来
        constexpr std::size_t kWordyBodyBytes = 4U * 1024U * 1024U;

        /// 「够小、压得动、但不到外派门槛」的正文大小：默认门槛是 8 KiB，这里取 2 KiB
        constexpr std::size_t kMidBodyBytes = 2U * 1024U;

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
                        "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
                };
                std::uint32_t randomState = 0x2545F491U;
                std::string   text;
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
         * @brief 大正文那一份的前 kMidBodyBytes 字节：压得动，但不到默认的外派门槛
         * @return const std::string & 正文本体（同一份静态语料，不另造）
         */
        const std::string &midBody()
        {
            static const std::string body = wordyLargeBody().substr(0, kMidBodyBytes);
            return body;
        }

        /**
         * @brief 造一台只有「大正文 + 中等正文 + 不压缩的小正文」三条路由的服务器，压缩中间件由调用方按循环现造
         * @details 几种执行位置共用同一份路由与同一份正文，唯一的变量就是压缩落在哪个线程上：
         *          `/huge` 过外派门槛、`/mid` 在门槛之下、`/quick` 连压缩门槛都不到
         * @param makeMiddleware 拿到本服务器的循环、造出要挂的压缩中间件
         * @param isHugeHandled 输入输出：大正文路由被调用过就置真，调用方据此确定重叠窗口
         * @return std::unique_ptr<RunningHttpServerFixture> 已在监听的服务器
         */
        std::unique_ptr<RunningHttpServerFixture> makeCompressionProbeFixture(const std::function<MiddlewareFunc(Core::EventLoop &)> &makeMiddleware,
                                                                              std::atomic<bool>                                      &isHugeHandled)
        {
            const RouteRegistrar registerRoutes = [&](Router &router, Core::EventLoop &loop)
            {
                router.get("/huge",
                           [&isHugeHandled](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setBody(wordyLargeBody());
                               // 交回中间件之前先亮旗：调用方据此知道「服务端正要开始压」，
                               // 重叠是自己构造出来的，不靠睡眠去猜调度
                               isHugeHandled.store(true);
                               co_return;
                           });
                router.get("/mid",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setBody(midBody());
                               co_return;
                           });
                router.get("/quick",
                           [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                           {
                               response.setHeader("content-type", "text/plain; charset=utf-8");
                               response.setBody("ok");
                               co_return;
                           });
                router.addMiddleware(makeMiddleware(loop));
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, registerRoutes, HttpParserLimits{},
                                                                      [](TestHttpServer &) {});
            EXPECT_TRUE(fixture->awaitRunning(kCompressionTestTimeout));
            return fixture;
        }

        /// 一次「大正文与第二条请求重叠」的测量结果：两条各自的耗时与完整响应
        struct LoopOverlapMeasurement
        {
            long long                     secondElapsedMilliseconds{0}; ///< 第二条从发出到读完的耗时
            long long                     hugeElapsedMilliseconds{0};   ///< 大正文那条从发出到读完的耗时
            std::optional<ParsedResponse> secondResponse{};             ///< 第二条的完整响应（读不到则为空）
            std::optional<ParsedResponse> hugeResponse{};               ///< 大正文那条的完整响应
        };

        /**
         * @brief 起一台探针服务器，让一条 4 MiB 大正文与第二条请求重叠，量第二条等了多久
         * @details 判据一律走**两次测量之比**而不是绝对毫秒线：CI 实测满载并行时同一条循环上的
         *          第二条请求光调度就等到 69ms，任何写死的毫秒线都会假红；而「第二条回来时第一条
         *          还没答完」这种先后也判不出来（大正文的传输远长于压缩，分块写出的间隙循环照样
         *          能接第二条，实测就地版的先后与外置版一致）。本机同一次运行的实测是外置 30ms、
         *          就地 153ms，比值约 5 倍，两个数出自同一台机器同一份负载，比值的量纲与噪声同向
         * @param offload 压缩是否交给工作线程（false 即就地压，走的是同一个中间件的另一条分支）
         * @param secondPath 第二条请求打的路径：/quick 连压缩门槛都不到，/mid 在门槛之下
         * @param options 挂上去的压缩选项（含外派门槛那道）
         * @param workerCount 外派用的工作线程数：要造「排在长任务后面」必须给 1 条
         * @return LoopOverlapMeasurement 两条各自的耗时与响应；服务端没起来时全为空
         */
        [[nodiscard]] LoopOverlapMeasurement measureLoopOverlap(const bool offload, const std::string &secondPath, const CompressionOptions options, const std::size_t workerCount)
        {
            Core::AsyncExecutor                             executor{workerCount};
            std::atomic<bool>                               isHugeHandled{false};
            const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionProbeFixture(
                    [&](Core::EventLoop &loop) -> MiddlewareFunc
                    {
                        if (offload)
                        {
                            return compressionMiddleware(loop, executor, options);
                        }
                        return compressionMiddleware(options);
                    },
                    isHugeHandled);

            LoopOverlapMeasurement measurement;
            const std::uint16_t    port = fixture->listeningPort();
            if (port == 0U)
            {
                return measurement;
            }

            std::thread hugeReader(
                    [&]
                    {
                        const auto began                    = std::chrono::steady_clock::now();
                        measurement.hugeResponse            = sendAndReadResponse(port, makeRequestText("GET /huge HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
                        measurement.hugeElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
                    });

            // 重叠窗口由 /huge 路由自己亮旗构造：它一进处理器就说明服务端正要开始压，不靠睡眠猜调度
            const auto deadline = std::chrono::steady_clock::now() + kCompressionTestTimeout;
            while (!isHugeHandled.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            const auto beganSecond     = std::chrono::steady_clock::now();
            measurement.secondResponse = sendAndReadResponse(port, makeRequestText("GET " + secondPath + " HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
            measurement.secondElapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beganSecond).count();

            hugeReader.join();
            return measurement;
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

        const std::optional<ParsedResponse> response = sendAndReadResponse(port, makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(port, makeRequestText("GET /declared-length HTTP/1.1", {"accept-encoding: gzip"}), kCompressionTestTimeout);
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
     * @brief 压完不比原文短就不换表示：过阈值、内容类型也可压的高熵正文，三种编码都不该上线
     * @details 只看「够长、类型可压」的实现会给这份语料盖上 content-encoding 再发一个更大的体：
     *          对端要多解一次、链路多跑几字节、缓存里还留下一份比原文更胖的变体。逐条编码各走一次，
     *          判据是压缩前后的实际字节数。
     */
    TEST(CompressionMiddleware, SkipsBodiesThatCompressionWouldNotShorten)
    {
        const std::string_view body = highEntropyBody();

        // 前置事实：这三种压缩器今天确实会把这份语料撑大。它变红说明压缩器换了实现，该换语料而不是改判据
        const std::optional<std::string> gzipProbe   = gzipCompress(body);
        const std::optional<std::string> zstdProbe   = zstdCompress(body);
        const std::optional<std::string> brotliProbe = brotliCompress(body);
        ASSERT_TRUE(gzipProbe.has_value() && zstdProbe.has_value() && brotliProbe.has_value()) << "压缩器连压都压不出结果";
        ASSERT_GT(gzipProbe->size(), body.size()) << "gzip 语料已不是「没有收益」那一类";
        ASSERT_GT(zstdProbe->size(), body.size()) << "zstd 语料已不是「没有收益」那一类";
        ASSERT_GT(brotliProbe->size(), body.size()) << "brotli 语料已不是「没有收益」那一类";

        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);
        const std::uint16_t                             port    = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        for (const std::string_view encoding: {"gzip", "br", "zstd"})
        {
            const std::optional<ParsedResponse> response =
                    sendAndReadResponse(port, makeRequestText("GET /high-entropy HTTP/1.1", {std::string{"accept-encoding: "} + std::string{encoding}}), kCompressionTestTimeout);
            ASSERT_TRUE(response.has_value()) << encoding << "：没有读到完整响应";

            EXPECT_FALSE(hasHeaderLine(response->headers, "content-encoding")) << encoding << "：压完更大却仍然换了表示\n" << response->headers;
            EXPECT_FALSE(hasHeaderLine(response->headers, "vary:")) << encoding << "：没换表示就不该新增 vary";
            EXPECT_EQ(response->body, body) << encoding << "：正文被换成了更差的表示";
        }
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
     * @brief 钉住：304 也必须按「这一请求本会被压缩」改写验证器（补 Vary、ETag 转弱）
     * @details 同一请求的 200 给的是 W/"…" + vary: accept-encoding，而 304 原先因「无正文不压缩」
     *          整段跳过改写，回的是强校验器且不带 Vary（RFC 9110 §15.4.5 要求 304 回带 200 本该给出的
     *          这些头部）。缓存据此会把压缩副本与未压缩副本当成同一份表示（§8.8.1 禁止的正是这个）。
     */
    TEST(CompressionMiddleware, WeakensValidatorAndKeepsVaryOnNotModified)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> compressedVariant = sendAndReadResponse(
                fixture->listeningPort(), makeRequestText("GET /etagged-revalidated HTTP/1.1", {"accept-encoding: gzip", "if-none-match: \"strong-validator\""}),
                kCompressionTestTimeout);
        ASSERT_TRUE(compressedVariant.has_value()) << "没有读到 304 响应";
        ASSERT_TRUE(compressedVariant->headers.starts_with("HTTP/1.1 304")) << compressedVariant->headers;
        EXPECT_TRUE(hasHeaderLine(compressedVariant->headers, "vary: accept-encoding")) << "304 没回带 Vary：\n" << compressedVariant->headers;
        EXPECT_TRUE(hasHeaderLine(compressedVariant->headers, "etag: W/\"strong-validator\"")) << "304 回的是强校验器：\n" << compressedVariant->headers;

        // 对照：只接受 identity 的客户端本来就不会被压缩，304 也就不该被降级或加 Vary
        const std::optional<ParsedResponse> identityVariant = sendAndReadResponse(
                fixture->listeningPort(), makeRequestText("GET /etagged-revalidated HTTP/1.1", {"accept-encoding: identity", "if-none-match: \"strong-validator\""}),
                kCompressionTestTimeout);
        ASSERT_TRUE(identityVariant.has_value()) << "没有读到对照响应";
        ASSERT_TRUE(identityVariant->headers.starts_with("HTTP/1.1 304")) << identityVariant->headers;
        EXPECT_FALSE(hasHeaderLine(identityVariant->headers, "vary: accept-encoding")) << "本不压缩却加了 Vary：\n" << identityVariant->headers;
        EXPECT_TRUE(hasHeaderLine(identityVariant->headers, "etag: \"strong-validator\"")) << identityVariant->headers;
    }

    /**
     * @brief 对端同时接受三种编码时按偏好选 zstd（压缩率与速度综合最好）
     */
    TEST(CompressionMiddleware, PrefersZstdWhenClientAdvertisesAllCodecs)
    {
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeCompressionFixture(kTestThresholdBytes);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, deflate, br, zstd"}), kCompressionTestTimeout);
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

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large HTTP/1.1", {"accept-encoding: gzip, br, zstd;q=0"}), kCompressionTestTimeout);
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

        const std::unique_ptr<RunningHttpServerFixture> inLoopFixture =
                makeCompressionProbeFixture([](Core::EventLoop &) { return compressionMiddleware({.minimumBodySize = kTestThresholdBytes}); }, isHugeHandled);
        const std::unique_ptr<RunningHttpServerFixture> offloadedFixture = makeCompressionProbeFixture(
                [&executor](Core::EventLoop &loop) { return compressionMiddleware(loop, executor, {.minimumBodySize = kTestThresholdBytes}); }, isHugeHandled);
        ASSERT_NE(inLoopFixture->listeningPort(), 0U);
        ASSERT_NE(offloadedFixture->listeningPort(), 0U);

        const std::string                   requestText       = makeRequestText("GET /huge HTTP/1.1", {"accept-encoding: gzip"});
        const std::optional<ParsedResponse> inLoopResponse    = sendAndReadResponse(inLoopFixture->listeningPort(), requestText, kCompressionTestTimeout);
        const std::optional<ParsedResponse> offloadedResponse = sendAndReadResponse(offloadedFixture->listeningPort(), requestText, kCompressionTestTimeout);
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
     * @details 这是外置的全部目的，因此判据要能报红：换成就地版，第二条请求就得排在整段压缩之后。
     *          判据取两次测量之比：本机同一次运行实测外置 30ms、就地 153ms。绝对毫秒线在满载的
     *          runner 上站不住（CI 实测第二条请求光调度就等到 69ms），而「先后」也判不出来——
     *          大正文的传输远长于压缩，分块写出的间隙循环照样接得到第二条，就地版与外置版同序
     */
    TEST(CompressionMiddleware, OffloadedCompressionLeavesTheLoopFreeWhileCompressing)
    {
        const CompressionOptions     options{.minimumBodySize = kTestThresholdBytes};
        const LoopOverlapMeasurement offloaded = measureLoopOverlap(true, "/quick", options, 2);
        const LoopOverlapMeasurement inLoop    = measureLoopOverlap(false, "/quick", options, 2);

        ASSERT_TRUE(offloaded.secondResponse.has_value() && offloaded.hugeResponse.has_value()) << "外置档有请求没读到完整响应";
        ASSERT_TRUE(inLoop.secondResponse.has_value() && inLoop.hugeResponse.has_value()) << "就地档有请求没读到完整响应";
        EXPECT_EQ(offloaded.secondResponse->body, "ok");
        ASSERT_GT(inLoop.secondElapsedMilliseconds, 0) << "就地档的第二条请求没量到耗时，比值判据是空的";

        // 两倍余量：就地档里第二条至少等完一整段压缩，外置档只等一次建连与调度；比值的噪声与
        // 机器快慢同向进两边， runner 快慢翻不了盘
        EXPECT_LT(offloaded.secondElapsedMilliseconds * 2, inLoop.secondElapsedMilliseconds)
                << "外置只降到 " << offloaded.secondElapsedMilliseconds << "ms，而就地是 " << inLoop.secondElapsedMilliseconds << "ms：差距不足以说明压缩真的挪出了循环线程";
    }

    /**
     * @brief 没到外派门槛的正文留在循环上：那一跳比压一次更贵，外派会同时拖慢响应与循环
     * @details 外派有独立的一道门槛 `offloadMinimumBodySize`（默认 8 KiB），2 KiB 的 /mid 因此按默认
     *          就不该被交出去。对照档是把那道门槛配成 0（所有正文都外派），工作线程只给一条，于是
     *          这条小正文必然排在 4 MiB 那份后面。判据同样是两次测量之比，不设绝对毫秒线
     */
    TEST(CompressionMiddleware, SmallBodiesStayOnTheLoopWhenOffloading)
    {
        const LoopOverlapMeasurement gated   = measureLoopOverlap(true, "/mid", CompressionOptions{.minimumBodySize = kTestThresholdBytes}, 1);
        const LoopOverlapMeasurement ungated = measureLoopOverlap(true, "/mid", CompressionOptions{.minimumBodySize = kTestThresholdBytes, .offloadMinimumBodySize = 0}, 1);

        ASSERT_TRUE(gated.secondResponse.has_value() && gated.hugeResponse.has_value()) << "默认门槛那一档有请求没读到完整响应";
        ASSERT_TRUE(ungated.secondResponse.has_value() && ungated.hugeResponse.has_value()) << "全外派那一档有请求没读到完整响应";
        ASSERT_GT(ungated.secondElapsedMilliseconds, 0) << "全外派档没量到耗时，比值判据是空的";
        EXPECT_LT(gated.secondElapsedMilliseconds * 2, ungated.secondElapsedMilliseconds)
                << "默认门槛下这条 " << kMidBodyBytes << " 字节正文等了 " << gated.secondElapsedMilliseconds << "ms，而全外派只等 " << ungated.secondElapsedMilliseconds
                << "ms：这道门槛没起作用";

        // 门槛改变的只是执行位置，不是压不压：默认档仍要真的压过，而且能解回原样
        EXPECT_TRUE(hasHeaderLine(gated.secondResponse->headers, "content-encoding: gzip")) << "小正文压根没被压缩：\n" << gated.secondResponse->headers;
        const std::optional<std::string> restoredMid = gunzip(gated.secondResponse->body);
        ASSERT_TRUE(restoredMid.has_value()) << "小正文压出来的响应解不开";
        EXPECT_EQ(*restoredMid, midBody());
    }

} // namespace AsynGyanis::Net
