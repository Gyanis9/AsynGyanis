// 多线程 HTTP/HTTPS 服务器示例：每线程一个 EventLoop + HttpServer/HttpsServer（SO_REUSEPORT）
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Exception/Exception.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerConfig.h"
#include "Net/Quic/QuicServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Platform/System/CpuAffinity.h"
#include "Platform/System/ProcessInfo.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// 示例程序以可读性为先：各模块统一挂在 AsynGyanis 之下，这里引入根命名空间，
// 正文继续写 Net::/Core::/Base:: 即可，不必逐处补全限定名
using namespace AsynGyanis;

namespace
{
    std::atomic g_running{true};

    /// /big 的正文大小：与微基准 `*-response-compress-256k` 同档，两侧读数可互相印证
    constexpr std::size_t kLargeBodyBytes = 256 * 1024;

    void handleSignal(int)
    {
        g_running.store(false);
    }

    /**
     * @brief 构造一条固定内容的可压缩大正文，全进程只造一次
     * @details 词序由线性同余发生器驱动：按固定周期重复的词表会被压缩器当成一次超长匹配，
     *          256 KiB 能压到 1 KiB 量级，那样量出来的「每字节压缩成本」比真实正文低一个数量级。
     *          种子与倍频常数固定，因此每次运行拿到的是同一份字节，跨运行、跨探针可比。
     * @return const std::string & 正文本体（构造后不再改动，可被各工作线程并发只读）
     */
    [[nodiscard]] const std::string &largeCompressibleBody()
    {
        static const std::string body = []
        {
            static constexpr std::string_view vocabulary[] = {
                "server", "engine", "request", "connection", "scheduler", "socket", "buffer", "response",
                "timeout", "header", "payload", "cipher", "packet", "stream", "window", "priority",
            };
            // 数值是 Numerical Recipes 的 LCG 常数；只需伪随机，不需高质量随机源
            std::uint32_t randomState = 0x2545F491u;
            std::string text;
            text.reserve(kLargeBodyBytes + 1);
            while (text.size() < kLargeBodyBytes)
            {
                randomState = randomState * 1664525u + 1013904223u;
                text += vocabulary[(randomState >> 16) % std::size(vocabulary)];
                // 每 16 个词换行：留出与真实文本一样的行结构，压缩器的匹配长度才有东西可吃
                text += (text.size() % 16 == 0) ? '\n' : ' ';
            }
            return text;
        }();
        return body;
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
            // pid 一并给出：多进程模式下它同时是「这条请求落到哪个 worker」的答案，
            // 部署排查与压测都靠它对上号（不必再去翻进程表）
            response.setBody(R"({"status":"ok","version":"1.0.0","server":"AsynGyanis","pid":)" +
                             std::to_string(Platform::ProcessInfo::currentProcessId()) + "}");
            co_return;
        });

        router.get("/bench", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody("OK");
            co_return;
        });

        // 响应压缩链路的端到端落点：/bench 的 2 字节正文永远到不了压缩阈值，所以「压完还能不能
        // 正确走完整条网络路径」（h1/h2 的序列化、content-length、vary）此前只有单测覆盖，
        // 这条路由给了进程外探针一个真 socket 的可比对象：同一地址带与不带 Accept-Encoding 各要一次
        router.get("/big", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody(largeCompressibleBody());
            co_return;
        });

        // 流式响应（SSE）验收用：分两段写出，客户端逐段收到就说明分块路径真的通
        router.get("/sse", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.startChunkedResponse(200);
            response.setHeader("Content-Type", "text/event-stream");
            if (!co_await response.writeChunk("data: one\n\n"))
            {
                co_return;
            }
            static_cast<void>(co_await response.writeChunk("data: two\n\n"));
            co_return;
        });

        // WebSocket 验收用：h1 的 Upgrade 与 h3 的扩展 CONNECT（RFC 9220）都走这条路由，
        // 收到一条就原样回一条——回显本身就把「帧进得来、也出得去」两件事一起验了
        router.get("/ws", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.upgradeToWebSocket(
                    [](Net::WebSocketPeer &peer) -> Core::Task<>
                    {
                        while (const auto message = co_await peer.receive())
                        {
                            if (!co_await peer.sendText(message->payload))
                            {
                                co_return;
                            }
                        }
                        co_return;
                    });
            co_return;
        });
    }

    /// 优雅关闭的等待上限：给在途请求留出把响应发完的时间，超出后由 drain 内部强制收口
    constexpr std::chrono::milliseconds kShutdownDrainTimeout{5000};

    /// 启动确认的等待上限：绑定与监听都在协程的第一步做完，正常只需毫秒级；给足余量但不许无界等待
    constexpr std::chrono::milliseconds kStartupConfirmTimeout{2000};

    /**
     * @brief 等到所有监听器进入监听态，或时限到点
     * @details start() 与 listen() 都是分离投递的常驻协程：绑定失败会在协程里抛出，服务器就停在
     *          「没在监听」的状态。日志上的「已启动」写在那之前，不核一次的话操作者看到的是
     *          一个活着、什么也不听、退出码还是 0 的进程。
     * @param listeningServers 真正承担监听的 TCP 服务器（接受分发模式下只有接受器那台）
     * @param http3Server HTTP/3 服务端；未启用时为空指针
     * @param timeout 等待上限
     * @return std::size_t 时限到点仍未进入监听态的监听器条数
     */
    std::size_t waitForListenersToComeUp(const std::vector<Net::TcpServer *> &listeningServers, const Net::QuicServer *http3Server,
                                         const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::size_t pendingCount = 0;
            for (const Net::TcpServer *server : listeningServers)
            {
                pendingCount += server->isRunning() ? 0U : 1U;
            }
            // h3 的端口是绑定成功才写下的非 0 值，读它就等于问「UDP 起来了吗」
            if (http3Server != nullptr && http3Server->listeningPort() == 0)
            {
                ++pendingCount;
            }

            if (pendingCount == 0 || std::chrono::steady_clock::now() >= deadline)
            {
                return pendingCount;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

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

    /**
     * @brief 投递给 HTTP/3 服务端的优雅收口协程
     * @details QuicServer 归它的循环所有，drain() 只能在那个线程上跑（它会遍历连接表）；
     *          计数与 TCP 侧同一用法，主线程据此判断它是否已经收手。
     * @param server 目标服务端
     * @param drainTimeout 交给 drain 的最长等待时长
     * @param remainingDrainCount 输入输出：尚未完成的 drain 条数，完成一条减一
     * @return Core::Task<> 协程，drain 返回后立即完成
     */
    Core::Task<> drainHttp3ServerTask(Net::QuicServer &server, const std::chrono::milliseconds drainTimeout,
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
    bool        dispatchAccept = false; // 一个监听器 + N 个工作循环（不依赖 SO_REUSEPORT）
    bool        pinThreadsToCores = false; // 启动时把每个工作循环线程绑到一枚逻辑核上
    bool        compressResponses = false; // 按 Accept-Encoding 协商压缩响应（zstd/br/gzip）
    bool        compressInLoop    = false; // --compress-sync：压缩留在循环线程上做完，只作对照用
    std::size_t maxInflightBodyBytes = 0; // 0 = 不限制在途正文字节总量
    std::size_t workerProcessCount = 1;   // 1 = 单进程；大于 1 时由 master 起这么多 worker 进程
    bool        isWorkerProcess = false; // 由 master 起的 worker 进程（内部开关，用户不必手写）
    bool        useHttp3 = false; // 额外在同一个端口号的 UDP 上提供 HTTP/3（QUIC，需要证书）
    bool        showUsage = false;
    std::string certificateFile = "cert.pem";
    std::string keyFile  = "key.pem";
    std::string configFile;

    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view arg = argv[i]; arg == "--host")
        {
            host = Samples::readOptionValue(argc, argv, i, "--host", "一个监听地址");
            ++i;
        }
        // 数值选项一律走同一个严格解析器：原先逐条 std::stoi/std::stoull 有三副面孔——非数字直接抛出
        // （整个进程无一句解释地终止）、超范围经强转绕回（--port 99999 静默听在 34463）、负数绕回极大值
        // （--threads -4 变成 42 亿条线程），而选项排在末尾却没有值时又被当成「没给这个选项」跳过
        else if (arg == "--port")
        {
            port = static_cast<uint16_t>(Samples::readNumericOption(argc, argv, i, "--port", 1U, 65535U));
            ++i;
        }
        else if (arg == "--threads")
        {
            // 上限 4096：线程数按可用核数量级取，超出这个数只会把机器起爆，出现即视为笔误
            threads = static_cast<unsigned>(Samples::readNumericOption(argc, argv, i, "--threads", 0U, 4096U));
            ++i;
        }
        else if (arg == "--max-connections-per-ip")
        {
            maxConnectionsPerIp = static_cast<std::size_t>(
                    Samples::readNumericOption(argc, argv, i, "--max-connections-per-ip", 0U, std::numeric_limits<std::uint64_t>::max()));
            ++i;
        }
        else if (arg == "--https")
            useHttps = true;
        else if (arg == "--h2c")
            useHttp2Cleartext = true;
        else if (arg == "--h3")
            useHttp3 = true;
        else if (arg == "--metrics")
            exposeMetrics = true;
        else if (arg == "--log-json")
            logJson = true;
        else if (arg == "--dispatch-accept")
            dispatchAccept = true;
        else if (arg == "--pin-threads")
            pinThreadsToCores = true;
        else if (arg == "--compress")
            compressResponses = true;
        else if (arg == "--compress-sync")
        {
            // 一并打开压缩，只是把执行位置换回循环线程：与默认的外置路径对照才看得出差多少
            compressResponses = true;
            compressInLoop    = true;
        }
        else if (arg == "--max-inflight-body")
        {
            maxInflightBodyBytes = static_cast<std::size_t>(
                    Samples::readNumericOption(argc, argv, i, "--max-inflight-body", 0U, std::numeric_limits<std::uint64_t>::max()));
            ++i;
        }
        else if (arg == "--cert")
        {
            certificateFile = Samples::readOptionValue(argc, argv, i, "--cert", "一个证书文件路径");
            ++i;
        }
        else if (arg == "--key")
        {
            keyFile = Samples::readOptionValue(argc, argv, i, "--key", "一个私钥文件路径");
            ++i;
        }
        else if (arg == "--config")
        {
            configFile = Samples::readOptionValue(argc, argv, i, "--config", "一个配置文件路径");
            ++i;
        }
        else if (arg == "--workers")
        {
            // 上限与 --threads 同一条理由：这么多进程只会把机器起爆，出现即视为笔误
            workerProcessCount = static_cast<std::size_t>(Samples::readNumericOption(argc, argv, i, "--workers", 0U, 4096U));
            ++i;
        }
        else if (arg == "--worker")
            isWorkerProcess = true;
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
    // 默认 Info：Debug 下每请求一行日志会把吞吐压出可见的损失，需要细看时再自己调低
    rootLogger.setLevel(Base::LogLevel::Info);

    if (showUsage)
    {
        LOG_INFO("Usage: echo_server [--host localhost] [--port 8080] [--threads N]");
        LOG_INFO("                  [--https] [--cert cert.pem] [--key key.pem] [--h2c] [--h3]");
        LOG_INFO("                  [--max-connections-per-ip N] [--metrics] [--config <文件>]");
        LOG_INFO("  --threads 0 = auto (min(4, hw_concurrency)), 1 = single-threaded");
        LOG_INFO("  --h2c 明文连接按 HTTP/2（先验知识）服务，需客户端直接发连接前奏（仅 HTTP 端可用）");
        LOG_INFO("  --h3 额外在同一个端口号的 UDP 上提供 HTTP/3：走同一套路由与处理器，需要证书（QUIC 自带 TLS）");
        LOG_INFO("  --max-connections-per-ip 0 = 不限制单个来源的并发连接数（默认）");
        LOG_INFO("  --metrics 暴露 GET /metrics（Prometheus 文本）与 GET /healthz；开了 --h3 时 h3 的请求数/状态码类一并计入");
        LOG_INFO("            本框架不做鉴权，公网可达时请自行加中间件或交给反向代理屏蔽");
        LOG_INFO("  --log-json 日志改成每行一个 JSON 对象（采集端按键取值，不必再写正则）");
        LOG_INFO("  --pin-threads 启动时把每条工作循环线程绑到一枚逻辑核上（按线程池下标顺序占核，");
        LOG_INFO("            线程数多于可用核数时多出来的线程保持可迁移；容器里按 cpuset 放行的核算）");
        LOG_INFO("  --dispatch-accept 一个监听器 + N 个工作循环：连接由接受循环轮转交给工作循环服务；");
        LOG_INFO("            不依赖 SO_REUSEPORT，因此 Windows 上开多线程也要用它（否则每个线程各绑一次同端口，");
        LOG_INFO("            内核不会分摊，全部连接都压在其中一条监听器上）");
        LOG_INFO("  --compress 按 Accept-Encoding 协商压缩响应正文（zstd/br/gzip 按偏好选择，默认 1 KiB 起压，静态文件也适用）；");
        LOG_INFO("            压缩交给工作线程做，完成后回到本连接的循环线程续上，循环不会为一次压缩停摆");
        LOG_INFO("  --compress-sync 同样开压缩，但留在事件循环线程上同步做完（只作对照：实测一条 256 KiB");
        LOG_INFO("            正文的 gzip 会把同循环小请求的 p50 从 37us 顶到 5.9ms）");
        LOG_INFO("  --max-inflight-body 在途正文总量上限（字节，0 = 不限）：挡住多条连接同时压着大正文；");
        LOG_INFO("            超出的请求回 503，明文、HTTPS 与 h3 三端共用同一份账");
        LOG_INFO("  --workers N 用 N 个 worker 进程服务同一个端口（默认 1 = 单进程）：");
        LOG_INFO("            master 只做编排不服务，各 worker 靠 SO_REUSEPORT 分别监听同一端口，");
        LOG_INFO("            SIGTERM/SIGINT 会让 worker 各自体面退出；");
        LOG_INFO("            注意进程间不共享状态：单来源限额、限流上限与指标计数都是每进程一份");
        LOG_INFO("  --worker 内部开关：由 master 传给 worker，用户不必手写");
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
            LOG_ERROR_EXCEPTION(configurationException, "配置读取失败，服务未启动。文件：{}，原因：{}", configFile, configurationException.what());
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

    // 多进程：master 只做编排，自己不服务——既当 master 又当 worker 会让「谁在服务」含糊，
    // 也会让「worker 崩了补一个」这条路径多一种要处理的形态。参数原样转给 worker，
    // 只多一个 --worker（worker 据此跳过这一段，直接去跑服务器）
    if (workerProcessCount > 1 && !isWorkerProcess)
    {
        try
        {
            Core::WorkerSupervisor::Configuration supervisorConfiguration;
            supervisorConfiguration.executablePath = argv[0];
            supervisorConfiguration.workerArguments.assign(argv + 1, argv + argc);
            supervisorConfiguration.workerArguments.emplace_back("--worker");
            supervisorConfiguration.workerCount = workerProcessCount;

            Core::WorkerSupervisor supervisor(std::move(supervisorConfiguration));
            LOG_INFO_FMT("多进程模式：{} 个 worker（master 进程号 {} 只做编排；Ctrl+C 或 SIGTERM 会让 worker 各自体面退出）",
                         workerProcessCount, Platform::ProcessInfo::currentProcessId());
            if (!supervisor.run())
            {
                // 整池 worker 都「起来就崩」：原因上一条条记在日志里，这里只把结果落到退出码上，
                // 否则一次彻底失败在进程管理器与脚本眼里跟一次正常停机长得很一样
                LOG_ERROR("多进程模式：全部 worker 都因「起来就崩」被放弃，服务未运行（原因见上面的错误日志）");
                return 1;
            }
        } catch (const Base::Exception &supervisorException)
        {
            LOG_ERROR_EXCEPTION(supervisorException, "多进程模式无法启动，服务未运行。原因：{}", supervisorException.what());
            return 1;
        }
        return 0;
    }

    // h2c 说的是明文连接；TLS 上的 h2 由 ALPN 协商决定，不需要（也不该）用这个开关
    if (useHttps && useHttp2Cleartext)
    {
        LOG_ERROR("--h2c 只对明文端有意义：TLS 上的 h2 由 ALPN 协商，请去掉 --h2c");
        return 1;
    }

    // QUIC 自带 TLS：没有证书就起不了 h3，与其起一个永远握不上手的监听器，不如在启动期直接说清
    if (useHttp3 && !useHttps)
    {
        LOG_ERROR("--h3 需要证书：QUIC 自带 TLS，请与 --https 一起用（--cert/--key）");
        return 1;
    }

    if (threads == 0)
        // 自动档按「本进程实际可用的核数」收口（容器 CPU 配额与许可核集合都算），再压到示例的 4 条
        // 上限：与 IoContext/ThreadPool 的自动档同一口径，否则受限环境下这里会起满宿主核数条循环
        threads = std::max(1u, std::min(4u, static_cast<unsigned>(Platform::CpuAffinity::recommendedWorkerCount())));

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const char *proto = useHttps ? "https" : "http";
    LOG_INFO_FMT("echo_server starting — {}://{}:{} threads={} pid={}{}", proto, host, port, threads,
                 Platform::ProcessInfo::currentProcessId(), isWorkerProcess ? " (worker)" : "");

    // 多线程运行时
    // 压缩用的工作线程池要在 context 之前声明：析构按声明的逆序，因此它会比那些循环活得久，
    // 在途压缩完成时总还能把恢复投回目标循环。线程数与循环条数同量级就够——N 条循环最多
    // 同时产生 N 次压缩，多出来的线程只会排队
    std::optional<Core::AsyncExecutor> compressionExecutor;
    if (compressResponses && !compressInLoop)
    {
        compressionExecutor.emplace(threads);
    }

    Core::IoContext context(threads);

    auto address = Core::InetAddress::resolve(host, port);
    if (!address)
    {
        LOG_ERROR_FMT("Failed to resolve host: {}", host);
        return 1;
    }

    auto &   pool          = context.threadPool();
    auto actualThreads = static_cast<unsigned>(pool.threadCount());

    // 绑核是部署方的选择：开了之后每条循环线程固定占一枚逻辑核，减少调度迁移与缓存被踩。
    // 线程数多于可用核数时只有前若干条被绑（实现里会记 WARN），这里只如实报出开关与核数
    if (pinThreadsToCores)
    {
        pool.setThreadsPinnedToCores(true);
        LOG_INFO_FMT("已开启工作线程绑核：{} 条循环线程按线程池下标依次占核，本进程可用核 {} 枚",
                     actualThreads, Platform::CpuAffinity::availableCoreCount());
    }

    LOG_INFO_FMT("Actual worker threads: {} (logical cores: {})", actualThreads, std::thread::hardware_concurrency());

    // 每线程一个服务器实例（SO_REUSEPORT 内核级负载均衡）
    std::vector<std::unique_ptr<Net::TcpServer>> servers;
    std::vector<Core::Task<>>                    acceptTasks;
    /// 真正承担监听的那几台（分发模式下只有接受器：工作服务器从不自己 accept，isRunning 恒为 false）
    std::vector<Net::TcpServer *> listeningServers;
    servers.reserve(actualThreads);
    acceptTasks.reserve(actualThreads);
    listeningServers.reserve(actualThreads + 1);

    // 按来源 IP 的限额只有一份、在所有监听器之间共享：内核按 SO_REUSEPORT 把新连接分给不同循环上的
    // 监听器，若每个服务器各持一份计数，单个来源的实际上限会乘上监听器数量，限额等于失效。
    // 未配置时留空指针，setPerIpConnectionLimiter(nullptr) 表示不作该限制
    std::shared_ptr<Net::PerIpConnectionLimiter> perIpConnectionLimiter;
    if (configuration.maximumConnectionsPerIp > 0)
    {
        perIpConnectionLimiter = std::make_shared<Net::PerIpConnectionLimiter>(configuration.maximumConnectionsPerIp);
        LOG_INFO_FMT("单来源并发上限 {}（所有 {} 个监听器共享同一份计数）", configuration.maximumConnectionsPerIp, actualThreads);
    }

    // h3 的统计要并进哪一份采集端：全进程共用一份，抓任意一个监听器的 /metrics 都能同时看到
    // TCP 端与 h3 的量。多监听器各持一份采集端时，一次抓取只报得出其中一台的数，
    // 计数器还会在两次抓取之间变小，采集侧的 rate() 与告警都会失真，因此这里刻意共用
    std::shared_ptr<Net::HttpMetricsCollector> http3MetricsCollector;
    // 所有监听器共用的那一份采集端：由第一台建起来的服务器交出，之后的每台都换接到它上面
    std::shared_ptr<Net::HttpMetricsCollector> sharedMetricsCollector;
    // 把这台服务器的采集端接到全进程共用的那一份：第一台交出它自己的，之后的都换接过去。
    // 明文与 TLS 两条通道都调它，因此按泛型收参数（HttpServer 与 HttpsServer 各自实现同名接口）
    const auto joinSharedMetricsCollector = [&](auto &server)
    {
        if (sharedMetricsCollector == nullptr)
        {
            sharedMetricsCollector = server->metricsCollector();
        }
        else
        {
            server->setMetricsCollector(sharedMetricsCollector);
        }
        // h3 也并进同一份：抓一次 /metrics 就覆盖 TCP 与 QUIC 两条服务路径
        http3MetricsCollector = sharedMetricsCollector;
    };
    // request-id 生成器同样借第一条服务器的：h1/h2/h3 落定的 id 前缀指向同一台机器，
    // 与 --metrics 无关（request-id 不是指标端点的一部分，一直开着）
    std::shared_ptr<Net::HttpRequestIdGenerator> http3RequestIdGenerator;

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

    // 压缩中间件的两种落点：默认交给工作线程（一次 gzip 大正文要占住循环线程几毫秒，
    // 这期间同循环的其他连接什么都做不了）；--compress-sync 留在循环里压，作对照
    const auto makeCompressionMiddleware = [&](Core::EventLoop &loop)
    {
        if (compressionExecutor.has_value())
        {
            return Net::compressionMiddleware(loop, *compressionExecutor);
        }
        return Net::compressionMiddleware();
    };

    // 按 --https 决定造哪种协议的服务器；返回基类指针，两条路径共用一套构造逻辑
    const auto buildHttpServer = [&](Core::EventLoop &loop)
    {
        auto server = std::make_unique<Net::HttpServer>(loop, *address);
        setupRoutes(server->router());
        server->setPerIpConnectionLimiter(perIpConnectionLimiter);
        server->setMaxConnections(configuration.maximumConnections);
        server->setLimits(configuration.limits);
        server->setParserLimits(configuration.parserLimits);
        server->setMemoryBudget(inflightBodyBudget);
        if (rateLimitBucket != nullptr)
        {
            server->router().addMiddleware(Net::tokenBucketRateLimiterMiddleware(rateLimitBucket));
        }

        if (compressResponses)
        {
            server->router().addMiddleware(makeCompressionMiddleware(loop));
        }

        // h2c：明文连接按先验知识直接说 HTTP/2（对端不发前奏就会被回 GOAWAY）。默认关闭
        if (useHttp2Cleartext)
        {
            server->setHttp2CleartextEnabled(true);
        }

        if (http3RequestIdGenerator == nullptr)
        {
            http3RequestIdGenerator = server->requestIdGenerator();
        }

        // 指标与健康检查端点是显式开关：不打开就完全没有暴露面
        if (configuration.exposeMetrics)
        {
            // 先接上共用的采集端再开端点：端点读的是 stats()，接线早于晚于它都不影响，
            // 但顺序固定下来能让「抓到的数是谁的」这件事一眼可读
            joinSharedMetricsCollector(server);
            server->enableMetricsEndpoint();
            server->enableHealthEndpoint();
        }
        return server;
    };

    const auto buildHttpsServer = [&](Core::EventLoop &loop)
    {
        auto server = std::make_unique<Net::HttpsServer>(loop, *address, certificateFile, keyFile);
        setupRoutes(server->router());
        if (compressResponses)
        {
            server->router().addMiddleware(makeCompressionMiddleware(loop));
        }
        server->setPerIpConnectionLimiter(perIpConnectionLimiter);
        server->setMaxConnections(configuration.maximumConnections);
        server->setLimits(configuration.limits);
        server->setParserLimits(configuration.parserLimits);
        server->setMemoryBudget(inflightBodyBudget);
        if (rateLimitBucket != nullptr)
        {
            server->router().addMiddleware(Net::tokenBucketRateLimiterMiddleware(rateLimitBucket));
        }

        if (http3RequestIdGenerator == nullptr)
        {
            http3RequestIdGenerator = server->requestIdGenerator();
        }

        // 指标与健康检查端点同样是显式开关；与明文侧同一形态（HttpsServer 自己的实现）
        if (configuration.exposeMetrics)
        {
            // 明文与 TLS 两侧共用那一份采集端：同一个端口号上开着两种协议时，抓哪一侧都是全量
            joinSharedMetricsCollector(server);
            server->enableMetricsEndpoint();
            server->enableHealthEndpoint();
        }
        return server;
    };

    const auto buildServer = [&](Core::EventLoop &loop) -> std::unique_ptr<Net::TcpServer>
    {
        return useHttps ? std::unique_ptr<Net::TcpServer>(buildHttpsServer(loop))
                        : std::unique_ptr<Net::TcpServer>(buildHttpServer(loop));
    };

    // 每台服务器所属的循环下标：收尾要按「它自己的循环」投递停止/排水任务。分发模式下最后一台
    // （只接受与派发的那台）与前面的工作循环不同循环，按下标猜会把任务投到别的线程上
    std::vector<unsigned> serverLoopIndexes;
    serverLoopIndexes.reserve(actualThreads + 1);

    if (dispatchAccept)
    {
        // 接受分发：接受循环只接受与派发，连接对象与协议工作全落在工作循环上
        auto distributor = std::make_shared<Core::ConnectionDistributor>();
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            std::unique_ptr<Net::TcpServer> worker = buildServer(pool.eventLoop(i));
            Net::TcpServer *rawWorker = worker.get();
            distributor->addWorker(pool.eventLoop(i),
                                   [rawWorker](const int fileDescriptor)
                                   {
                                       rawWorker->adoptConnection(fileDescriptor);
                                   });
            servers.push_back(std::move(worker));
            serverLoopIndexes.push_back(i);
        }

        // 只接受的那一台：自己从不建连接，因此不需要协议侧配置，但要占用同一个监听地址
        std::unique_ptr<Net::TcpServer> acceptor = buildServer(pool.eventLoop(0));
        Core::Task<> acceptTask = acceptor->startAccepting(distributor);
        pool.eventLoop(0).scheduler().schedule(acceptTask.handle());
        acceptTasks.push_back(std::move(acceptTask));
        // 分发模式下只有这一台自己 accept，工作服务器那几台的 isRunning 恒为 false
        listeningServers.push_back(acceptor.get());
        servers.push_back(std::move(acceptor));
        serverLoopIndexes.push_back(0);

        LOG_INFO_FMT("Accept dispatch enabled: 1 acceptor + {} worker loop(s)", actualThreads);
    } else
    {
#ifdef _WIN32
        // Windows 没有 SO_REUSEPORT：多线程时每个线程各绑一次同端口，谁来收连接由系统决定（不可预期），
        // 多数连接会压在其中一条监听器上。这里明说，免得把「多线程没提速」当成别的问题去查
        if (actualThreads > 1)
        {
            LOG_WARN_FMT("Windows 上没有 SO_REUSEPORT：{} 个监听器绑同一端口，连接不会在内核层分摊；"
                         "要多核扩展请加 --dispatch-accept",
                         actualThreads);
        }
#endif
        for (unsigned i = 0; i < actualThreads; ++i)
        {
            std::unique_ptr<Net::TcpServer> server = buildServer(pool.eventLoop(i));
            Core::Task<> task = server->start();
            pool.eventLoop(i).scheduler().schedule(task.handle());
            acceptTasks.push_back(std::move(task));
            listeningServers.push_back(server.get());
            servers.push_back(std::move(server));
            serverLoopIndexes.push_back(i);
        }
    }

    LOG_INFO_FMT("{} {}Server instances created, all accept tasks scheduled", actualThreads, useHttps ? "Https" : "Http");

    // HTTP/3 与 h1/h2 共存：它走 UDP，与上面的 TCP 端用同一个端口号互不干扰。
    // 只起一台而不做多监听器分发：QuicServer 内部已经按连接标识把报文分派到各自的连接，
    // 再叠一层 SO_REUSEPORT 只会把同一条连接的报文散到互不相识的监听器上
    Net::Router                      http3Router;
    std::unique_ptr<Net::QuicServer> http3Server;
    std::optional<Core::Task<>>      http3ListenTask;
    if (useHttp3)
    {
        setupRoutes(http3Router);
        if (compressResponses)
        {
            // 三条通道一律走外置版：h3 的会话收口现在会先叫醒并等完挂在业务协程上的在途动作
            // （Http3Session::abandonPendingStreams），「等它跑完」这条前提在 QUIC 侧同样成立
            http3Router.addMiddleware(makeCompressionMiddleware(pool.eventLoop(0)));
        }
        // 限流中间件挂在路由上，与明文/TLS 两侧同一份令牌桶：只限 h1/h2 等于给 h3 留了条后门
        if (rateLimitBucket != nullptr)
        {
            http3Router.addMiddleware(Net::tokenBucketRateLimiterMiddleware(rateLimitBucket));
        }

        Net::QuicServer::Configuration http3Configuration;
        http3Configuration.certificateFile = certificateFile;
        http3Configuration.privateKeyFile  = keyFile;
        // 与 h1/h2 用同一份解析上限：h3 的正文总量上限同样不该由样本自己去猜
        http3Configuration.parserLimits    = configuration.parserLimits;
        // 在途正文预算与 HTTP 侧共用同一份账：h3 的正文也驻留在进程内存里，
        // 不给它这份账就等于 --max-inflight-body 只管两条 TCP 通道
        http3Configuration.memoryBudget    = inflightBodyBudget;
        // 指标打开时 h3 的请求数、状态码类与单流取消并进上面那份采集端；没打开则为空指针、不采集
        http3Configuration.metricsCollector = http3MetricsCollector;
        // request-id 与两条 TCP 通道共用一份生成器：同一台机器上 h3 的 id 前缀不该另起一套
        http3Configuration.requestIdGenerator = http3RequestIdGenerator;
        // 连接级限额同样一份：h3 的「单连接最多多少条请求」与两条 TCP 通道同值
        http3Configuration.serverLimits = std::make_shared<const Net::HttpServerLimits>(configuration.limits);
        // 单来源并发上限也交给 h3：同一来源从 TCP 还是 QUIC 进来都算在同一个名额里
        http3Configuration.perIpConnectionLimiter = perIpConnectionLimiter;

        try
        {
            http3Server = std::make_unique<Net::QuicServer>(pool.eventLoop(0), http3Configuration);
            http3Server->setRouter(http3Router);
            http3ListenTask.emplace(http3Server->listen(*address));
            pool.eventLoop(0).scheduler().schedule(http3ListenTask->handle());
        } catch (const Base::Exception &http3Exception)
        {
            LOG_ERROR_EXCEPTION(http3Exception, "HTTP/3 服务端起不来，已退出。原因：{}", http3Exception.what());
            return 1;
        }
        LOG_INFO_FMT("HTTP/3 已在同一个端口号的 UDP 上监听（udp/{}）", port);
    }

    pool.start();

    // 报成功之前先确认监听器真的进入了监听态：绑定失败发生在分离投递的协程里（异常由 Task 记进错误日志，
    // 见 Core/Coroutine/Task.h 的无人接手上报），这里再不核一次的话，端口上其实没人守着，
    // 进程却照旧一副在服务的样子、退出码还是 0
    const std::size_t stuckListenerCount = waitForListenersToComeUp(listeningServers, http3Server.get(), kStartupConfirmTimeout);
    if (stuckListenerCount > 0)
    {
        LOG_ERROR_FMT("{} 个监听器在 {}ms 内没有进入监听状态，服务未运行。监听地址：{}（协程里抛出的原因见上面的错误日志）",
                      stuckListenerCount, kStartupConfirmTimeout.count(), address->toString());
        // 关停走既有那条路径：真的起来的那几台照样体面收口，不另起一套收尾
        g_running.store(false);
    }
    else
    {
        LOG_INFO("" + std::string(proto) + " server started  "+ proto + "://" + address->toString());
        LOG_INFO("Worker threads: " + std::to_string(actualThreads) + " (logical cores: " + std::to_string(std::thread::hardware_concurrency()) + ")");
        LOG_INFO("Endpoints: GET /  |  GET /json  |  GET /bench  |  GET /big");
        LOG_INFO("Press Ctrl+C to exit");

        // 指标端点是进程内的口径：多 worker 进程共用同一个监听端口时，一次抓取只命中其中一个进程，
        // 计数器会在两次抓取之间回落（同一进程内的多个监听器已在装配时共用一份采集端，跨进程没有
        // 这条通道）。不写出来的话，运维看到的就是「流量忽大忽小」而不是「这里少了一份进程」
        if (configuration.exposeMetrics && workerProcessCount > 1)
        {
            LOG_WARN_FMT("--metrics 与 --workers {} 一起用：每条抓取只命中一个 worker 进程的口径，"
                         "计数器会在两次抓取之间变小；要按进程聚合请让每个进程各自暴露一个抓取端点，或按单进程跑",
                         workerProcessCount);
        }
    }

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
        pool.eventLoop(serverLoopIndexes[index]).scheduler().scheduleRemote(stopTask.handle());
        shutdownTasks.push_back(std::move(stopTask));
    }

    // drain 自己在各自的循环线程上排队推进，主线程只等这个计数归零
    std::atomic<std::size_t> remainingDrainCount{servers.size()};
    for (std::size_t index = 0; index < servers.size(); ++index)
    {
        Core::Task<> drainTask = drainServerTask(*servers[index], kShutdownDrainTimeout, remainingDrainCount);
        pool.eventLoop(serverLoopIndexes[index]).scheduler().scheduleRemote(drainTask.handle());
        shutdownTasks.push_back(std::move(drainTask));
    }

    LOG_INFO_FMT("Draining {} server instance(s), in-flight requests get up to {}ms...", servers.size(), kShutdownDrainTimeout.count());
    while (remainingDrainCount.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // HTTP/3 与 TCP 侧同一形状收口：挡新连接、给每条连接发 GOAWAY、等在途请求做完，
    // 到期由 drain 自己兜底强关。它必须投回自己那条循环（drain 要遍历连接表）
    if (http3Server != nullptr)
    {
        std::atomic<std::size_t> remainingHttp3DrainCount{1};
        Core::Task<>             http3DrainTask       = drainHttp3ServerTask(*http3Server, kShutdownDrainTimeout, remainingHttp3DrainCount);
        pool.eventLoop(0).scheduler().scheduleRemote(http3DrainTask.handle());
        LOG_INFO_FMT("Draining HTTP/3 server, in-flight requests get up to {}ms...", kShutdownDrainTimeout.count());
        while (remainingHttp3DrainCount.load(std::memory_order_acquire) > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // 与上面两条 TCP 路径一样：帧收进 shutdownTasks，别在它还被循环持有时就先离开作用域
        shutdownTasks.push_back(std::move(http3DrainTask));
    }

    context.stop();

    LOG_INFO("Server stopped successfully");
    // 启动没确认成功时不能报 0：编排脚本与进程管理器都靠退出码判断这次启动算不算成了
    return stuckListenerCount == 0 ? 0 : 1;
}
