/**
 * @file main.cpp
 * @brief 多线程 HTTP/HTTPS 服务器示例 — 每线程一个 EventLoop + HttpServer/HttpsServer（SO_REUSEPORT）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <vector>

// 示例程序以可读性为先：各模块统一挂在 AsynGyanis 之下，这里引入根命名空间，
// 正文继续写 Net::/Core::/Base:: 即可，不必逐处补全限定名
using namespace AsynGyanis;

namespace
{
    std::atomic g_running{true};

    void handleSignal(int)
    {
        g_running.store(false);
    }

    void setupRoutes(Net::Router &router)
    {
        router.get("/", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody("Hello World");
            co_return;
        });

        router.get("/json", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "application/json");
            response.setBody(R"({"status":"ok","version":"1.0.0","server":"AsynGyanis"})");
            co_return;
        });

        router.get("/bench", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody("OK");
            co_return;
        });
    }

    /// 优雅关闭的等待上限：给在途请求留出把响应发完的时间，超出后由 drain 内部强制收口
    constexpr std::chrono::milliseconds kShutdownDrainTimeout{5000};

    /**
     * @brief 在服务器所属的循环线程上执行 stop()
     * @details stop() 关闭的监听描述符正被该循环上的 accept 协程使用，只能在那个线程上调用；
     *          它本身是同步动作，包成一个立即完成的协程是为了便于按 scheduleRemote 投递。
     * @param server 目标服务器
     * @return Core::Task<> 协程，调用 stop() 后立即完成
     */
    Core::Task<> stopServerTask(Net::TcpServer &server)
    {
        server.stop();
        co_return;
    }

    /**
     * @brief 投递给单个服务器的优雅关闭协程
     * @details 跑完 drain() 后递减待完成计数，主线程据此判断所有服务器都已收手。
     * @param server 目标服务器
     * @param drainTimeout 交给 drain 的最长等待时长
     * @param remainingDrainCount 输入输出：尚未完成的 drain 条数，完成一条减一
     * @return Core::Task<> 协程，drain 返回后立即完成
     */
    Core::Task<> drainServerTask(Net::TcpServer &server, const std::chrono::milliseconds drainTimeout,
                                 std::atomic<std::size_t> &remainingDrainCount)
    {
        co_await server.drain(drainTimeout);
        remainingDrainCount.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
    }
}

