// DatagramSocket 单元测试：绑定、收发（带对端地址）、无数据与非法参数的错误面
#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include "PlatformTestSupport.h"

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
            sockaddr_in   addressV4{};
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
            return leftV4.sin_family == rightV4.sin_family && leftV4.sin_addr.s_addr == rightV4.sin_addr.s_addr && leftV4.sin_port == rightV4.sin_port;
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

        /**
         * @brief 在时限内做一次批次收包（套接字非阻塞，因此要轮询）
         * @param socket 接收套接字
         * @param slots 槽位数组（每槽带好缓冲与容量）
         * @param slotCount 槽位数
         * @return ssize_t 交付条数；超时或真错误分别交回 0 与 -1
         * @details 与 `receiveWithTimeout` 同一形状：本用例要判的是「一次批次能交出几条」，
         *          因此这里只在**一次**调用里等出条目，拿到几条就算几条，不替被测代码合批。
         */
        ssize_t receiveBatchWithTimeout(const DatagramSocket &socket, DatagramSocket::BatchSlot *const slots, const std::size_t slotCount)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t batchCount = socket.receiveBatch(slots, slotCount);
                if (batchCount > 0)
                {
                    return batchCount;
                }
                if (batchCount < 0)
                {
                    return -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            return 0;
        }

        /**
         * @brief 造一个已绑定的回环数据报套接字，交回来的是**裸描述符**
         * @param port 输出：内核分配的端口
         * @return int 描述符；负值表示没造出来
         * @details 接管接口的输入是别人已经绑好的描述符，而 DatagramSocket 没有「交出所有权」的
         *          出口（析构必关），所以夹具只能自己按平台方式建
         */
        int rawBoundUdpSocket(std::uint16_t &port)
        {
            port                  = 0U;
            const int  descriptor = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            const auto address    = makeLoopbackAddress(0);
            if (descriptor < 0 || ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address.storage), address.length) != 0)
            {
                if (descriptor >= 0)
                {
                    static_cast<void>(FileDescriptor::close(descriptor));
                }
                return -1;
            }
            sockaddr_in local{};
            socklen_t   length = static_cast<socklen_t>(sizeof(local));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&local), &length) != 0)
            {
                static_cast<void>(FileDescriptor::close(descriptor));
                return -1;
            }
            port = ntohs(local.sin_port);
            return descriptor;
        }

        /**
         * @brief 造一个已在监听的回环流套接字（用来验「接管会拒掉非数据报的描述符」）
         * @param port 输出：内核分配的端口
         * @return int 描述符；负值表示没造出来
         */
        int rawListeningTcpSocket(std::uint16_t &port)
        {
            port                  = 0U;
            const int  descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            const auto address    = makeLoopbackAddress(0);
            if (descriptor < 0 || ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address.storage), address.length) != 0 || ::listen(descriptor, 4) != 0)
            {
                if (descriptor >= 0)
                {
                    static_cast<void>(FileDescriptor::close(descriptor));
                }
                return -1;
            }
            sockaddr_in local{};
            socklen_t   length = static_cast<socklen_t>(sizeof(local));
            static_cast<void>(::getsockname(descriptor, reinterpret_cast<sockaddr *>(&local), &length));
            port = ntohs(local.sin_port);
            return descriptor;
        }

        /**
         * @brief 打开一个**有效但根本不是套接字**的句柄（设备文件），当接管拒绝面的输入
         * @return int 描述符；负值表示没打开成
         * @details 接管第一步问的是 SO_TYPE，问不出来的那一档与「是套接字但不是数据报」是两回事：
         *          前者要换传进来的东西，后者是交出方送错了类型，混成一档就把两条不同的下一步并成了
         *          一条。只在 POSIX 上判：Windows 的整数句柄空间里，普通文件的 CRT 描述符与 SOCKET
         *          不同域，而本层的 close() 走 closesocket——夹具要另配一套收口才不至于误关。
         */
#if !ASYN_PLATFORM_WIN32
        int plainFileHandle()
        {
            return ::open("/dev/null", O_RDONLY);
        }
