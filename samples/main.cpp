/**
 * @file main.cpp
 * @brief 多线程 HTTP 服务器示例 — 每线程一个 EventLoop + HttpServer（SO_REUSEPORT）
 * @copyright Copyright (c) 2026
 */
#include "Base/LogCommon.h"
#include "Base/Logger.h"
#include "Base/LogSink.h"
#include "Core/EventLoop.h"
#include "Core/IoContext.h"
#include "Core/InetAddress.h"
#include "Core/Scheduler.h"
#include "Core/Task.h"
#include "Core/ThreadPool.h"
#include "Net/HttpResponse.h"
#include "Net/HttpServer.h"
#include "Net/Router.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    std::atomic<bool> g_running{true};

    void handleSignal(int)
    {
        g_running.store(false);
    }

    void setupRoutes(Net::Router &router)
    {
        router.get("/", [](Net::HttpRequest &, Net::HttpResponse &res) -> Core::Task<void>
        {
            res.setStatus(200);
            res.setHeader("Content-Type", "text/plain");
            res.setBody("Hello World");
            co_return;
        });

        router.get("/json", [](Net::HttpRequest &, Net::HttpResponse &res) -> Core::Task<void>
        {
            res.setStatus(200);
            res.setHeader("Content-Type", "application/json");
            res.setBody(R"({"status":"ok","version":"1.0.0","server":"AsynGyanis"})");
            co_return;
        });

        router.get("/bench", [](Net::HttpRequest &, Net::HttpResponse &res) -> Core::Task<void>
        {
            res.setStatus(200);
            res.setHeader("Content-Type", "text/plain");
            res.setBody("OK");
            co_return;
        });
    }
} // namespace

int main(int argc, char **argv)
{
    std::string host    = "localhost";
    uint16_t    port    = 8080;
    unsigned    threads = 0; // 0 = hardware_concurrency

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--threads" && i + 1 < argc)
            threads = static_cast<unsigned>(std::stoi(argv[++i]));
        else if (arg == "--help")
        {
            std::cout << "Usage: echo_server [--host localhost] [--port 8080] [--threads 0]\n";
            return 0;
        }
    }

    if (threads == 0)
        threads = std::thread::hardware_concurrency();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    // ── 初始化日志 ──
    auto &rootLogger = Base::LoggerRegistry::instance().getRootLogger();
    rootLogger.addSink(std::make_unique<Base::ConsoleSink>());
    rootLogger.setLevel(Base::LogLevel::INFO);

    LOG_INFO_FMT("echo_server starting — host={} port={} threads={}", host, port, threads);

    // ── 多线程运行时 ──
    Core::IoContext ctx(threads);

    auto addr = Core::InetAddress::resolve(host, port);
    if (!addr)
    {
        LOG_ERROR_FMT("Failed to resolve host: {}", host);
        return 1;
    }

    auto &   pool          = ctx.threadPool();
    unsigned actualThreads = static_cast<unsigned>(pool.threadCount());

    LOG_INFO_FMT("Actual worker threads: {} (logical cores: {})", actualThreads,
                 std::thread::hardware_concurrency());

    // ── 每线程一个 HttpServer（SO_REUSEPORT 内核级负载均衡） ──
    std::vector<std::unique_ptr<Net::HttpServer>> servers;
    std::vector<Core::Task<>>                     acceptTasks;
    servers.reserve(actualThreads);
    acceptTasks.reserve(actualThreads);

    for (unsigned i = 0; i < actualThreads; ++i)
    {
        auto &loop   = pool.eventLoop(i);
        auto  server = std::make_unique<Net::HttpServer>(loop, *addr);

        setupRoutes(server->router());

        auto task = server->start();
        loop.scheduler().schedule(task.handle());
        acceptTasks.push_back(std::move(task));

        servers.push_back(std::move(server));
    }

    LOG_INFO_FMT("{} HttpServer instances created, all accept tasks scheduled", actualThreads);

    pool.start();

    std::cout << "[info] HTTP 服务器已启动  http://" << addr->toString() << "\n";
    std::cout << "[info] 工作线程数: " << actualThreads
            << "（逻辑核数: " << std::thread::hardware_concurrency() << "）\n";
    std::cout << "[info] 端点: GET /  |  GET /json  |  GET /bench\n";
    std::cout << "[info] 按 Ctrl+C 退出\n\n";

    // ── 等待退出信号 ──
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Received shutdown signal, stopping server…");
    ctx.stop();

    LOG_INFO("Server stopped successfully");
    return 0;
}