int main(int argc, char **argv)
{
    std::string host     = "localhost";
    uint16_t    port     = 8080;
    unsigned    threads  = 0; // 0 = auto (optimized for local benchmarks)
    std::size_t maxConnectionsPerIp = 0; // 0 = 不限制单个来源的并发连接数
    bool        useHttps = false;
    bool        useHttp2Cleartext = false;
    bool        exposeMetrics = false;
    std::string certificateFile = "cert.pem";
    std::string keyFile  = "key.pem";

    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view arg = argv[i]; arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--threads" && i + 1 < argc)
            threads = static_cast<unsigned>(std::stoi(argv[++i]));
        else if (arg == "--max-connections-per-ip" && i + 1 < argc)
            maxConnectionsPerIp = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (arg == "--https")
            useHttps = true;
        else if (arg == "--h2c")
            useHttp2Cleartext = true;
        else if (arg == "--metrics")
            exposeMetrics = true;
        else if (arg == "--cert" && i + 1 < argc)
            certificateFile = argv[++i];
        else if (arg == "--key" && i + 1 < argc)
            keyFile = argv[++i];
        else if (arg == "--help")
        {
            LOG_INFO("Usage: echo_server [--host localhost] [--port 8080] [--threads N]");
            LOG_INFO("                  [--https] [--cert cert.pem] [--key key.pem] [--h2c]");
            LOG_INFO("                  [--max-connections-per-ip N] [--metrics]");
            LOG_INFO("  --threads 0 = auto (min(4, hw_concurrency)), 1 = single-threaded");
            LOG_INFO("  --h2c 明文连接按 HTTP/2（先验知识）服务，需客户端直接发连接前奏（仅 HTTP 端可用）");
            LOG_INFO("  --max-connections-per-ip 0 = 不限制单个来源的并发连接数（默认）");
            LOG_INFO("  --metrics 暴露 GET /metrics（Prometheus 文本）与 GET /healthz，仅 HTTP 端可用；");
            LOG_INFO("            本框架不做鉴权，公网可达时请自行加中间件或交给反向代理屏蔽");
            return 0;
        }
    }

    // h2c 说的是明文连接；TLS 上的 h2 由 ALPN 协商决定，不需要（也不该）用这个开关
    if (useHttps && useHttp2Cleartext)
    {
        LOG_ERROR("--h2c 只对明文端有意义：TLS 上的 h2 由 ALPN 协商，请去掉 --h2c");
        return 1;
    }

    if (threads == 0)
        threads = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    auto &rootLogger = Base::LoggerRegistry::instance().getRootLogger();
    rootLogger.addSink(std::make_unique<Base::ConsoleSink>());
    rootLogger.setLevel(Base::LogLevel::Debug);

    const char *proto = useHttps ? "https" : "http";
    LOG_INFO_FMT("echo_server starting — {}://{}:{} threads={}", proto, host, port, threads);

    // 多线程运行时
    Core::IoContext context(threads);

    auto address = Core::InetAddress::resolve(host, port);
    if (!address)
    {
        LOG_ERROR_FMT("Failed to resolve host: {}", host);
        return 1;
    }

    auto &   pool          = context.threadPool();
    auto actualThreads = static_cast<unsigned>(pool.threadCount());

    LOG_INFO_FMT("Actual worker threads: {} (logical cores: {})", actualThreads, std::thread::hardware_concurrency());

    // 每线程一个服务器实例（SO_REUSEPORT 内核级负载均衡）
    std::vector<std::unique_ptr<Net::TcpServer>> servers;
    std::vector<Core::Task<>>                    acceptTasks;
    servers.reserve(actualThreads);
    acceptTasks.reserve(actualThreads);

    // 按来源 IP 的限额只有一份、在所有监听器之间共享：内核按 SO_REUSEPORT 把新连接分给不同循环上的
    // 监听器，若每个服务器各持一份计数，单个来源的实际上限会乘上监听器数量，限额等于失效。
    // 未配置时留空指针，setPerIpConnectionLimiter(nullptr) 表示不作该限制
    std::shared_ptr<Net::PerIpConnectionLimiter> perIpConnectionLimiter;
    if (maxConnectionsPerIp > 0)
    {
        perIpConnectionLimiter = std::make_shared<Net::PerIpConnectionLimiter>(maxConnectionsPerIp);
        LOG_INFO_FMT("Per-IP connection limit: {} (shared by all {} listener(s))", maxConnectionsPerIp, actualThreads);
    }

    if (useHttps)
    {
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            auto &loop   = pool.eventLoop(i);
            auto  server = std::make_unique<Net::HttpsServer>(loop, *address, certificateFile, keyFile);

            setupRoutes(server->router());
            server->setPerIpConnectionLimiter(perIpConnectionLimiter);

            auto task = server->start();
            loop.scheduler().schedule(task.handle());
            acceptTasks.push_back(std::move(task));

            servers.push_back(std::move(server));
        }
    } else
    {
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            auto &loop   = pool.eventLoop(i);
            auto  server = std::make_unique<Net::HttpServer>(loop, *address);

            setupRoutes(server->router());
            server->setPerIpConnectionLimiter(perIpConnectionLimiter);

            // h2c：明文连接按先验知识直接说 HTTP/2（对端不发前奏就会被回 GOAWAY）。默认关闭
            if (useHttp2Cleartext)
            {
                server->setHttp2CleartextEnabled(true);
            }

            // 指标与健康检查端点是显式开关：不打开就完全没有暴露面
            if (exposeMetrics)
            {
                server->enableMetricsEndpoint();
                server->enableHealthEndpoint();
            }

            auto task = server->start();
            loop.scheduler().schedule(task.handle());
            acceptTasks.push_back(std::move(task));

            servers.push_back(std::move(server));
        }
    }

    LOG_INFO_FMT("{} {}Server instances created, all accept tasks scheduled", actualThreads, useHttps ? "Https" : "Http");

    pool.start();

    LOG_INFO("" + std::string(proto) + " server started  "+ proto + "://" + address->toString());
    LOG_INFO("Worker threads: " + std::to_string(actualThreads) + " (logical cores: " + std::to_string(std::thread::hardware_concurrency()) + ")");
    LOG_INFO("Endpoints: GET /  |  GET /json  |  GET /bench");
    LOG_INFO("Press Ctrl+C to exit");

    // 等待退出信号
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Received shutdown signal, stopping server...");

    // 关停分三步：停止接受新连接 → 等在途请求做完（超时兜底强关）→ 停运行时。
    // 前两步都要在服务器所属的循环线程上执行（它们要动那个循环正在使用的监听器与套接字），
    // 因此统一按 scheduleRemote 投递；任务对象必须留到跑完，由本向量持有到 main 结束
    std::vector<Core::Task<>> shutdownTasks;
    shutdownTasks.reserve(servers.size() * 2);

    for (std::size_t index = 0; index < servers.size(); ++index)
    {
        Core::Task<> stopTask = stopServerTask(*servers[index]);
        pool.eventLoop(index).scheduler().scheduleRemote(stopTask.handle());
        shutdownTasks.push_back(std::move(stopTask));
    }

    // drain 自己在各自的循环线程上排队推进，主线程只等这个计数归零
    std::atomic<std::size_t> remainingDrainCount{servers.size()};
    for (std::size_t index = 0; index < servers.size(); ++index)
    {
        Core::Task<> drainTask = drainServerTask(*servers[index], kShutdownDrainTimeout, remainingDrainCount);
        pool.eventLoop(index).scheduler().scheduleRemote(drainTask.handle());
        shutdownTasks.push_back(std::move(drainTask));
    }

    LOG_INFO_FMT("Draining {} server instance(s), in-flight requests get up to {}ms...", servers.size(), kShutdownDrainTimeout.count());
    while (remainingDrainCount.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    context.stop();

    LOG_INFO("Server stopped successfully");
    return 0;
}
