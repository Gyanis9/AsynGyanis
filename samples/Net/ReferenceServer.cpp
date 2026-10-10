// 多线程 HTTP/HTTPS 服务器示例：每线程一个 EventLoop + HttpServer/HttpsServer（SO_REUSEPORT）
#include "Base/Config/ConfigManager.h"
#include "Base/Exception/Exception.h"
#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Process/GracefulShutdown.h"
#include "Core/Process/ReloadSignal.h"
#include "Core/Process/ServiceWatchdog.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/SessionTicketKeyRing.h"
#include "Net/Acme/AcmeAutomationConfig.h"
#include "Net/Acme/AcmeCertificateManager.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerAssembly.h"
#include "Net/Http/HttpServerConfig.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Net/Tracing/HttpTracingMiddleware.h"
#include "Net/Tracing/TracingConfiguration.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Platform/System/CpuAffinity.h"
#include "Platform/System/ProcessInfo.h"
#include "Platform/System/ServiceNotification.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>

#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// 示例程序以可读性为先：各模块统一挂在 AsynGyanis 之下，这里引入根命名空间，
// 正文继续写 Net::/Core::/Base:: 即可，不必逐处补全限定名
using namespace AsynGyanis;

namespace
{
    std::atomic g_running{true};

    /// /big 的正文大小：与微基准 `*-response-compress-256k` 同档，两侧读数可互相印证
    constexpr std::size_t kLargeBodyBytes = 256 * 1024;

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
                    "server",  "engine", "request", "connection", "scheduler", "socket", "buffer", "response",
                    "timeout", "header", "payload", "cipher",     "packet",    "stream", "window", "priority",
            };
            // 数值是 Numerical Recipes 的 LCG 常数；只需伪随机，不需高质量随机源
            std::uint32_t randomState = 0x2545F491u;
            std::string   text;
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
        router.get("/",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(200);
                       response.setHeader("Content-Type", "text/plain");
                       response.setBody("Hello World");
                       co_return;
                   });

        router.get("/json",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(200);
                       response.setHeader("Content-Type", "application/json");
                       // pid 一并给出：多进程模式下它同时是「这条请求落到哪个 worker」的答案，
                       // 部署排查与压测都靠它对上号（不必再去翻进程表）
                       // version 取 CMake 的 project(... VERSION ...)（由 samples/CMakeLists.txt 那条
                       // target_compile_definitions 递进来）：这里曾写死一份 1.0.0，项目到 2.4.0 之后
                       // 它就成了假读数——而这段正文正是别人起业务时第一眼要抄的形状
                       response.setBody(std::string{R"({"status":"ok","version":")" ASYN_PROJECT_VERSION R"(","server":"AsynGyanis","pid":)"} +
                                        std::to_string(Platform::ProcessInfo::currentProcessId()) + "}");
                       co_return;
                   });

        router.get("/trace",
                   [](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       // 链路上下文的自检出口：把本条请求所在的 trace-id / span-id / 采样位吐回来。
                       // 没装 --trace-context 且上游也没给字段时就报 null——「不在任何链路里」这个区分本身要看得到。
                       // 只回显经过严格校验的十六进制标识：tracestate 的取值是外部文本，原样拼进 JSON 就是注入
                       response.setStatus(200);
                       response.setHeader("Content-Type", "application/json");
                       std::string body = R"({"traceId":)";
                       if (const auto context = Net::extractTraceContext(request); context.has_value())
                       {
                           body += '"';
                           body.append(context->traceIdText());
                           body += R"(","spanId":")";
                           body.append(context->parentIdText());
                           body += R"(","sampled":)";
                           body += context->isSampled() ? "true" : "false";
                       } else
                       {
                           body += R"(null,"spanId":null,"sampled":null)";
                       }
                       body += ",\"pid\":" + std::to_string(Platform::ProcessInfo::currentProcessId()) + "}";
                       response.setBody(std::move(body));
                       co_return;
                   });

        router.get("/bench",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(200);
                       response.setHeader("Content-Type", "text/plain");
                       response.setBody("OK");
                       co_return;
                   });

        // 响应压缩链路的端到端落点：/bench 的 2 字节正文永远到不了压缩阈值，所以「压完还能不能
        // 正确走完整条网络路径」（h1/h2 的序列化、content-length、vary）此前只有单测覆盖，
        // 这条路由给了进程外探针一个真 socket 的可比对象：同一地址带与不带 Accept-Encoding 各要一次
        router.get("/big",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(200);
                       response.setHeader("Content-Type", "text/plain");
                       response.setBody(largeCompressibleBody());
                       co_return;
                   });

        // 流式响应（SSE）验收用：分两段写出，客户端逐段收到就说明分块路径真的通
        router.get("/sse",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
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
        // 收到一条就原样回一条——回显本身就把「帧进得来、也出得去」两件事一起验了。
        // 消息类型必须照搬：把 Binary 回成 Text 会让二进制协议的客户端解不出内容，
        // 而 Autobahn 1.2.x / 9.2.x / 12.2.x 那几族判据（含空负载与分片）盯的就是这一条
        router.get("/ws",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.upgradeToWebSocket(
                               [](Net::WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       const bool isSent = message->opCode == Net::WebSocketOpCode::Binary ? co_await peer.sendBinary(message->payload)
                                                                                                           : co_await peer.sendText(message->payload);
                                       if (!isSent)
                                       {
                                           co_return;
                                       }
                                   }
                                   co_return;
                               });
                       co_return;
                   });
    }

    /// 排空预算的下限：处理器很快（演示配置里 writeTimeout 只有几秒）时也别让关停一闪而过，
    /// 留出「编排器把这台摘出负载」的那一段。真正的排空时长在下面按配置算
    constexpr std::chrono::milliseconds kMinimumShutdownDrainTimeout{5000};

    /// 启动确认的等待上限：绑定与监听都在协程的第一步做完，正常只需毫秒级；给足余量但不许无界等待
    constexpr std::chrono::milliseconds kStartupConfirmTimeout{2000};

    /// 叫停 ACME 常驻协程后每轮重试的间隔（等的是「帧已退出」的计数，不是循环对象本身）
    constexpr std::chrono::milliseconds kAcmeRenewalLoopWaitSlice{50};

    /// ACME 常驻协程的收口等待上限：停放一片是 1 秒，所以 100 轮（5 秒）给到两倍余量；到点打 WARN 继续收尾
    constexpr int kAcmeRenewalLoopWaitRoundLimit = 100;

    /// 非签发进程盯盘的节拍：证书按月才换一次，15 秒的取新延迟摆在 30 天的续期窗口前面无关紧要
    constexpr std::chrono::milliseconds kCertificateRotationPollInterval{15000};

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
    std::size_t waitForListenersToComeUp(const std::vector<Net::TcpServer *> &listeningServers, const Net::QuicServer *http3Server, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::size_t pendingCount = 0;
            for (const Net::TcpServer *server: listeningServers)
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
    Core::Task<> drainServerTask(Net::TcpServer &server, const std::chrono::milliseconds drainTimeout, std::atomic<std::size_t> &remainingDrainCount)
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
    Core::Task<> drainHttp3ServerTask(Net::QuicServer &server, const std::chrono::milliseconds drainTimeout, std::atomic<std::size_t> &remainingDrainCount)
    {
        co_await server.drain(drainTimeout);
        remainingDrainCount.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
    }

    /**
     * @brief 承载一条 ACME 常驻协程，并在它收口时把「还在跑的条数」减一
     * @details 管理器交回的帧要由调用方投进循环并持有到退出（见 `AcmeCertificateManager` 的 @note，
     *          跟随协程同一形状）。这里套一层协程做两件事：① `co_await` 它，帧归这条协程管，退出点
     *          变得可判定；② 退出时刻反映到一块 `std::atomic<int>` 上——循环对象与协程句柄都不能从
     *          主线程碰（循环的线程契约），而一个原子计数可以，于是收尾能等到「真的都退出了」再停循环，
     *          而不是猜一段睡眠够不够长。计数而不是布尔量：本进程可能同时在跑续期循环与跟盘协程两条。
     * @param loopTask 被承载的那条协程（按值收下，帧在协程帧销毁前一直持有它）
     * @param pendingLoopCount 输入输出：在跑的常驻协程条数，收口一条减一
     * @return Core::Task<> 协程，被承载的那条退出后立即完成
     */
    Core::Task<> runAcmeResidentLoopTask(Core::Task<void> loopTask, std::atomic<int> &pendingLoopCount)
    {
        co_await std::move(loopTask);
        pendingLoopCount.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
    }
} // namespace