#endif
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
            ASSERT_TRUE(listeners.back().isValid()) << "第 " << index + 1 << " 个监听器绑不上共用端口 " << port << "，套接字错误码 " << PlatformError::lastSocketErrorCode();
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
        const auto                              deadline           = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
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

        EXPECT_EQ(totalReceivedCount, static_cast<std::size_t>(kFlowCount)) << "共用端口的 " << kListenerCount << " 个监听器一共只收到 " << totalReceivedCount << " 条，报文不该丢";

        const std::size_t activeListenerCount = static_cast<std::size_t>(std::ranges::count_if(receivedCount, [](const std::size_t count) { return count > 0; }));
        EXPECT_GT(activeListenerCount, 1U) << "全部 " << totalReceivedCount << " 条报文都落在同一个监听器上（分布 " << receivedCount[0] << '/' << receivedCount[1] << '/'
                                           << receivedCount[2] << "）：这些监听器都报告绑定成功，其余的其实一条也收不到";
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
        EXPECT_EQ(std::string_view(payloadBuffer.data(), static_cast<std::size_t>(receivedByteCount)), kPayload) << "收到的内容与发出的不一致";
        EXPECT_TRUE(isSameIpv4Endpoint(peerAddress, sender.localAddress())) << "来源地址不是发送方的地址：端口 " << portOf(peerAddress) << " 与 " << portOf(sender.localAddress());
    }

    /**
     * @brief 钉住：空报文发得出、收得到，来源地址照给
     * @details 头文件把「0 长度就是合法空报文」写成契约（拿零长报文做保活探测是常见做法），而实现
     *          此前不分长度地拒掉 `nullptr`——于是 `send(peer, nullptr, 0)` 这一最自然的写法只得到
     *          kInvalidArgument。接收侧要能把「收到一条空报文」与「暂时没有数据」分开：前者是 0，
     *          后者是 -1 加 kWouldBlock，因此本用例断言 0 才有意义。
     * @note 「说有字节却没给缓冲区」仍是非法（见 InvalidSocketAndArgumentsFailSafely）。
     */
    TEST(DatagramSocket, SendsAndReceivesAnEmptyDatagram)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        ASSERT_EQ(sender.send(receiver.localAddress(), nullptr, 0), static_cast<ssize_t>(0)) << "空报文应当照发，套接字错误码 " << PlatformError::lastSocketErrorCode();

        std::array<char, 16> payloadBuffer{};
        SocketAddress        peerAddress;
        const ssize_t        receivedByteCount = receiveWithTimeout(receiver, payloadBuffer.data(), payloadBuffer.size(), peerAddress);
        ASSERT_EQ(receivedByteCount, static_cast<ssize_t>(0)) << "没收到空报文（-1 表示压根没到，不是收到零字节）";
        EXPECT_TRUE(isSameIpv4Endpoint(peerAddress, sender.localAddress())) << "空报文也该带来源地址";
    }

    /**
     * @brief 一次批次收包把排在套接字上的多条报文一起交付，内容、顺序与来源地址都要如实
     * @details 这是 QUIC 收包侧的那笔账：一条连接上的几个包本来每包付一次 `recvfrom` 加一次就绪
     *          等待。本层对外的承诺是「一次调用把已经排在套接字上的报文一起交出来」，两侧同形——
     *          实测 Windows 的逐条退化档同样一次交出排好的三条，所以本用例判的是这条共同语义，
     *          把实现退回「每次一条」时红在第一条批次调用只交 1 上。两侧的差别只在**付几次系统调用**
     *          （Linux 一次 `recvmmsg`，Windows 三次 `recvfrom`），那一格不在这里判，写在头文件的
     *          @note 里供读数时对照。
     */
    TEST(DatagramSocket, DeliversQueuedDatagramsAcrossBatchSlots)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        const std::array<std::string_view, 3> payloads{std::string_view{"batch-one"}, std::string_view{"batch-two"}, std::string_view{"batch-three"}};
        for (const std::string_view payload: payloads)
        {
            ASSERT_EQ(sender.send(receiver.localAddress(), payload.data(), payload.size()), static_cast<ssize_t>(payload.size()))
                    << "发送失败，套接字错误码 " << PlatformError::lastSocketErrorCode();
        }

        std::array<std::vector<std::uint8_t>, DatagramSocket::kMaximumBatchSlotCount> slotBuffers{};
        std::array<DatagramSocket::BatchSlot, DatagramSocket::kMaximumBatchSlotCount> slots{};
        for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
        {
            slotBuffers[slotIndex].resize(64);
            slots[slotIndex].buffer   = slotBuffers[slotIndex].data();
            slots[slotIndex].capacity = slotBuffers[slotIndex].size();
        }

        // 第一批：本层对外的承诺是「一次调用把已经排在套接字上的报文一起交出来」，两侧同形
        const ssize_t firstBatchCount = receiveBatchWithTimeout(receiver, slots.data(), slots.size());
        ASSERT_GT(firstBatchCount, 0) << "一次都没收到：批次接口连排好的报文都交不出来";
        EXPECT_EQ(firstBatchCount, static_cast<ssize_t>(payloads.size())) << "一次批次没把排好的三条全交出来（退回「每次一条」的收法时这一格会红在 1）";

        std::vector<std::string> collectedContents{};
        const auto noteBatch = [&collectedContents](const DatagramSocket::BatchSlot *const batchSlots, const std::size_t batchCount, const DatagramSocket &senderSocket)
        {
            for (std::size_t slotIndex = 0; slotIndex < batchCount; ++slotIndex)
            {
                collectedContents.emplace_back(std::string(static_cast<const char *>(batchSlots[slotIndex].buffer), batchSlots[slotIndex].receivedByteCount));
                EXPECT_TRUE(isSameIpv4Endpoint(batchSlots[slotIndex].peerAddress, senderSocket.localAddress())) << "第 " << slotIndex << " 槽的来源地址不是发送方";
            }
        };
        noteBatch(slots.data(), static_cast<std::size_t>(firstBatchCount), sender);

        // 余下的（Windows 那一档要分三批收完）：按截止时刻轮询，不靠调度运气
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (collectedContents.size() < payloads.size() && std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t batchCount = receiveBatchWithTimeout(receiver, slots.data(), slots.size());
            if (batchCount <= 0)
            {
                continue;
            }
            noteBatch(slots.data(), static_cast<std::size_t>(batchCount), sender);
        }

        ASSERT_EQ(collectedContents.size(), payloads.size()) << "批次收包把排好的报文漏掉了（实收 " << collectedContents.size() << " 条）";
        for (std::size_t payloadIndex = 0; payloadIndex < payloads.size(); ++payloadIndex)
        {
            EXPECT_EQ(collectedContents[payloadIndex], payloads[payloadIndex]) << "第 " << payloadIndex << " 条内容与顺序不符：数据报按到达顺序交付";
        }
    }

    /**
     * @brief 一次批次发包把多条报文一起交给内核，接收侧按发出顺序逐条收到
     * @details 与收包那格配对：QUIC 一轮 flush 攒出的多个报文（ACK + 加密握手包 + 若干流数据帧）
     *          由此一次 `sendmmsg` 交出去，而不是每包一次 `sendto`。判据取「三条都到、顺序保持、
     *          返回值就是交出的条数」——把实现退回逐条发时这三条照样绿，红的是下面那格「用法错误
     *          要整批当场拒」：那条一旦写成「发一半再拒」就会让对端收到半批报文。
     */
    TEST(DatagramSocket, SendsQueuedDatagramsInOneBatch)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        const std::array<std::string_view, 3> payloads{std::string_view{"send-batch-one"}, std::string_view{"send-batch-two"}, std::string_view{"send-batch-three"}};
        std::array<DatagramSocket::BatchSendItem, payloads.size()> items{};
        for (std::size_t index = 0; index < payloads.size(); ++index)
        {
            items[index].peerAddress = receiver.localAddress();
            items[index].buffer      = payloads[index].data();
            items[index].length      = payloads[index].size();
        }

        const ssize_t sentCount = sender.sendBatch(items.data(), items.size());
        ASSERT_EQ(sentCount, static_cast<ssize_t>(payloads.size())) << "一次批次没把三条都交给内核，套接字错误码 " << PlatformError::lastSocketErrorCode();

        std::vector<std::string>                                                      receivedContents{};
        std::array<std::vector<std::uint8_t>, DatagramSocket::kMaximumBatchSlotCount> slotBuffers{};
        std::array<DatagramSocket::BatchSlot, DatagramSocket::kMaximumBatchSlotCount> slots{};
        for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
        {
            slotBuffers[slotIndex].resize(64);
            slots[slotIndex].buffer   = slotBuffers[slotIndex].data();
            slots[slotIndex].capacity = slotBuffers[slotIndex].size();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (receivedContents.size() < payloads.size() && std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t batchCount = receiveBatchWithTimeout(receiver, slots.data(), slots.size());
            for (std::size_t slotIndex = 0; slotIndex < static_cast<std::size_t>(std::max(batchCount, static_cast<ssize_t>(0))); ++slotIndex)
            {
                receivedContents.emplace_back(std::string(static_cast<const char *>(slots[slotIndex].buffer), slots[slotIndex].receivedByteCount));
            }
        }

        ASSERT_EQ(receivedContents.size(), payloads.size()) << "批次发出的报文有没到达的（实收 " << receivedContents.size() << " 条）";
        for (std::size_t payloadIndex = 0; payloadIndex < payloads.size(); ++payloadIndex)
        {
            EXPECT_EQ(receivedContents[payloadIndex], payloads[payloadIndex]) << "第 " << payloadIndex << " 条与发出顺序不符：数据报要按序交付";
        }
    }

    /**
     * @brief 批次发包的用法错误整批当场拒，一条也不许先发出去
     * @details 「前 k 条已交、第 k+1 条起未交」是这条通道唯一可能的切分形状（数据报不会部分写出），
     *          所以「发一半再拒第 3 条」会留下一个对端无从分辨的半批。本层选的是动手之前判完。
     */
    TEST(DatagramSocket, SendBatchRejectsTheWholeBatchBeforeSending)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        const std::string                            first = "should-not-go-out";
        const std::string                            third = "bad-shape";
        std::array<DatagramSocket::BatchSendItem, 3> items{};
        items[0].peerAddress = receiver.localAddress();
        items[0].buffer      = first.data();
        items[0].length      = first.size();
        items[1].peerAddress = receiver.localAddress();
        items[1].buffer      = nullptr;
        items[1].length      = 0; // 空报文是合法形状（见 SendsAndReceivesAnEmptyDatagram），这一格要让它照样通过判据
        items[2].peerAddress = receiver.localAddress();
        items[2].buffer      = nullptr;
        items[2].length      = third.size(); // 说有字节却没给缓冲：非法形状排在第三位

        EXPECT_EQ(sender.sendBatch(items.data(), items.size()), static_cast<ssize_t>(-1)) << "整批里有一条非法形状却没当场拒";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument) << "整批拒掉的错码不是「参数不合法」";

        std::array<char, 64> probeBuffer{};
        SocketAddress        probePeer;
        EXPECT_LT(receiver.receive(probeBuffer.data(), probeBuffer.size(), probePeer), 0) << "非法形状被拒之前，前面那些合法报文已经发出去了（半批）";
    }

    /**
     * @brief 批次接口对非法形状当场判错，不等系统调用去报
     * @details 与 `receive()`/`send()` 同一条口径：本端自己能决定的失败要在这层说清并给出改法，
     *          否则调用方拿到的是一个不含起因的 errno。
     */
    TEST(DatagramSocket, ReceiveBatchRejectsInvalidArguments)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid());

        std::array<DatagramSocket::BatchSlot, 2> slots{};
        slots[0].buffer   = nullptr;
        slots[0].capacity = 16;
        EXPECT_EQ(socket.receiveBatch(nullptr, 1), static_cast<ssize_t>(-1)) << "空槽位数组没被判错";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument) << "空槽位数组的错码不是「参数不合法」";
        EXPECT_EQ(socket.receiveBatch(slots.data(), 0), static_cast<ssize_t>(-1)) << "条数 0 没被判错";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument) << "条数 0 的错码不是「参数不合法」";
        EXPECT_EQ(socket.receiveBatch(slots.data(), 1), static_cast<ssize_t>(-1)) << "槽位没带缓冲就该判错：交给内核是 EINVAL，读起来不像本层的错";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument) << "空缓冲槽位的错码不是「参数不合法」";
    }

    /**
     * @brief 没有可读报文时批次交回 0 条，而不是 -1 加一个要调用方猜的错码
     * @details 「此刻没数据」在这条通道上是常态而不是错误：QUIC 的收循环据此等下一次可读再来。
     *          把它报成 -1 会让每一个空闲唤醒都长得像一次读数失败。
     */
    TEST(DatagramSocket, ReceiveBatchReturnsZeroWhenNothingIsQueued)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid());

        std::array<std::vector<std::uint8_t>, 2> slotBuffers{};
        std::array<DatagramSocket::BatchSlot, 2> slots{};
        for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
        {
            slotBuffers[slotIndex].resize(32);
            slots[slotIndex].buffer   = slotBuffers[slotIndex].data();
            slots[slotIndex].capacity = slotBuffers[slotIndex].size();
        }

        EXPECT_EQ(socket.receiveBatch(slots.data(), slots.size()), static_cast<ssize_t>(0)) << "空闲套接字的批次该交 0 条";
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
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kWouldBlock) << "无数据应当报「暂时没有」而不是别的错误";
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

        const std::uint16_t sourcePort  = portOf(source.localAddress());
        const int           displacedFd = target.fileDescriptor();

        target = std::move(source);

        EXPECT_FALSE(source.isValid()) << "移动赋值之后源侧仍自称有效，同一只套接字会被关第二次";
        ASSERT_TRUE(target.isValid());
        EXPECT_EQ(portOf(target.localAddress()), sourcePort) << "赋值之后接过的应是对方那只套接字";

        // 被顶替的那只必须已经关闭：还开着的话 getsockname 照常成功，端口也就一直被占着
        sockaddr_storage staleAddress{};
        socklen_t        staleLength = sizeof(staleAddress);
        EXPECT_NE(::getsockname(displacedFd, reinterpret_cast<sockaddr *>(&staleAddress), &staleLength), 0) << "被顶替的套接字没被关掉，它的端口会一直被占到进程退出";

        // 字面量上的「把自己 std::move 给自己」会被编译器直接判成缺陷（GCC 的 -Wself-move），
        // 而这里要验的正是实现里那道 `this != &other` 自检，因此经由一个别名引用把同一个对象递进去
        DatagramSocket &sameSocket = target;
        target                     = std::move(sameSocket);
        EXPECT_TRUE(target.isValid()) << "自赋值把套接字关掉后又接到自己空出来的描述符上";
        EXPECT_EQ(portOf(target.localAddress()), sourcePort) << "自赋值不该改变已经拥有的套接字";
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（Windows）：绑好的数据报套接字已清掉继承位，POSIX 那一侧由 FD_CLOEXEC 用例覆盖
     * @details Winsock 句柄**默认就是可继承的**，只有创建路径显式清这一位才不泄漏给子进程；这与
     *          POSIX「默认不继承、要靠 SOCK_CLOEXEC 才不继承」正好相反，所以两侧各钉一条而不是共用。
     *          漏清时 worker 子进程会替父进程持着这个端口，父进程退出后重启即报地址占用。
     */
    TEST(DatagramSocket, BoundSocketIsNotInheritable)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket socket = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(socket.isValid());

        EXPECT_TRUE(TestSupport::isNotInheritable(socket.fileDescriptor())) << "绑好的数据报套接字可被继承：worker 子进程会在父进程退出之后继续占着这个端口";
    }
