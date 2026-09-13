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
#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Exception/Exception.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerConfig.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
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
    bool        logJson = false; // 日志按 JSON Lines 输出，供采集端解析
    std::size_t maxInflightBodyBytes = 0; // 0 = 不限制在途正文字节总量
    bool        showUsage = false;
    std::string certificateFile = "cert.pem";
    std::string keyFile  = "key.pem";
    std::string configFile;

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
        else if (arg == "--log-json")
            logJson = true;
        else if (arg == "--max-inflight-body" && i + 1 < argc)
            maxInflightBodyBytes = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (arg == "--cert" && i + 1 < argc)
            certificateFile = argv[++i];
        else if (arg == "--key" && i + 1 < argc)
            keyFile = argv[++i];
        else if (arg == "--config" && i + 1 < argc)
            configFile = argv[++i];
        else if (arg == "--help")
        {
            // 只记下意图、就地不输出：用法说明要走日志器，而日志器取决于 --log-json，此刻还没装配
            showUsage = true;
            break;
        }
    }

    // 日志先于其余装配就绪：用法说明、参数与配置错误的原因都从这里出去。装配得太晚，
    // 这些早于就绪的输出会落进没有 sink 的根记录器，一条都留不下
    auto &rootLogger  = Base::LoggerRegistry::instance().getRootLogger();
    auto  consoleSink = std::make_unique<Base::ConsoleSink>();
    if (logJson)
    {
        // 格式化与落地是两件事：换掉格式化器就能让控制台吐 JSON Lines，Sink 本身不动
        consoleSink->setFormatter(std::make_unique<Base::JsonFormatter>());
    }
    rootLogger.addSink(std::move(consoleSink));
    rootLogger.setLevel(Base::LogLevel::Debug);

    if (showUsage)
    {
        LOG_INFO("Usage: echo_server [--host localhost] [--port 8080] [--threads N]");
        LOG_INFO("                  [--https] [--cert cert.pem] [--key key.pem] [--h2c]");
        LOG_INFO("                  [--max-connections-per-ip N] [--metrics] [--config <文件>]");
        LOG_INFO("  --threads 0 = auto (min(4, hw_concurrency)), 1 = single-threaded");
        LOG_INFO("  --h2c 明文连接按 HTTP/2（先验知识）服务，需客户端直接发连接前奏（仅 HTTP 端可用）");
        LOG_INFO("  --max-connections-per-ip 0 = 不限制单个来源的并发连接数（默认）");
        LOG_INFO("  --metrics 暴露 GET /metrics（Prometheus 文本）与 GET /healthz，仅 HTTP 端可用；");
        LOG_INFO("            本框架不做鉴权，公网可达时请自行加中间件或交给反向代理屏蔽");
        LOG_INFO("  --log-json 日志改成每行一个 JSON 对象（采集端按键取值，不必再写正则）");
        LOG_INFO("  --max-inflight-body 在途正文总量上限（字节，0 = 不限）：挡住多条连接同时压着大正文；");
        LOG_INFO("            超出的请求回 503，仅 HTTP 端可用（HTTPS 端暂未接入该预算）");
        LOG_INFO("  --config 从配置文件读 server 段（限额、按 IP 限额、限流、指标开关）；");
        LOG_INFO("            命令行上显式给出的开关优先于文件，详见 Net/Http/HttpServerConfig.h 的键名说明");
        return 0;
    }

    // 配置优先级：命令行 > 配置文件 > 内置默认值。文件是「这台服务的常态配置」，
    // 命令行是「这一次运行的临时改动」，临时改动优先
    Net::HttpServerConfiguration configuration;
    if (!configFile.empty())
    {
        // 整块都在 try 里：读文件、取段、校验任何一步失败都只让这次启动失败并说明原因。
        // 配置错误不该把进程直接 abort 掉——那连一条可读的原因都留不下
        try
        {
            if (!Base::ConfigManager::instance().loadFiles({configFile}).success)
            {
                LOG_ERROR_FMT("读取配置文件失败，服务未启动。路径：{}", configFile);
                return 1;
            }

            // ConfigManager 内部按键的点分路径扁平存放，get() 取不到任何中间层节点，
            // getSection() 才把 server 段还原成嵌套对象。读取器要的是「以 server 为根的文档」，
            // 这里补上段名这一层外壳：它只认文档结构，不关心配置来自文件还是内存
            Base::ConfigObject document;
            document.emplace(std::string(Net::kHttpServerConfigSection),
                             Base::ConfigManager::instance().getSection(Net::kHttpServerConfigSection));
            configuration = Net::readHttpServerConfiguration(Base::ConfigValue(std::move(document)));
        } catch (const std::exception &configurationException)
        {
            LOG_ERROR_FMT("配置读取失败，服务未启动。文件：{}，原因：{}", configFile, configurationException.what());
            return 1;
        }
        LOG_INFO_FMT("已读取配置 {}：最大连接 {}，单来源 {}，限流 {} 请求/s（桶 {}），指标 {}",
                     configFile, configuration.maximumConnections, configuration.maximumConnectionsPerIp,
                     configuration.requestsPerSecond, configuration.rateLimitBurstCapacity,
                     configuration.exposeMetrics ? "开" : "关");
    }

    // 命令行覆盖：显式给出的开关优先于文件里的同名项
    if (maxConnectionsPerIp > 0)
    {
        configuration.maximumConnectionsPerIp = maxConnectionsPerIp;
    }
    if (exposeMetrics)
    {
        configuration.exposeMetrics = true;
    }

    // --max-inflight-body 当前只接在明文 HTTP 端：HTTPS 侧的会话链（HttpsSession/Http2Session）
    // 还没有接这份预算，静默忽略等于给人一个「开了但其实没生效」的假象，因此直接拒绝这种组合
    if (useHttps && maxInflightBodyBytes > 0)
    {
        LOG_ERROR("--max-inflight-body 当前只对明文 HTTP 端生效，请去掉 --https 或改用限流/连接数限制");
        return 1;
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
    if (configuration.maximumConnectionsPerIp > 0)
    {
        perIpConnectionLimiter = std::make_shared<Net::PerIpConnectionLimiter>(configuration.maximumConnectionsPerIp);
        LOG_INFO_FMT("单来源并发上限 {}（所有 {} 个监听器共享同一份计数）", configuration.maximumConnectionsPerIp, actualThreads);
    }

    // 限流桶同样只有一份：它要的正是「进程级全局 RPS 上限」，各持一份等于上限乘以监听器数
    std::shared_ptr<Net::TokenBucket> rateLimitBucket;
    if (configuration.requestsPerSecond > 0.0)
    {
        rateLimitBucket = std::make_shared<Net::TokenBucket>(configuration.requestsPerSecond, configuration.rateLimitBurstCapacity);
        LOG_INFO_FMT("全局限流 {} 请求/s（桶容量 {}，所有监听器共享同一个桶）", configuration.requestsPerSecond,
                     configuration.rateLimitBurstCapacity);
    }

    // 在途正文预算同样只有一份：它要的是「整个进程的正文占用上限」，各监听器各持一份等于上限乘以监听器数
    std::shared_ptr<Net::HttpMemoryBudget> inflightBodyBudget;
    if (maxInflightBodyBytes > 0)
    {
        inflightBodyBudget = std::make_shared<Net::HttpMemoryBudget>(maxInflightBodyBytes);
        LOG_INFO_FMT("在途正文总量上限 {} 字节（所有 {} 个监听器共享同一份账）", maxInflightBodyBytes, actualThreads);
    }

    if (useHttps)
    {
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            auto &loop   = pool.eventLoop(i);
            auto  server = std::make_unique<Net::HttpsServer>(loop, *address, certificateFile, keyFile);

            setupRoutes(server->router());
            server->setPerIpConnectionLimiter(perIpConnectionLimiter);
            server->setMaxConnections(configuration.maximumConnections);
            server->setLimits(configuration.limits);
            server->setParserLimits(configuration.parserLimits);
            if (rateLimitBucket != nullptr)
            {
                server->router().addMiddleware(Net::tokenBucketRateLimiterMiddleware(rateLimitBucket));
            }

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
            server->setMaxConnections(configuration.maximumConnections);
            server->setLimits(configuration.limits);
            server->setParserLimits(configuration.parserLimits);
            if (rateLimitBucket != nullptr)
            {
                server->router().addMiddleware(Net::tokenBucketRateLimiterMiddleware(rateLimitBucket));
            }

            // 在途正文预算按整段「收正文 → 应答写完」记账，因此是服务器级配置而非连接级
            server->setMemoryBudget(inflightBodyBudget);

            // h2c：明文连接按先验知识直接说 HTTP/2（对端不发前奏就会被回 GOAWAY）。默认关闭
            if (useHttp2Cleartext)
            {
                server->setHttp2CleartextEnabled(true);
            }

            // 指标与健康检查端点是显式开关：不打开就完全没有暴露面
            if (configuration.exposeMetrics)
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
