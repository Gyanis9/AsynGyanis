// TestHttpStreaming.cpp —— 响应侧 chunked 流式发送（HttpResponse::startChunkedResponse/writeChunk + 会话收尾）：
//   一. 帧与头部：用假发送回调逐段核对 <hex 长度>\r\n<数据>\r\n 帧、头部只发一次、
//       带 transfer-encoding: chunked 且不带 content-length、空段不发；
//   二. 拒绝面：1xx/204/304 不能进流式模式、已设整块正文不能再进流式、流式下不能再设整块正文、
//       未进模式或未装配回调时 writeChunk 报错；
//   三. 真正流式：真实回环请求上分三段写出，客户端拿到按序相邻的三帧并以 0\r\n\r\n 收尾；
//   四. 边写边到：处理器写出第一段后**等客户端读到它**才继续写，客户端因此必须在该处理器结束前
//       拿到第一段与承载它的头部 —— 这是本功能的要点，必须实测；
//   五. 消息边界：终止块之后同一条连接上的后续请求（含「一段都没写的流式响应」）照常应答；
//   六. 半途断开与半途异常：客户端中途关闭、处理器写了一段后抛异常，两种都不得崩溃或挂死；
//   七. 卡住的写侧必须被放出来：客户端中途关闭（内核回 RST）与客户端只停读（只能靠写超时 + 清扫
//       关连接）两种情况，停等可写的处理器协程都要在有限时间内被唤醒并收手（帧释放为判据）。
//
// 回环夹具（RunningHttpServerFixture / LoopbackClient）在 HttpTestSupport.h 中，与其它回环用例共用一份。

