// TestSseStream.cpp —— SSE 事件流（SseStream）：
//   一. 头部与帧：真实回环上逐字节核对头部（chunked、text/event-stream、cache-control: no-cache、
//       无 content-length）与帧内容；
//   二. 帧格式：多行 data 的三种换行都拆行且帧内不残留 \r、event → id → retry → data 的字段顺序、
//       空 data 仍产出一条空 data: 行、注释帧且不影响后续事件；
//   三. 拒绝面：eventName/eventId 含 CR/LF/NUL、单帧超上限（含「恰好等于上限」的对照）、retry 为负；
//   四. 渐进性：第二条事件只在处理器被放行之后才到达；
//   五. 连接不可用后短路返回 false（不再写连接、也不再校验参数）。
//
// 时序约定：凡对响应字节做「完整帧解码」的断言，都必须先等到终止块 "0\r\n\r\n"（见 waitForChunkedEnd），
// 否则响应仍在途、帧不完整，解码助手必然失败。渐进性用例按设计不等待终止块，改用子串断言。
//
// 回环夹具（RunningHttpServerFixture / LoopbackClient）在 HttpTestSupport.h 中，与其它回环用例共用一份。

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/SseStream.h"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Net
{
    using namespace HttpTestSupport;

    namespace
    {
        /// 事件帧里 data 部分之外的固定开销："data: " 前缀（6 字节）加行尾与收尾空行（各 1 字节）
        constexpr std::size_t kDataFrameOverhead = 6U + 2U;

        /**
         * @brief 驱动一个惰性协程到终结点并取出 bool 结果
         * @details SseStream 的接口都是惰性协程：不 resume 就不执行函数体，因此「直接 EXPECT_THROW
         *          包住 sendEvent(...)」什么都测不到。这里按「resume 到终结点 + await_resume 取结果」
         *          两步驱动（与 TestHttpStreaming 的 driveWriteChunk 同一手法）；协程体内抛出的异常
         *          在 await_resume() 时重新抛出，所以 EXPECT_THROW 要包住本调用
         * @param task 待驱动的任务，所有权随参数转移
         * @return bool 协程的返回值
         */
        bool driveTask(Core::Task<bool> task)
        {
            task.handle().resume();
            return task.await_resume();
        }

        /**
         * @brief 等客户端收到分块传输的终止块
         * @details 终止块到达即意味着处理器已结束、流式响应正常收尾，此后响应字节才是完整的
         * @param client 回环客户端
         * @param responseText 输入输出：累计读到的字节
         * @return true 在 kWaitTimeout 内收到终止块
         */
        bool waitForChunkedEnd(LoopbackClient &client, std::string &responseText)
        {
            return client.waitForText(responseText, "0\r\n\r\n", kWaitTimeout);
        }

        /**
         * @brief 把 chunked 响应的正文解码出来
         * @details **只在确认收到终止块之后调用**：本函数要求解到 "0\r\n\r\n" 才返回 true，
         *          响应还在途时帧不完整，必然失败。不处理 chunk 扩展，本框架也不发扩展。
         * @param rawResponse 已收到的全部响应字节
         * @param payload 输出：解码出的正文，进入调用时先清空
         * @return true 已解到终止块
         */
        bool decodeChunkedBody(const std::string &rawResponse, std::string &payload)
        {
            // 出参进入调用时先清空：调用方复用同一个串时不会读到上一次的残留
            payload.clear();

            const std::size_t headEndPosition = rawResponse.find("\r\n\r\n");
            if (headEndPosition == std::string::npos)
            {
                return false;
            }

            std::size_t cursor = headEndPosition + 4;
            while (true)
            {
                const std::size_t lengthLineEnd = rawResponse.find("\r\n", cursor);
                if (lengthLineEnd == std::string::npos)
                {
                    return false;
                }

                // 长度行是十六进制的字节数，逐字符消费完才算合法
                const std::string_view lengthText(rawResponse.data() + cursor, lengthLineEnd - cursor);
                std::size_t            chunkLength = 0;
                const auto [parsedEnd, parseError] =
                        std::from_chars(lengthText.data(), lengthText.data() + lengthText.size(), chunkLength, 16);
                if (parseError != std::errc() || parsedEnd != lengthText.data() + lengthText.size())
                {
                    return false;
                }

                cursor = lengthLineEnd + 2;
                if (chunkLength == 0)
                {
                    // 零长度块就是终止块：正文到此结束
                    return true;
                }
                if (cursor + chunkLength + 2 > rawResponse.size())
                {
                    return false;
                }

                payload.append(rawResponse, cursor, chunkLength);
                cursor += chunkLength;
                if (rawResponse.compare(cursor, 2, "\r\n") != 0)
                {
                    return false;
                }
                cursor += 2;
            }
        }

        /**
         * @brief 起一台服务器、注册一条 SSE 路由并返回客户端已收到的完整响应
         * @details 注册的路由只做一件事：把 handler 交给本次请求的响应对象执行。
         * @param path 路由路径
         * @param handler 处理函数
         * @param responseText 输出：累计读到的响应字节
         * @return true 已等到终止块，响应字节完整
         */
        template<typename HandlerType>
        bool runSseRequest(const std::string &path, HandlerType handler, std::string &responseText)
        {
            RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                                             [&path, handler](Router &router, Core::EventLoop &)
                                             {
                                                 router.get(path, handler);
                                             });

            if (!fixture.awaitRunning(kWaitTimeout))
            {
                return false;
            }
            const std::uint16_t listeningPort = fixture.listeningPort();
            if (listeningPort == 0)
            {
                return false;
            }

            LoopbackClient client(listeningPort);
            if (!client.isValid())
            {
                return false;
            }
            if (!client.sendText(makeRequestText("GET " + path + " HTTP/1.1"), kWaitTimeout))
            {
                return false;
            }
            return waitForChunkedEnd(client, responseText);
        }
    } // namespace

    TEST(SseStream, WritesSseHeadAndEventFrameBytesExactly)
    {
        // 钉住头部与帧的逐字节形态：头部必须声明 chunked 与 text/event-stream、不得出现 content-length，
        // 帧必须恰好是「data: hello + 行尾 + 空行」。先等终止块，响应字节完整后再解码
        std::string responseText;
        const bool  isComplete = runSseRequest("/sse-head",
                                               [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                               {
                                                   SseStream stream(response);
                                                   co_await stream.sendEvent("hello");
                                                   co_return;
                                               },
                                               responseText);
        ASSERT_TRUE(isComplete) << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        const std::size_t headEndPosition = responseText.find("\r\n\r\n");
        ASSERT_NE(headEndPosition, std::string::npos) << "响应里没有完整的头部块：" << responseText;
        const std::string headText = responseText.substr(0, headEndPosition);
        EXPECT_NE(headText.find("HTTP/1.1 200"), std::string::npos) << headText;
        EXPECT_NE(headText.find("content-type: text/event-stream"), std::string::npos) << headText;
        EXPECT_NE(headText.find("cache-control: no-cache"), std::string::npos) << headText;
        EXPECT_NE(headText.find("transfer-encoding: chunked"), std::string::npos) << headText;
        EXPECT_EQ(headText.find("content-length"), std::string::npos) << "SSE 不得带 content-length：" << headText;

        std::string payload;
        ASSERT_TRUE(decodeChunkedBody(responseText, payload)) << "响应字节无法按 chunked 解码：" << responseText;
        EXPECT_EQ(payload, "data: hello\n\n") << "帧内容应当逐字节稳定，实际：" << payload;
    }

    TEST(SseStream, SplitsMultilineDataOnAllLineBreakFormsWithoutCarriageReturn)
    {
        // 钉住 data 的拆行：\n、\r\n 与裸 \r 都算换行，每段一行 data:，帧内不残留 \r
        std::string responseText;
        const bool  isComplete = runSseRequest("/sse-multiline",
                                               [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                               {
                                                   SseStream stream(response);
                                                   co_await stream.sendEvent("line-one\nline-two\rline-three\r\nline-four");
                                                   co_return;
                                               },
                                               responseText);
        ASSERT_TRUE(isComplete) << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        std::string payload;
        ASSERT_TRUE(decodeChunkedBody(responseText, payload)) << "响应字节无法按 chunked 解码：" << responseText;
        EXPECT_EQ(payload, "data: line-one\ndata: line-two\ndata: line-three\ndata: line-four\n\n")
                << "三种换行都应当拆成独立的 data: 行，实际：" << payload;
        EXPECT_EQ(payload.find('\r'), std::string::npos) << "帧内不得残留 CR：" << payload;
    }

    TEST(SseStream, WritesEventIdRetryAndDataFieldsInOrder)
    {
        // 钉住字段顺序与格式：event → id → retry → data，末尾一个空行
        std::string responseText;
        const bool  isComplete = runSseRequest("/sse-fields",
                                               [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                               {
                                                   SseStream stream(response);
                                                   co_await stream.sendEvent("payload-value", "update", "42",
                                                                             std::chrono::milliseconds{3000});
                                                   co_return;
                                               },
                                               responseText);
        ASSERT_TRUE(isComplete) << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        std::string payload;
        ASSERT_TRUE(decodeChunkedBody(responseText, payload)) << "响应字节无法按 chunked 解码：" << responseText;
        EXPECT_EQ(payload, "event: update\nid: 42\nretry: 3000\ndata: payload-value\n\n")
                << "字段顺序或格式与约定不符，实际：" << payload;
    }

    TEST(SseStream, EmitsEmptyDataLineForEmptyData)
    {
        // 钉住空 data：仍是一条合法事件，产出一条空 data: 行加收尾空行
        std::string responseText;
        const bool  isComplete = runSseRequest("/sse-empty-data",
                                               [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                               {
                                                   SseStream stream(response);
                                                   co_await stream.sendEvent("");
                                                   co_return;
                                               },
                                               responseText);
        ASSERT_TRUE(isComplete) << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        std::string payload;
        ASSERT_TRUE(decodeChunkedBody(responseText, payload)) << "响应字节无法按 chunked 解码：" << responseText;
        EXPECT_EQ(payload, "data: \n\n") << "空 data 应当产出一条空 data: 行，实际：" << payload;
    }

    TEST(SseStream, SendsCommentFramesWithoutDisturbingFollowingEvents)
    {
        // 钉住注释帧：`: <注释>\n\n`，注释文本里的换行拆成多条注释行，且不吞掉后续事件的帧
        std::string responseText;
        const bool  isComplete = runSseRequest("/sse-comment",
                                               [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                               {
                                                   SseStream stream(response);
                                                   co_await stream.sendComment("ping\npong");
                                                   co_await stream.sendEvent("after-comment");
                                                   co_return;
                                               },
                                               responseText);
        ASSERT_TRUE(isComplete) << "流式响应未以终止块收尾：上界 kWaitTimeout，已收到：" << responseText;

        std::string payload;
        ASSERT_TRUE(decodeChunkedBody(responseText, payload)) << "响应字节无法按 chunked 解码：" << responseText;
        EXPECT_EQ(payload, ": ping\n: pong\n\ndata: after-comment\n\n")
                << "注释帧格式不对，或影响了后续事件，实际：" << payload;
    }

    TEST(SseStream, RejectsInvalidEventFieldsOversizedFramesAndNegativeRetry)
    {
        // 钉住拒绝面：单行字段含 CR/LF/NUL、整帧超上限（连边界一起对照）、retry 为负。
        // 校验发生在写任何字节之前，因此不需要真实连接，用一句「丢弃字节」的假回调驱动即可
        HttpResponse response;
        response.setChunkSender(
                [](const std::string_view) -> Core::Task<bool>
                {
                    co_return true;
                });
        SseStream stream(response);
        EXPECT_TRUE(stream.isOpen());

        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", "bad\nevent"))), Base::LogicException);
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", "bad\revent"))), Base::LogicException);
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", std::string_view("bad\0event", 9)))),
                     Base::LogicException);
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", "ok", "bad\nid"))), Base::LogicException);
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", "ok", "bad\rid"))), Base::LogicException);
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", "ok", std::string_view("bad\0id", 6)))),
                     Base::LogicException);

        // retry 是用法错误：抛兄弟分支 invalid_argument，与 LogicException 同属 std::logic_error
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent("data", {}, {}, std::chrono::milliseconds{-1}))),
                     Base::InvalidArgumentException);

        // 边界对照：帧长度恰好等于上限时接受，多一个字节即拒绝 —— 差异只来自帧长
        const std::string atLimitData(SseStream::kMaximumFrameLength - kDataFrameOverhead, 'x');
        EXPECT_TRUE(driveTask(stream.sendEvent(atLimitData))) << "帧恰好等于上限时应当被接受";

        const std::string overLimitData(SseStream::kMaximumFrameLength - kDataFrameOverhead + 1, 'x');
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendEvent(overLimitData))), Base::LogicException);

        // 注释帧同样受单帧上限约束
        const std::string overLimitComment(SseStream::kMaximumFrameLength, 'y');
        EXPECT_THROW(static_cast<void>(driveTask(stream.sendComment(overLimitComment))), Base::LogicException);

        // 被拒的帧一个字节都没写出去，流本身仍然可用
        EXPECT_TRUE(stream.isOpen()) << "校验失败不该把流标记成不可用";
    }

    TEST(SseStream, DeliversSecondEventOnlyAfterHandlerIsReleased)
    {
        // 钉住渐进性：处理器写出第一条事件后停在「等客户端读到它」上，此时第二条尚未产生。
        // 本用例只做子串断言：等终止块会把「第二条是否真的在放行之后才到」这个观测点抹掉
        std::atomic<bool> clientObservedFirstEvent{false};
        std::atomic<bool> handlerFinished{false};

        RunningHttpServerFixture fixture(
                HttpServerLimits{}, std::chrono::milliseconds{50}, {},
                [&clientObservedFirstEvent, &handlerFinished](Router &router, Core::EventLoop &loop)
                {
                    router.get("/sse-progressive",
                               [&loop, &clientObservedFirstEvent, &handlerFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        SseStream stream(response);
                        if (!co_await stream.sendEvent("first-event-marker"))
                        {
                            handlerFinished.store(true, std::memory_order_release);
                            co_return;
                        }

                        // 第一帧没被对端读到就不往下写。轮询上限 2 秒（远大于客户端读第一帧的期望
                        // 耗时）保证实现有问题时用例干净失败，而不是把测试线程挂死
                        Core::Timer waitTimer(loop);
                        for (int pollRound = 0;
                             pollRound < 2000 && !clientObservedFirstEvent.load(std::memory_order_acquire); ++pollRound)
                        {
                            co_await waitTimer.waitFor(std::chrono::milliseconds(1));
                        }

                        co_await stream.sendEvent("second-event-marker");
                        handlerFinished.store(true, std::memory_order_release);
                        co_return;
                    });
                });

        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);
        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /sse-progressive HTTP/1.1"), kWaitTimeout));

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "data: first-event-marker", kWaitTimeout))
                << "处理器结束之前没能读到第一条事件（帧被攒到处理器结束才发）：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_FALSE(handlerFinished.load(std::memory_order_acquire))
                << "读到第一条事件时处理器已经结束，无法证明事件是边写边到的";
        EXPECT_EQ(responseText.find("data: second-event-marker"), std::string::npos)
                << "读到第一条事件时不该已经出现第二条：" << responseText;

        // 放行处理器：它接着写第二条事件；这里仍然只等子串，不等终止块
        clientObservedFirstEvent.store(true, std::memory_order_release);
        ASSERT_TRUE(client.waitForText(responseText, "data: second-event-marker", kWaitTimeout))
                << "放行之后第二条事件未到达：上界 kWaitTimeout，已收到：" << responseText;
        EXPECT_NE(responseText.find("content-type: text/event-stream"), std::string::npos) << responseText;

        client.closeNow();
    }

    TEST(SseStream, ShortCircuitsAfterTheConnectionBecomesUnavailable)
    {
        // 钉住短路：writeChunk 返回 false 之后 isOpen() 恒为 false，后续调用既不写连接也不抛异常
        // （对端已经收不到任何东西，抛「参数非法」只会误导调用方）
        int        writeAttemptCount = 0;
        HttpResponse response;
        response.setChunkSender(
                [&writeAttemptCount](const std::string_view) -> Core::Task<bool>
                {
                    ++writeAttemptCount;
                    co_return false;
                });
        SseStream stream(response);
        EXPECT_TRUE(stream.isOpen());

        EXPECT_FALSE(driveTask(stream.sendEvent("first"))) << "写出失败时应当返回 false";
        EXPECT_FALSE(stream.isOpen()) << "写出失败之后应当标记为不可用";
        const int attemptCountAfterFailure = writeAttemptCount;

        EXPECT_FALSE(driveTask(stream.sendEvent("second", "bad\nevent")));
        EXPECT_FALSE(driveTask(stream.sendEvent("third", {}, {}, std::chrono::milliseconds{-5})));
        EXPECT_FALSE(driveTask(stream.sendComment("late-comment")));
        EXPECT_EQ(writeAttemptCount, attemptCountAfterFailure) << "不可用之后不该再向连接写字节";
        EXPECT_FALSE(stream.isOpen());
    }
} // namespace AsynGyanis::Net
