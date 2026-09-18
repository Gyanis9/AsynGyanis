// 流式请求正文（Router::postStreaming()/putStreaming() + HttpRequestBody）的端到端用例
// 覆盖场景：
//   一. 早派发（确定性）：头部收齐即进处理器——客户端只发出正文前若干字节时，处理器已拿到首段；
//       以「首段到达」为放行信号再补发剩余正文，证明派发发生在正文收齐之前
//   二. 全量交付：128 KiB 正文按到达批次读完，总字节数一致且段数大于一（不是一次整块交付）
//   三. 分块传输：Transfer-Encoding: chunked 的正文同样按解码后的字节流式交付
//   四. 未读尽排空：处理器只读一段就应答，剩余正文被排空，连接按 keep-alive 复用
//   五. 普通路由的流同样可用：请求收齐后派发，流交出全量正文后 EOF
//   六. Expect: 100-continue：先收到 100 Continue 再进流式派发，正文照常交付

#include "Net/Http/HttpRequestBody.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    using namespace HttpTestSupport;

    namespace
    {
        /// 造一段内容可辨认的正文（内容本身不参与断言，长度才是）
        std::string makePayload(const std::size_t byteCount)
        {
            std::string payload(byteCount, '\0');
            for (std::size_t index = 0; index < byteCount; ++index)
            {
                payload[index] = static_cast<char>('a' + (index % 26));
            }
            return payload;
        }

        /// 一条带 Content-Length 的 POST 请求头（正文由调用方随后补发）
        std::string uploadHeadRequest(const std::string_view path, const std::size_t contentLength)
        {
            std::string request = "POST ";
            request += path;
            request += " HTTP/1.1\r\nHost: loopback\r\nContent-Length: ";
            request += std::to_string(contentLength);
            request += "\r\n\r\n";
            return request;
        }
    } // namespace

    /**
     * @brief 钉住：流式路由在头部收齐、正文未收完时就已派发（背压与「边收边处理」的前提）
     */
    TEST(HttpStreamingBody, DispatchesBeforeBodyCompletes)
    {
        constexpr std::size_t kTotalBytes = 64;
        constexpr std::string_view kHeadPortion = "early";

        std::atomic<bool> firstChunkObserved{false};
        std::atomic<std::size_t> firstChunkLength{0};

        const auto registerRoutes = [&firstChunkObserved, &firstChunkLength](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload", [&firstChunkObserved, &firstChunkLength](
                                                        HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                bool        isFirst    = true;
                while (co_await stream->readNext())
                {
                    if (isFirst)
                    {
                        // 首段到达即置位：客户端据此确认「正文没发完，处理器已经跑起来了」
                        firstChunkLength.store(stream->chunk().size(), std::memory_order_release);
                        firstChunkObserved.store(true, std::memory_order_release);
                        isFirst = false;
                    }
                    totalBytes += stream->chunk().size();
                }
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        // 只发请求头与前 5 个正文字节：正文没发完，服务器就应经流式派发把这段交进处理器
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", kTotalBytes) + std::string(kHeadPortion), kWaitTimeout));
        EXPECT_TRUE(waitForCondition(
                [&firstChunkObserved]
                {
                    return firstChunkObserved.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "正文未收完时处理器没有拿到首段：流式派发没有发生";
        EXPECT_LE(firstChunkLength.load(std::memory_order_acquire), kHeadPortion.size())
                << "首段里出现了客户端尚未发送的字节";

        // 补发剩余正文，处理器读到 EOF 后应答
        const std::string payload = makePayload(kTotalBytes);
        ASSERT_TRUE(client.sendText(std::string_view(payload).substr(kHeadPortion.size()), kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "bytes=" + std::to_string(kTotalBytes), kWaitTimeout)) << receivedText;
    }

    /**
     * @brief 钉住：大正文按到达批次交付，总字节数一致且确实分了多段（不是一次整块交付）
     */
    TEST(HttpStreamingBody, DeliversLargeBodyInMultipleChunks)
    {
        constexpr std::size_t kTotalBytes = 128 * 1024;

        std::atomic<std::size_t> observedChunkCount{0};

        const auto registerRoutes = [&observedChunkCount](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload", [&observedChunkCount](
                                                        HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                std::size_t chunkCount = 0;
                while (co_await stream->readNext())
                {
                    totalBytes += stream->chunk().size();
                    ++chunkCount;
                }
                observedChunkCount.store(chunkCount, std::memory_order_release);
                response.setBody("chunks=" + std::to_string(chunkCount) + ",bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", kTotalBytes), kWaitTimeout));
        ASSERT_TRUE(client.sendText(makePayload(kTotalBytes), kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "bytes=" + std::to_string(kTotalBytes), kWaitTimeout)) << receivedText;
        EXPECT_GE(observedChunkCount.load(std::memory_order_acquire), 2U)
                << "128 KiB 正文只交付了一段：说明它被整块缓冲后才派发，而不是流式交付";
    }

    /**
     * @brief 钉住：chunked 传输的正文按解码后的字节流式交付
     */
    TEST(HttpStreamingBody, StreamsChunkedTransferEncoding)
    {
        constexpr std::size_t kTotalBytes = 32 * 1024;

        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                while (co_await stream->readNext())
                {
                    totalBytes += stream->chunk().size();
                }
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        // 分块编码：三次 8 KiB 数据块 + 终止块
        std::string request = "POST /upload HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: chunked\r\n\r\n";
        const std::string payload = makePayload(kTotalBytes);
        for (std::size_t offset = 0; offset < payload.size(); offset += 8 * 1024)
        {
            const std::string_view segment(payload.data() + offset, std::min<std::size_t>(8 * 1024, payload.size() - offset));
            request += "2000\r\n";
            request += segment;
            request += "\r\n";
        }
        request += "0\r\n\r\n";

        ASSERT_TRUE(client.sendText(request, kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "bytes=" + std::to_string(kTotalBytes), kWaitTimeout)) << receivedText;
    }

    /**
     * @brief 钉住：处理器没读完正文也安全——剩余字节被排空，连接按 keep-alive 继续服务
     */

    /**
     * @brief 流式消费方每轮把缓冲抽干，正文上限照样按「累计已收」生效
     * @details 这条判据曾经只看当前缓冲长度：处理器每轮把已交付的字节取走，缓冲一直是空的，
     *          上限于是形同虚设、对端可以无限收正文（第五轮修掉的正是这条）。
     *          用例刻意分 4 次发、每次等处理器取走再发下一批，把「边读边取走」这条路径钉住：
     *          累计 64 字节而上限只有 16，必须判越界并按 413 收口
     */
    TEST(HttpStreamingBody, EnforcesCumulativeBodyLimitWhileConsumerDrains)
    {
        constexpr std::size_t kChunkBytes        = 16;
        constexpr std::size_t kChunkCount        = 4;
        constexpr std::size_t kMaximumBodyBytes  = 16;

        HttpParserLimits limits;
        limits.maximumBodySize = kMaximumBodyBytes;

        std::atomic<std::size_t> drainedByteCount{0};

        const auto registerRoutes = [&drainedByteCount](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload", [&drainedByteCount](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                while (co_await stream->readNext())
                {
                    totalBytes += stream->chunk().size();
                    // 每取走一段就报一次：用例据此确认「处理器确实在持续把缓冲抽干」
                    drainedByteCount.store(totalBytes, std::memory_order_release);
                    // 让出一次：把交付与解析交错开，判据才真的落在「累计」上
                    co_await std::suspend_never{};
                }
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes, limits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        std::string request = "POST /upload HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: chunked\r\n\r\n";
        ASSERT_TRUE(client.sendText(request, kWaitTimeout));

        // 分次发：每次发一块 16 字节，等处理器把上一块取走（最多等 200ms）再发下一块
        for (std::size_t index = 0; index < kChunkCount; ++index)
        {
            if (!client.sendText("10\r\n" + std::string(kChunkBytes, 'x') + "\r\n", kWaitTimeout))
            {
                break; // 已被收口：后面不用再发了
            }
            static_cast<void>(waitForCondition(
                    [&drainedByteCount, index]
                    {
                        return drainedByteCount.load(std::memory_order_acquire) >= (index + 1) * kChunkBytes;
                    },
                    std::chrono::milliseconds{200}));
        }
        static_cast<void>(client.sendText("0\r\n\r\n", kWaitTimeout));

        std::string accumulated;
        const bool isRejected = client.waitForText(accumulated, "413", kWaitTimeout) ||
                                client.waitForClosure(accumulated, kWaitTimeout);
        EXPECT_TRUE(isRejected) << "累计正文超过上限却没有被判越界，累计收到 " << accumulated.size() << " 字节";
        EXPECT_EQ(accumulated.find("bytes=" + std::to_string(kChunkBytes * kChunkCount)), std::string::npos)
                << "累计正文超过上限却把整份正文都交给了处理器（判据又退回看当前缓冲了）：" << accumulated;
    }

    TEST(HttpStreamingBody, DrainsUnreadBodyAndKeepsConnectionAlive)
    {
        constexpr std::size_t kTotalBytes = 32 * 1024;

        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload-first-chunk", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                // 只读一段就返回：剩余正文由会话排空后复用连接
                const bool hasChunk = co_await stream->readNext();
                response.setBody(hasChunk ? "first-chunk" : "empty");
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload-first-chunk", kTotalBytes), kWaitTimeout));
        ASSERT_TRUE(client.sendText(makePayload(kTotalBytes), kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "first-chunk", kWaitTimeout)) << receivedText;

        // 同一连接上的第二条请求必须照常服务：排空（或收口声明）之外不该留下半条报文
        ASSERT_TRUE(client.sendText("GET /hello HTTP/1.1\r\nHost: loopback\r\n\r\n", kWaitTimeout));
        ASSERT_TRUE(client.waitForText(receivedText, "served-hello", kWaitTimeout)) << receivedText;
    }

    /**
     * @brief 钉住：普通（非流式）路由上 bodyStream() 同样可用——收齐后一次交出全量正文再 EOF
     */
    TEST(HttpStreamingBody, RegularRouteAlsoExposesBodyStream)
    {
        constexpr std::size_t kTotalBytes = 1024;

        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.post("/echo-stream", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::string assembled;
                while (co_await stream->readNext())
                {
                    assembled.append(stream->chunk());
                }
                response.setBody("stream-bytes=" + std::to_string(assembled.size()));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        ASSERT_TRUE(client.sendText(uploadHeadRequest("/echo-stream", kTotalBytes), kWaitTimeout));
        ASSERT_TRUE(client.sendText(makePayload(kTotalBytes), kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "stream-bytes=" + std::to_string(kTotalBytes), kWaitTimeout)) << receivedText;
    }

    /**
     * @brief 钉住：Expect: 100-continue 与流式派发的配合——先回 100，再交正文
     */
    TEST(HttpStreamingBody, ContinueExpectationProceedsBeforeStreamingDispatch)
    {
        constexpr std::size_t kTotalBytes = 2048;

        const auto registerRoutes = [](Router &router, Core::EventLoop &)
        {
            router.postStreaming("/upload", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                HttpRequestBody *stream = request.bodyStream();
                if (stream == nullptr)
                {
                    response.setBody("no-stream");
                    co_return;
                }

                std::size_t totalBytes = 0;
                while (co_await stream->readNext())
                {
                    totalBytes += stream->chunk().size();
                }
                response.setBody("bytes=" + std::to_string(totalBytes));
                co_return;
            });
        };

        RunningHttpServerFixture fixture({}, std::chrono::milliseconds{50}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());

        // 只发头部并声明 Expect：对端在等本端的 100 表态
        std::string head = "POST /upload HTTP/1.1\r\nHost: loopback\r\nContent-Length: ";
        head += std::to_string(kTotalBytes);
        head += "\r\nExpect: 100-continue\r\n\r\n";
        ASSERT_TRUE(client.sendText(head, kWaitTimeout));

        std::string receivedText;
        ASSERT_TRUE(client.waitForText(receivedText, "100 Continue", kWaitTimeout)) << receivedText;

        ASSERT_TRUE(client.sendText(makePayload(kTotalBytes), kWaitTimeout));
        ASSERT_TRUE(client.waitForText(receivedText, "bytes=" + std::to_string(kTotalBytes), kWaitTimeout)) << receivedText;
    }
} // namespace AsynGyanis::Net
