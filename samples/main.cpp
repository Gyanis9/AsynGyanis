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
}

int main(int argc, char **argv)
{
    std::string host     = "localhost";
    uint16_t    port     = 8080;
    unsigned    threads  = 0; // 0 = auto (optimized for local benchmarks)
    bool        useHttps = false;
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
        else if (arg == "--https")
            useHttps = true;
        else if (arg == "--cert" && i + 1 < argc)
            certificateFile = argv[++i];
        else if (arg == "--key" && i + 1 < argc)
            keyFile = argv[++i];
        else if (arg == "--help")
        {
            LOG_INFO("Usage: echo_server [--host localhost] [--port 8080] [--threads N]");
            LOG_INFO("                  [--https] [--cert cert.pem] [--key key.pem]");
            LOG_INFO("  --threads 0 = auto (min(4, hw_concurrency)), 1 = single-threaded");
            return 0;
        }
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

    if (useHttps)
    {
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            auto &loop   = pool.eventLoop(i);
            auto  server = std::make_unique<Net::HttpsServer>(loop, *address, certificateFile, keyFile);

            setupRoutes(server->router());

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
    context.stop();

    LOG_INFO("Server stopped successfully");
    return 0;
}
