// 出站响应的逐批交付（HttpClientRequest::responseBodyReceiver）：交在流上、早停、以及代加编码声明的让位
//
// 判据分三块：①解析器把已收正文交出去之后，累计上限照旧按「一共收了多少」判；②挂了接收口就按到达
// 批次交付（整份攒完再交一次的实现会在这里露出来），且本端不再代加 accept-encoding；③接收口返回 false
// 之后连接必须当场关掉——留着它还回池，下一条请求会把剩下的正文当自己的响应头读。

#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpResponseParser.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
    using AsynGyanis::Net::HttpResponseInfo;
    using AsynGyanis::Net::HttpResponseParser;
    using AsynGyanis::Net::Router;
    using AsynGyanis::Net::HttpTestSupport::RunningHttpServerFixture;
    using AsynGyanis::TestSupport::kWaitTimeout;

    constexpr std::chrono::milliseconds kRequestTimeout{8000};
    /// 比回环套接字缓冲大一个量级的正文：整份攒完再交只会有一批，按到达交则一定不止
    constexpr std::size_t kStreamedBodyByteCount = 256U * 1024U;

    /// 服务端这一路看到的「请求里有没有 accept-encoding」：代加声明该为流式交付让位
    std::atomic<bool> gSawAcceptEncodingHeader{false};

    /**
     * @brief 一次流式交付的观测结果
     */
    struct DeliveryObservation
    {
        std::vector<std::string> batches{};           ///< 按到达次序交回来的各批正文
        bool                     sawLastBatch{false}; ///< 是否收到过 isLastBatch 为真的那一次
        int                      headStatus{};        ///< 第一批上拿到的响应状态码
        std::string              returnedBody{};      ///< 交回来的响应正文（挂了接收口应当为空）
        std::string              failureReason{};     ///< 失败原因；空表示这一趟成了
    };

    /**
     * @brief 发一次带接收口的请求（具名协程：IIFE 的闭包对象在全表达式结束时就成了悬空 this）
     * @param client 带池的客户端
     * @param url 目标地址（按值收：本协程会带着它 co_await）
     * @param observation 观测结果
     * @param stopAfterBatchCount 收到这么多批就返回 false 主动收口；0 表示交完整个流
     */
    Task<void> streamOnceTask(HttpClient &client, std::string url, DeliveryObservation &observation, std::size_t stopAfterBatchCount)
    {
        HttpClientRequest request;
        request.responseBodyReceiver = [&observation, stopAfterBatchCount](const HttpResponseInfo &head, const std::string_view batch, const bool isLastBatch) -> Task<bool>
        {
            observation.headStatus = head.statusCode;
            observation.batches.emplace_back(batch);
            if (isLastBatch)
            {
                observation.sawLastBatch = true;
            }
            // 交完这一批就收口：剩下的正文不该再被读，这条连接也不能还池
            // （HTTP/1.1 没有「流」可结——h2 与 h3 上同一个返回只结那一条流，见另外两份通路用例）
            co_return stopAfterBatchCount == 0 || observation.batches.size() < stopAfterBatchCount;
        };
        try
        {
            const std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response = co_await client.send(url, request, kRequestTimeout);
            if (response == nullptr)
            {
                observation.failureReason = "请求失败（客户端交回空响应）";
            } else
            {
                observation.returnedBody = response->body;
            }
        } catch (const std::exception &failure)
        {
            observation.failureReason = failure.what();
        }
        co_return;
    }

    /**
     * @brief 不挂接收口的一次普通 GET（对照组：代加的 accept-encoding 声明照旧要上线）
     */
    Task<void> plainGetTask(EventLoop &loop, HttpClient &client, std::string url)
    {
        const std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response = co_await client.get(url);
        static_cast<void>(response);
        loop.stop();
        co_return;
    }

    /**
     * @brief 同一个客户端上连发两次：先半路收口一条大的，再完整收一条小的
     * @details 两条必须共用一个池——否则第一条留下的脏连接根本没有被复用的机会，
     *          「关掉」这条动作就测不到（分开两个客户端跑时，把 close 去掉用例照样绿，实测过）
     */
    Task<void> twoRequestsTask(EventLoop &loop, HttpClient &client, std::string bigUrl, std::string smallUrl, DeliveryObservation &stopped, DeliveryObservation &followUp)
    {
        co_await streamOnceTask(client, bigUrl, stopped, 1);
        co_await streamOnceTask(client, smallUrl, followUp, 0);
        loop.stop();
        co_return;
    }

    /**
     * @brief 发一次请求然后把循环停下（具名协程；跑完就停，不等调度时序）
     */
    Task<void> stopAfterRequestTask(EventLoop &loop, HttpClient &client, std::string url, DeliveryObservation &observation, std::size_t stopAfterBatchCount)
    {
        co_await streamOnceTask(client, url, observation, stopAfterBatchCount);
        loop.stop();
        co_return;
    }

    /**
     * @brief 在一条全新的循环上，用一个带池的客户端发一次带接收口的请求
     * @param url 目标地址
     * @param observation 观测结果
     * @param stopAfterBatchCount 收到这么多批就返回 false 主动收口；0 表示交完整个流
     */
    void runStreamingRequest(const std::string &url, DeliveryObservation &observation, const std::size_t stopAfterBatchCount)
    {
        EventLoop  loop;
        HttpClient client(loop, HttpOutboundConnectionPool::Config{});
        Task<void> task = stopAfterRequestTask(loop, client, url, observation, stopAfterBatchCount);
        loop.scheduler().schedule(task.handle());
        loop.run();
    }

    /**
     * @brief 装两条路由：一大块正文（用来分辨「逐批」与「整份」）与一条小的（用来验连接没被弄脏）
     */
    void registerStreamingRoutes(Router &router, EventLoop &)
    {
        router.get("/big",
                   [](HttpRequest &, HttpResponse &response) -> Task<void>
                   {
                       response.setStatus(200);
                       response.setBody(std::string(kStreamedBodyByteCount, 'a'));
                       co_return;
                   });
        router.get("/small",
                   [](HttpRequest &request, HttpResponse &response) -> Task<void>
                   {
                       gSawAcceptEncodingHeader.store(request.getHeader("accept-encoding").has_value());
                       response.setStatus(200);
                       response.setBody("tiny");
                       co_return;
                   });
    }
} // namespace

