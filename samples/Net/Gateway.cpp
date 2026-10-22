// 反向代理/网关示例：入站用 HttpServer，出站用 HttpClient，同一个请求在两者之间只搬状态、正文与
// 一条上游标识头。
//
// 这份示例刻意只放在 samples/：引擎里还没有反向代理这一层，先有真实消费方再决定收不收入库。
// 它同时是一份「缺什么」的实测清单——选择器只有最朴素的轮流，健康检查与加权轮转在末尾按
// gated 记，表示现有件能拼、但还没有可复用的入库形状。
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Server/HttpServer.h"
#include "Net/Http/Routing/Router.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /// 上游标识头：网关原样带回，示例用它判断这一条到底走了哪台上游
    constexpr std::string_view kUpstreamHeaderName = "x-upstream";

    /**
     * @brief 一次探针的观测结果
     * @note 只由探针协程写；主线程等到 done 的 release/acquire 之后才读
     */
    struct ProbeResults
    {
        bool                     ready{false}; ///< 三个监听器都在时限内答了 200
        std::vector<int>         statuses;     ///< 四条响应的状态码（拿不到响应记 0）
        std::vector<std::string> bodies;       ///< 四条响应的正文
        std::vector<std::string> upstreams;    ///< 四条响应的 x-upstream 头
    };

    /**
     * @brief 在时限内反复问一个端点，直到它答 200
     * @details 监听器是在服务器主协程被调度之后才开始 bind/listen 的，起跑顺序不该靠「睡一觉碰运气」；
     *          连接被拒时 HttpClient 立刻返回空，随后用循环的定时器等 50ms 再试
     * @param loop 承载等待的循环
     * @param port 目标端口
     * @return Core::Task<bool> 10 秒内答了 200 即 true
     */
    Core::Task<bool> waitForEndpoint(Core::EventLoop &loop, const std::uint16_t port)
    {
        const std::string url      = "http://127.0.0.1:" + std::to_string(port) + "/who";
        const auto        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::unique_ptr<Net::HttpClientResponse> response = co_await Net::HttpClient::get(loop, url, std::chrono::seconds{1});
            if (response != nullptr && response->statusCode == 200)
            {
                co_return true;
            }
            co_await loop.timerQueue().waitFor(std::chrono::milliseconds{50});
        }
        co_return false;
    }

    /**
     * @brief 等三个监听器就绪，再按顺序发四条 GET 经网关并记下响应
     * @param loop 承载本次等待的循环（不与网关共用，避免循环被一条挂起的等待卡住）
     * @param gatewayPort 网关端口
     * @param upstreamPorts 两台上游端口（用于就绪等待）
     * @param selector 网关的轮流选择器（就绪请求会推进它，量测前归零）
     * @param results 观测结果
     * @param done 完成标志：release 写，主线程 acquire 读
     * @return Core::Task<> 跑完即返回
     */
    Core::Task<> probeGateway(Core::EventLoop &loop, const std::uint16_t gatewayPort, const std::vector<std::uint16_t> &upstreamPorts, std::atomic<std::size_t> &selector,
                              ProbeResults &results, std::atomic<bool> &done)
    {
        results.ready = co_await waitForEndpoint(loop, upstreamPorts[0]) && co_await waitForEndpoint(loop, upstreamPorts[1]) && co_await waitForEndpoint(loop, gatewayPort);
        if (!results.ready)
        {
            done.store(true, std::memory_order_release);
            co_return;
        }
        // 就绪等待经网关问了一次 /who，选择器已经前进一格；量测从它自己的第 0 格开始，
        // 否则 A/B/A/B 的判据会被这次热身请求错开
        selector.store(0, std::memory_order_relaxed);

        const std::string url = "http://127.0.0.1:" + std::to_string(gatewayPort) + "/who";
        for (int index = 0; index < 4; ++index)
        {
            const std::unique_ptr<Net::HttpClientResponse> response = co_await Net::HttpClient::get(loop, url, std::chrono::seconds{5});
            if (response == nullptr)
            {
                results.statuses.push_back(0);
                results.bodies.emplace_back();
                results.upstreams.emplace_back();
                continue;
            }
            results.statuses.push_back(response->statusCode);
            results.bodies.push_back(response->body);
            const std::optional<std::string_view> upstream = response->headerValue(kUpstreamHeaderName);
            results.upstreams.emplace_back(upstream.has_value() ? std::string(*upstream) : std::string{});
        }
        done.store(true, std::memory_order_release);
        co_return;
    }

    /**
     * @brief 起一台只回答自己身份的上游
     * @param loop 所属循环
     * @param port 监听端口
     * @param identity 上游标识（写进正文与 x-upstream 头）
     * @return std::unique_ptr<Net::HttpServer> 已注册路由、尚未 start 的服务器
     */
    [[nodiscard]] std::unique_ptr<Net::HttpServer> makeUpstream(Core::EventLoop &loop, const std::uint16_t port, std::string identity)
    {
        auto server = std::make_unique<Net::HttpServer>(loop, *Core::InetAddress::resolve("127.0.0.1", port));
        server->router().get("*",
                             [identity = std::move(identity)](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
                             {
                                 response.setStatus(200);
                                 static_cast<void>(response.setHeader(kUpstreamHeaderName, identity));
                                 response.setBody(identity);
                                 co_return;
                             });
        return server;
    }
} // namespace

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== 网关示例开始 ===");

    const std::uint16_t basePort = Samples::readPortArgument(argc, argv, 0);
    Samples::requirePortHeadroom(basePort, 2);
    const std::uint16_t upstreamAPort = basePort;
    const std::uint16_t upstreamBPort = static_cast<std::uint16_t>(basePort + 1);
    const std::uint16_t gatewayPort   = static_cast<std::uint16_t>(basePort + 2);

    // 四条循环：两台上游、网关、探针客户端。探针不与网关共用循环——网关转发时会在自己的循环上
    // 等上游，探针再挤进同一条循环就看不清「是上游慢还是网关的循环没空」。
    Core::IoContext  context(4);
    Core::EventLoop &upstreamALoop = context.threadPool().eventLoop(0);
    Core::EventLoop &upstreamBLoop = context.threadPool().eventLoop(1);
    Core::EventLoop &gatewayLoop   = context.threadPool().eventLoop(2);
    Core::EventLoop &probeLoop     = context.threadPool().eventLoop(3);

    const std::unique_ptr<Net::HttpServer> upstreamA = makeUpstream(upstreamALoop, upstreamAPort, "A");
    const std::unique_ptr<Net::HttpServer> upstreamB = makeUpstream(upstreamBLoop, upstreamBPort, "B");

    auto                             gateway = std::make_unique<Net::HttpServer>(gatewayLoop, *Core::InetAddress::resolve("127.0.0.1", gatewayPort));
    const std::vector<std::uint16_t> upstreamPorts{upstreamAPort, upstreamBPort};
    std::atomic<std::size_t>         nextUpstream{0};
    gateway->router().get("*",
                          [&gatewayLoop, &upstreamPorts, &nextUpstream](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<>
                          {
                              const std::size_t                              index    = nextUpstream.fetch_add(1, std::memory_order_relaxed) % upstreamPorts.size();
                              const std::string                              target   = "http://127.0.0.1:" + std::to_string(upstreamPorts[index]) + std::string(request.path());
                              const std::unique_ptr<Net::HttpClientResponse> upstream = co_await Net::HttpClient::get(gatewayLoop, target, std::chrono::seconds{5});
                              if (upstream == nullptr)
                              {
                                  response.setStatus(502);
                                  response.setBody("上游不可达");
                                  co_return;
                              }
                              response.setStatus(upstream->statusCode);
                              if (const std::optional<std::string_view> upstreamHeader = upstream->headerValue(kUpstreamHeaderName); upstreamHeader.has_value())
                              {
                                  static_cast<void>(response.setHeader(kUpstreamHeaderName, *upstreamHeader));
                              }
                              response.setBody(upstream->body);
                              co_return;
                          });

    // 服务器主协程必须被调度到各自循环上，且帧要活到收尾：本向量一路持有到 main 结束
    std::vector<Core::Task<>> startTasks;
    startTasks.push_back(upstreamA->start());
    startTasks.push_back(upstreamB->start());
    startTasks.push_back(gateway->start());
    upstreamALoop.scheduler().schedule(startTasks[0].handle());
    upstreamBLoop.scheduler().schedule(startTasks[1].handle());
    gatewayLoop.scheduler().schedule(startTasks[2].handle());
    context.threadPool().start();

    ProbeResults      results;
    std::atomic<bool> done{false};
    Core::Task<>      probeTask = probeGateway(probeLoop, gatewayPort, upstreamPorts, nextUpstream, results, done);
    probeLoop.scheduler().scheduleRemote(probeTask.handle());

    const bool finished = Samples::waitUntil([&done] { return done.load(std::memory_order_acquire); }, std::chrono::seconds{30}, std::chrono::milliseconds{20});
    auto      &samples  = Samples::checklist();
    samples.check(finished && results.ready, "三个监听器在 10 秒内就绪，四条经网关转发的请求在 30 秒内全部回来");

    if (finished)
    {
        const bool allOk = results.statuses.size() == 4 && results.bodies.size() == 4 && results.upstreams.size() == 4;
        samples.check(allOk && results.statuses == std::vector<int>(4, 200), "四条响应都是 200：网关把上游状态原样交回");
        samples.check(allOk && results.bodies == std::vector<std::string>{"A", "B", "A", "B"} && results.upstreams == std::vector<std::string>{"A", "B", "A", "B"},
                      "轮流选择真的生效：正文与 x-upstream 按 A/B/A/B 交替，不是把所有请求都压在一台上游");
    }

    // 这两格是这份示例要暴露的缺口，不是失败：现有件能拼，但都还只是消费方自己的编排。
    samples.skip("上游健康检查", "HttpClient 能发探测请求，但周期任务、失败摘除与恢复需要一条池级状态；引擎里还没有可复用的 UpstreamPool");
    samples.skip("加权轮转", "现有选择器只有轮流；权重需要一份选择器状态，引擎里暂无可复用形状（等第二个消费方再入库）");

    context.stop();
    LOG_INFO("=== 网关示例结束 ===");
    return Samples::finishSample("NetGateway");
}
