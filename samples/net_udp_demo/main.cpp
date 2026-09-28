// Net UDP 服务端自检：两种起步方式（按地址 bind / 接手别人绑好的口）、逐条交付、零长报文、
// 主动下发、统计计数、处理器抛异常后的存活，以及两种启动顺序不许混用。
//
// 为什么要有这份示例：UdpServer 是 Net 里最新的公开服务面，而 `scripts/run_samples.py` 把每个示例都当
// 一道 LSan 门禁跑（示例在容器里带 ASan/LSan/UBSan 跑完，泄漏与非定义行为会自己报出来）。单测钉的是
// 判据，示例钉的是「照库外使用者那样写一遍能不能跑通」。
//
// 接手那一格在本程序里就地造出来：bind 一份数据报套接字 → 沿一条交接通道把它**交给本进程** →
// `DatagramSocket::adopt` 包成本层持有者 → 交给 UdpServer 的接手构造。与跨进程那一格的唯一差别是通道
// 另一头在别的进程号上，而那条链路另有两进程端到端用例守着。
//
// 结论一律攒进观测结构，断言与日志留在主线程：事件循环线程上不该做同步落盘式的日志。
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Udp/UdpServer.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
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
        bool          bindsEphemeralPort{false};         ///< 按地址起监听时端口 0 能问回内核分配的实际端口
        bool          echoesPayloadByteForByte{false};   ///< 回显的字节与送来的一字不差，且按来源回包
        bool          deliversZeroLengthDatagram{false}; ///< 零长报文照交付（不当成「没收到」）
        bool          pushesUnsolicitedDatagram{false};  ///< sendTo 的主动下发能落到对端
        bool          countsInStats{false};              ///< received/sent/failed 三笔账与实际操作对得上
        bool          survivesThrowingHandler{false};    ///< 处理器抛异常后被接住，服务照常接下一条
        bool          rejectsZeroBufferCapacity{false};  ///< 缓冲容量为 0 在构造期就被拒
        bool          adoptsHandedOverSocket{false};     ///< 接手来的套接字上端口照旧、报文有回话
        bool          adoptedPortFollowsSocket{false};   ///< 接手模式下端口来自交来的那份套接字
        bool          rejectsMixingStartupModes{false};  ///< 接手的调带地址 listen() 被当场拒
        bool          rejectsAddressModeWithoutAddress{false}; ///< 按地址构造的调无参 listen() 同样被拒
        bool          stopReleasesServeLoop{false};      ///< stop() 之后收循环退出、端口能再被起用
        std::uint16_t ephemeralPort{0};                  ///< 端口 0 实际拿到的端口号
        std::uint16_t adoptedPort{0};                    ///< 接手档里那份套接字原本绑着的端口
    };

    Observations g_observations;

    /// 整轮检查是否跑完
    std::atomic<bool> g_isRunFinished{false};

    /// 处理器留下的观测：来源端口与最后一次的正文长度
    std::atomic<std::uint16_t> g_seenSourcePort{0};
    std::atomic<std::size_t>   g_seenPayloadLength{0};

    /**
     * @brief 本机回环的裸地址结构
     * @param port 端口（主机序）
     * @return Platform::SocketAddress 可直接交给套接字收发的地址
     */
    Platform::SocketAddress loopbackSocketAddress(const std::uint16_t port)
    {
        Platform::SocketAddress address{};
        auto                   &ipv4 = reinterpret_cast<sockaddr_in &>(address.storage);
        ipv4.sin_family              = AF_INET;
        ipv4.sin_port                = htons(port);
#if ASYN_PLATFORM_WIN32
        ipv4.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
#else
        ipv4.sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
        address.length = sizeof(sockaddr_in);
        return address;
    }

    /**
     * @brief 按 sockaddr 里的端口换算成本端地址对象（回包与 listen 入参都用它）
     * @param address 平台地址
     * @return Core::InetAddress 同一份地址
     */
    Core::InetAddress addressOf(const Platform::SocketAddress &address)
    {
        return Core::InetAddress(address.storage, address.length);
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
     * @brief 等端口发布出来：非 0 的 listeningPort() 是「已在监听」的唯一凭据
     * @param loop 当前循环
     * @param server 被观察的服务端
     * @return Core::Task<std::uint16_t> 等到的端口号；超时交回 0
     */
    Core::Task<std::uint16_t> waitForPort(Core::EventLoop &loop, const Net::UdpServer &server)
    {
        for (int roundCount = 0; roundCount < 400; ++roundCount)
        {
            // 只在循环线程上读成员：本协程与收循环同属这个循环，不存在跨线程访问
            if (const std::uint16_t port = server.listeningPort(); port != 0U)
            {
                co_return port;
            }
            co_await yieldFor(loop, std::chrono::milliseconds{5});
        }
        co_return 0U;
    }

    /**
     * @brief 从对端套接字上收一条报文：非阻塞读数 + 协程让出，不占死循环线程
     * @param loop 当前循环
     * @param client 对端套接字（非阻塞）
     * @param buffer 输出：收到的字节
     * @param receivedLength 输出：长度（零长报文是 0，不是「没收到」）
     * @return Core::Task<bool> 时限内收到为 true
     */
    Core::Task<bool> receiveDatagram(Core::EventLoop &loop, const Platform::DatagramSocket &client, std::vector<std::uint8_t> &buffer, ssize_t &receivedLength)
    {
        Platform::SocketAddress peerAddress{};
        for (int roundCount = 0; roundCount < 400; ++roundCount)
        {
            buffer.assign(2048U, 0U);
            const ssize_t arrived = client.receive(buffer.data(), buffer.size(), peerAddress);
            if (arrived >= 0)
            {
                // 零长报文合法到达（arrived == 0）；-1 才是「此刻没数据」或读数报错
                receivedLength = arrived;
                co_return true;
            }
            co_await yieldFor(loop, std::chrono::milliseconds{5});
        }
        co_return false;
    }

    /**
     * @brief 造一台 UDP 服务端的配置：回显处理器 + 2 KiB 收包缓冲
     * @return Net::UdpServer::Configuration 可直接交给构造
     */
    Net::UdpServer::Configuration makeConfiguration()
    {
        Net::UdpServer::Configuration configuration;
        configuration.maximumDatagramByteCount = 2048U;
        configuration.onMessage = [](const Core::InetAddress sourceAddress, const std::span<const std::uint8_t> payload) -> Core::Task<std::vector<std::uint8_t>>
        {
            g_seenSourcePort.store(sourceAddress.port(), std::memory_order_release);
            g_seenPayloadLength.store(payload.size(), std::memory_order_release);

            // 正文是 "boom" 就抛：验证处理器抛异常会被接住而不是带走整台监听器
            if (payload.size() == 4U && std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size()) == "boom")
            {
                throw Base::Exception("测试用：处理器抛异常");
            }

            co_return std::vector<std::uint8_t>(payload.begin(), payload.end());
        };
        return configuration;
    }

    /**
     * @brief 把一份已绑好的数据报套接字沿通道交回本进程，取回接手用的那一份
     * @details 顺序是「开通道 → 本端连上它 → accept 出交棒那一头 → 写出移交 → 从连上的那一头读回」；
     *          一条移交消息远小于通道缓冲，所以同一线程先写后读不会堵。
     * @param boundDatagramDescriptor 交出方的数据报描述符（本函数不接管也不关闭）
     * @return std::expected<int, std::string> 第二份引用；失败交中文原因
     */
    std::expected<int, std::string> handBackDatagramSocket(const int boundDatagramDescriptor)
    {
        auto channel = Platform::Socket::openHandoffChannel();
        if (!channel.has_value())
        {
            return std::unexpected("开不出交接通道，错误码 " + std::to_string(channel.error().value()));
        }

        const int connector = Platform::Socket::connectHandoffChannel(channel->address);
        if (connector < 0)
        {
            Platform::Socket::closeHandoffChannel(*channel);
            return std::unexpected("连不上自己刚开的交接通道");
        }

        const int peer = Platform::Socket::acceptHandoffPeer(channel->listener);
        if (peer < 0)
        {
            static_cast<void>(Platform::FileDescriptor::close(connector));
            Platform::Socket::closeHandoffChannel(*channel);
            return std::unexpected("accept 不出通道对端");
        }

        const bool isHandedOff =
            Platform::Socket::writeListeningSocketHandoff(peer, boundDatagramDescriptor, static_cast<std::uint64_t>(Platform::ProcessInfo::currentProcessId()));
        const int adoptedDescriptor = isHandedOff ? Platform::Socket::readListeningSocketHandoff(connector) : -1;

        static_cast<void>(Platform::FileDescriptor::close(peer));
        static_cast<void>(Platform::FileDescriptor::close(connector));
        Platform::Socket::closeHandoffChannel(*channel);

        if (!isHandedOff)
        {
            return std::unexpected("写出移交失败");
        }
        if (adoptedDescriptor < 0)
        {
            return std::unexpected("读回移交失败");
        }
        return adoptedDescriptor;
    }

    /**
     * @brief 跑完整轮自检
     * @param loop 承载服务端的循环（主线程不参与收发，只等结束标记）
     * @return Core::Task<> 检查跑完时完成
     */
    Core::Task<> runChecks(Core::EventLoop &loop)
    {
        // listen 的协程帧必须活到收循环退出之后，这里由本协程局部容器持有（与各示例同一纪律）
        std::vector<Core::Task<>> listenTasks;

        // 对端：一条普通的非阻塞数据报套接字，自绑一个临时端口
        Platform::DatagramSocket client = Platform::DatagramSocket::bindTo(loopbackSocketAddress(0));
        if (!client.isValid())
        {
            g_isRunFinished.store(true, std::memory_order_release);
            co_return;
        }
        const std::uint16_t clientPort = addressOf(client.localAddress()).port();

        // —— 1. 按地址起监听：端口 0 要问回内核分配的实际端口，回显按来源回去 ——
        {
            Net::UdpServer        server(loop, makeConfiguration());
            listenTasks.push_back(server.listen(Core::InetAddress::localhost(0)));
            loop.scheduler().schedule(listenTasks.back().handle());

            const std::uint16_t port = co_await waitForPort(loop, server);
            g_observations.bindsEphemeralPort = port != 0U;
            g_observations.ephemeralPort      = port;

            if (port != 0U)
            {
                const std::string text = "ping-over-udp";
                static_cast<void>(client.send(loopbackSocketAddress(port), text.data(), text.size()));

                std::vector<std::uint8_t> buffer;
                ssize_t                   receivedLength = -1;
                const bool                echoed         = co_await receiveDatagram(loop, client, buffer, receivedLength);
                g_observations.echoesPayloadByteForByte =
                    echoed && receivedLength == static_cast<ssize_t>(text.size()) && std::string_view(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(receivedLength)) == text &&
                    g_seenSourcePort.load(std::memory_order_acquire) == clientPort;

                // —— 2. 零长报文照交付：无连接协议里「一条不带内容的报文」常常就是全部输入。
                //        处理器回空正文＝不作答（那是另一条口径），所以这里判的是「交付没交付」，
                //        用处理器记下的正文长度与已交付条数来断，而不是等一条永远不会来的应答 ——
                static_cast<void>(client.send(loopbackSocketAddress(port), "", 0));
                g_seenPayloadLength.store(12345U, std::memory_order_release);
                co_await yieldFor(loop, std::chrono::milliseconds{60});
                g_observations.deliversZeroLengthDatagram = g_seenPayloadLength.load(std::memory_order_acquire) == 0U && server.stats().receivedDatagramCount == 2U;

                // —— 3. 处理器抛异常被接住，服务照常 ——
                const std::string boom = "boom";
                static_cast<void>(client.send(loopbackSocketAddress(port), boom.data(), boom.size()));
                co_await yieldFor(loop, std::chrono::milliseconds{60});
                const std::string after = "still-here";
                static_cast<void>(client.send(loopbackSocketAddress(port), after.data(), after.size()));
                const bool stillAlive = co_await receiveDatagram(loop, client, buffer, receivedLength);
                g_observations.survivesThrowingHandler =
                    stillAlive && receivedLength == static_cast<ssize_t>(after.size()) && server.stats().failedHandlerCount == 1U;

                // —— 4. 主动下发：服务端往指定来源推一条不等来的报文 ——
                const std::string pushed = "push-from-server";
                const bool        isPushed = co_await server.sendTo(Core::InetAddress::localhost(clientPort), std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pushed.data()), pushed.size()));
                const bool        pushArrived = co_await receiveDatagram(loop, client, buffer, receivedLength);
                g_observations.pushesUnsolicitedDatagram =
                    isPushed && pushArrived && receivedLength == static_cast<ssize_t>(pushed.size()) &&
                    std::string_view(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(receivedLength)) == pushed;

                // —— 5. 统计：交付 4 条（问答、零长、抛异常、问答），发出 3 条（两次回显 + 一次下发，
                //        零长那条处理器回的是空正文＝不作答），未送出 0 条 ——
                const Net::UdpServer::Stats stats = server.stats();
                g_observations.countsInStats = stats.receivedDatagramCount == 4U && stats.sentDatagramCount == 3U && stats.unsentDatagramCount == 0U && stats.failedHandlerCount == 1U;
            }

            // —— 6. 收口：stop() 置标记并关掉套接字，叫醒挂在读数上的协程。判据是那条 listen 协程
            //        真的跑完了（帧在此之前的 60 毫秒里应已收到 final_suspend），而不是「调用没炸」——
            //        只置标记不关套接字的实现会让收循环永远挂在读数上，端口带着半死的循环被占住 ——
            const std::size_t serveTaskIndex = listenTasks.size() - 1;
            server.stop();
            co_await yieldFor(loop, std::chrono::milliseconds{60});
            g_observations.stopReleasesServeLoop = listenTasks[serveTaskIndex].isReady();
        }

        // —— 7. 配置校验：缓冲容量为 0 在构造期就被拒（连一条空报文都放不下）——
        {
            Net::UdpServer::Configuration badConfiguration = makeConfiguration();
            badConfiguration.maximumDatagramByteCount      = 0U;
            try
            {
                Net::UdpServer server(loop, badConfiguration);
            } catch (const Base::InvalidArgumentException &failure)
            {
                // 抛出来还不够：说明要点名是哪一项配错，否则调用方只能翻源码
                g_observations.rejectsZeroBufferCapacity = std::string_view(failure.what()).find("缓冲") != std::string_view::npos;
            } catch (...)
            {
                g_observations.rejectsZeroBufferCapacity = false;
            }
        }

        // —— 8. 接手档：端口由别人 bind，服务端只接手 ——
        {
            Platform::DatagramSocket boundSocket = Platform::DatagramSocket::bindTo(loopbackSocketAddress(0));
            const std::uint16_t      boundPort   = boundSocket.isValid() ? addressOf(boundSocket.localAddress()).port() : 0U;

            if (const auto handedBack = handBackDatagramSocket(boundSocket.fileDescriptor()); handedBack.has_value() && boundPort != 0U)
            {
                auto adopted = Platform::DatagramSocket::adopt(*handedBack);
                if (adopted.has_value())
                {
                    Net::UdpServer adoptingServer(loop, makeConfiguration(), std::move(*adopted));

                    // 混用两种顺序必须被拒：接手的对象不许再去 bind 一个端口
                    try
                    {
                        co_await adoptingServer.listen(Core::InetAddress::localhost(0));
                    } catch (const Base::InvalidArgumentException &)
                    {
                        g_observations.rejectsMixingStartupModes = true;
                    }

                    listenTasks.push_back(adoptingServer.listen());
                    loop.scheduler().schedule(listenTasks.back().handle());

                    const std::uint16_t servingPort = co_await waitForPort(loop, adoptingServer);
                    g_observations.adoptedPortFollowsSocket = servingPort == boundPort;
                    g_observations.adoptedPort              = servingPort;

                    if (servingPort == boundPort && boundPort != 0U)
                    {
                        const std::string text = "to-adopted-port";
                        static_cast<void>(client.send(loopbackSocketAddress(boundPort), text.data(), text.size()));

                        std::vector<std::uint8_t> buffer;
                        ssize_t                   receivedLength = -1;
                        const bool              echoed = co_await receiveDatagram(loop, client, buffer, receivedLength);
                        g_observations.adoptsHandedOverSocket =
                            echoed && receivedLength == static_cast<ssize_t>(text.size()) &&
                            std::string_view(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(receivedLength)) == text;
                    }

                    adoptingServer.stop();
                    co_await yieldFor(loop, std::chrono::milliseconds{60});
                }
            }

            // 反方向的混用：按地址构造的对象调无参 listen()，没有可服务的套接字
            Net::UdpServer addressServer(loop, makeConfiguration());
            try
            {
                co_await addressServer.listen();
            } catch (const Base::InvalidArgumentException &)
            {
                g_observations.rejectsAddressModeWithoutAddress = true;
            }
        }

        g_isRunFinished.store(true, std::memory_order_release);
        co_return;
    }
} // namespace