namespace AsynGyanis::Net
{
    /**
     * @brief 钉住：解析器把已收正文交出去之后，累计上限照旧按「一共收了多少」判
     * @details 逐段取走是流式交付的内存账所在。上限若看「还留在缓冲里多少」，一边取走一边收
     *          等于把闸门拆了——对端可以无限送正文
     */
    TEST(HttpClientStreamingResponse, DrainedBodyStillCountsAgainstTheTotalCap)
    {
        HttpResponseParser parser(16);
        static_cast<void>(parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"));
        ASSERT_TRUE(parser.isHeadComplete()) << "头部收齐了却没报出来：接收口拿不到头部";

        std::string taken;
        static_cast<void>(parser.feed("10\r\n0123456789abcdef\r\n"));
        ASSERT_EQ(parser.takeBodyBytes(taken), 16U) << "取走的字节数应与已收正文一致";
        EXPECT_EQ(taken, "0123456789abcdef");

        // 再来一块：累计已经到 16，这一块的任何一部分都把总量顶过上限
        static_cast<void>(parser.feed("4\r\nijk"));
        EXPECT_TRUE(parser.hasFailed()) << "取走之后就不判上限了：那等于让对端决定本进程收多少";
        EXPECT_TRUE(parser.isBodyOverLimit()) << "越界要说是「本端上限」，不是「报文不合规范」：解法在调用方手里";
    }

    /**
     * @brief 钉住：设了接收口就按到达批次交付，最后一批带 isLastBatch，交回的响应正文为空
     */
    TEST(HttpClientStreamingResponse, DeliversSeveralBatchesBeforeTheStreamEnds)
    {
        const RunningHttpServerFixture fixture{HttpServerLimits{}, std::chrono::milliseconds{100}, HttpTestSupport::SlowRouteOptions{}, registerStreamingRoutes};
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器没起来";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/big";

        DeliveryObservation observation;
        runStreamingRequest(url, observation, 0);

        EXPECT_TRUE(observation.failureReason.empty()) << observation.failureReason;
        EXPECT_EQ(observation.headStatus, 200) << "第一批上就该拿到响应头部";
        EXPECT_GE(observation.batches.size(), 2U) << "只有一批：接收口又整份攒完了，跟不设它没有区别";
        EXPECT_TRUE(observation.sawLastBatch) << "没有一批带 isLastBatch：调用方分不清「收完」与「断了」";
        EXPECT_TRUE(observation.returnedBody.empty()) << "挂了接收口之后，交回的响应不该再持有一份正文";

        std::string wholeBody;
        for (const std::string &batch: observation.batches)
        {
            wholeBody += batch;
        }
        EXPECT_EQ(wholeBody.size(), kStreamedBodyByteCount) << "各批拼起来必须正好是整条正文";
        EXPECT_TRUE(std::all_of(wholeBody.begin(), wholeBody.end(), [](const char byte) { return byte == 'a'; })) << "批次的边界或次序错了";
    }

    /**
     * @brief 钉住：挂了接收口就不代加 accept-encoding，交出去的是对端发的原样字节
     */
    TEST(HttpClientStreamingResponse, DoesNotAdvertiseAcceptEncodingForStreamedResponses)
    {
        gSawAcceptEncodingHeader.store(false);
        const RunningHttpServerFixture fixture{HttpServerLimits{}, std::chrono::milliseconds{100}, HttpTestSupport::SlowRouteOptions{}, registerStreamingRoutes};
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器没起来";
        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/small";

        DeliveryObservation observation;
        runStreamingRequest(url, observation, 0);
        EXPECT_TRUE(observation.failureReason.empty()) << observation.failureReason;
        EXPECT_FALSE(gSawAcceptEncodingHeader.load()) << "本端代加了编码声明：交回来的字节会是压过又没人解的，比不流式更糟";

        // 对照：不挂接收口时那条声明照旧要加（同一条客户端、同一条路由）
        gSawAcceptEncodingHeader.store(false);
        EventLoop  plainLoop;
        HttpClient plainClient(plainLoop, HttpOutboundConnectionPool::Config{});
        Task<void> plainTask = plainGetTask(plainLoop, plainClient, url);
        plainLoop.scheduler().schedule(plainTask.handle());
        plainLoop.run();
        EXPECT_TRUE(gSawAcceptEncodingHeader.load()) << "不挂接收口时那条声明必须照旧加：这是流式交付之外的既有行为";
    }

    /**
     * @brief 钉住：接收口返回 false 就在此收口，头部仍然交回，且这条连接不被复用
     * @details 剩下的正文留在通路里，连接必须当场关掉——留着它还回池，下一条请求会把剩下的字节
     *          当自己的响应头读，表现是第二条请求莫名失败
     */
    TEST(HttpClientStreamingResponse, EarlyStopKeepsTheHeadAndDoesNotPoisonTheConnection)
    {
        const RunningHttpServerFixture fixture{HttpServerLimits{}, std::chrono::milliseconds{100}, HttpTestSupport::SlowRouteOptions{}, registerStreamingRoutes};
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器没起来";
        const std::string baseUrl = "http://127.0.0.1:" + std::to_string(fixture.listeningPort());

        // 两条请求共用一个客户端与一个池：第一条脏了的连接若还池，第二条就会读到它剩下的正文
        EventLoop           loop;
        HttpClient          client(loop, HttpOutboundConnectionPool::Config{});
        DeliveryObservation stopped;
        DeliveryObservation followUp;
        Task<void>          task = twoRequestsTask(loop, client, baseUrl + "/big", baseUrl + "/small", stopped, followUp);
        loop.scheduler().schedule(task.handle());
        loop.run();

        EXPECT_TRUE(stopped.failureReason.empty()) << stopped.failureReason;
        EXPECT_EQ(stopped.batches.size(), 1U) << "返回 false 之后不该再交第二批";
        EXPECT_EQ(stopped.headStatus, 200) << "主动收口也要把头部交回来";
        EXPECT_FALSE(stopped.sawLastBatch) << "正文没走完，不该谎称那是最后一批";

        EXPECT_TRUE(followUp.failureReason.empty()) << "第二条请求被第一条的残留弄坏了：" << followUp.failureReason;
        std::string followUpBody;
        for (const std::string &batch: followUp.batches)
        {
            followUpBody += batch;
        }
        EXPECT_EQ(followUpBody, "tiny");
    }

} // namespace AsynGyanis::Net
