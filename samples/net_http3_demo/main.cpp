// Net HTTP/3 与 QUIC 服务端自检：配置校验、端口 0 起监听、报文面容错、定时驱动、统计与优雅收口
//
// 框架没有 QUIC/HTTP/3 客户端（src/Net/Quic 只有服务端一侧），因此本程序只能从「外面」
// 驱动服务端不依赖握手的那半张脸：起停、绑定、乱码与畸形长头的容错、定时驱动、统计与排空。
// 握手之后的路径（真实 h3 请求）由单测与 aioquic 真机脚本覆盖，这里刻意不假装能验。
//
// 结论一律攒进观测结构，断言与日志都留在主线程做：事件循环线程上不该去做同步落盘式的日志。
#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if ASYN_PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace AsynGyanis;

namespace
{
    /// 自检攒下来的观测：全部在循环线程上写，主线程等标记置起后再读
    struct Observations
    {
        bool        rejectsMissingCertificate{false};   ///< 证书文件不存在时 listen() 是否当场抛出
        bool        certificateRejectionIsActionable{false}; ///< 抛出的中文说明里是否点名了证书
        bool        rejectsMissingPrivateKey{false};    ///< 私钥不存在时同样被拒
        bool        bindsEphemeralPort{false};          ///< 端口 0 起监听后能读回内核分配的实际端口
        bool        honoursRequestedPort{false};        ///< 指定端口时读回的就是那个端口
        bool        survivesGarbageDatagrams{false};    ///< 乱码数据报不建连接、不崩、端口还在
        bool        survivesMalformedInitial{false};    ///< 形状像 Initial 的畸形长头同样被挡下
        bool        reportsNoConnection{false};         ///< 上述期间在线连接数一直是 0
        bool        statsReadableWithoutCollector{false};///< 没配采集端时 stats() 仍可读且为零
        bool        survivesRouterAndHandlerWiring{false};///< 接上路由器与直通回调后仍照常容错
        bool        expiryTickerKeepsServiceAlive{false};///< 空闲超时与节拍定时在没有连接时也能跑
        bool        drainReturnsImmediatelyWhenIdle{false};///< 无在途连接时 drain() 立刻返回
        std::uint16_t ephemeralPort{0};                 ///< 端口 0 实际拿到的端口号
    };

    Observations g_observations;

    /// 整轮检查是否跑完
    std::atomic<bool> g_isRunFinished{false};

    /**
     * @brief 从示例映像往上找仓库里的测试证书夹具
     * @param executablePath argv[0]
     * @param fileName 夹具文件名
     * @return std::filesystem::path 找到的路径；找不到时为空
     */
    std::filesystem::path locateFixture(const char *executablePath, const std::string_view fileName)
    {
        std::error_code error;
        auto            cursor = std::filesystem::weakly_canonical(std::filesystem::absolute(executablePath), error);
        if (error)
        {
            return {};
        }
        for (int level = 0; level < 8 && !cursor.empty(); ++level)
        {
            cursor = cursor.parent_path();
            std::error_code checkError;
            const std::filesystem::path candidate = cursor / "tests/Core/fixtures" / std::string{fileName};
            if (std::filesystem::exists(candidate, checkError))
            {
                return candidate;
            }
        }
        return {};
    }

    /**
     * @brief 本机回环的裸地址结构
     * @param port 端口（主机序）
     * @return Platform::SocketAddress 可直接交给协程收发的地址
     */
    Platform::SocketAddress loopbackSocketAddress(const std::uint16_t port)
    {
        Platform::SocketAddress address{};
        auto &                  ipv4 = reinterpret_cast<sockaddr_in &>(address.storage);
        ipv4.sin_family = AF_INET;
        ipv4.sin_port   = htons(port);
#if ASYN_PLATFORM_WIN32
        ipv4.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
#else
        ipv4.sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
        address.length = sizeof(sockaddr_in);
        return address;
    }

