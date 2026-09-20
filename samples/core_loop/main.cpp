// Core 运行时示例：事件循环与线程池、调度器投递、定时器、IO 监听器、协程帧池、可取消令牌、
// 异步解析、UDP 与 TCP 的协程收发（含聚合写）
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Cancelable.h"
#include "Core/Coroutine/CoroutinePool.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/EventNotifier.h"
#include "Platform/IO/Socket.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /// 示例期间收集的结果：只在循环线程上写、主线程等它跑完再读，因此不需要锁
    struct RuntimeProbe
    {
        bool isLocalPostDelivered{false};
        bool isRemotePostDelivered{false};
        bool isTimerElapsed{false};
        bool isWatcherAwakened{false};
        bool isSecondWaitAwoken{false};
        bool isResolverSucceeded{false};
        bool isUdpRoundTripSucceeded{false};
        bool isTcpRoundTripSucceeded{false};
        bool isVectoredSendSucceeded{false};
        bool isCoroutinePoolOwning{false};
        bool isCancelRequested{false};
        bool isSecondCancelRejected{false};
        bool isSchedulerIdleAtEnd{false};
    };

    /// @return Platform::SocketAddress 一个 IPv4 回环地址（port 为主机序，0 表示让内核挑）
    Platform::SocketAddress makeLoopbackAddress(const std::uint16_t port)
    {
        Platform::SocketAddress address{};
        auto                   &ipv4 = reinterpret_cast<sockaddr_in &>(address.storage);
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.length = static_cast<socklen_t>(sizeof(sockaddr_in));
        return address;
    }

    /// @return std::uint16_t 一个 SocketAddress 里的端口（主机序）
    std::uint16_t portOf(const Platform::SocketAddress &address)
    {
        const auto &ipv4 = reinterpret_cast<const sockaddr_in &>(address.storage);
        return ntohs(ipv4.sin_port);
    }

    /**
     * @brief UDP 协程收发：绑定之后给自己发一条再收回来
     * @param loop 承载本次等待的循环
     * @return bool 收到的字节与发出的一致，且对端端口就是本端
     */
    Core::Task<bool> probeUdp(Core::EventLoop &loop)
    {
        auto platformSocket = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
        if (!platformSocket.isValid())
        {
            co_return false;
        }
        Core::AsyncUdpSocket udp(loop, std::move(platformSocket));
        const std::string    text = "udp-ping";
        const auto           self = udp.localAddress();
        if (co_await udp.asyncSendTo(self, text.data(), text.size()) != static_cast<ssize_t>(text.size()))
        {
            co_return false;
        }
        std::uint8_t             buffer[32]{};
        const auto               received = co_await udp.asyncReceiveFrom(buffer, sizeof(buffer));
        co_return received.receivedByteCount == static_cast<ssize_t>(text.size()) &&
                  std::memcmp(buffer, text.data(), static_cast<std::size_t>(received.receivedByteCount)) == 0 &&
                  portOf(received.peerAddress) == portOf(self);
    }

    /**
     * @brief 在一条已 accept 的连接上做一轮协程收发
     * @tparam SendBody 发送动作的工厂：把 asyncSend 或 asyncSendVectored 统一成同一写法
     * @param loop 承载本次等待的循环
     * @param port 监听端口
     * @param requestText 客户端要发的请求
     * @param isVectored true 时服务端用聚合写回两段缓冲
     * @return Core::Task<bool> 请求与应答都对得上才算通过
     */
    Core::Task<bool> probeTcpOnPort(Core::EventLoop &loop, const std::uint16_t port, const std::string &requestText,
                                    const bool isVectored)
    {
        auto listener = Core::AsyncSocket::create(loop, AF_INET, SOCK_STREAM);
        const auto listenerAddress = Core::InetAddress::resolve("127.0.0.1", port);
        if (!listenerAddress.has_value() || !listener.bind(*listenerAddress) || !listener.listen(8))
        {
            co_return false;
        }

        auto client = Core::AsyncSocket::create(loop, AF_INET, SOCK_STREAM);
        static_cast<void>(co_await client.asyncConnect(*listenerAddress));

        sockaddr_storage peerStorage{};
        socklen_t        peerLength = sizeof(peerStorage);
        const int        acceptedDescriptor =
                Platform::Socket::accept(listener.fileDescriptor(), reinterpret_cast<sockaddr *>(&peerStorage), &peerLength);
        if (acceptedDescriptor < 0)
        {
            co_return false;
        }
        auto server = Core::AsyncSocket(loop, acceptedDescriptor);

        static_cast<void>(co_await client.asyncSend(requestText.data(), requestText.size()));
        std::uint8_t requestBuffer[16]{};
        const ssize_t receivedRequestByteCount = co_await server.asyncReceive(requestBuffer, sizeof(requestBuffer));
        if (receivedRequestByteCount != static_cast<ssize_t>(requestText.size()) ||
            std::memcmp(requestBuffer, requestText.data(), requestText.size()) != 0)
        {
            co_return false;
        }

        // 回程按 isVectored 分两条路：一条普通 asyncSend，一条把两段缓冲一次交出去
        std::string firstHalf = "AB";
        std::string secondHalf = "CD";
        const Platform::Socket::WriteBuffer buffers[] = {{firstHalf.data(), firstHalf.size()}, {secondHalf.data(), secondHalf.size()}};
        if (isVectored)
        {
            static_cast<void>(co_await server.asyncSendVectored(buffers, 2));
        }
        else
        {
            static_cast<void>(co_await server.asyncSend(secondHalf.data(), secondHalf.size()));
        }
        std::uint8_t responseBuffer[8]{};
        const ssize_t receivedResponseByteCount = co_await client.asyncReceive(responseBuffer, sizeof(responseBuffer));
        const std::string expectedResponse = isVectored ? "ABCD" : "CD";
        co_return receivedResponseByteCount == static_cast<ssize_t>(expectedResponse.size()) &&
                  std::memcmp(responseBuffer, expectedResponse.data(), expectedResponse.size()) == 0;
    }

    /**
     * @brief 全部检查都跑在同一条循环上：投递、定时器、监听器、解析、收发、帧池、取消令牌
     * @param loop 目标循环
     * @param port 本次用的基准端口（TCP 两项各占相邻端口）
     * @param probe 输出：收集到的各项结果
     * @param doneSignal 输出：跑完后置位
     * @return Core::Task<> 协程，跑完即返回
     */
    Core::Task<> runProbes(Core::EventLoop &loop, const std::uint16_t port, RuntimeProbe &probe, std::atomic<bool> &doneSignal)
    {
        loop.scheduler().postLocal([&probe] { probe.isLocalPostDelivered = true; });
        // runOne 只吃已经排好的活：本线程投递当场就该被执行
        static_cast<void>(loop.scheduler().runOne());

        Core::Timer timer(loop);
        const auto  startedAt = std::chrono::steady_clock::now();
        co_await timer.waitFor(std::chrono::milliseconds{30});
        probe.isTimerElapsed = std::chrono::steady_clock::now() - startedAt >= std::chrono::milliseconds{25};

        Platform::EventNotifier notifier;
        Core::IoWatcher         watcher(loop, notifier.readDescriptor());
        // 两轮「通知 → 等待」跑在同一个常驻注册对象上，验「注册一次即可反复等待」这条自述：
        // 等待是否真被唤醒由 await 的返回值证明（叫不醒就挂在那次 await 上，整轮的 isFinished 会红）
        // 轮与轮之间必须走 drain()：notify() 是合并唤醒的，标记不清掉就一直不再写字节，
        // 第二轮的 notify 根本发不出东西（自己读描述符也不行——Linux 上它是 eventfd，按 1 字节读只会得到 EINVAL）
        notifier.notify();
        probe.isWatcherAwakened = co_await watcher.waitReadable();
        notifier.drain();

        notifier.notify();
        probe.isSecondWaitAwoken = co_await watcher.waitReadable();
        notifier.drain();

        const auto resolved = co_await Core::AsyncResolver::resolve(loop, "localhost", port);
        probe.isResolverSucceeded = !resolved.empty();

        probe.isUdpRoundTripSucceeded = co_await probeUdp(loop);
        probe.isTcpRoundTripSucceeded = co_await probeTcpOnPort(loop, port + 2, "request", false);
        probe.isVectoredSendSucceeded = co_await probeTcpOnPort(loop, port + 4, "AB", true);

        auto  &pool = Core::CoroutinePool::instance();
        void  *block = pool.allocate(64);
        probe.isCoroutinePoolOwning = pool.owns(block);
        pool.deallocate(block, 64);

        Core::Cancelable cancellation;
        static_cast<void>(cancellation.requestStop());
        probe.isCancelRequested = cancellation.isStopRequested() && cancellation.stopToken().stop_requested();
        probe.isSecondCancelRejected = !cancellation.requestStop();

        probe.isSchedulerIdleAtEnd = loop.scheduler().localQueueSize() == 0 && !loop.scheduler().hasWork();

        doneSignal.store(true, std::memory_order_release);
        co_return;
    }
}

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    const std::uint16_t port = Samples::readPortArgument(argc, argv, 20);

    LOG_INFO("=== Core 运行时示例开始 ===");

    Core::IoContext  context(2);
    Core::ThreadPool &pool = context.threadPool();
    pool.start();
    Core::EventLoop &loop = pool.eventLoop(0);

    RuntimeProbe      probe;
    std::atomic<bool> isDone{false};
    Core::Task<>      probesTask = runProbes(loop, port, probe, isDone);
    // 跨线程投递交给 scheduleRemote：它连带唤醒那条循环
    loop.scheduler().scheduleRemote(probesTask.handle());

    // 另一条循环上验一次远程投递：主线程投、那条循环执行
    std::atomic<bool> isRemotePosted{false};
    Core::EventLoop  &otherLoop = pool.eventLoop(1 % pool.threadCount());
    otherLoop.scheduler().postRemote([&isRemotePosted] { isRemotePosted.store(true, std::memory_order_release); });
    static_cast<void>(otherLoop.scheduler().runOne());

    const bool isFinished = Samples::waitUntil([&isDone] { return isDone.load(std::memory_order_acquire); },
                                              std::chrono::seconds{30});

    auto &samples = Samples::checklist();
    samples.check(pool.threadCount() == 2, "IoContext 起了两条各自持有事件循环的线程");
    samples.check(isFinished, "整套运行时检查在时限内跑完（没有挂在未唤醒的等待上）");
    samples.check(probe.isLocalPostDelivered, "调度器的本线程投递当场被执行");
    samples.check(isRemotePosted.load(std::memory_order_acquire), "跨线程 postRemote 排进了目标循环的队列");
    samples.check(probe.isTimerElapsed, "Core::Timer 到点唤醒协程");
    samples.check(probe.isWatcherAwakened, "IoWatcher 等到了事件通知器的唤醒（等待没有靠超时脱身）");
    samples.check(probe.isSecondWaitAwoken, "同一个常驻注册对象的第二次等待也被新事件唤醒（注册一次即可反复等待）");
    samples.check(probe.isResolverSucceeded, "异步解析器给出结果");
    samples.check(probe.isUdpRoundTripSucceeded, "UDP 协程收发完成一回环往返");
    samples.check(probe.isTcpRoundTripSucceeded, "TCP 协程收发完成一回环往返");
    samples.check(probe.isVectoredSendSucceeded, "聚合写把两段缓冲按序送到对端");
    samples.check(probe.isCoroutinePoolOwning, "协程帧池认得自己发出去的块");
    samples.check(probe.isCancelRequested && probe.isSecondCancelRejected, "Cancelable 只允许一次停止请求");
    samples.check(probe.isSchedulerIdleAtEnd, "跑完之后调度器队列是空的");

    context.stop();
    static_cast<void>(probesTask);
    return Samples::finishSample("core_loop");
}