int main(int argc, char **argv)
{
    std::string                host    = "localhost";
    uint16_t                   port    = 8080;
    unsigned                   threads = 0;         // 0 = auto (optimized for local benchmarks)
    std::optional<std::size_t> maxConnectionsPerIp; // 没给就走配置文件/内置默认（显式给 0 = 不限）
    bool                       useHttps             = false;
    bool                       useHttp2Cleartext    = false;
    bool                       exposeMetrics        = false;
    bool                       logJson              = false; // 日志按 JSON Lines 输出，供采集端解析
    bool                       dispatchAccept       = false; // 一个监听器 + N 个工作循环（不依赖 SO_REUSEPORT）
    bool                       pinThreadsToCores    = false; // 启动时把每个工作循环线程绑到一枚逻辑核上
    bool                       compressResponses    = false; // 按 Accept-Encoding 协商压缩响应（zstd/br/gzip）
    bool                       compressInLoop       = false; // --compress-sync：压缩留在循环线程上做完，只作对照用
    std::size_t                maxInflightBodyBytes = 0;     // 0 = 不限制在途正文字节总量
    std::size_t                workerProcessCount   = 1;     // 1 = 单进程；大于 1 时由 master 起这么多 worker 进程
    bool                       isWorkerProcess      = false; // 由 master 起的 worker 进程（内部开关，用户不必手写）
    bool                       useHttp3             = false; // 额外在同一个端口号的 UDP 上提供 HTTP/3（QUIC，需要证书）
    bool                       useTraceContext      = false; // 挂 W3C Trace Context 中间件，把链路上下文归一化到请求头上
    bool                       showUsage            = false;
    std::string                certificateFile      = "cert.pem";
    std::string                keyFile              = "key.pem";
    /// 命令行有没有显式给过证书/私钥：开了 acme 之后这两条要与 acme 的落点同解，
    /// 而「没给」（走上面那两个默认值）与「给成了别的路径」是两种处置，只能按是否给过来判
    bool certificateFileGiven = false;
    bool keyFileGiven         = false;
    /// 会话票据密钥文件，可重复给（首份签发、其余只解开旧票据）；空 = 按 OpenSSL 默认随机密钥
    std::vector<std::string> ticketKeyFiles;
    std::string              configFile;
    /// 本进程在 worker 池里的序号：由 master 逐份传下来（见 WorkerSupervisor），用来给管理口错开端口
    unsigned workerIndex = 0;
    /// 静态目录（--static）：空表示不提供服务。三条通道（h1/h2/h3）共用同一个目录，与 --metrics、
    /// --compress 一样是「一份配置喂三个监听器」，不给某一条留后门
    std::string staticDirectory;

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
        } else if (arg == "--threads")
        {
            // 上限 4096：线程数按可用核数量级取，超出这个数只会把机器起爆，出现即视为笔误
            threads = static_cast<unsigned>(Samples::readNumericOption(argc, argv, i, "--threads", 0U, 4096U));
            ++i;
        } else if (arg == "--max-connections-per-ip")
        {
            maxConnectionsPerIp = static_cast<std::size_t>(Samples::readNumericOption(argc, argv, i, "--max-connections-per-ip", 0U, std::numeric_limits<std::uint64_t>::max()));
            ++i;
        } else if (arg == "--static")
        {
            staticDirectory = Samples::readOptionValue(argc, argv, i, "--static", "一个静态文件目录");
            ++i;
        } else if (arg == "--https")
            useHttps = true;
        else if (arg == "--h2c")
            useHttp2Cleartext = true;
        else if (arg == "--h3")
            useHttp3 = true;
        else if (arg == "--trace-context")
            useTraceContext = true;
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
        } else if (arg == "--max-inflight-body")
        {
            maxInflightBodyBytes = static_cast<std::size_t>(Samples::readNumericOption(argc, argv, i, "--max-inflight-body", 0U, std::numeric_limits<std::uint64_t>::max()));
            ++i;
        } else if (arg == "--cert")
        {
            certificateFile      = Samples::readOptionValue(argc, argv, i, "--cert", "一个证书文件路径");
            certificateFileGiven = true;
            ++i;
        } else if (arg == "--ticket-key")
        {
            // 可重复：轮换的形态就是「新的插首位、旧的留在后面」，与 nginx 的同名指令一致
            ticketKeyFiles.push_back(Samples::readOptionValue(argc, argv, i, "--ticket-key", "一个会话票据密钥文件路径（48 或 80 字节）"));
            ++i;
        } else if (arg == "--key")
        {
            keyFile      = Samples::readOptionValue(argc, argv, i, "--key", "一个私钥文件路径");
            keyFileGiven = true;
            ++i;
        } else if (arg == "--config")
        {
            configFile = Samples::readOptionValue(argc, argv, i, "--config", "一个配置文件路径");
            ++i;
        } else if (arg == "--workers")
        {
            // 上限与 --threads 同一条理由：这么多进程只会把机器起爆，出现即视为笔误
            workerProcessCount = static_cast<std::size_t>(Samples::readNumericOption(argc, argv, i, "--workers", 0U, 4096U));
            ++i;
        } else if (arg == "--worker")
            isWorkerProcess = true;
        else if (arg == "--worker-index")
        {
            // 由 master 逐份传下来的槽位序号（内部开关，用户不必手写）：worker 补起时序号不变，
            // 所以它的管理口号也不变，采集端抓到的始终是同一个进程
            workerIndex = static_cast<unsigned>(Samples::readNumericOption(argc, argv, i, "--worker-index", 0U, 4095U));
            ++i;
        } else if (arg == "--help")
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
        LOG_INFO("Usage: ReferenceServer [--host localhost] [--port 8080] [--threads N]");
        LOG_INFO("                  [--https] [--cert cert.pem] [--key key.pem] [--h2c] [--h3] [--static <目录>]");
        LOG_INFO("                  [--ticket-key <文件>] [--max-connections-per-ip N] [--metrics] [--config <文件>]");
        LOG_INFO("  --ticket-key TLS 会话票据密钥文件（48 或 80 字节二进制，openssl rand 48 > ticket.key）：");
        LOG_INFO("                  可重复给（首份签发、其余只解旧票据，即轮换）。多进程 --workers 下各进程装同一份，");
        LOG_INFO("                  客户端第二次连接被分到别的进程也能恢复会话；不给则每进程一份随机密钥、跨进程必落空");
        LOG_INFO("  --threads 0 = auto (min(4, hw_concurrency)), 1 = single-threaded");
        LOG_INFO("  --h2c 明文连接按 HTTP/2（先验知识）服务，需客户端直接发连接前奏（仅 HTTP 端可用）");
        LOG_INFO("  --h3 额外在同一个端口号的 UDP 上提供 HTTP/3：走同一套路由与处理器，需要证书（QUIC 自带 TLS）；"
                 "同时让 TCP 侧响应带上 alt-svc 通告，客户端由此自己学到 h3 端口");
        LOG_INFO("  --max-connections-per-ip 单个来源的并发上限；不给走配置文件/内置默认 256，显式给 0 = 不限");
        LOG_INFO("  --trace-context 挂 W3C Trace Context 中间件：上游带了合法的 traceparent 就原样沿用，");
        LOG_INFO("            缺席或畸形（含同名多条）则新起一条链路并写回请求头，业务读 GET /trace 就能看到；");
        LOG_INFO("            同时把自己的条目 asyn=<span-id> 挪到 tracestate 最前（上游条目次序不动）");
        LOG_INFO("  --config 的 tracing 段开链路记录：tracing.enabled + service_name 起一节 SERVER，");
        LOG_INFO("            tracing.file.path 每节一行 JSON 落盘、tracing.otlp.endpoint 按 OTLP/HTTP 发给采集端；");
        LOG_INFO("            比例、批量与时限分别是 sample_ratio / batch_span_count / export_interval_ms");
        LOG_INFO("  --metrics 暴露 GET /metrics（Prometheus 文本）、GET /healthz、GET /readyz 与 GET /debug/loops（进程内每条事件循环一行的 JSON，");
        LOG_INFO("          看哪条循环被处理器占住）；开了 --h3 时 h3 的请求数/状态码类一并计入");
        LOG_INFO("          /readyz 只问「还在不在接新连接」：stop() 或 drain() 之后回 503，正在排空在途请求的这段");
        LOG_INFO("          时间里编排器就该把这台摘出负载，而 /healthz 仍回 200（进程还在转，杀掉它才会真丢请求）");
        LOG_INFO("          这四项的收口只能写在 --config 的 server 段里：ops_bearer_token 给 /metrics 与 /debug/loops");
        LOG_INFO("          挂一道 Bearer 闸门（/healthz 与 /readyz 刻意不挡，存活与就绪探针要能被编排器无凭据访问）；");
        LOG_INFO("          metrics_port + metrics_address 让运维端点另起一台只听指定地址的服务器（默认回环），");
        LOG_INFO("          多进程 --workers 下每个进程各听 metrics_port+序号，采集端按进程抓");
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
        LOG_INFO("  --max-inflight-body 在途正文总量上限（整机字节数，0 = 不限；配置里对应 server.memory_budget_bytes，");
        LOG_INFO("            本开关是它的命令行覆盖）：挡住多条连接同时压着大正文，超出的请求回 503；");
        LOG_INFO("            按 --workers 摊到每个进程，明文、HTTPS 与 h3 三端共用同一份账");
        LOG_INFO("  --workers N 用 N 个 worker 进程服务同一个端口（默认 1 = 单进程）：");
#ifdef _WIN32
        // 本示例在 Windows 上起不了多进程：那边没有 SO_REUSEPORT，多个进程各自 bind 同端口只会有一条
        // 监听器收到连接，要靠 master 移交监听套接字——那是 CoreWorker 那份示例演示的形状，
        // 本示例的 worker 分支没有接管移交描述字的入口。宁可这里说清、启动即失败并报原因，
        // 也不让「--workers 4」看起来跑起了四路服务
        LOG_INFO("            本示例仅 POSIX 支持（靠 SO_REUSEPORT 分摊）；Windows 上会在构造编排者时");
        LOG_INFO("            报错退出，要多进程请看 CoreWorker 那份示例；");