#include "Base/Exception/Exception.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpResponse.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    using namespace HttpTestSupport;

    namespace
    {
        /// 客户端中途断开后的等待上限：写侧要等内核回 RST 或等写超时清扫，比一般等待更宽松
        constexpr std::chrono::milliseconds kDisconnectWaitTimeout{6000};

        /**
         * @brief 在测试线程上同步驱动一次 writeChunk 并取出结果
         * @details writeChunk 是惰性协程：不 resume 就不执行函数体，因此「直接 EXPECT_THROW 包住
         *          writeChunk(...)」什么都测不到。这里按「resume 到终结点 + await_resume 取结果」
         *          两步驱动（与 TestTask 同一手法），抛出的异常会在取结果时重新抛出
         * @param chunkTask writeChunk 返回的任务，所有权随参数转移
         * @return true 该段已写出
         */
        bool driveWriteChunk(Core::Task<bool> chunkTask)
        {
            chunkTask.handle().resume();
            return chunkTask.await_resume();
        }

        /**
         * @brief 处理器协程帧的存活探针
         * @details 进入协程体置位、退出（正常返回或异常展开）复位。处理器卡在写等待上时该标记
         *          会一直为真，因此它是「协程到底有没有被唤醒并收手」的唯一可靠判据——
         *          帧若一直挂着，既不写日志也不会让任何别的断言失败
         */
        class HandlerFrameGuard
        {
        public:
            explicit HandlerFrameGuard(std::atomic<bool> &isAlive) :
                m_isAlive(isAlive)
            {
                m_isAlive.store(true, std::memory_order_release);
            }

            ~HandlerFrameGuard()
            {
                m_isAlive.store(false, std::memory_order_release);
            }

            HandlerFrameGuard(const HandlerFrameGuard &) = delete;

            HandlerFrameGuard &operator=(const HandlerFrameGuard &) = delete;

        private:
            std::atomic<bool> &m_isAlive; ///< 被观测的存活标记（非拥有，随用例局部量存活）
        };

        /**
         * @brief 注册一条把三段正文依次写出的流式路由
         * @param router 目标路由器
         * @param path 路由路径
         * @param loop 事件循环，供处理函数内建定时器
         * @param parts 依次写出的正文段
         * @details 段间空一小会儿：分段是真被发出去的，而不是在内存里攒成一份整块正文
         */
        void addStreamingRoute(Router &router, const std::string &path, Core::EventLoop &loop,
                               const std::array<std::string_view, 3> &parts)
        {
            router.get(path, [&loop, parts](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.startChunkedResponse(200);
                response.setHeader("content-type", "text/event-stream");

                Core::Timer gapTimer(loop);
                for (const std::string_view part: parts)
                {
                    co_await gapTimer.waitFor(std::chrono::milliseconds(5));
                    if (!co_await response.writeChunk(part))
                    {
                        co_return;
                    }
                }
                co_return;
            });
        }
    } // namespace

    TEST(HttpStreaming, WritesChunkFramesAndChunkedHeadExactlyOnce)
    {
        // 钉住序列化层：头部长什么样、帧长什么样、头部只发一次、空段不发。
        // 用假发送回调驱动，不依赖任何传输层
        HttpResponse response;
        std::string  sentBytes;
        response.setChunkSender(
                [&sentBytes](const std::string_view segment) -> Core::Task<bool>
                {
                    sentBytes.append(segment);
                    co_return true;
                });

        response.startChunkedResponse(201);
        EXPECT_TRUE(response.isChunkedResponse());
        EXPECT_FALSE(response.hasSentChunkedHead());
        response.setHeader("content-type", "text/event-stream");

        // 第一段：头部（含 chunked 声明）随首段一起上线
        ASSERT_TRUE(driveWriteChunk(response.writeChunk("hello")));
        EXPECT_TRUE(response.hasSentChunkedHead());

        const std::size_t headEndPosition = sentBytes.find("\r\n\r\n");
        ASSERT_NE(headEndPosition, std::string::npos) << "首段里没有完整的头部块：" << sentBytes;
        const std::string headText = sentBytes.substr(0, headEndPosition);
        EXPECT_NE(headText.find("HTTP/1.1 201"), std::string::npos) << headText;
        EXPECT_NE(headText.find("transfer-encoding: chunked"), std::string::npos) << headText;
        EXPECT_NE(headText.find("content-type: text/event-stream"), std::string::npos) << headText;
        EXPECT_EQ(headText.find("content-length"), std::string::npos) << "流式响应不得出现 content-length：" << headText;
        EXPECT_EQ(sentBytes.substr(headEndPosition + 4), "5\r\nhello\r\n") << "首段之后应紧跟一个分块帧：" << sentBytes;

        // 第二段：头部不再重复，只有帧本身
        const std::size_t firstFrameEndPosition = sentBytes.size();
        ASSERT_TRUE(driveWriteChunk(response.writeChunk(" world!")));
        EXPECT_EQ(sentBytes.substr(firstFrameEndPosition), "7\r\n world!\r\n");
        EXPECT_EQ(countStatusLines(sentBytes), 1u) << "头部被重复发出：" << sentBytes;

        // 空段：允许调用但不发任何字节（零长度块是终止块的语义）
        const std::size_t lengthBeforeEmptyChunk = sentBytes.size();
        EXPECT_TRUE(driveWriteChunk(response.writeChunk("")));
        EXPECT_EQ(sentBytes.size(), lengthBeforeEmptyChunk) << "空段不该产生任何字节";
    }

    TEST(HttpStreaming, RejectsMisuseOfStreamingApi)
    {
        // 钉住拒绝面：状态码不允许正文、正文与流式互斥（两个方向）、未进模式与未装配回调
        for (const int bodylessStatusCode: {100, 103, 204, 304})
        {
            HttpResponse response;
            EXPECT_THROW(response.startChunkedResponse(bodylessStatusCode), Base::LogicException)
                    << "状态码 " << bodylessStatusCode << " 的响应不允许正文，不该被接受";
        }

        // 方向一：已经设过整块正文，再进流式模式（不报错就等于把这段正文悄悄丢掉）
        HttpResponse withBody;
        withBody.setBody("already-set");
        EXPECT_THROW(withBody.startChunkedResponse(200), Base::LogicException);

        // 方向二：流式模式下不能再设整块正文，堆正文与映射正文两条路都要拦
        HttpResponse chunked;
        chunked.startChunkedResponse(200);
        EXPECT_THROW(chunked.setBody("late-body"), Base::LogicException);
        EXPECT_THROW(chunked.setMappedBody(Platform::MemoryMappedFile{}), Base::LogicException);

        // 未装配发送回调（会话之外构造的响应对象）：报错而不是把正文段静默丢掉
        EXPECT_THROW(static_cast<void>(driveWriteChunk(chunked.writeChunk("data"))), Base::LogicException);

        // 未进入流式模式就写正文段：文案必须指出正确的做法
        HttpResponse plain;
        try
        {
            static_cast<void>(driveWriteChunk(plain.writeChunk("data")));
            FAIL() << "非流式模式下 writeChunk 应当抛异常";
        } catch (const Base::LogicException &exception)
        {
            EXPECT_NE(std::string(exception.what()).find("startChunkedResponse"), std::string::npos)
                    << "报错文案应指出先调用 startChunkedResponse：" << exception.what();
        }

        // 头部上线之后再改状态码：改什么都到不了对端，必须报错
        HttpResponse streaming;
        streaming.setChunkSender(
                [](const std::string_view) -> Core::Task<bool>
                {
                    co_return true;
                });
        streaming.startChunkedResponse(200);
        ASSERT_TRUE(driveWriteChunk(streaming.writeChunk("first")));
        EXPECT_TRUE(streaming.hasSentChunkedHead());
        EXPECT_THROW(streaming.startChunkedResponse(503), Base::LogicException);
    }

    TEST(HttpStreaming, StreamsThreeChunksInOrderOverRealLoopback)
    {
        // 钉住真正流式：一条真实连接上分三次写出的正文，客户端按序拿到三帧并以终止块收尾
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                                         [](Router &router, Core::EventLoop &loop)
                                         {
                                             addStreamingRoute(router, "/stream", loop,
                                                               std::array<std::string_view, 3>{"first-part", "second-part", "third-part"});
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_FALSE(fixture.startThrew());

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /stream HTTP/1.1"), kWaitTimeout));

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "0\r\n\r\n", kWaitTimeout))
                << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        EXPECT_NE(responseText.find("HTTP/1.1 200"), std::string::npos) << responseText;
        EXPECT_NE(responseText.find("transfer-encoding: chunked"), std::string::npos) << responseText;
        EXPECT_EQ(responseText.find("content-length"), std::string::npos) << "流式响应不得写 content-length：" << responseText;

        // 三帧相邻且按序：中间没有别的内容，正文就是三段拼接（长度分别为 a/b/a 的十六进制）
        constexpr std::string_view kFirstFrame  = "a\r\nfirst-part\r\n";
        constexpr std::string_view kSecondFrame = "b\r\nsecond-part\r\n";
        constexpr std::string_view kThirdFrame  = "a\r\nthird-part\r\n";

        const std::size_t firstFramePosition  = responseText.find(kFirstFrame);
        const std::size_t secondFramePosition = responseText.find(kSecondFrame);
        const std::size_t thirdFramePosition  = responseText.find(kThirdFrame);
        ASSERT_NE(firstFramePosition, std::string::npos) << responseText;
        ASSERT_NE(secondFramePosition, std::string::npos) << responseText;
        ASSERT_NE(thirdFramePosition, std::string::npos) << responseText;
        EXPECT_EQ(secondFramePosition, firstFramePosition + kFirstFrame.size()) << "两段之间夹了别的内容：" << responseText;
        EXPECT_EQ(thirdFramePosition, secondFramePosition + kSecondFrame.size()) << "两段之间夹了别的内容：" << responseText;
        EXPECT_TRUE(responseText.ends_with(std::string(kThirdFrame) + "0\r\n\r\n"))
                << "末尾应当是最后一段紧跟终止块：" << responseText;

        client.closeNow();
    }

    TEST(HttpStreaming, FirstChunkReachesClientBeforeHandlerFinishes)
    {
        // 钉住「边写边到」：处理器写完第一段后卡在「等客户端读到它」上，客户端因此只能在
        // 处理器尚未结束时读到第一段与承载它的头部。框架若把正文攒到处理器结束才发，
        // 这个等待必然超时（客户端读不到）或读到时处理器已结束（断言失败）
        std::atomic<bool> clientObservedFirstChunk{false};
        std::atomic<bool> handlerFinished{false};

        RunningHttpServerFixture fixture(
                HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                [&clientObservedFirstChunk, &handlerFinished](Router &router, Core::EventLoop &loop)
                {
                    router.get("/early",
                               [&loop, &clientObservedFirstChunk, &handlerFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        response.startChunkedResponse(200);
                        response.setHeader("content-type", "text/event-stream");
                        if (!co_await response.writeChunk("first-part"))
                        {
                            handlerFinished.store(true, std::memory_order_release);
                            co_return;
                        }

                        // 第一段没被对端读到就不往下写。轮询上限 2 秒（远大于客户端读第一段的
                        // 期望耗时）保证实现有问题时用例干净失败，而不是把测试线程挂死
                        Core::Timer waitTimer(loop);
                        for (int pollRound = 0; pollRound < 2000 && !clientObservedFirstChunk.load(std::memory_order_acquire);
                             ++pollRound)
                        {
                            co_await waitTimer.waitFor(std::chrono::milliseconds(1));
                        }

                        co_await response.writeChunk("second-part");
                        handlerFinished.store(true, std::memory_order_release);
                        co_return;
                    });
                });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /early HTTP/1.1"), kWaitTimeout));

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "a\r\nfirst-part\r\n", kWaitTimeout))
                << "处理器结束之前没能读到第一段（正文被攒到处理器结束才发）：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_FALSE(handlerFinished.load(std::memory_order_acquire))
                << "读到第一段时处理器已经结束，无法证明正文是边写边到的";
        EXPECT_NE(responseText.find("transfer-encoding: chunked"), std::string::npos)
                << "第一段到达时，承载它的头部也必须已经到达：" << responseText;
        EXPECT_EQ(responseText.find("second-part"), std::string::npos) << "读到第一段时不该已经出现第二段";

        // 放行处理器：它接着写第二段，会话再补终止块
        clientObservedFirstChunk.store(true, std::memory_order_release);
        ASSERT_TRUE(client.waitForText(responseText, "0\r\n\r\n", kWaitTimeout))
                << "第二段与终止块未在时限内到达：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_TRUE(handlerFinished.load(std::memory_order_acquire));
        EXPECT_NE(responseText.find("b\r\nsecond-part\r\n"), std::string::npos) << responseText;
        EXPECT_TRUE(responseText.ends_with("0\r\n\r\n")) << responseText;

        client.closeNow();
    }

    TEST(HttpStreaming, KeepsConnectionUsableAcrossChunkedAndPlainResponses)
    {
        // 钉住消息边界：chunked 的 0\r\n\r\n 就是本条消息的末尾，因此同一条连接上还能继续发请求；
        // 「一段都没写的流式响应」也必须补出头部与终止块，否则对端会一直等下去
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/stream-once", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                             {
                                                 response.startChunkedResponse(200);
                                                 response.setHeader("x-stream", "once");
                                                 co_await response.writeChunk("streamed-once");
                                                 co_return;
                                             });
                                             // 只声明进入流式模式、一段都不写：收尾由会话补头部与终止块
                                             router.get("/empty-stream", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                             {
                                                 response.startChunkedResponse(200);
                                                 co_return;
                                             });
                                         });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 第一条：写了正文段的流式响应
        ASSERT_TRUE(client.sendText(makeRequestText("GET /stream-once HTTP/1.1"), kWaitTimeout));
        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "0\r\n\r\n", kWaitTimeout))
                << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;
        const std::size_t firstResponseLength = responseText.size();
        EXPECT_NE(responseText.find("d\r\nstreamed-once\r\n"), std::string::npos) << responseText;
        EXPECT_EQ(responseText.find("content-length"), std::string::npos) << "流式响应不得写 content-length：" << responseText;
        EXPECT_EQ(responseText.find("connection: close"), std::string::npos)
                << "消息边界已由终止块给出，不必为流式响应额外宣告 close：" << responseText;

        // 第二条：同一连接上的空流式响应（头部与终止块一起发出）
        ASSERT_TRUE(client.sendText(makeRequestText("GET /empty-stream HTTP/1.1"), kWaitTimeout));
        ASSERT_TRUE(client.waitForText(responseText, "\r\n\r\n0\r\n\r\n", kWaitTimeout))
                << "空流式响应未在时限内补出头部与终止块：上界 kWaitTimeout，已收到：" << responseText;
        const std::string emptyStreamResponse = responseText.substr(firstResponseLength);
        EXPECT_NE(emptyStreamResponse.find("HTTP/1.1 200"), std::string::npos) << emptyStreamResponse;
        EXPECT_NE(emptyStreamResponse.find("transfer-encoding: chunked"), std::string::npos) << emptyStreamResponse;
        EXPECT_EQ(emptyStreamResponse.find("content-length"), std::string::npos) << "流式响应不得写 content-length：" << emptyStreamResponse;

        // 第三条：普通响应照常被服务，证明两条流式消息的边界都没有多算或漏算
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1"), kWaitTimeout));
        ASSERT_TRUE(client.waitForText(responseText, "served-hello", kWaitTimeout))
                << "流式响应之后同一条连接上的普通请求没被服务：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_EQ(countStatusLines(responseText), 3u) << "应当恰好三条响应：" << responseText;

        client.closeNow();
    }

    TEST(HttpStreaming, TerminatesAndClosesWhenHandlerThrowsAfterFirstChunk)
    {
        // 钉住异常收尾：头部已随第一段上线，改 500 是不可能的，只能补终止块并按关闭收口
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/boom-stream", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                             {
                                                 response.startChunkedResponse(200);
                                                 co_await response.writeChunk("half-body");
                                                 throw Base::Exception("测试用：流式响应写到一半业务抛异常");
                                                 co_return;
                                             });
                                         });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /boom-stream HTTP/1.1"), kWaitTimeout));

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "0\r\n\r\n", kWaitTimeout))
                << "半途抛异常后没有补终止块：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_NE(responseText.find("HTTP/1.1 200"), std::string::npos) << responseText;
        EXPECT_EQ(responseText.find("500"), std::string::npos) << "头部已上线，不可能再改 500：" << responseText;
        EXPECT_NE(responseText.find("9\r\nhalf-body\r\n"), std::string::npos) << responseText;
        EXPECT_TRUE(responseText.ends_with("0\r\n\r\n")) << responseText;

        // 正文只发了一半，连接不复用：会话收口，客户端读到 EOF
        EXPECT_TRUE(client.waitForClosure(responseText, kWaitTimeout)) << "半途失败之后连接没有收口";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话没有从连接管理器摘除";
        EXPECT_FALSE(fixture.startThrew());
    }

    TEST(HttpStreaming, SurvivesClientDisconnectMidStream)
    {
        // 钉住半途断开：客户端读到第一段后关闭连接，写侧迟早失败，处理器据此收手、会话正常收尾，
        // 整个过程不得崩溃或挂死，且**处理器协程必须在有限时间内被释放**（帧一直挂着同样是缺陷：
        // 它连同连接与缓冲会滞留到进程退出，而这一点不会有任何日志或断言失败来提示）。
        //
        // 本平台（Windows/wepoll）上写侧有两条被唤醒的路径，两条都在断言覆盖之下：
        // 一、对端带未读数据关闭会让内核回 RST，epoll 上报错误事件，等可写的协程由该事件恢复；
        // 二、关闭描述符本身不会唤醒 epoll 等待者——唤醒由 IoWatcher 析构把等待者投回调度器完成
        //     （见 Core 侧 CloseWakesCoroutineBlockedOnSend 与本文件 ReleasesWriterParkedOnSweepClose）。
        // 处理器最终以传输层异常而不是 writeChunk 返回 false 收场，因此「有没有收手」只能由
        // 协程帧是否释放来判定，不能用处理器自己置的标记
        HttpServerLimits limits;
        limits.writeTimeout = std::chrono::milliseconds{200};

        std::atomic<bool> isHandlerFrameAlive{false};
        std::atomic<int>  writtenChunkCount{0};

        RunningHttpServerFixture fixture(
                limits, std::chrono::milliseconds{50}, {},
                [&isHandlerFrameAlive, &writtenChunkCount](Router &router, Core::EventLoop &)
                {
                    router.get("/endless", [&isHandlerFrameAlive, &writtenChunkCount](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        const HandlerFrameGuard frameGuard(isHandlerFrameAlive);
                        response.startChunkedResponse(200);

                        // 一直写到对端不再可用为止：断开的信号从 writeChunk 的返回值（对端已收口）
                        // 或它抛出的传输层异常（写等待被关闭/事件失败）上来，处理器不做特殊处理。
                        // 轮数上限只为兜住极端情况
                        const std::string payload(64 * 1024, 'x');
                        for (int round = 0; round < 64; ++round)
                        {
                            if (!co_await response.writeChunk(payload))
                            {
                                break;
                            }
                            writtenChunkCount.fetch_add(1, std::memory_order_relaxed);
                        }

                        co_return;
                    });
                });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /endless HTTP/1.1"), kWaitTimeout));

        // 先确认流真的在往外写：读到头部即说明首段（头部随首段上线）已经发出
        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "transfer-encoding: chunked", kWaitTimeout))
                << "流式响应未开始：上界 kWaitTimeout，已收到：" << responseText;
        // 64 KiB 的段长写出来就是十六进制的 10000，后面紧跟正文首字节
        ASSERT_TRUE(client.waitForText(responseText, "10000\r\nx", kWaitTimeout)) << "第一段正文没发出来：" << responseText;

        // 中途断开：客户端接收缓冲里还有未读字节，close 会让内核回 RST，写侧随即失败
        client.closeNow();

        // 会话必须收口（写超时 + 清扫兜底），且服务器主协程不得被带崩
        EXPECT_FALSE(fixture.startThrew()) << "一条断开的流式连接把服务器主协程带崩了";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kDisconnectWaitTimeout)) << "会话在客户端断开后没有收口";
        // 会话收口意味着处理器已被 await 完：此刻它必须已经离开等可写、帧已释放
        EXPECT_FALSE(isHandlerFrameAlive.load(std::memory_order_acquire))
                << "会话都收口了，处理器协程仍挂在写等待上：帧与缓冲一直滞留，直到进程退出";
        EXPECT_LT(writtenChunkCount.load(std::memory_order_relaxed), 64)
                << "对端已经断开，写侧却宣称 64 段全部成功";
    }

    TEST(HttpStreaming, ReleasesWriterParkedOnSweepClose)
    {
        // 钉住「关闭描述符必须自己唤醒等可写的协程」这条路径：客户端只停读、**不关连接**，
        // 内核不会回 RST，因此没有任何错误事件可用——写侧唯一的唤醒者就是清扫协程的
        // connection->close()（经 IoWatcher 析构把等待者投回调度器并以「未就绪」恢复）。
        // 少了这一步，卡在 writeChunk 上的处理器会一直挂着，帧与连接滞留到进程退出
        HttpServerLimits limits;
        limits.writeTimeout = std::chrono::milliseconds{200};

        std::atomic<bool> isHandlerFrameAlive{false};
        std::atomic<int>  writtenChunkCount{0};

        RunningHttpServerFixture fixture(
                limits, std::chrono::milliseconds{50}, {},
                [&isHandlerFrameAlive, &writtenChunkCount](Router &router, Core::EventLoop &)
                {
                    router.get("/endless", [&isHandlerFrameAlive, &writtenChunkCount](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        const HandlerFrameGuard frameGuard(isHandlerFrameAlive);
                        response.startChunkedResponse(200);

                        // 一直写到对端读不动为止：这里不会有 RST，写侧只会落在等可写上，
                        // 直到写超时到期后清扫协程关掉连接
                        const std::string payload(64 * 1024, 'x');
                        for (int round = 0; round < 64; ++round)
                        {
                            if (!co_await response.writeChunk(payload))
                            {
                                break;
                            }
                            writtenChunkCount.fetch_add(1, std::memory_order_relaxed);
                        }

                        co_return;
                    });
                });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /endless HTTP/1.1"), kWaitTimeout));

        // 读一小段确认流已经开始，随后停读且**不关连接**：写侧因此只能等写超时
        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "10000\r\nx", kWaitTimeout)) << "第一段正文没发出来：" << responseText;

        // 写超时 200ms + 清扫节拍 50ms：清扫关连接必须把停在等可写上的处理器放出来
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kDisconnectWaitTimeout)) << "慢消费者连接没有被清扫收口";
        EXPECT_FALSE(isHandlerFrameAlive.load(std::memory_order_acquire))
                << "清扫关连接之后处理器协程仍挂在写等待上：关闭没有唤醒等可写的等待者";
        EXPECT_LT(writtenChunkCount.load(std::memory_order_relaxed), 64)
                << "对端早已停止读取，写侧却宣称 64 段全部成功";
        EXPECT_FALSE(fixture.startThrew()) << "把写侧卡住的连接收口时把服务器主协程带崩了";
    }
} // namespace AsynGyanis::Net
