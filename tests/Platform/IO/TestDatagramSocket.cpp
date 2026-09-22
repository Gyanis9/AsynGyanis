// DatagramSocket 单元测试：绑定、收发（带对端地址）、无数据与非法参数的错误面
#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 有界重试统一使用的最长等待毫秒数
        constexpr int kWaitTimeoutMilliseconds = 2000;

        /**
         * @brief 造一个回环 IPv4 地址
         * @param port 端口；0 表示由内核分配
         * @return SocketAddress 地址值
         */
        SocketAddress makeLoopbackAddress(const std::uint16_t port)
        {
            SocketAddress address;
            sockaddr_in    addressV4{};
            addressV4.sin_family      = AF_INET;
            addressV4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addressV4.sin_port        = htons(port);
            std::memcpy(&address.storage, &addressV4, sizeof(addressV4));
            address.length = sizeof(addressV4);
            return address;
        }

        /**
         * @brief 取地址里的端口（只处理 IPv4，本文件的地址都由 makeLoopbackAddress 造）
         * @param address 目标地址
         * @return std::uint16_t 端口
         */
        std::uint16_t portOf(const SocketAddress &address)
        {
            sockaddr_in addressV4{};
            std::memcpy(&addressV4, &address.storage, sizeof(addressV4));
            return ntohs(addressV4.sin_port);
        }

        /**
         * @brief 两个地址是否指向同一个 IPv4 端点
         * @param left 左地址
         * @param right 右地址
         * @return true 同族、同 IP、同端口
         */
        bool isSameIpv4Endpoint(const SocketAddress &left, const SocketAddress &right)
        {
            sockaddr_in leftV4{};
            sockaddr_in rightV4{};
            std::memcpy(&leftV4, &left.storage, sizeof(leftV4));
            std::memcpy(&rightV4, &right.storage, sizeof(rightV4));
            return leftV4.sin_family == rightV4.sin_family && leftV4.sin_addr.s_addr == rightV4.sin_addr.s_addr &&
                   leftV4.sin_port == rightV4.sin_port;
        }

        /**
         * @brief 在时限内收一条报文（套接字非阻塞，因此要轮询）
         * @param socket 接收套接字
         * @param buffer 接收缓冲
         * @param peerAddress 输出参数：来源地址
         * @return ssize_t 收到的字节数；超时返回 -1
         */
        ssize_t receiveWithTimeout(const DatagramSocket &socket, void *buffer, const std::size_t capacity, SocketAddress &peerAddress)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t receivedByteCount = socket.receive(buffer, capacity, peerAddress);
                if (receivedByteCount >= 0)
                {
                    return receivedByteCount;
                }
                if (PlatformError::lastSocketErrorCode() != PlatformError::kWouldBlock)
                {
                    return -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            return -1;
        }
    } // namespace

    /**
     * @brief 绑定到端口 0 时内核分配端口，取回的本地地址必须是实际在用的那个
     */
    TEST(DatagramSocket, BindsAndReportsAllocatedLocalAddress)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid()) << "绑定失败，套接字错误码 " << PlatformError::lastSocketErrorCode();
        EXPECT_GE(socket.fileDescriptor(), 0) << "有效套接字应当给出描述符";

        const SocketAddress localAddress = socket.localAddress();
        EXPECT_EQ(localAddress.length, sizeof(sockaddr_in)) << "回环 IPv4 的地址长度应当是 sockaddr_in";
        EXPECT_EQ(localAddress.storage.ss_family, AF_INET);
        EXPECT_NE(portOf(localAddress), 0) << "端口给 0 时应当取回内核分配的实际端口";
    }

    /**
     * @brief 共用一个 UDP 端口的多个监听器：报文要分到不止一个监听器上
     * @details 「绑得上」不是这里的性质——只设 SO_REUSEADDR 的内核让每个监听器都绑定成功，却把全部报文
     *          交给最后绑上的那一个（实测 24 条流全落在第 3 个监听器，前两个各 0 条且不报任何错误）。
     *          多进程 worker 各绑同一端口做 h3 横向扩展正是这个形态，故断言落在「收到过流量的监听器
     *          个数」上：加上 SO_REUSEPORT 后同一负载实测分成 9/6/9。删掉 bindTo() 里那次 setReusePort
     *          本例必红
     */
    TEST(DatagramSocket, SharedPortSpreadsDatagramsAcrossListeners)
    {
        ASSERT_TRUE(Socket::initialize());

        // 没有该选项的平台（Windows、3.9 之前的内核）不存在「多监听器分摊」这回事
        const int probeDescriptor = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ASSERT_TRUE(FileDescriptor::isValid(probeDescriptor));
        const bool isReusePortSupported = Socket::setReusePort(probeDescriptor);
        FileDescriptor::close(probeDescriptor);
        if (!isReusePortSupported)
        {
            GTEST_SKIP() << "本平台没有 SO_REUSEPORT，共享端口的场景不适用";
        }

        // 三个监听器：与 --workers 3 的进程数对应，两个无法区分「1 个收全部」与「恰好分了一半」
        constexpr std::size_t kListenerCount = 3;
        // 每条流用自己的临时源端口：内核按四元组哈希选监听器，源端口相同就只会命中同一个
        constexpr int kFlowCount = 24;

        std::vector<DatagramSocket> listeners;
        listeners.reserve(kListenerCount);
        listeners.emplace_back(DatagramSocket::bindTo(makeLoopbackAddress(0)));
        ASSERT_TRUE(listeners.front().isValid()) << "第一个绑定就失败了，套接字错误码 " << PlatformError::lastSocketErrorCode();
        const std::uint16_t port = portOf(listeners.front().localAddress());

        for (std::size_t index = 1; index < kListenerCount; ++index)
        {
            listeners.emplace_back(DatagramSocket::bindTo(makeLoopbackAddress(port)));
            ASSERT_TRUE(listeners.back().isValid()) << "第 " << index + 1 << " 个监听器绑不上共用端口 " << port
                                                    << "，套接字错误码 " << PlatformError::lastSocketErrorCode();
        }

        const SocketAddress listeningAddress = makeLoopbackAddress(port);
        for (int flow = 0; flow < kFlowCount; ++flow)
        {
            // 发完即关：客户端的临时端口各条不同，报文此刻已落在某个监听器的接收队列里
            const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
            ASSERT_TRUE(sender.isValid());
            const std::string payload = "flow" + std::to_string(flow);
            ASSERT_EQ(sender.send(listeningAddress, payload.data(), payload.size()), static_cast<ssize_t>(payload.size()))
                    << "第 " << flow << " 条流发送失败，套接字错误码 " << PlatformError::lastSocketErrorCode();
        }

        // 等到条数收齐或时限到点：不靠「睡一会儿大概就到了」，构造不出条件时本例应当红
        std::array<std::size_t, kListenerCount> receivedCount{};
        std::size_t                             totalReceivedCount = 0;
        const auto                              deadline           = std::chrono::steady_clock::now() +
                                                                     std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (totalReceivedCount < static_cast<std::size_t>(kFlowCount) && std::chrono::steady_clock::now() < deadline)
        {
            for (std::size_t index = 0; index < listeners.size(); ++index)
            {
                std::array<char, 64> payloadBuffer{};
                SocketAddress        peerAddress;
                while (listeners[index].receive(payloadBuffer.data(), payloadBuffer.size(), peerAddress) > 0)
                {
                    ++receivedCount[index];
                    ++totalReceivedCount;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        EXPECT_EQ(totalReceivedCount, static_cast<std::size_t>(kFlowCount))
                << "共用端口的 " << kListenerCount << " 个监听器一共只收到 " << totalReceivedCount << " 条，报文不该丢";

        const std::size_t activeListenerCount = static_cast<std::size_t>(
                std::ranges::count_if(receivedCount, [](const std::size_t count)
                                      {
                                          return count > 0;
                                      }));
        EXPECT_GT(activeListenerCount, 1U) << "全部 " << totalReceivedCount << " 条报文都落在同一个监听器上（分布 "
                                           << receivedCount[0] << '/' << receivedCount[1] << '/' << receivedCount[2]
                                           << "）：这些监听器都报告绑定成功，其余的其实一条也收不到";
    }

    /**
     * @brief 一条端口上的两个对端：发出去的内容与来源地址都要如实回到接收方
     * @details 数据报与流的最大差别就在这里——接收方必须知道这条来自谁，否则一个端口服务多条连接无从谈起
     */
    TEST(DatagramSocket, SendsAndReceivesWithPeerAddress)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        constexpr std::string_view kPayload = "asyn-datagram";
        ASSERT_EQ(sender.send(receiver.localAddress(), kPayload.data(), kPayload.size()), static_cast<ssize_t>(kPayload.size()))
                << "发送失败，套接字错误码 " << PlatformError::lastSocketErrorCode();

        std::array<char, 64> payloadBuffer{};
        SocketAddress        peerAddress;
        const ssize_t        receivedByteCount = receiveWithTimeout(receiver, payloadBuffer.data(), payloadBuffer.size(), peerAddress);

        ASSERT_EQ(receivedByteCount, static_cast<ssize_t>(kPayload.size())) << "没有收到报文";
        EXPECT_EQ(std::string_view(payloadBuffer.data(), static_cast<std::size_t>(receivedByteCount)), kPayload)
                << "收到的内容与发出的不一致";
        EXPECT_TRUE(isSameIpv4Endpoint(peerAddress, sender.localAddress()))
                << "来源地址不是发送方的地址：端口 " << portOf(peerAddress) << " 与 " << portOf(sender.localAddress());
    }

    /**
     * @brief 没有数据时收，返回 -1 且错误码是 kWouldBlock（不阻塞、不误报成功）
     */
    TEST(DatagramSocket, ReceiveWithoutDataReportsWouldBlock)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid());

        std::array<char, 16> payloadBuffer{};
        SocketAddress        peerAddress;
        EXPECT_EQ(socket.receive(payloadBuffer.data(), payloadBuffer.size(), peerAddress), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kWouldBlock)
                << "无数据应当报「暂时没有」而不是别的错误";
        EXPECT_EQ(peerAddress.length, 0U) << "没收到报文时不该给出来源地址";
    }

    /**
     * @brief 未设置或地址族不认识的地址：绑定当场失败，不去猜协议族也不去绑任意地址
     */
    TEST(DatagramSocket, RejectsUnsetLocalAddress)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(SocketAddress{});
        EXPECT_FALSE(socket.isValid()) << "没给地址不该绑成功";
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);
    }

    /**
     * @brief 超过单条报文上限的发送当场判错，不交给系统调用去报平台各自的错误码
     */
    TEST(DatagramSocket, RejectsPayloadBeyondDatagramLimit)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        std::vector<char> oversizedPayload(DatagramSocket::kMaximumDatagramBytes + 1, 'x');
        EXPECT_EQ(sender.send(receiver.localAddress(), oversizedPayload.data(), oversizedPayload.size()), -1);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument) << "超限应当是由本层给出的参数非法";
    }

    /**
     * @brief 无效套接字与非法参数上的收发一律安全失败，不崩不猜
     */
    TEST(DatagramSocket, InvalidSocketAndArgumentsFailSafely)
    {
        // 网络库初始化由调用方负责（Winsock 未起时连 ::socket 都会失败）：本用例是独立进程里跑的，
        // 不能指望别的用例先初始化过
        ASSERT_TRUE(Socket::initialize());

        DatagramSocket invalidSocket;
        EXPECT_FALSE(invalidSocket.isValid());
        EXPECT_LT(invalidSocket.fileDescriptor(), 0);
        EXPECT_EQ(invalidSocket.localAddress().length, 0U);

        char          payloadBuffer[8]{};
        SocketAddress peerAddress = makeLoopbackAddress(1);
        EXPECT_EQ(invalidSocket.receive(payloadBuffer, sizeof(payloadBuffer), peerAddress), -1);
        EXPECT_EQ(invalidSocket.send(peerAddress, payloadBuffer, sizeof(payloadBuffer)), -1);
        invalidSocket.close(); // 幂等：析构还会再调一次
        EXPECT_FALSE(invalidSocket.isValid());

        // 有效套接字上的空缓冲/空指针也算用法错误
        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid());
        EXPECT_EQ(socket.receive(nullptr, 16, peerAddress), -1);
        EXPECT_EQ(socket.receive(payloadBuffer, 0, peerAddress), -1);
        EXPECT_EQ(socket.send(peerAddress, nullptr, 4), -1);
        EXPECT_EQ(socket.send(peerAddress, payloadBuffer, 0), -1);
    }

    /**
     * @brief 移动之后所有权易主：源侧变无效，目标侧照常收发
     */
    TEST(DatagramSocket, MoveTransfersOwnership)
    {
        ASSERT_TRUE(Socket::initialize());

        DatagramSocket source = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(source.isValid());
        const std::uint16_t boundPort = portOf(source.localAddress());

        DatagramSocket target = std::move(source);
        EXPECT_FALSE(source.isValid()) << "移动之后源对象仍自称有效";
        ASSERT_TRUE(target.isValid());
        EXPECT_EQ(portOf(target.localAddress()), boundPort) << "移动不该换掉已绑定的端口";
    }

    /**
     * @brief 钉住：移动赋值要关掉被顶替的那只，并且自赋值不能伤到自己
     * @details 只测移动构造的话，「赋值时忘了先关自己」这一条要等套接字被反复复用才显形——被顶替的
     *          那只 UDP 套接字会继续占着端口到进程退出。而 `a = std::move(a)` 少一个自赋值自检，
     *          就会先关掉自己再把已经变成 -1 的描述符接回来，端口无声消失。
     */
    TEST(DatagramSocket, MoveAssignmentClosesTheDisplacedSocketAndIgnoresSelfAssignment)
    {
        ASSERT_TRUE(Socket::initialize());

        DatagramSocket source = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(source.isValid()) << "绑定失败，套接字错误码 " << PlatformError::lastSocketErrorCode();
        DatagramSocket target = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(target.isValid()) << "绑定失败，套接字错误码 " << PlatformError::lastSocketErrorCode();

        const std::uint16_t sourcePort   = portOf(source.localAddress());
        const int           displacedFd  = target.fileDescriptor();

        target = std::move(source);

        EXPECT_FALSE(source.isValid()) << "移动赋值之后源侧仍自称有效，同一只套接字会被关第二次";
        ASSERT_TRUE(target.isValid());
        EXPECT_EQ(portOf(target.localAddress()), sourcePort) << "赋值之后接过的应是对方那只套接字";

        // 被顶替的那只必须已经关闭：还开着的话 getsockname 照常成功，端口也就一直被占着
        sockaddr_storage staleAddress{};
        socklen_t      staleLength = sizeof(staleAddress);
        EXPECT_NE(::getsockname(displacedFd, reinterpret_cast<sockaddr *>(&staleAddress), &staleLength), 0)
                << "被顶替的套接字没被关掉，它的端口会一直被占到进程退出";

        // 字面量上的「把自己 std::move 给自己」会被编译器直接判成缺陷（GCC 的 -Wself-move），
        // 而这里要验的正是实现里那道 `this != &other` 自检，因此经由一个别名引用把同一个对象递进去
        DatagramSocket &sameSocket = target;
        target = std::move(sameSocket);
        EXPECT_TRUE(target.isValid()) << "自赋值把套接字关掉后又接到自己空出来的描述符上";
        EXPECT_EQ(portOf(target.localAddress()), sourcePort) << "自赋值不该改变已经拥有的套接字";
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（POSIX）：绑好的套接字带 FD_CLOEXEC，spawn 出去的 worker 不会替父进程占端口
     * @details 多进程 worker 用 fork+exec 起子进程。少了这个标志，子进程继承描述符：父进程退出后
     *          端口仍被占着（正是 Process 那份继承清单要收窄到三个标准句柄的原因），而且子进程
     *          与父进程共用同一条接收队列，会把 h3 报文读进一个永不处理它的进程里。
     */
    TEST(DatagramSocket, BoundSocketIsMarkedCloseOnExec)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid()) << "绑定失败，套接字错误码 " << PlatformError::lastSocketErrorCode();

        const int descriptorFlags = ::fcntl(socket.fileDescriptor(), F_GETFD);
        ASSERT_GE(descriptorFlags, 0) << "读不到描述符标志";
        EXPECT_TRUE((descriptorFlags & FD_CLOEXEC) != 0)
                << "描述符没标 FD_CLOEXEC，子进程会继承它并占住这个 UDP 端口（标志实际为 " << descriptorFlags << "）";
    }
#endif
} // namespace AsynGyanis::Platform