int main()
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Net UDP 服务端示例开始 ===");

    // 本示例的端口一律交给内核（端口 0）：端口档位在这里没有真实消费方，
    // 传进来也没人读，故不做成命令行开关——留一个「填了却不生效」的档位比不做更坏
    Core::IoContext  context(1);
    auto            &pool = context.threadPool();
    Core::EventLoop &loop = pool.eventLoop(0);

    Core::Task<> runTask = runChecks(loop);
    loop.scheduler().schedule(runTask.handle());
    pool.start();

    const bool isRunDone = Samples::waitUntil([] { return g_isRunFinished.load(std::memory_order_acquire); }, std::chrono::seconds{60}, std::chrono::milliseconds{20});

    auto &samples = Samples::checklist();
    samples.check(isRunDone, "整轮检查在时限内跑完（负向用例没有挂住）");
    samples.check(g_observations.bindsEphemeralPort, "按地址起监听时端口 0 问得回内核分配的实际端口");
    samples.check(g_observations.echoesPayloadByteForByte, "回显一字不差，且处理器看到的来源端口就是发包那一条套接字");
    samples.check(g_observations.deliversZeroLengthDatagram, "零长报文照交付，不被当成「没收到」");
    samples.check(g_observations.survivesThrowingHandler, "处理器抛异常被接住并计数，下一条报文照常应答");
    samples.check(g_observations.pushesUnsolicitedDatagram, "sendTo 的主动下发落到对端");
    samples.check(g_observations.countsInStats, "received/sent/unsent/failed 四笔账与实际操作对得上");
    samples.check(g_observations.stopReleasesServeLoop, "stop() 叫醒挂在读数上的协程，收循环退出");
    samples.check(g_observations.rejectsZeroBufferCapacity, "收包缓冲容量为 0 在构造期就被拒");
    samples.check(g_observations.rejectsMixingStartupModes, "接手来的服务端调带地址的 listen() 被当场拒");
    samples.check(g_observations.rejectsAddressModeWithoutAddress, "按地址构造的调无参 listen() 同样被拒");
    samples.check(g_observations.adoptedPortFollowsSocket, "接手模式下的端口就是交过来那一份套接字上的端口");
    samples.check(g_observations.adoptsHandedOverSocket, "发往那个端口的报文在接手来的服务端上有回话");

    context.stop();
    return Samples::finishSample("net_udp_demo");
}
