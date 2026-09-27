// SSE 客户端解析：任意字节边界下的事件还原，以及配合出站接收口的一次真实往返

#include "Net/Http/Client/SseEventParser.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http/SseStream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::HttpClient;
    using AsynGyanis::Net::HttpClientRequest;
    using AsynGyanis::Net::HttpOutboundConnectionPool;
    using AsynGyanis::Net::HttpRequest;
    using AsynGyanis::Net::HttpResponse;
    using AsynGyanis::Net::Router;
    using AsynGyanis::Net::SseEvent;
    using AsynGyanis::Net::SseEventParser;
    using AsynGyanis::Net::SseStream;
    using AsynGyanis::Net::HttpTestSupport::RunningHttpServerFixture;
    using AsynGyanis::TestSupport::kWaitTimeout;

    constexpr std::chrono::milliseconds kRequestTimeout{8000};

    /// 把整段文本逐字节喂进去：这是对「切分边界」最苛刻的一种，比按行喂更能暴露状态机的问题
    std::vector<SseEvent> parseByteByByte(const std::string &stream)
    {
        SseEventParser parser;
        for (const char byte: stream)
        {
            parser.feed(std::string_view{&byte, 1});
        }
        parser.endOfStream();
        std::vector<SseEvent> events;
        while (const auto event = parser.nextEvent())
        {
            events.push_back(*event);
        }
        return events;
    }

    /// 把整段一次喂进去
    std::vector<SseEvent> parseWhole(const std::string &stream)
    {
        SseEventParser parser;
        parser.feed(stream);
        parser.endOfStream();
        std::vector<SseEvent> events;
        while (const auto event = parser.nextEvent())
        {
            events.push_back(*event);
        }
        return events;
    }

    /**
     * @brief 走一次真实的 SSE 往返：出站接收口逐批喂解析器，事件按到达次序落到 collected
     * @note 具名协程而不是 IIFE：后者的闭包对象在全表达式结束时就没了，恢复时读到的是已退栈的存储
     */
    Task<void> collectStreamTask(EventLoop &loop, HttpClient &client, std::string url, std::vector<SseEvent> &collected, std::string &failureReason)
    {
        SseEventParser    parser;
        HttpClientRequest request;
        request.responseBodyReceiver = [&parser, &collected](const AsynGyanis::Net::HttpResponseInfo &, const std::string_view batch, const bool isLastBatch) -> Task<bool>
        {
            parser.feed(batch);
            if (isLastBatch)
            {
                parser.endOfStream();
            }
            while (const auto event = parser.nextEvent())
            {
                collected.push_back(*event);
            }
            co_return true;
        };
        const std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response = co_await client.send(url, request, kRequestTimeout);
        if (response == nullptr)
        {
            failureReason = "出站请求失败（客户端交回空响应）";
        }
        loop.stop();
        co_return;
    }
} // namespace

namespace AsynGyanis::Net
{
    /**
     * @brief 钉住：三种行分隔符与 CRLF 被切成两半的情况都要能还原出同一条事件
     * @details 出站接收口给的批次边界完全由对端与网络决定，CRLF 跨批是常态：
     *          把单独的 CR 当成「行结束」会让紧跟的 LF 变成一条空行，事件就被凭空拆成两条
     */
    TEST(SseEventParser, RestoresEventsSplitAtArbitraryByteBoundaries)
    {
        const std::string           stream = "event: alarm\r\ndata: first\r\n\r\n"
                                             "data: second\n\n"
                                             "data: third\r\r";
        const std::vector<SseEvent> events = parseByteByByte(stream);

        ASSERT_EQ(events.size(), 3U) << "逐字节喂进去时事件条数不对";
        EXPECT_EQ(events[0].type, "alarm");
        EXPECT_EQ(events[0].data, "first");
        EXPECT_EQ(events[1].data, "second");
        EXPECT_EQ(events[2].data, "third");
        EXPECT_TRUE(events[1].type.empty()) << "没写 event: 的条目的类型应为空（默认的 message）";
    }

    /**
     * @brief 钉住：多条 data 以换行拼接，冒号后那一个空格属于分隔符，id 在流内沿用
     */
    TEST(SseEventParser, JoinsDataLinesStripsOneSpaceAndCarriesTheIdForward)
    {
        const std::vector<SseEvent> events = parseWhole("id: 7\ndata: line1\ndata: line2\n\n"
                                                        "data:after-no-space\n\n"
                                                        "data:  two spaces\n\n");
        ASSERT_EQ(events.size(), 3U);
        EXPECT_EQ(events[0].data, "line1\nline2") << "多条 data 是按行拼接的，末尾不多留换行";
        EXPECT_EQ(events[0].lastEventId, "7");
        EXPECT_EQ(events[1].data, "after-no-space") << "冒号后没有空格时，取值从下一字节开始";
        EXPECT_EQ(events[1].lastEventId, "7") << "id 是流内的状态：没重写的条目要带上一次的值";
        EXPECT_EQ(events[2].data, " two spaces") << "只去掉一个空格，第二个属于正文";
    }

