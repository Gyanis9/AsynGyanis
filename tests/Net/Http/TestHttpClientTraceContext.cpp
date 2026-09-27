// 出站请求的链路上下文注入用例：traceparent 该由客户端写出、原样沿用、还是当场拒绝
//
// 对端是自家 HTTP 服务器，路由把收到的 traceparent 原样回显进正文——「客户端到底往线上写了什么」
// 于是直接从响应里读出来，不必给客户端开任何观测口。h1 与 h2c 两条通路各判一次，
// 因为注入点在所有通路的共同上游，走岔一边就算没接上。

#include "Net/Http/Client/HttpClient.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http/TraceContext.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::extractTraceContext;
    using AsynGyanis::Net::HttpClient;
    using AsynGyanis::Net::HttpClientRequest;
    using AsynGyanis::Net::HttpMethod;
    using AsynGyanis::Net::kTraceparentHeaderName;
    using AsynGyanis::Net::TraceIdentifiers;
    using AsynGyanis::Net::Traceparent;
    using AsynGyanis::Net::HttpTestSupport::RunningHttpServerFixture;

    constexpr std::chrono::milliseconds kRequestTimeout{5000};

    /**
     * @brief 静态 send() 的发车协程：把正文与失败原因分别落到调用方给的串里
     * @details 参数全部按值/按引用落在协程帧能管住的地方：闭包对象不参与（帧记的是地址）
     */
    Task<void> sendOnceTask(EventLoop &loop, std::string url, HttpClientRequest request, std::string &observedBody, std::string &observedReason)
    {
        try
        {
            const auto sent = co_await HttpClient::send(loop, url, std::move(request), kRequestTimeout);
            if (sent.has_value())
            {
                observedBody = sent->body;
            } else
            {
                observedReason = sent.error();
            }
        } catch (const std::exception &failure)
        {
            observedReason = failure.what();
        }
        loop.stop();
        co_return;
    }

    /// 在一条全新的循环上发一次请求，回正文；失败原因落到 reason
    std::string sendOnce(std::string_view url, const HttpClientRequest &request, std::string &reason)
    {
        EventLoop   loop;
        std::string body;
        Task<void>  task = sendOnceTask(loop, std::string{url}, request, body, reason);
        loop.scheduler().schedule(task.handle());
        loop.run();
        return body;
    }

    /// 回显收到的 traceparent（没有就回 absent）
    void registerEchoRoute(AsynGyanis::Net::Router &router)
    {
        router.get("/echo-traceparent",
                   [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<void>
                   {
                       response.setBody(std::string{request.firstHeaderValueView(kTraceparentHeaderName).value_or(std::string_view{"absent"})});
                       co_return;
                   });
    }

    /// 起一台只服务回显路由的服务器
    std::unique_ptr<RunningHttpServerFixture> startEchoServer()
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(AsynGyanis::Net::HttpServerLimits{}, std::chrono::milliseconds{1000},
                                                                  AsynGyanis::Net::HttpTestSupport::SlowRouteOptions{},
                                                                  [](AsynGyanis::Net::Router &router, EventLoop &) { registerEchoRoute(router); });
        EXPECT_TRUE(fixture->awaitRunning(std::chrono::seconds{5}));
        return fixture;
    }

    /**
     * @brief 用带连接池的实例客户端发一次请求
     * @details 实例那一路才走 h2/h3 的分叉，注入点在两条通路共同的上游：只测静态入口
     *          不足以证明「h2 也带得上」。
     */
    Task<void> sendWithClientTask(EventLoop &loop, HttpClient &client, std::string url, HttpClientRequest request, std::string &observedBody, std::string &observedReason)
    {
        try
        {
            const std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response = co_await client.send(url, request, kRequestTimeout);
            if (response != nullptr)
            {
                observedBody = response->body;
            } else
            {
                observedReason = "实例客户端没拿到响应";
            }
        } catch (const std::exception &failure)
        {
            observedReason = failure.what();
        }
        // 池里那条 h2 连接还留着空闲清扫的定时器：不叫停循环，run() 就永不返回
        loop.stop();
        co_return;
    }

    /// 指向回显路由的完整 URL
    std::string echoUrl(const std::uint16_t port)
    {
        return "http://127.0.0.1:" + std::to_string(port) + "/echo-traceparent";
    }
} // namespace