    /**
     * @brief 让协程交出控制权，等满时长后回来
     * @param loop 当前循环
     * @param duration 让出多久
     * @return Core::Task<> 到点即返回
     */
    Core::Task<> yieldFor(Core::EventLoop &loop, const std::chrono::milliseconds duration)
    {
        Core::Timer ticker(loop);
        co_await ticker.waitFor(duration);
        co_return;
    }

    /**
     * @brief 起一台服务端并把 listen 协程排进循环
     * @tparam ServerHolder 持有 QuicServer 的智能指针类型
     * @param loop 承载服务的循环
     * @param configuration 服务端配置
     * @param localAddress 本地地址（端口 0 表示由内核分配）
     * @param listenTasks 输出：协程帧要活到跑完，由调用方持有
     * @return std::unique_ptr<Net::QuicServer> 已提交 listen 的服务端
     */
    template <typename TaskList>
    std::unique_ptr<Net::QuicServer> startServer(Core::EventLoop &loop, const Net::QuicServer::Configuration &configuration,
                                                 const Core::InetAddress &localAddress, TaskList &listenTasks)
    {
        auto server = std::make_unique<Net::QuicServer>(loop, configuration);
        listenTasks.push_back(server->listen(localAddress));
        loop.scheduler().schedule(listenTasks.back().handle());
        return server;
    }