#else
        LOG_INFO("            master 只做编排不服务，各 worker 靠 SO_REUSEPORT 分别监听同一端口，");
        LOG_INFO("            SIGTERM/SIGINT 会让 worker 各自体面退出；");
#endif
        LOG_INFO("            注意进程间不共享状态：单来源限额、限流上限与指标计数都是每进程一份");
        LOG_INFO("  --worker 内部开关：由 master 传给 worker，用户不必手写");
        LOG_INFO("  --config 从配置文件读 server 段（限额、按 IP 限额、限流、指标开关、运维端点令牌）、");
        LOG_INFO("            logging 段（root 与各日志器的等级、控制台/文件/滚动 Sink）与 acme 段");
        LOG_INFO("            （证书自动化：enabled/directory_url/domains/落点四件/contact_email/tos_accepted/");
        LOG_INFO("            challenge/两个节奏/dns 子段）——开了 acme 之后服务端证书与私钥的路径以 acme 的落点为准，");
        LOG_INFO("            监听器与续期写盘必须是同一条路径，否则签完的新那张装不回去；");
        LOG_INFO("            命令行上显式给出的开关优先于文件，详见 Net/Http/HttpServerConfig.h 的键名说明；");
        LOG_INFO("            运维令牌只能写在文件里：命令行上的令牌会进 shell 历史与进程列表");
        return 0;
    }

    // 配置优先级：命令行 > 配置文件 > 内置默认值。文件是「这台服务的常态配置」，
    // 命令行是「这一次运行的临时改动」，临时改动优先
    Net::HttpServerConfiguration configuration;
    Net::TracingConfiguration    tracingConfiguration;
    /// 证书自动化：段没写就是关着（默认值即「不建管理器」），其余一切不合法在读配置那一刻抛出
    Net::AcmeAutomationConfiguration acmeConfiguration;

    // 这两个都要早于任何工作线程：POSIX 的信号屏蔽字由子线程继承，先起的线程没被打上屏蔽，
    // 那之后一枚 SIGHUP 会按缺省动作直接把进程干掉。声明顺序也有讲究——serviceStatus 在前、
    // reloadSignal 在后，于是析构时重载观察者先停、通知套接字后关，动作里那几个 send() 不会碰到已关的描述符。
    // 接管是无条件的：装了之后 SIGHUP 不再终止进程，所以不能按「有没有 --config」分成两种语义
    // ——那会让同一条信号在两种部署里行为不同，最难查。没配文件时它回一条状态说明为什么什么都没重读
    Platform::ServiceNotification serviceStatus;
    Core::ReloadSignal            reloadSignal;

    // SIGHUP 这条运维入口无条件装着：接管装上之后它不再按缺省动作终止进程，所以不能按「有没有
    // --config」分成两种语义——同一条信号在两种部署里行为不同，是最难归因的那一类。没配文件时
    // 它回一条状态说明为什么什么都没重读，而不是悄悄什么都不做
    reloadSignal.onReload(
            [configFile, &serviceStatus]
            {
                if (configFile.empty())
                {
                    static_cast<void>(serviceStatus.send(Platform::ServiceNotification::statusState("没配 --config：没有可重读的配置")));
                    LOG_WARN("SIGHUP 到了，但本进程没配 --config：没有可重读的文件（要热重载请带 --config，或走文件监听那一路）");
                    return;
                }

                // systemd 的 reload 握手是两条一对：RELOADING=1 让监督者推迟超时，重读结束再补一条
                // READY=1 收尾。只发前一条，那一侧会等到超时再把进程杀掉
                static_cast<void>(serviceStatus.send(Platform::ServiceNotification::kReloadingState));

                const Base::ConfigLoadResult reloaded = Base::ConfigManager::instance().reload();
                if (reloaded.success)
                {
                    // logging 段重新装载——这条路上真正会变的运行时行为就是等级与滚动参数。
                    // 限额、TLS 证书路径、监听地址与线程数都在启动期定型，不随这次重载改变：
                    // 说清哪几样会生效，比让人以为 reload 等于 nginx 那种整服重建重要
                    Base::LoggerConfigLoader::loadFromConfig("logging", std::filesystem::path(configFile).parent_path());
                    LOG_INFO_FMT("SIGHUP：配置已重读（{}），logging 段按新值生效；server 段与 acme 段是启动期定型的，这次不改", configFile);
                } else
                {
                    std::string reasons;
                    for (const auto &reason: reloaded.errors)
                    {
                        reasons += reasons.empty() ? reason : "；" + reason;
                    }
                    LOG_ERROR_FMT("SIGHUP：配置重读失败（{}），仍按原配置继续服务", reasons);
                }

                static_cast<void>(serviceStatus.send(Platform::ServiceNotification::kReadyState));
                static_cast<void>(serviceStatus.send(Platform::ServiceNotification::statusState(reloaded.success ? "配置已重读" : "配置重读失败，仍按原配置继续服务")));
            });
    LOG_INFO("SIGHUP 已接管：kill -HUP <pid> 会重读配置文件并重新装载 logging 段（Windows 没有这条约定，走文件监听那一路）");

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

            // logging 段与 server 段读自同一份文件，就得在这里一起装好：不装的话部署方写的等级与
            // 滚动参数只有 ConfigManager 知道，症状是「配置写了没生效」而不是报错。基准目录取配置文件
            // 所在目录，让配置里的相对路径（logs/app.log）落在部署者预期的位置而不是当前工作目录。
            // 装完之后 root 只剩配置里那些 Sink：控制台安静下来是这份配置的本意，不是示例坏了
            Base::LoggerConfigLoader::loadFromConfig("logging", std::filesystem::path(configFile).parent_path());
            // ConfigManager 内部按键的点分路径扁平存放，get() 取不到任何中间层节点，
            // getSection() 才把 server 段还原成嵌套对象。读取器要的是「以 server 为根的文档」，
            // 这里补上段名这一层外壳：它只认文档结构，不关心配置来自文件还是内存
            Base::ConfigObject document;
            document.emplace(std::string(Net::kHttpServerConfigSection), Base::ConfigManager::instance().getSection(Net::kHttpServerConfigSection));
            document.emplace(std::string(Net::kTracingConfigSection), Base::ConfigManager::instance().getSection(Net::kTracingConfigSection));
            document.emplace(std::string(Net::kAcmeConfigSection), Base::ConfigManager::instance().getSection(Net::kAcmeConfigSection));
            // 这里必须用圆括号：花括号会去配 initializer_list 那个构造，整份文档就变成一个数组，
            // 各读取器都找不到自己的段而全部走默认值——现象只是「配置写了没生效」
            const Base::ConfigValue configurationDocument(std::move(document));
            configuration = Net::readHttpServerConfiguration(configurationDocument);
            // 链路段与 server 段同批读：读不出来的写法（未知键、类型不符）不该等到装配出口时才炸
            tracingConfiguration = Net::readTracingConfiguration(configurationDocument);
            // acme 段也在这一批读：证书自动化最常见的现场是「写了但没生效」，而它的第一处出口是几天之后
            // 第一次查到期，那时候没人还记得配置文件里写过什么。读不出、交叉判据不过都当场终止启动
            acmeConfiguration = Net::readAcmeConfiguration(configurationDocument);
        } catch (const std::exception &configurationException)
        {
            LOG_ERROR_EXCEPTION(configurationException, "配置读取失败，服务未启动。文件：{}，原因：{}", configFile, configurationException.what());
            return 1;
        }
        LOG_INFO_FMT("已读取配置 {}：最大连接 {}，单来源 {}，限流 {} 请求/s（桶 {}），指标 {}", configFile, configuration.maximumConnections, configuration.maximumConnectionsPerIp,
                     configuration.requestsPerSecond, configuration.rateLimitBurstCapacity, configuration.exposeMetrics ? "开" : "关");
    }

    // 排空预算取「服务器自己承诺的响应产出预算」，而不是写死一个数：`write_timeout_ms` 是三条通道对
    // 「一个处理器最多能占多久」的上限（见 HttpServerLimits.h），排空比它短就等于在每次部署时把已受理的
    // 长处理器整批切掉。只取较大的一边：配置把它压得很小时仍留住下限，让编排有时间把这台摘出负载。
    // 于是这台进程的关停上界等于运维给的宽限期——`write_timeout_ms` 配多大就要准备等多久
    const std::chrono::milliseconds shutdownDrainTimeout{std::max(kMinimumShutdownDrainTimeout, configuration.limits.writeTimeout)};
    LOG_INFO_FMT("优雅排空预算 {}ms（取 write_timeout_ms={} 与下限 {}ms 的较大一边）：在途请求超过这个数就会被强制收口", shutdownDrainTimeout.count(),
                 configuration.limits.writeTimeout.count(), kMinimumShutdownDrainTimeout.count());

    // 链路装配：开关在配置里（tracing.enabled），没配就是 nullptr——后续的中间件注册与日志都按空指针走。
    // 地址写错、服务名缺失这类问题在这里当场终止启动：留着一个发不出东西的出口，比不记链路更糟
    std::shared_ptr<Net::Tracer> tracer;
    try
    {
        tracer = Net::buildTracer(tracingConfiguration);
    } catch (const std::exception &tracingException)
    {
        LOG_ERROR_EXCEPTION(tracingException, "链路装配失败，服务未启动。原因：{}", tracingException.what());
        return 1;
    }
    // 关着也要说一句：「配置文件里写了 tracing 段却没生效」是这类装配最难查的形态，
    // 只有一行启动日志能把它与「段根本没读到」分开
    if (tracer != nullptr)
    {
        if (!useTraceContext)
        {
            // 归一化那一步没开：畸形或残缺的 traceparent 会被本节直接当上级用（一节仍然记，只是父子关系跟着上游的写法走）
            LOG_WARN("链路：开了 tracing.enabled 但没开 --trace-context，本节会按请求头上原样的上下文起，畸形写法不会重起一条");
        }
        LOG_INFO_FMT("链路：开（服务名 {}，采样比例 {}，出口 = 文件 {} + OTLP {}）", tracingConfiguration.serviceName, tracingConfiguration.sampleRatio,
                     tracingConfiguration.spanFilePath.empty() ? "未配" : tracingConfiguration.spanFilePath,
                     tracingConfiguration.otlpEndpoint.empty() ? "未配" : tracingConfiguration.otlpEndpoint);
    } else
    {
        LOG_INFO_FMT("链路：关（tracing.enabled 未开，或服务未给出服务名；当前读到 enabled={}、service_name={}）", tracingConfiguration.enabled ? "true" : "false",
                     tracingConfiguration.serviceName.empty() ? "未配" : tracingConfiguration.serviceName);
    }

    // 命令行覆盖：显式给出的开关优先于文件里的同名项。这里必须用「有没有给」而不是「是否 > 0」判：
    // 0 现在是一个有意义的取值（显式不限），拿它当「没给」会让 --max-connections-per-ip 0 失效
    if (maxConnectionsPerIp.has_value())
    {
        configuration.maximumConnectionsPerIp = *maxConnectionsPerIp;
    }
    if (exposeMetrics)
    {
        configuration.exposeMetrics = true;
    }

    // 生效值必须打出来：这几个键「配置文件里没写」与「显式写了 0」在线上长得一模一样，而前者取的是
    // 内置有限默认、后者是真的不限。不打这一行，部署方只能等撞上限那天才知道自己跑的是哪一档。
    // 摊分规则不在这里重写——装配出口用的是同一个 perProcessShare，两边各算一套就会印出一个假数
    const std::size_t workerProcessTotal           = std::max<std::size_t>(1, workerProcessCount);
    const std::size_t perProcessMaximumConnections = Net::perProcessShare(configuration.maximumConnections, workerProcessTotal);
    const std::size_t perProcessMaximumPerIp       = Net::perProcessShare(configuration.maximumConnectionsPerIp, workerProcessTotal);
    // 在途正文预算的口径与连接数一致：配置与 --max-inflight-body 都是整机的数，摊到本进程才是账
    const std::size_t wholeMachineInflightBodyBytes = maxInflightBodyBytes > 0 ? maxInflightBodyBytes : configuration.memoryBudgetBytes;
    const std::size_t perProcessInflightBodyBytes   = Net::perProcessShare(wholeMachineInflightBodyBytes, workerProcessTotal);
    const auto        capText                       = [](const std::size_t value) { return value == 0 ? std::string("不限（显式配 0）") : std::to_string(value); };
    LOG_INFO_FMT("并发限额：整机 {} 摊给 {} 个进程 → 每进程 {}（每台监听器的生效值见下面一行）；单来源（本进程多台共用一份限额器）→ 每进程 {}；请求速率 {}，在途正文总量 {}",
                 capText(configuration.maximumConnections), workerProcessTotal, capText(perProcessMaximumConnections), capText(perProcessMaximumPerIp),
                 configuration.requestsPerSecond > 0.0 ? std::format("{:.0f} 请求/s", configuration.requestsPerSecond) : std::string("不限（默认）"),
                 wholeMachineInflightBodyBytes == 0 ? std::string("不限（默认）") : std::to_string(wholeMachineInflightBodyBytes) + " 字节");

    // 多进程：master 只做编排，自己不服务——既当 master 又当 worker 会让「谁在服务」含糊，
    // 也会让「worker 崩了补一个」这条路径多一种要处理的形态。参数原样转给 worker，
    // 多两个内部开关：--worker（据此跳过这一段，直接去跑服务器）与 --worker-index（本进程的槽位号）
    if (workerProcessCount > 1 && !isWorkerProcess)
    {
        try
        {
            Core::WorkerSupervisor::Configuration supervisorConfiguration;
            supervisorConfiguration.executablePath = argv[0];
            supervisorConfiguration.workerArguments.assign(argv + 1, argv + argc);
            supervisorConfiguration.workerArguments.emplace_back("--worker");
            // 每个 worker 拿到自己那一份槽位号：管理口按「metrics_port + 序号」错开，采集端才能
            // 一次抓一个进程并把各进程的数加总，而不是随机命中某一台
            supervisorConfiguration.workerIndexArgument = "--worker-index";
            supervisorConfiguration.workerCount         = workerProcessCount;

            Core::WorkerSupervisor supervisor(std::move(supervisorConfiguration));
            LOG_INFO_FMT("多进程模式：{} 个 worker（master 进程号 {} 只做编排；Ctrl+C 或 SIGTERM 会让 worker 各自体面退出）", workerProcessCount,
                         Platform::ProcessInfo::currentProcessId());
            // 这条形态不向服务管理器报就绪：被监督的是 master，而 worker 各报一次会让那一侧收到
            // N 条 READY=1，第二条起只是噪声。要用 Type=notify 就按单进程跑，或者用 Type=exec
            // （master 起来即算成功，配合本引擎自己的收尾顺序）
            LOG_INFO("多进程模式不向服务管理器上报 READY=1：监督对象是 master 进程，而 worker 各报一次会变成重复上报");
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
    // 静态目录在碰网络之前先验：三条通道共用这一份配置，等到某一侧的构造里抛出，报出来的就是
    // 「某个监听器起不来」，而不是「你给的目录不存在」这么一句直接能用的话
    if (!staticDirectory.empty() && !std::filesystem::is_directory(staticDirectory))
    {
        LOG_ERROR_FMT("--static 给的不是一个存在的目录：「{}」（h1/h2/h3 三条通道共用它，任一通道都起不来）", staticDirectory);
        return 1;
    }
    if (useHttp3 && !useHttps)
    {
        LOG_ERROR("--h3 需要证书：QUIC 自带 TLS，请与 --https 一起用（--cert/--key）");
        return 1;
    }

    if (!ticketKeyFiles.empty() && !useHttps)
    {
        // 票据是 TLS 的东西：明文 HTTP 上没有它的位置，静默收下就等于让部署方以为共享已经生效
        LOG_ERROR("--ticket-key 需要 TLS：请与 --https（或 --h3，它自带 TLS）一起用");
        return 1;
    }

    if (!ticketKeyFiles.empty())
    {
        // 启动期先把密钥文件校验一遍：不合格要的是「一句人话 + 退出码 1」，而不是把异常抛穿到
        // 建服务器的循环线程上。真正装载仍在各服务器构造时做（换代要按路径重读，路径才是身份）
        try
        {
            std::vector<std::string> validatedKeys;
            Core::SessionTicketKeyRing::readKeyFiles(ticketKeyFiles, validatedKeys);
        } catch (const Base::Exception &keyFailure)
        {
            LOG_ERROR_FMT("ReferenceServer 启动失败：{}", keyFailure.what());
            return 1;
        }
    }

    // 证书自动化开着的时候，服务端加载的证书与私钥这两条路径以 acme 段的落点为准：监听器加载与
    // 续期写盘必须是同一条路径（`reloadCertificate()` 按监听器原来那条路径重读），否则签完的新那张
    // 永远装不回去——磁盘月月换、线上还是旧的，而这正是面板上看不出来的那种静默。
    // 显式给过的 --cert/--key 与 acme 落点不一致时不替谁猜意图：以 acme 为准，并把「顶掉了什么」
    // 说成一行 WARN。这里刻意不拒：拒会把「两个写法指的是同一张证书」的部署也一并挡在门外，
    // 而真正的同解判据在 validateAcmeAssembly 里（那边按规范式比对），装配这一处只需要把身份定下来
    if (acmeConfiguration.isEnabled)
    {
        const std::filesystem::path acmeCertificateFile = acmeConfiguration.manager.certificateFile;
        const std::filesystem::path acmePrivateKeyFile  = acmeConfiguration.manager.privateKeyFile;
        if ((certificateFileGiven && std::filesystem::path(certificateFile) != acmeCertificateFile) || (keyFileGiven && std::filesystem::path(keyFile) != acmePrivateKeyFile))
        {
            LOG_WARN_FMT("acme 开着：证书身份以 acme 段的落点为准，--cert/--key 被顶掉。命令行给的是 {} / {}，本进程将按 {} / {} 加载并续期", certificateFile, keyFile,
                         acmeCertificateFile.string(), acmePrivateKeyFile.string());
        }
        certificateFile = acmeCertificateFile.string();
        keyFile         = acmePrivateKeyFile.string();

        // 落点上还没有身份时 TLS 监听器根本起不来，而 OpenSSL 报回来的是一句 PEM 解析错误，不会提
        // 「先签一张」。首次签发归 acme_issuance_probe（那是操作者显式启动的一次性动作），本进程的
        // 常驻循环只负责此后每次到期前的续期与热装回——把这条边界在启动期说清，比留一句 OpenSSL 黑话有用
        std::error_code existenceError;
        if (!std::filesystem::exists(certificateFile, existenceError) || !std::filesystem::exists(keyFile, existenceError))
        {
            LOG_ERROR_FMT("acme 开着，但落点上还没有可加载的身份：证书 {}、私钥 {}（两者都得先存在）。首次签发请用 acme_issuance_probe 跑一次，"
                          "此后由本进程的常驻续期循环接手",
                          certificateFile, keyFile);
            return 1;
        }
    }

    if (threads == 0)
        // 自动档按「本进程实际可用的核数」收口（容器 CPU 配额与许可核集合都算），再压到示例的 4 条
        // 上限：与 IoContext/ThreadPool 的自动档同一口径，否则受限环境下这里会起满宿主核数条循环
        threads = std::max(1u, std::min(4u, static_cast<unsigned>(Platform::CpuAffinity::recommendedWorkerCount())));

    // 停机信号的接管交给库：屏蔽字必须在工作线程起来之前设好（子线程继承掩码），所以这两句
    // 紧挨在算出线程数之后、造 IoContext 之前。动作本身只是把一个原子量置假，
    // 与循环无关，因此用不绑事件循环的那种构造（绑了反而要求那时已经有循环在跑）
    // 通知对象与重载观察者都在读配置之前那一段就建好了（信号屏蔽字必须早于工作线程），这里只装停机接管
    static_cast<void>(Core::GracefulShutdown::blockStopSignals());
    Core::GracefulShutdown shutdown;
    shutdown.onShutdown([] { g_running.store(false); });
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const char *proto = useHttps ? "https" : "http";
    LOG_INFO_FMT("ReferenceServer starting — {}://{}:{} threads={} pid={}{}", proto, host, port, threads, Platform::ProcessInfo::currentProcessId(),
                 isWorkerProcess ? " (worker)" : "");

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

    auto &pool          = context.threadPool();
    auto  actualThreads = static_cast<unsigned>(pool.threadCount());

    // 绑核是部署方的选择：开了之后每条循环线程固定占一枚逻辑核，减少调度迁移与缓存被踩。
    // 线程数多于可用核数时只有前若干条被绑（实现里会记 WARN），这里只如实报出开关与核数
    if (pinThreadsToCores)
    {
        pool.setThreadsPinnedToCores(true);
        LOG_INFO_FMT("已开启工作线程绑核：{} 条循环线程按线程池下标依次占核，本进程可用核 {} 枚", actualThreads, Platform::CpuAffinity::availableCoreCount());
    }

    LOG_INFO_FMT("Actual worker threads: {} (logical cores: {})", actualThreads, std::thread::hardware_concurrency());

    // 看门狗的节拍挂在循环上：喂出去的那条 WATCHDOG=1 说的是「这批循环还在转」，用一条独立线程喂
    // 就等于把卡死的循环报成健康——那正是这条通道唯一要抓的形态。声明在 context 之后，于是析构时
    // 本对象先走、循环已经 stop() 过（帧只能在没人再恢复它们的时候销毁），通知套接字最后关。
    Core::ServiceWatchdog serviceWatchdog(pool, serviceStatus);

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
    if (perProcessMaximumPerIp > 0)
    {
        // 建的是「本进程这一份」的限额器，与装配出口摊出来的数必须同源，否则会被出口的
        // 「共享实例与配置不一致」判据当场拒——那条拒正是为了让这种偏差不能静默存在
        perIpConnectionLimiter = std::make_shared<Net::PerIpConnectionLimiter>(perProcessMaximumPerIp);
        LOG_INFO_FMT("单来源并发上限 {}（本进程内 {} 个监听器共享同一份计数；整机口径 {} 已按 {} 个进程摊过）", perProcessMaximumPerIp, actualThreads,
                     configuration.maximumConnectionsPerIp, workerProcessTotal);
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
        } else
        {
            server->setMetricsCollector(sharedMetricsCollector);
        }
        // h3 也并进同一份：抓一次 /metrics 就覆盖 TCP 与 QUIC 两条服务路径
        http3MetricsCollector = sharedMetricsCollector;
    };
    // request-id 生成器同样借第一条服务器的：h1/h2/h3 落定的 id 前缀指向同一台机器，
    // 与 --metrics 无关（request-id 不是指标端点的一部分，一直开着）
    std::shared_ptr<Net::HttpRequestIdGenerator> http3RequestIdGenerator;

    // 限流桶同样只有一份：它要的是「本进程这一份的全局 RPS 上限」，各持一份等于上限乘以监听器数。
    // 桶里的数必须由摊分出口给：装配出口会拿桶上的速率与容量比对摊分结果，不一致就拒绝装配
    std::shared_ptr<Net::TokenBucket> rateLimitBucket;
    const Net::PerProcessRateLimit    rateShare = Net::perProcessRateLimit(configuration.requestsPerSecond, configuration.rateLimitBurstCapacity, workerProcessTotal);
    if (rateShare.requestsPerSecond > 0.0)
    {
        rateLimitBucket = std::make_shared<Net::TokenBucket>(rateShare.requestsPerSecond, rateShare.burstCapacity);
        LOG_INFO_FMT("全局限流：整机 {} 请求/s（桶容量 {}）摊给 {} 个进程 → 每台 {:.4g} 请求/s（桶容量 {:.4g}），本进程内所有监听器共享同一个桶", configuration.requestsPerSecond,
                     configuration.rateLimitBurstCapacity, workerProcessTotal, rateShare.requestsPerSecond, rateShare.burstCapacity);
    }

    // 在途正文预算同样只有一份：它要的是「本进程的正文占用上限」，各监听器各持一份等于上限乘以监听器数。
    // 数值口径与限额器、限流桶一致（整机摊到每台），装配入口会拿对象上的上限与摊分结果比对，不一致就拒
    std::shared_ptr<Net::HttpMemoryBudget> inflightBodyBudget;
    if (perProcessInflightBodyBytes > 0)
    {
        inflightBodyBudget = std::make_shared<Net::HttpMemoryBudget>(perProcessInflightBodyBytes);
        LOG_INFO_FMT("在途正文总量：整机 {} 字节摊给 {} 个进程 → 每台 {} 字节（本进程内所有监听器与 h3 共享同一份账）", wholeMachineInflightBodyBytes, workerProcessTotal,
                     perProcessInflightBodyBytes);
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

    // 配置键到 setter 的对接只有一处实现（见 Net/Http/HttpServerAssembly.h）：这里只交进
    // 「跨监听器共用的那几份对象」与「整机限额要摊给几个进程」，其余键（含在途正文预算）由装配出口落
    const auto assembleServer = [&](auto &server)
    {
        Net::HttpServerAssemblyContext assemblyContext;
        assemblyContext.sharedPerIpLimiter    = perIpConnectionLimiter;
        assemblyContext.sharedRateLimitBucket = rateLimitBucket;
        assemblyContext.sharedMemoryBudget    = inflightBodyBudget;
        assemblyContext.workerProcessCount    = workerProcessTotal;
        // 一台进程里跑几条监听器就要乘几：TcpServer 的并发计数是每台一份的，只按进程摊会放行 L 倍
        assemblyContext.listenersPerProcess = actualThreads;
        if (const auto outcome = Net::applyHttpServerConfiguration(*server, configuration, assemblyContext); !outcome)
        {
            // 只可能来自「共享对象与配置标量不一致」这一种自相矛盾的配置。装配发生在起服务之前，
            // 两个构造循环里都没有能把错误带回 main 的通道，因此在此处打印原因并退出（退出码与
            // 其余「配置不成立」的出口一致），而不是静默按其中一份生效
            LOG_ERROR_FMT("服务器装配被拒：{}", outcome.error());
            std::exit(1);
        }
    };

    // --h3 与 TCP 监听在同一个端口号的 UDP 上（见下面 h3 启动那段），所以通告值能直接推出来：
    // 不必让部署方再报一次端口（报错了客户端会一直撞一个没人听的端口），也不需要新开关。
    // 只在 TCP 侧的路由器上挂——已经在 h3 上的请求不需要被告知怎么切到 h3，中间件自己也按协议版本跳过
    const auto advertiseHttp3IfEnabled = [&](Net::Router &router)
    {
        if (useHttp3)
        {
            router.addMiddleware(Net::altSvcMiddleware(port));
        }
    };

    // --trace-context：把链路上下文归一化到请求头上（合法的沿用、缺席或畸形的重起），
    // 业务与下游读的是同一份状态；三端（h1/h2/https 与 h3）各挂一次，见 Net/Http/TraceContext.h
    const auto enableTraceContextIfRequested = [&](Net::Router &router)
    {
        if (useTraceContext)
        {
            Net::TraceContextOptions options;
            // tracestate 里代表本进程的条目，值取本段的 span-id
            options.vendorKey = "asyn";
            router.addMiddleware(Net::traceContextMiddleware(std::move(options)));
        }
        // 链路一节不依赖上面那步（没归一化就按头上原样的上下文起），但两步都在时顺序要紧：
        // 一节要按归一化之后的上下文起，否则上游写错一环就把整条链路的父子关系带歪
        if (tracer != nullptr)
        {
            router.addMiddleware(Net::tracingSpanMiddleware(tracer));
        }
    };

    // 按 --https 决定造哪种协议的服务器；返回基类指针，两条路径共用一套构造逻辑
    const auto buildHttpServer = [&](Core::EventLoop &loop)
    {
        auto server = std::make_unique<Net::HttpServer>(loop, *address);
        setupRoutes(server->router());
        advertiseHttp3IfEnabled(server->router());
        enableTraceContextIfRequested(server->router());
        assembleServer(server);
        // 静态目录要在 start() 之前登记：它往本监听器的路由器上挂兜底路由
        if (!staticDirectory.empty())
        {
            server->staticFileDir(staticDirectory);
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

        // 端点的注册在装配那一步按 expose_metrics 做掉了；这里只把它接到全进程共用的采集端上
        if (configuration.exposeMetrics)
        {
            joinSharedMetricsCollector(server);
        }
        return server;
    };

    const auto buildHttpsServer = [&](Core::EventLoop &loop)
    {
        auto server = std::make_unique<Net::HttpsServer>(loop, *address, certificateFile, keyFile);
        // 各 worker 进程装同一份密钥，客户端被分到哪个进程都解得开票据；不给则每进程一份随机密钥
        if (!ticketKeyFiles.empty())
        {
            server->loadSessionTicketKeys(ticketKeyFiles);
        }
        setupRoutes(server->router());
        advertiseHttp3IfEnabled(server->router());
        enableTraceContextIfRequested(server->router());
        if (compressResponses)
        {
            server->router().addMiddleware(makeCompressionMiddleware(loop));
        }
        assembleServer(server);
        // 静态目录要在 start() 之前登记：它往本监听器的路由器上挂兜底路由
        if (!staticDirectory.empty())
        {
            server->staticFileDir(staticDirectory);
        }

        if (http3RequestIdGenerator == nullptr)
        {
            http3RequestIdGenerator = server->requestIdGenerator();
        }

        // 端点注册同样交给装配那一步；这里只接共用采集端（明文与 TLS 两侧抓哪一侧都是全量）
        if (configuration.exposeMetrics)
        {
            joinSharedMetricsCollector(server);
        }
        return server;
    };

    const auto buildServer = [&](Core::EventLoop &loop) -> std::unique_ptr<Net::TcpServer>
    { return useHttps ? std::unique_ptr<Net::TcpServer>(buildHttpsServer(loop)) : std::unique_ptr<Net::TcpServer>(buildHttpServer(loop)); };

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
            std::unique_ptr<Net::TcpServer> worker    = buildServer(pool.eventLoop(i));
            Net::TcpServer                 *rawWorker = worker.get();
            distributor->addWorker(pool.eventLoop(i), [rawWorker](const int fileDescriptor) { rawWorker->adoptConnection(fileDescriptor); });
            servers.push_back(std::move(worker));
            serverLoopIndexes.push_back(i);
        }

        // 只接受的那一台：自己从不建连接，因此不需要协议侧配置，但要占用同一个监听地址
        std::unique_ptr<Net::TcpServer> acceptor   = buildServer(pool.eventLoop(0));
        Core::Task<>                    acceptTask = acceptor->startAccepting(distributor);
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
            Core::Task<>                    task   = server->start();
            pool.eventLoop(i).scheduler().schedule(task.handle());
            acceptTasks.push_back(std::move(task));
            listeningServers.push_back(server.get());
            servers.push_back(std::move(server));
            serverLoopIndexes.push_back(i);
        }
    }

    LOG_INFO_FMT("{} {}Server instances created, all accept tasks scheduled", actualThreads, useHttps ? "Https" : "Http");
    // 打印的数与下发的数必须同源：这条用的是装配出口同一个 perProcessShare（分母同样是进程数 × 台数），
    // 另写一套就会印出一个看着对而实际没生效的数
    LOG_INFO_FMT("并发限额（每监听器生效值）：整机 {} 摊给 {} 个进程 × 每进程 {} 条监听器 → 每台 {}", capText(configuration.maximumConnections), workerProcessTotal, actualThreads,
                 capText(Net::perProcessShare(configuration.maximumConnections, workerProcessTotal * static_cast<std::size_t>(actualThreads))));

    // 运维端点另起一台只听管理口的服务器：两件事一次解决——① 来源收口（业务口可以开在 0.0.0.0，
    // 管理口默认只听回环）；② 多进程时每个进程一个端口（base + 本进程序号），采集端按进程聚合，
    // 不再出现「一次抓取随机命中某个 worker、计数器在两次抓取之间回落」。
    // 端点与闸门通过 registerOperationEndpoints 挂，与装配出口那条路径同一份实现
    std::unique_ptr<Net::HttpServer> adminServer;
    std::optional<Core::Task<>>      adminListenTask;
    if (configuration.exposeMetrics && configuration.metricsPort != 0)
    {
        const std::uint32_t adminPort = static_cast<std::uint32_t>(configuration.metricsPort) + workerIndex;
        if (adminPort > std::numeric_limits<std::uint16_t>::max())
        {
            // 端口回绕会去听一个谁也没配的号（比如 base=65534、index=3 → 1），Prometheus 抓不到数
            // 还以为是服务的问题，因此宁可直接不起
            LOG_ERROR_FMT("管理口端口算不出合法值：metrics_port={} 加上进程序号 {} 超出 65535，服务未启动", configuration.metricsPort, workerIndex);
            return 1;
        }
        const auto adminAddress = Core::InetAddress::parseLiteral(configuration.metricsAddress, static_cast<std::uint16_t>(adminPort));
        if (!adminAddress.has_value())
        {
            LOG_ERROR_FMT("metrics_address「{}」解析不出来，服务未启动", configuration.metricsAddress);
            return 1;
        }
        adminServer = std::make_unique<Net::HttpServer>(pool.eventLoop(0), *adminAddress);
        // 管理口刻意不调 setHttp2CleartextEnabled()：抓取端是 h1，运维面上多一条协议栈只是多一处入口。
        // 业务口那边开没开 h2c 与这里无关（理由见 HttpServerConfiguration::metricsPort 的说明）
        // 共用那份采集端：管理口要报的是整进程的数，不是它自己那台服务器的零
        joinSharedMetricsCollector(adminServer);
        Net::registerOperationEndpoints(*adminServer, configuration);
        adminListenTask = adminServer->start();
        pool.eventLoop(0).scheduler().schedule(adminListenTask->handle());
        listeningServers.push_back(adminServer.get());
        servers.push_back(std::move(adminServer));
        serverLoopIndexes.push_back(0);
        LOG_INFO_FMT("运维端点单独听在 {}:{}（业务口上不注册这四个端点）", configuration.metricsAddress, adminPort);
    }

    // HTTP/3 与 h1/h2 共存：它走 UDP，与上面的 TCP 端用同一个端口号互不干扰。
    // 只起一台而不做多监听器分发：QuicServer 内部已经按连接标识把报文分派到各自的连接，
    // 再叠一层 SO_REUSEPORT 只会把同一条连接的报文散到互不相识的监听器上
    Net::Router                      http3Router;
    std::unique_ptr<Net::QuicServer> http3Server;
    std::optional<Core::Task<>>      http3ListenTask;
    if (useHttp3)
    {
        setupRoutes(http3Router);
        enableTraceContextIfRequested(http3Router);
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
        http3Configuration.certificateFile       = certificateFile;
        http3Configuration.privateKeyFile        = keyFile;
        http3Configuration.sessionTicketKeyFiles = ticketKeyFiles;
        // 与 h1/h2 用同一份解析上限：h3 的正文总量上限同样不该由样本自己去猜
        http3Configuration.parserLimits = configuration.parserLimits;
        // 在途正文预算与 HTTP 侧共用同一份账：h3 的正文也驻留在进程内存里，
        // 不给它这份账就等于 --max-inflight-body 只管两条 TCP 通道
        http3Configuration.memoryBudget = inflightBodyBudget;
        // 指标打开时 h3 的请求数、状态码类与单流取消并进上面那份采集端；没打开则为空指针、不采集
        http3Configuration.metricsCollector = http3MetricsCollector;
        // request-id 与两条 TCP 通道共用一份生成器：同一台机器上 h3 的 id 前缀不该另起一套
        http3Configuration.requestIdGenerator = http3RequestIdGenerator;
        // 连接级限额同样一份：h3 的「单连接最多多少条请求」与两条 TCP 通道同值
        http3Configuration.serverLimits = std::make_shared<const Net::HttpServerLimits>(configuration.limits);
        // 单来源并发上限也交给 h3：同一来源从 TCP 还是 QUIC 进来都算在同一个名额里
        http3Configuration.perIpConnectionLimiter = perIpConnectionLimiter;
        // 整机并发上限同样要给 h3，否则配置里的 maximum_connections 只管两条 TCP 通道，而 h3 守着
        // QuicServer 自带的默认档：同一份配置下三条通道的口径不一致，本轮新增的 h3 满载读数也会
        // 对着一个没人配过的数跳变。给的是**摊到本进程**的那一份（与 TCP 侧同源），否则 --workers 4
        // 就是四倍放行。0 要原样传下去：QuicServer 与 HTTP 侧一样把 0 定为「不限」（那边的注释与
        // 读口用例都在），而能走到这一行的 0 只可能是操作方显式写的——配置里没写时它是内置的 4096。
        // 此前这里用 `> 0` 挡下 0，等于上面刚印出「每台 不限（显式配 0）」，h3 却仍卡在 1024
        http3Configuration.maximumConnections = perProcessMaximumConnections;

        try
        {
            http3Server = std::make_unique<Net::QuicServer>(pool.eventLoop(0), http3Configuration);
            // 印的就是刚传下去那个数：0 要印成「不限」而不是 0，否则两条日志里一个写 0 一个写
            // 「不限」，运维还是会去翻配置文件确认这台到底卡在哪
            LOG_INFO_FMT("HTTP/3 的并发连接上限 {}（就是上面「每台」那一份，三条通道同一个数）",
                         http3Configuration.maximumConnections == 0 ? std::string("不限（显式配 0）") : std::to_string(http3Configuration.maximumConnections));
            http3Server->setRouter(http3Router);
            // 顺序有讲究：QuicServer::staticFileDir() 要求路由器已经挂上（没挂就抛），因此排在
            // setRouter 之后；与两条 TCP 通道同一份目录，不给 h3 留一条「只能打路由」的偏路
            if (!staticDirectory.empty())
            {
                http3Server->staticFileDir(staticDirectory);
            }
            http3ListenTask.emplace(http3Server->listen(*address));
            pool.eventLoop(0).scheduler().schedule(http3ListenTask->handle());
        } catch (const Base::Exception &http3Exception)
        {
            LOG_ERROR_EXCEPTION(http3Exception, "HTTP/3 服务端起不来，已退出。原因：{}", http3Exception.what());
            return 1;
        }
        LOG_INFO_FMT("HTTP/3 已在同一个端口号的 UDP 上监听（udp/{}）", port);
    }

    // 证书自动化的装配排在所有监听器建好之后：装回动作要遍历的就是这些对象，而「本进程有没有可装回的
    // 对象」本身是启动期就该判的前提（配了 acme 却没有 TLS 口，等于让管理器每月下载一张没人读的证书）。
    // 判据本体在库里（`Net::validateAcmeAssembly`），这里只交现场事实：TLS 口在不在、公网明文口在不在、
    // 是不是多 worker 进程、监听器实际加载哪两条路径
    std::unique_ptr<Net::AcmeCertificateManager> acmeManager;
    // 帧声明在管理器之后：作用域结束时先拆帧再拆管理器，与 `AcmeCertificateManager` 的 @note
    // （「调用方等到帧退出再销毁本对象」）那条寿命关系对得上；池与循环声明得更早，因此拆得更晚，
    // 帧被销毁时循环已经停手，不会有人再去 resume 一条已经拆掉的帧
    std::optional<Core::Task<>> acmeRenewalLoopTask;
    std::optional<Core::Task<>> acmeRotationFollowTask;
    std::atomic<bool>           acmeFollowStopRequested{false};
    std::atomic<int>            acmePendingLoopCount{0};
    if (acmeConfiguration.isEnabled)
    {
        // 签发只归一个进程做：机构的速率限制按账户计而不是按进程计，N 个进程各建一份管理器等于 N 倍
        // 撞同一个额度，而它们签出来的每张都要各自落盘互踩。多 worker 形态下由槽位 0 持管理器，
        // 其余进程改成跟盘；单进程形态（不是 master 拉起来的 worker）自然就是签发方
        const bool             runsCertificateAutomation = !isWorkerProcess || workerIndex == 0;
        Net::AcmeAssemblyFacts facts;
        // 「有几台可装回」按真的能转成 HttpsServer 的那几台数，而不是按 --https/--h3 两个开关猜：
        // 开关说了而对象不在（或类型不对），装回动作就会遍历到一个都不改，而磁盘月月换——
        // 数出来的那几台同时打进启动日志，读日志的人当场就能核对目标数量对不对
        std::size_t tlsServerCount = 0;
        for (const std::unique_ptr<Net::TcpServer> &server: servers)
        {
            if (dynamic_cast<Net::HttpsServer *>(server.get()) != nullptr)
            {
                ++tlsServerCount;
            }
        }
        // --h3 现在必须与 --https 同用，所以这两项看着重复；分开写是因为「h3 自己就是一台 TLS 监听器」
        // 这件事不该靠另一条启动期校验间接成立——那条校验哪天放宽，这里就得跟着变
        facts.hasTlsListener              = tlsServerCount > 0 || http3Server != nullptr;
        facts.hasPublicPlaintextListener  = !useHttps;
        facts.runsMultipleWorkerProcesses = workerProcessCount > 1;
        // 本进程一定拿得到新身份：签发方就地装回，非签发方跑跟盘协程——两条机制在下面按上面那个
        // 布尔量各装其一，所以这条事实是分支结构的读数，不是自我声明
        facts.picksUpCertificateFromDisk = true;
        facts.listenerCertificateFile    = certificateFile;
        facts.listenerPrivateKeyFile     = keyFile;

        const auto assembly = Net::validateAcmeAssembly(acmeConfiguration, facts);
        if (!assembly.has_value())
        {
            LOG_ERROR_FMT("证书自动化装配不下去，服务未启动。原因：{}", assembly.error());
            return 1;
        }

        Core::EventLoop &acmeLoop = pool.eventLoop(0);
        // DNS-01 的动作对只在**签发方**这一档构造：提供方不认识、凭据两条环境变量缺失都在
        // buildDns01TxtWriter 里当场抛，报出来的是「缺哪两条」，而不是几天之后第一次续期失败的那句机构错误。
        // 跟盘的进程什么都不签，因此也不该被要求握着能改域名记录的那对凭据——把凭据发给每一个进程
        // 只会成倍扩大暴露面
        Net::AcmeDns01TxtWriter dns01TxtWriter;
        if (acmeConfiguration.usesDns01() && runsCertificateAutomation)
        {
            try
            {
                dns01TxtWriter = Net::buildDns01TxtWriter(acmeLoop, acmeConfiguration);
            } catch (const Base::Exception &dnsFailure)
            {
                LOG_ERROR_EXCEPTION(dnsFailure, "证书自动化装配不下去，服务未启动。原因：{}", dnsFailure.what());
                return 1;
            }
        }

        // 装回动作遍历本进程的全部 TLS 监听器：分发模式与多线程下每台都有自己的 SSL_CTX，漏一台就是
        // 那一台继续用旧身份。逐台重装再汇总失败，第一处失败不该吞掉后面几台的处置
        const auto reloadIntoListeners = [&servers, http3Server = http3Server.get()]() -> std::expected<void, std::string>
        {
            std::string failureText;
            std::size_t reloadedTlsServerCount = 0;
            std::size_t serverOrdinal          = 0;
            for (const std::unique_ptr<Net::TcpServer> &server: servers)
            {
                ++serverOrdinal;
                auto *tlsServer = dynamic_cast<Net::HttpsServer *>(server.get());
                if (tlsServer == nullptr)
                {
                    // 明文业务口与只听回环的运维口都不终止 TLS，跳过是常态而不是异常
                    continue;
                }
                if (tlsServer->reloadCertificate())
                {
                    ++reloadedTlsServerCount;
                } else
                {
                    failureText += failureText.empty() ? "" : "；";
                    failureText += std::format("第 {} 台 HTTPS 监听器重装失败", serverOrdinal);
                }
            }
            if (http3Server != nullptr && !http3Server->reloadCertificate())
            {
                failureText += failureText.empty() ? "" : "；";
                failureText += "HTTP/3 服务端重装失败";
            }
            if (!failureText.empty())
            {
                return std::unexpected(std::format("磁盘上已经是新证书，但 {}（已装回的 {} 台不受影响）——线上身份此刻是混的，"
                                                   "请按证书自动化失败处置",
                                                   failureText, reloadedTlsServerCount));
            }
            LOG_INFO_FMT("证书自动化：新证书已装回 {} 台 HTTPS 监听器{}{}，新连接立即改用", reloadedTlsServerCount, http3Server != nullptr ? "与 HTTP/3 服务端" : "",
                         "（在手的连接沿用旧上下文，握完手才换）");
            return {};
        };

        // 域名列表逐条打出来：多域名是本层的支持面，日志里只写第一条会让人以为其余几条没配上
        std::string domainText;
        for (const std::string &domainName: acmeConfiguration.manager.domainNames)
        {
            domainText += domainText.empty() ? "" : ",";
            domainText += domainName;
        }

        if (runsCertificateAutomation)
        {
            acmeManager = std::make_unique<Net::AcmeCertificateManager>(acmeLoop, acmeConfiguration.manager, reloadIntoListeners, std::move(dns01TxtWriter));
            ++acmePendingLoopCount;
            acmeRenewalLoopTask.emplace(runAcmeResidentLoopTask(acmeManager->runRenewalLoop(), acmePendingLoopCount));
            acmeLoop.scheduler().schedule(acmeRenewalLoopTask->handle());
            LOG_INFO_FMT(
                    "证书自动化：开（机构 {}，域名 {}，落点 {}，通道 {}，每 {} 分钟查一次到期、到期前 {} 天内就该续；装回目标 {} 台 HTTPS 监听器 + HTTP/3 {}）",
                    acmeConfiguration.manager.directoryUrl, domainText, acmeConfiguration.manager.certificateFile.string(), acmeConfiguration.usesDns01() ? "dns-01" : "http-01",
                    std::chrono::duration_cast<std::chrono::minutes>(acmeConfiguration.manager.renewalCheckInterval).count(),
                    std::chrono::duration_cast<std::chrono::days>(acmeConfiguration.manager.renewBeforeExpiry).count(), tlsServerCount, http3Server != nullptr ? "在" : "不在");
        } else
        {
            // 非签发进程：磁盘上那张文件一换就重装本进程的 TLS 监听器。签发方是**先写私钥再写证书链**、
            // 两张各一次原子替换，所以中间必然有一瞬间是「新私钥 + 旧证书」——那一次装回会被配对校验挡下，
            // 而跟随协程刻意不把没装上的变化认下，下一拍再试就撞上两张都到位的那一刻
            ++acmePendingLoopCount;
            acmeRotationFollowTask.emplace(runAcmeResidentLoopTask(
                    Net::followCertificateRotation(acmeLoop, certificateFile, keyFile, kCertificateRotationPollInterval, reloadIntoListeners, acmeFollowStopRequested),
                    acmePendingLoopCount));
            acmeLoop.scheduler().schedule(acmeRotationFollowTask->handle());
            LOG_INFO_FMT("证书自动化：本进程不签发（签发归槽位 0 那个进程），改为每 {} 秒跟一次盘；装回目标 {} 台 HTTPS 监听器 + HTTP/3 {}",
                         std::chrono::duration_cast<std::chrono::seconds>(kCertificateRotationPollInterval).count(), tlsServerCount, http3Server != nullptr ? "在" : "不在");
        }
    }

    pool.start();

    // 报成功之前先确认监听器真的进入了监听态：绑定失败发生在分离投递的协程里（异常由 Task 记进错误日志，
    // 见 Core/Coroutine/Task.h 的无人接手上报），这里再不核一次的话，端口上其实没人守着，
    // 进程却照旧一副在服务的样子、退出码还是 0
    const std::size_t stuckListenerCount = waitForListenersToComeUp(listeningServers, http3Server.get(), kStartupConfirmTimeout);
    if (stuckListenerCount > 0)
    {
        LOG_ERROR_FMT("{} 个监听器在 {}ms 内没有进入监听状态，服务未运行。监听地址：{}（协程里抛出的原因见上面的错误日志）", stuckListenerCount, kStartupConfirmTimeout.count(),
                      address->toString());
        // 关停走既有那条路径：真的起来的那几台照样体面收口，不另起一套收尾
        g_running.store(false);
    } else
    {
        LOG_INFO("" + std::string(proto) + " server started  " + proto + "://" + address->toString());
        LOG_INFO("Worker threads: " + std::to_string(actualThreads) + " (logical cores: " + std::to_string(std::thread::hardware_concurrency()) + ")");
        LOG_INFO("Endpoints: GET /  |  GET /json  |  GET /bench  |  GET /big");
        LOG_INFO("Press Ctrl+C to exit");

        // 只有真正确认过监听器在听了才报 READY=1：报早了，监督者会把「端口上其实没人」这一形态
        // 读成启动成功，而这条通道存在的意义恰恰是别让部署方看到第二种样子
        if (!serviceStatus.open())
        {
            // 「没有 NOTIFY_SOCKET」是按约定的正常形态，不是缺陷，所以这一句按信息级出：
            // 不在监督下跑（开发机、容器、sysvinit）时它就该这么响
            LOG_INFO_FMT("未向服务管理器上报状态：{}（这不是错误——不在监督下跑时没有可通知的对象）", serviceStatus.lastError());
        } else
        {
            const auto listenerCount = listeningServers.size();
            if (!serviceStatus.send(Platform::ServiceNotification::kReadyState))
            {
                LOG_WARN_FMT("READY=1 没有送达服务管理器：{}（那一侧会按启动超时处理本进程）", serviceStatus.lastError());
            }
            static_cast<void>(serviceStatus.send(Platform::ServiceNotification::statusState(
                    std::format("{} 个监听器已就绪，{} 线程，pid {}", listenerCount, actualThreads, Platform::ProcessInfo::currentProcessId()))));

            // 就绪报过之后才挂看门狗：监督者从 READY=1 起才开始计这条超时，先报就绪再喂才不错位。
            // 挂不上就报一句原因——没配窗口时那本来就是空操作，而「配了窗口却没开通路」这种半套
            // 环境要看得见，否则运维看到的服务被无理由重启就是最难归因的那种现场
            if (!serviceWatchdog.arm())
            {
                LOG_INFO_FMT("未挂服务管理器的看门狗：{}（这不是错误——没配 WatchdogSec= 时本就没有要喂的超时）", serviceWatchdog.lastError());
            }

            // 停机那条注册在确认之后：监督者从 STOPPING=1 起开始计停机超时，越早上报，
            // 正在排空的请求被硬切的机会越小。发送失败在这一刻已经没有通道可报告（日志器可能
            // 已经收口），留在监督者那一侧的超时表现为「收尾超时后被 SIGKILL」，看得见
            shutdown.onShutdown(
                    [&serviceStatus, listenerCount]
                    {
                        static_cast<void>(serviceStatus.send(Platform::ServiceNotification::kStoppingState));
                        static_cast<void>(serviceStatus.send(Platform::ServiceNotification::statusState(std::format("正在收尾，{} 个监听器", listenerCount))));
                    });
        }

        // 指标端点是进程内的口径：多 worker 进程共用同一个监听端口时，一次抓取只命中其中一个进程，
        // 计数器会在两次抓取之间回落（同一进程内的多个监听器已在装配时共用一份采集端，跨进程没有
        // 这条通道）。不写出来的话，运维看到的就是「流量忽大忽小」而不是「这里少了一份进程」。
        // 配了 metrics_port 就不提醒：每台各听一个端口（base + 槽位号），抓取本来就是按进程来的
        if (configuration.exposeMetrics && workerProcessCount > 1 && configuration.metricsPort == 0)
        {
            LOG_WARN_FMT("--metrics 与 --workers {} 一起用：每条抓取只命中一个 worker 进程的口径，"
                         "计数器会在两次抓取之间变小；要按进程聚合请配 server.metrics_port（每个进程各听 base+序号 一个口），或按单进程跑",
                         workerProcessCount);
        }
    }

    // 等待退出信号
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Received shutdown signal, stopping server...");

    // 证书自动化排在停服务器之前叫停：续期帧归循环 0，而那条循环要到下面停运行时才停，先叫停再等它
    // 真的退出，帧就不会带着一个已被销毁的管理器被丢下（这正是「收口丢弃挂起协程」那类缺陷的形状）。
    // 等待是有界的——停放被切成 1 秒一片；正在进行的那一轮签发刻意不打断，等满上限就打一行 WARN 继续收尾
    if (acmePendingLoopCount.load(std::memory_order_acquire) > 0)
    {
        if (acmeManager != nullptr)
        {
            acmeManager->stopRenewalLoop();
        }
        acmeFollowStopRequested.store(true, std::memory_order_release);
        for (int waitRoundCount = 0; waitRoundCount < kAcmeRenewalLoopWaitRoundLimit && acmePendingLoopCount.load(std::memory_order_acquire) > 0; ++waitRoundCount)
        {
            std::this_thread::sleep_for(kAcmeRenewalLoopWaitSlice);
        }
        if (acmePendingLoopCount.load(std::memory_order_acquire) > 0)
        {
            LOG_WARN_FMT("证书自动化：{} 条常驻协程在 {}ms 内没有收口（正在进行的那一轮签发不打断，半途掐掉会留下半个订单状态），"
                         "仍按既有顺序停服务；停机期间不会再有装回动作",
                         acmePendingLoopCount.load(std::memory_order_acquire),
                         std::chrono::duration_cast<std::chrono::milliseconds>(kAcmeRenewalLoopWaitSlice * kAcmeRenewalLoopWaitRoundLimit).count());
        }
    }

    // 关停分三步：停止接受新连接 → 等在途请求做完（超时兜底强关）→ 停运行时。
    // 排空预算 shutdownDrainTimeout 在启动那一处算好并打了出来（理由与下限都写在那里）
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
        Core::Task<> drainTask = drainServerTask(*servers[index], shutdownDrainTimeout, remainingDrainCount);
        pool.eventLoop(serverLoopIndexes[index]).scheduler().scheduleRemote(drainTask.handle());
        shutdownTasks.push_back(std::move(drainTask));
    }

    // HTTP/3 与 TCP 两条路必须**同一时刻开始**排空：`QuicServer::drain()` 的第一步就是挡新连接并给每条
    // 连接发 GOAWAY，若排在 TCP 那边等完之后才轮到它，最长 shutdownDrainTimeout 这段时间里 h3 还在接
    // 新连接，而业务口与管理口上的 /readyz 早已回 503——就绪那一格说的是整台进程，不是某一条通路。
    // 它照旧投回自己那条循环（drain 要遍历连接表），到期由 drain 自己兜底强关
    std::atomic<std::size_t>    remainingHttp3DrainCount{http3Server != nullptr ? 1U : 0U};
    std::optional<Core::Task<>> http3DrainTask;
    if (http3Server != nullptr)
    {
        http3DrainTask = drainHttp3ServerTask(*http3Server, shutdownDrainTimeout, remainingHttp3DrainCount);
        pool.eventLoop(0).scheduler().scheduleRemote(http3DrainTask->handle());
    }

    LOG_INFO_FMT("Draining {} server instance(s){}, in-flight requests get up to {}ms...", servers.size(), http3Server != nullptr ? " + HTTP/3" : "", shutdownDrainTimeout.count());
    while (remainingDrainCount.load(std::memory_order_acquire) > 0 || remainingHttp3DrainCount.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 与上面两条 TCP 路径一样：帧收进 shutdownTasks，别在它还被循环持有时就先离开作用域
    if (http3DrainTask.has_value())
    {
        shutdownTasks.push_back(std::move(*http3DrainTask));
    }

    context.stop();

    LOG_INFO("Server stopped successfully");
    // 启动没确认成功时不能报 0：编排脚本与进程管理器都靠退出码判断这次启动算不算成了
    return stuckListenerCount == 0 ? 0 : 1;
}