    /**
     * @brief 钉住：注释行与没有正文的事件都不派发，retry 与 id 的畸形取值整条忽略
     */
    TEST(SseEventParser, IgnoresCommentsEmptyDispatchesAndMalformedFieldValues)
    {
        // 含 NUL 的那一条要真的带 NUL 交进去：字符串字面量隐式转成 std::string 会在 NUL 处截断，
        // 后面的部分整段消失，用例就在测一条根本没发生过的输入
        std::string stream = ": keep-alive heartbeat\n"
                             "event: ping\n\n"
                             "retry: soon\n";
        stream += "id: bad";
        stream += '\0';
        stream += "id\n";
        stream += "data: kept\n\n";

        const std::vector<SseEvent> events = parseWhole(stream);
        ASSERT_EQ(events.size(), 1U) << "注释、只写了 event: 的一行、以及畸形 retry 都不该产出事件";
        EXPECT_EQ(events[0].data, "kept");
        EXPECT_TRUE(events[0].type.empty()) << "上一行只写了 event: 却没派发，那个类型也必须一起清掉：漏到这一条就成了对端没给过的类型";
        EXPECT_FALSE(events[0].hasRetry) << "retry 的取值不是纯数字就整条忽略（不回退、也不报错）";
        EXPECT_TRUE(events[0].lastEventId.empty()) << "含 NUL 的 id 按规范整条忽略";
    }

    /**
     * @brief 钉住：合法的 retry 会被带出来，且对端没写完最后一条事件时收线能把它补交出来
     */
    TEST(SseEventParser, ReportsRetryAndFlushesTheTailOnStreamEnd)
    {
        SseEventParser parser;
        parser.feed("retry: 1500\ndata: tick\n");
        // 对端在这里收线：没有那个收尾的空行，攒着的事件仍要交出去，否则最后一条就随连接丢了
        parser.endOfStream();
        const std::optional<SseEvent> flushed = parser.nextEvent();
        ASSERT_TRUE(flushed.has_value()) << "收线时没把攒着的事件补交出来";
        EXPECT_EQ(flushed->data, "tick");
        EXPECT_TRUE(flushed->hasRetry);
        EXPECT_EQ(flushed->retryMilliseconds, 1500U);
        EXPECT_FALSE(parser.nextEvent().has_value()) << "派发完不该还有事件";
    }

    /**
     * @brief 钉住：配合出站接收口，一条真实 SSE 流能按事件顺序收到
     * @details 服务端用的是自家 SseStream，因此这条同时是「写侧与读侧对同一份格式的理解一致」的证据
     */
    TEST(SseEventParser, ReceivesRealEventStreamOverHttp1)
    {
        const RunningHttpServerFixture fixture{HttpServerLimits{}, std::chrono::milliseconds{100}, HttpTestSupport::SlowRouteOptions{}, [](Router &router, EventLoop &)
                                               {
                                                   router.get("/sse",
                                                              [](HttpRequest &, HttpResponse &response) -> Task<void>
                                                              {
                                                                  SseStream stream(response);
                                                                  static_cast<void>(co_await stream.sendComment("打开前的握手"));
                                                                  static_cast<void>(co_await stream.sendEvent("第一件", "tick", "1"));
                                                                  static_cast<void>(co_await stream.sendEvent("第二件\n还带换行", "tick", "2"));
                                                                  co_return;
                                                              });
                                               }};
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器没起来";

        EventLoop             loop;
        HttpClient            client(loop, HttpOutboundConnectionPool::Config{});
        const std::string     url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/sse";
        std::vector<SseEvent> receivedEvents;
        std::string           failureReason;
        Task<void>            task = collectStreamTask(loop, client, url, receivedEvents, failureReason);
        loop.scheduler().schedule(task.handle());
        loop.run();

        EXPECT_TRUE(failureReason.empty()) << failureReason;
        ASSERT_EQ(receivedEvents.size(), 2U) << "两条事件应当都按到达次序交回来";
        EXPECT_EQ(receivedEvents[0].type, "tick");
        EXPECT_EQ(receivedEvents[0].data, "第一件");
        EXPECT_EQ(receivedEvents[0].lastEventId, "1");
        EXPECT_EQ(receivedEvents[1].data, "第二件\n还带换行") << "多条 data 拼回来的形状与写侧一致才算对上";
        EXPECT_EQ(receivedEvents[1].lastEventId, "2");
    }

} // namespace AsynGyanis::Net