    /**
     * @brief 等到某台服务端把端口亮出来
     * @param loop 当前循环（本函数让出控制权）
     * @param server 目标服务端
     * @return std::uint16_t 实际端口；超时未起来时为 0
     */
    Core::Task<std::uint16_t> waitForListeningPort(Core::EventLoop &loop, const Net::QuicServer &server)
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (const std::uint16_t port = server.listeningPort(); port != 0)
            {
                co_return port;
            }
            co_await yieldFor(loop, std::chrono::milliseconds{20});
        }
        co_return 0;
    }

    /**
     * @brief 造一条形状像 QUIC 长头 Initial、但内容无法解开的报文
     * @return std::vector<std::uint8_t> 报文字节
     */
    std::vector<std::uint8_t> makeMalformedInitialPacket()
    {
        std::vector<std::uint8_t> packet{0xC3, 0x00, 0x00, 0x00, 0x01, 0x08};
        for (int index = 0; index < 8; ++index)
        {
            packet.push_back(static_cast<std::uint8_t>(0xA0 + index));
        }
        packet.push_back(0x08);
        for (int index = 0; index < 8; ++index)
        {
            packet.push_back(static_cast<std::uint8_t>(0xB0 + index));
        }
        // 长头的剩余长度与 packet number 之后什么都没有：解密与状态机都该把它挡在外面
        packet.push_back(0x40);
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x01);
        packet.push_back(0x00);
        packet.push_back(0x00);
        return packet;
    }

    /**
     * @brief 从另一个 UDP 端口向目标端口连发数据报
     * @param loop 当前循环
     * @param targetPort 目标端口
     * @param payloads 要发的报文
     * @return Core::Task<std::size_t> 实际发出的条数
     */
    Core::Task<std::size_t> sendDatagrams(Core::EventLoop &loop, const std::uint16_t targetPort, const std::vector<std::vector<std::uint8_t>> &payloads)
    {
        auto platformSocket = Platform::DatagramSocket::bindTo(loopbackSocketAddress(0));
        if (!platformSocket.isValid())
        {
            co_return 0;
        }
        Core::AsyncUdpSocket socket(loop, std::move(platformSocket));
        const auto           target = loopbackSocketAddress(targetPort);

        std::size_t sentCount = 0;
        for (const std::vector<std::uint8_t> &payload: payloads)
        {
            if (co_await socket.asyncSendTo(target, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()))
            {
                ++sentCount;
            }
        }
        co_return sentCount;
    }

    /// 造一批内容各异的乱码数据报
    std::vector<std::vector<std::uint8_t>> makeGarbagePayloads(const std::size_t count)
    {
        std::vector<std::vector<std::uint8_t>> payloads;
        for (std::size_t index = 0; index < count; ++index)
        {
            std::vector<std::uint8_t> payload;
            for (std::size_t byte = 0; byte < 40; ++byte)
            {
                payload.push_back(static_cast<std::uint8_t>((index * 37 + byte * 11) & 0xFF));
            }
            payloads.push_back(std::move(payload));
        }
        return payloads;
    }

    /// 一份能跑起来的基线配置（证书与私钥由调用方给）
    Net::QuicServer::Configuration makeConfiguration(const std::string &certificateFile, const std::string &keyFile)
    {
        Net::QuicServer::Configuration configuration;
        configuration.certificateFile  = certificateFile;
        configuration.privateKeyFile   = keyFile;
        configuration.idleTimeout      = std::chrono::seconds{1};
        configuration.maximumConnections = 8;
        configuration.parserLimits     = Net::HttpParserLimits{};
        return configuration;
    }

    /**
     * @brief 停掉一台服务端并等它的 listen 协程真的退出
     * @details `stop()` 只置标记、不碰套接字（那是有意为之：它可能从别的线程调用），因此静默端口上
     *          的收包 await 不会自己醒来——不先把 listen 协程叫醒就销毁服务对象，退出路径上的
     *          `stop()` 就会踩到已释放的对象（ASan 实测 heap-use-after-free）。这里补一条数据报叫醒它。
     * @param loop 当前循环
     * @param server 目标服务端
     * @param port 它正在听的端口
     * @return Core::Task<> listen 协程已退出
     */
    Core::Task<> wakeAndStop(Core::EventLoop &loop, Net::QuicServer &server, const std::uint16_t port)
    {
        server.stop();
        static_cast<void>(co_await sendDatagrams(loop, port, makeGarbagePayloads(1)));
        co_await yieldFor(loop, std::chrono::milliseconds{300});
        co_return;
    }

    /**
     * @brief 跑一遍服务端自检，结论写进观测结构
     * @param loop 服务与探针共用的循环（探针全是要等待的协程，不阻塞这条循环）
     * @param certificateFile 证书夹具路径。**按值收**：listen() 这类惰性协程不能引用调用方的临时量
     * @param keyFile 私钥夹具路径，理由同上
     * @param fixedPort 指定端口（验「按请求的端口绑」）
     * @return Core::Task<> 跑完即返回
     */
    Core::Task<> runChecks(Core::EventLoop &loop, const std::string certificateFile, const std::string keyFile, const std::uint16_t fixedPort)
    {
        std::vector<Core::Task<>> listenTasks;

        // —— 1. 配置校验：证书与私钥的路径不对就该被当场拒掉 ——
        // TLS 上下文在构造期就建好，构造本身也可能抛：两者收在同一个 try 里，别让异常逃出协程
        {
            Net::QuicServer::Configuration badCertificate = makeConfiguration("does-not-exist.pem", keyFile);
            const auto                     address        = Core::InetAddress::resolve("127.0.0.1", 0);
            try
            {
                Net::QuicServer server(loop, badCertificate);
                co_await server.listen(*address);
            } catch (const Base::Exception &failure)
            {
                g_observations.rejectsMissingCertificate          = true;
                const std::string_view message                    = failure.what();
                g_observations.certificateRejectionIsActionable   = message.find("证书") != std::string_view::npos;
                LOG_INFO_FMT("证书被拒的说明：{}", failure.what());
            } catch (...)
            {
                g_observations.rejectsMissingCertificate = false;
            }
        }
        {
            Net::QuicServer::Configuration badKey     = makeConfiguration(certificateFile, "does-not-exist-key.pem");
            const auto                     address    = Core::InetAddress::resolve("127.0.0.1", 0);
            try
            {
                Net::QuicServer server(loop, badKey);
                co_await server.listen(*address);
            } catch (const Base::Exception &)
            {
                g_observations.rejectsMissingPrivateKey = true;
            } catch (...)
            {
                g_observations.rejectsMissingPrivateKey = false;
            }
        }

        // —— 2. 端口 0 起监听：内核分配的端口要能读回来 ——
        const auto ephemeralAddress = Core::InetAddress::resolve("127.0.0.1", 0);
        auto       server           = startServer(loop, makeConfiguration(certificateFile, keyFile), *ephemeralAddress, listenTasks);
        const std::uint16_t ephemeralPort = co_await waitForListeningPort(loop, *server);
        g_observations.bindsEphemeralPort = ephemeralPort != 0;
        g_observations.ephemeralPort      = ephemeralPort;
        if (ephemeralPort == 0)
        {
            g_isRunFinished.store(true, std::memory_order_release);
            co_return;
        }

        // 没配采集端时 stats() 仍可读，且除在线连接数外各计数为零
        const Net::HttpServerStats bareStatistics = server->stats();
        g_observations.statsReadableWithoutCollector =
                bareStatistics.totalRequestCount == 0 && bareStatistics.activeConnectionCount == 0 && server->connectionCount() == 0;

        // —— 3. 报文面容错：乱码与畸形长头都不该建连接、更不该让服务倒下 ——
        const std::size_t garbageSent = co_await sendDatagrams(loop, ephemeralPort, makeGarbagePayloads(30));
        co_await yieldFor(loop, std::chrono::milliseconds{200});
        g_observations.survivesGarbageDatagrams = garbageSent > 0 && server->listeningPort() == ephemeralPort && server->connectionCount() == 0;
        g_observations.reportsNoConnection      = server->connectionCount() == 0;

        std::vector<std::vector<std::uint8_t>> initialPackets;
        for (int repeat = 0; repeat < 5; ++repeat)
        {
            initialPackets.push_back(makeMalformedInitialPacket());
        }
        const std::size_t initialSent = co_await sendDatagrams(loop, ephemeralPort, initialPackets);
        co_await yieldFor(loop, std::chrono::milliseconds{200});
        g_observations.survivesMalformedInitial =
                initialSent == initialPackets.size() && server->listeningPort() == ephemeralPort && server->connectionCount() == 0;

        // —— 4. 定时驱动：idleTimeout 1 秒、节拍 10 毫秒，在没有任何连接时也要稳定跑 ——
        co_await yieldFor(loop, std::chrono::milliseconds{1400});
        g_observations.expiryTickerKeepsServiceAlive = server->listeningPort() == ephemeralPort && server->connectionCount() == 0;

        // —— 5. 接上路由器与直通出口后仍然照常容错（这两条是 h3 与「纯传输层」两种形态的入口）——
        Net::Router router;
        router.get("/hello", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setBody("hello-over-h3");
            co_return;
        });
        server->setRouter(router);
        server->setStreamDataHandler([](Net::QuicConnection &, const std::int64_t, const std::span<const std::uint8_t>, const bool) noexcept
        {
        });
        static_cast<void>(co_await sendDatagrams(loop, ephemeralPort, makeGarbagePayloads(10)));
        co_await yieldFor(loop, std::chrono::milliseconds{150});
        g_observations.survivesRouterAndHandlerWiring = server->listeningPort() == ephemeralPort && server->connectionCount() == 0;

        // —— 6. 排空与停止：没有连接时 drain() 不该白等，stop() 之后端口要能复用 ——
        const auto drainStartedAt = std::chrono::steady_clock::now();
        co_await server->drain(std::chrono::milliseconds{2000});
        const auto drainElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drainStartedAt);
        g_observations.drainReturnsImmediatelyWhenIdle = drainElapsed < std::chrono::milliseconds{1500} && server->connectionCount() == 0;

        co_await wakeAndStop(loop, *server, ephemeralPort);
        server.reset();
        // 「收循环真的退出了」这里量不出来，原来的「重新绑定同一 UDP 端口」探针去掉：
        // Platform::DatagramSocket::bindTo 无条件设 SO_REUSEADDR，而 Windows 与 Linux 都允许
        // 另一个套接字绑上仍被占用的 UDP 端口——探针在任何情况下都会成功，失败也没有可报的方向。
        // 循环退出由「整轮检查在时限内跑完」那一步与 wakeAndStop 自身的等待上限钉住；
        // 要真测端口释放，得绕开 bindTo 用不带 SO_REUSEADDR 的裸套接字，那是 Platform 层的活

        // —— 7. 指定端口起监听：读回来的就是那个端口（换一个端口重起，顺带验可重复起停）——
        const auto fixedAddress = Core::InetAddress::resolve("127.0.0.1", fixedPort);
        auto       fixedServer  = startServer(loop, makeConfiguration(certificateFile, keyFile), *fixedAddress, listenTasks);
        const std::uint16_t boundFixedPort = co_await waitForListeningPort(loop, *fixedServer);
        g_observations.honoursRequestedPort = boundFixedPort == fixedPort;
        co_await wakeAndStop(loop, *fixedServer, fixedPort);
        fixedServer.reset();

        g_isRunFinished.store(true, std::memory_order_release);
        co_return;
    }
} // namespace

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Net HTTP/3 与 QUIC 服务端示例开始 ===");

    auto &samples = Samples::checklist();

    const std::filesystem::path certificateFixture = locateFixture(argv[0], "test_ip_cert.pem");
    const std::filesystem::path keyFixture         = locateFixture(argv[0], "test_ip_key.pem");
    samples.check(!certificateFixture.empty() && !keyFixture.empty(), "找得到仓库里的自签证书夹具（示例不自己造证书）");
    if (certificateFixture.empty() || keyFixture.empty())
    {
        return Samples::finishSample("net_http3_demo");
    }

    // 按请求的端口绑那台服务器单独占基准端口往后第 3 个；余量在起跑前判掉，不要让端口绕回 0
    const std::uint16_t basePort = Samples::readPortArgument(argc, argv, 30);
    Samples::requirePortHeadroom(basePort, 3);
    const std::uint16_t fixedPort = static_cast<std::uint16_t>(basePort + 3);

    Core::IoContext context(1);
    auto &          pool = context.threadPool();
    Core::EventLoop &loop = pool.eventLoop(0);

    Core::Task<> runTask = runChecks(loop, certificateFixture.string(), keyFixture.string(), fixedPort);
    loop.scheduler().schedule(runTask.handle());
    pool.start();

    const bool isRunDone = Samples::waitUntil([]
                                              {
                                                  return g_isRunFinished.load(std::memory_order_acquire);
                                              },
                                              std::chrono::seconds{60}, std::chrono::milliseconds{20});
    LOG_INFO_FMT("端口 0 起监听实际拿到 udp/{}", g_observations.ephemeralPort);

    samples.check(isRunDone, "整轮检查在时限内跑完（listen 的负向用例没有挂住）");
    samples.check(g_observations.rejectsMissingCertificate, "证书路径不对时 listen() 当场抛出，而不是起一个永远握不上手的监听器");
    samples.check(g_observations.certificateRejectionIsActionable, "证书被拒时的中文说明点名的就是证书这件事");
    samples.check(g_observations.rejectsMissingPrivateKey, "私钥路径不对同样在启动期被拒");
    samples.check(g_observations.bindsEphemeralPort, "端口 0 起监听后能读回内核分配的实际端口（HTTP 侧没有这个口子）");
    samples.check(g_observations.honoursRequestedPort, "指定端口起监听时读回的就是那个端口，且服务可重复起停");
    samples.check(g_observations.survivesGarbageDatagrams, "30 条乱码数据报既没建出连接也没让服务倒下");
    samples.check(g_observations.survivesMalformedInitial, "形状像 Initial 的畸形长头被挡在连接表之外");
    samples.check(g_observations.reportsNoConnection, "上述期间在线连接数一直是 0（没有半截连接漏在表里）");
    samples.check(g_observations.statsReadableWithoutCollector, "没配采集端时 stats() 仍可读，且各计数为零");
    samples.check(g_observations.survivesRouterAndHandlerWiring, "接上路由器与流数据直通出口后仍照常容错");
    samples.check(g_observations.expiryTickerKeepsServiceAlive, "空闲超时与节拍定时在没有连接时也能稳定跑过一整个超时窗口");
    samples.check(g_observations.drainReturnsImmediatelyWhenIdle, "无在途连接时 drain() 立即返回，不白等期限");

    context.stop();
    return Samples::finishSample("net_http3_demo");
}