#endif

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
        EXPECT_TRUE((descriptorFlags & FD_CLOEXEC) != 0) << "描述符没标 FD_CLOEXEC，子进程会继承它并占住这个 UDP 端口（标志实际为 " << descriptorFlags << "）";
    }
#endif

    /**
     * @brief 报文比缓冲大时按容量截断交付：来源地址照旧可用，且整条报文算已消费
     * @details 这是 receive() 在 Windows 上唯一「把失败的系统调用当成功交付」的出口（WSAEMSGSIZE），
     *          Linux 侧同形状由 recvfrom 静默截断。两侧必须给同一个口径，否则上层得按平台分支读数。
     *          来源地址尤其要紧：截断的调用在 Windows 上是「失败」回来的，地址字段是否已被内核写进
     *          去没有承诺，而调用方拿它决定回包去向。
     */
    TEST(DatagramSocket, OversizedDatagramIsTruncatedToCapacityAndKeepsPeerAddress)
    {
        ASSERT_TRUE(Socket::initialize());

        const DatagramSocket receiver = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(receiver.isValid());
        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        // 取值带位置信息：只比长度的话「错位交付」也能等长，按位置比才看得出来
        constexpr std::size_t kPayloadBytes = 4096U;
        std::vector<char>     payload(kPayloadBytes);
        for (std::size_t index = 0; index < payload.size(); ++index)
        {
            payload[index] = static_cast<char>('A' + index % 26U);
        }
        ASSERT_EQ(sender.send(receiver.localAddress(), payload.data(), payload.size()), static_cast<ssize_t>(kPayloadBytes));

        // 缓冲用满：交付长度应当恰好等于容量（截断而不是拒收），且缓冲区每个字节都被写过
        constexpr std::size_t            kCapacityBytes = 64U;
        std::array<char, kCapacityBytes> buffer{};
        buffer.fill('\0');
        SocketAddress peerAddress;
        const ssize_t receivedByteCount = receiveWithTimeout(receiver, buffer.data(), buffer.size(), peerAddress);
        ASSERT_EQ(receivedByteCount, static_cast<ssize_t>(kCapacityBytes)) << "比缓冲大的报文没有按容量交付：Windows 那侧把它当硬错误丢掉、Linux 那侧短于容量都算口径不符";
        EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + receivedByteCount, payload.begin())) << "交付的不是报文开头的那一段，截断把数据错位了";
        EXPECT_TRUE(isSameIpv4Endpoint(peerAddress, sender.localAddress()))
                << "截断时交付了数据却丢了来源地址（端口 " << portOf(peerAddress) << " 与 " << portOf(sender.localAddress()) << "）：调用方会照着它把回包发进黑洞";

        // 数据报按整条交付：截断丢掉的后半不该留在队列里被下一次读拿到
        std::array<char, kCapacityBytes> leftover{};
        SocketAddress                    leftoverPeer;
        const ssize_t                    leftoverByteCount = receiver.receive(leftover.data(), leftover.size(), leftoverPeer);
        EXPECT_EQ(leftoverByteCount, -1) << "截断之后套接字里还剩 " << leftoverByteCount << " 字节：UDP 该丢掉整条报文而不是留半截";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kWouldBlock) << "截断后没有回到「无数据可读」态，错误码也不可信";
    }

    /**
     * @brief 接管已绑定的数据报套接字：端口跟着过来、报文收得到，而且接手方是非阻塞的
     * @details 这条钉的是「一条 UDP 端口给多个进程用」在数据报侧的落点：接手方拿到的不只是一个端口号，
     *          而是「往这个端口发的报文会落在我这里」。非阻塞单独判一次——交过来的套接字通常是阻塞态
     *          （Windows 按协议信息重建出来就是阻塞的），不改会把事件循环卡在 recvfrom 上。
     */
    TEST(DatagramSocket, AdoptsBoundDatagramSocketAndStillReceives)
    {
        ASSERT_TRUE(Socket::initialize());

        std::uint16_t port      = 0U;
        const int     rawSocket = rawBoundUdpSocket(port);
        ASSERT_GE(rawSocket, 0) << "夹具没能绑出端口，错误码 " << PlatformError::lastSocketErrorCode();

        auto adopted = DatagramSocket::adopt(rawSocket);
        ASSERT_TRUE(adopted.has_value()) << "接管失败，错误码 " << adopted.error().value();
        ASSERT_TRUE(adopted->isValid());

        // 端口是从那枚描述符问回来的：接手方不该知道调用方当初绑的是哪个端口
        EXPECT_EQ(portOf(adopted->localAddress()), port);

        // 此刻还没有人发过报文：阻塞的话这一步就不会返回，所以这条判据不靠调度时序
        std::array<char, 16> probe{};
        SocketAddress        probePeer{};
        EXPECT_EQ(adopted->receive(probe.data(), probe.size(), probePeer), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kWouldBlock) << "接手方没被置成非阻塞";

        const DatagramSocket sender = DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid()) << "发送侧绑定失败，错误码 " << PlatformError::lastSocketErrorCode();
        constexpr std::string_view kMessage = "take-me";
        ASSERT_EQ(sender.send(makeLoopbackAddress(port), kMessage.data(), kMessage.size()), static_cast<ssize_t>(kMessage.size()));

        std::array<char, 16> buffer{};
        SocketAddress        peer{};
        const ssize_t        receivedByteCount = receiveWithTimeout(*adopted, buffer.data(), buffer.size(), peer);
        ASSERT_EQ(receivedByteCount, static_cast<ssize_t>(kMessage.size())) << "发往该端口的报文没落到接手方手里";
        EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(receivedByteCount)), kMessage);
        EXPECT_TRUE(isSameIpv4Endpoint(peer, sender.localAddress())) << "接手方收到的报文没带上来源";
    }

    /**
     * @brief 拒绝面：描述符无效、类型不是数据报、还没 bind 三档分开拒，且拒的时候不动调用方的句柄
     * @details 静默接下来各自的代价：流套接字上「收数据报」永远收不到东西；没 bind 的套接字没有端口，
     *          接过来只是一台谁也不认识的服务器。第四档「句柄有效但不是套接字」只在 POSIX 上造得出输入，
     *          由 ReportsADistinctCauseWhenTheHandleIsNotASocket 判。
     *          不关描述符是所有权约定——**只在成功时**接管。
     */
    TEST(DatagramSocket, RejectsDescriptorsItCannotAdopt)
    {
        ASSERT_TRUE(Socket::initialize());

        const auto invalid = DatagramSocket::adopt(-1);
        ASSERT_FALSE(invalid.has_value());
        EXPECT_EQ(invalid.error(), std::make_error_code(std::errc::bad_file_descriptor));

        std::uint16_t tcpPort = 0U;
        const int     stream  = rawListeningTcpSocket(tcpPort);
        ASSERT_GE(stream, 0) << "夹具没能造出监听套接字";
        const auto wrongType = DatagramSocket::adopt(stream);
        ASSERT_FALSE(wrongType.has_value());
        EXPECT_EQ(wrongType.error(), std::make_error_code(std::errc::not_supported));
        // 被拒之后描述符还得能用：问得出类型就说明它仍然归调用方，接管方没有顺手关掉它
        int       stillThereType = 0;
        socklen_t typeLength     = static_cast<socklen_t>(sizeof(stillThereType));
        EXPECT_EQ(::getsockopt(stream, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&stillThereType), &typeLength), 0) << "接管失败却把调用方的描述符关掉了";
        static_cast<void>(FileDescriptor::close(stream));

        const int unbound = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ASSERT_GE(unbound, 0);
        const auto notBound = DatagramSocket::adopt(unbound);
        ASSERT_FALSE(notBound.has_value());
        EXPECT_EQ(notBound.error(), std::make_error_code(std::errc::invalid_argument));
        static_cast<void>(FileDescriptor::close(unbound));
    }

    /**
     * @brief 「句柄有效但根本不是套接字」单独占一档，不许并到「类型不是数据报」里去
     * @details 两档的下一步完全不同：前者要换传进来的东西（多半是把文件描述符当移交结果用了），后者要
     *          查交出方送过来的类型。合成 `not_supported` 的话，前者会被读成「本平台不支持接管」而去换
     *          平台——那正是这条并档最贵的一种误读。
     * @note 只在 POSIX 上判（理由写在 plainFileHandle 的说明里）
     * @note 证伪：把这两档合回一个分支（问不出类型也报 not_supported），本条红
     */
    TEST(DatagramSocket, ReportsADistinctCauseWhenTheHandleIsNotASocket)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "Windows 的整数句柄空间里造不出这一档：普通文件的 CRT 描述符与 SOCKET 不同域，且本层 close() 走 closesocket";
#else
        ASSERT_TRUE(Socket::initialize());

        const int fileHandle = plainFileHandle();
        ASSERT_GE(fileHandle, 0) << "夹具没能打开一个非套接字的句柄";

        const auto adopted = DatagramSocket::adopt(fileHandle);
        ASSERT_FALSE(adopted.has_value()) << "一枚普通文件句柄被当成数据报套接字接管了";
        EXPECT_EQ(adopted.error(), std::make_error_code(std::errc::not_a_socket)) << "报成了「类型不是数据报」：那是另一档成因，下一步要查的不是同一个地方";
        // 拒的时候仍然不动调用方的句柄：这条与上一档共用同一条所有权纪律
        static_cast<void>(FileDescriptor::close(fileHandle));
#endif
    }
} // namespace AsynGyanis::Platform