/**
 * @brief 填了 traceContext 就由客户端写出 traceparent，内容正是给出去的那份上下文
 */
TEST(HttpClientTraceContext, WritesTheHeaderBuiltFromTheGivenContext)
{
    const auto fixture = startEchoServer();
    ASSERT_TRUE(fixture != nullptr);

    HttpClientRequest request;
    request.method       = "GET";
    request.traceContext = Traceparent::generate(true);

    std::string       reason;
    const std::string echoed = sendOnce(echoUrl(fixture->listeningPort()), request, reason);
    EXPECT_TRUE(reason.empty()) << reason;
    EXPECT_EQ(echoed, Traceparent::value(*request.traceContext));

    // 头部里的段标识就是本端这一跳：下一跳据此把自己挂到本节之下
    AsynGyanis::Net::HttpRequest carrier;
    carrier.setUri("/");
    ASSERT_TRUE(carrier.setHeader(kTraceparentHeaderName, echoed));
    const std::optional<TraceIdentifiers> parsed = extractTraceContext(carrier);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->traceIdText(), request.traceContext->traceIdText());
    EXPECT_EQ(parsed->parentIdText(), request.traceContext->parentIdText());
    EXPECT_TRUE(parsed->isSampled());
}

/**
 * @brief 没填 traceContext 时行为与从前一致：手写的头部原样发出，不填就不发
 */
TEST(HttpClientTraceContext, PassesAHandWrittenHeaderThroughUnchanged)
{
    const auto fixture = startEchoServer();
    ASSERT_TRUE(fixture != nullptr);

    const std::string handWritten = Traceparent::value(Traceparent::generate(false));

    HttpClientRequest request;
    request.method  = "GET";
    request.headers = {{std::string(kTraceparentHeaderName), handWritten}};
    std::string reason;
    EXPECT_EQ(sendOnce(echoUrl(fixture->listeningPort()), request, reason), handWritten) << reason;

    // 两处都不给：头部不该凭空出现
    HttpClientRequest bare;
    bare.method = "GET";
    reason.clear();
    EXPECT_EQ(sendOnce(echoUrl(fixture->listeningPort()), bare, reason), "absent") << reason;
}

/**
 * @brief traceContext 与手写的 traceparent 同时给出属于用法错误：当场拒，不替调用方挑一个
 */
TEST(HttpClientTraceContext, RejectsTwoSourcesOfTheUpstreamContext)
{
    const auto fixture = startEchoServer();
    ASSERT_TRUE(fixture != nullptr);

    HttpClientRequest request;
    request.method       = "GET";
    request.traceContext = Traceparent::generate(true);
    request.headers      = {{std::string(kTraceparentHeaderName), Traceparent::value(Traceparent::generate(false))}};

    std::string       reason;
    const std::string echoed = sendOnce(echoUrl(fixture->listeningPort()), request, reason);
    EXPECT_TRUE(echoed.empty()) << "两处上级都给了就不该发出请求";
    EXPECT_NE(reason.find("一个上级上下文"), std::string::npos) << reason;
}

/**
 * @brief 带池的实例通路也写出同一份上下文：注入点在静态入口之外还有一处，两处都要钉
 * @details 三条承载（h1/h2/h3）读的正是这一份 headers，所以判「实例通路有没有接上」就够了；
 *          顺带确认这条连接真的进了池（否则它走的就不是被改的那一条路）。
 */
TEST(HttpClientTraceContext, InstancePathWritesTheHeaderToo)
{
    const auto fixture = startEchoServer();
    ASSERT_TRUE(fixture != nullptr);

    HttpClientRequest request;
    request.method       = "GET";
    request.traceContext = Traceparent::generate(true);

    EventLoop   clientLoop;
    HttpClient  client{clientLoop};
    std::string body;
    std::string reason;
    Task<void>  task = sendWithClientTask(clientLoop, client, echoUrl(fixture->listeningPort()), request, body, reason);
    clientLoop.scheduler().schedule(task.handle());
    clientLoop.run();

    EXPECT_TRUE(reason.empty()) << reason;
    EXPECT_EQ(body, Traceparent::value(*request.traceContext));
    EXPECT_EQ(client.idleConnectionCount(), 1U) << "这条请求没走带池的通路，判据就落不到注入点上";
}
