/**
 * @file TestSocketHandoff.cpp
 * @brief 监听套接字跨通道移交的用例：交出的一定要能在收端接着 accept，坏消息必须整体作废
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/Socket.h"

#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"
#include "PlatformTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 本机 loopback 上的等待上界：几百毫秒已经很富余，只用来兜住「通道根本没数据」这种情况
        constexpr int kChannelWaitMilliseconds = 3000;

        /// 一个回环 IPv4 端点
        sockaddr_in loopbackAddress(const std::uint16_t port)
        {
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = htons(port);
            return address;
        }

        /**
         * @brief 把 sockaddr_in 包成本层的地址对（存储 + 长度）
         * @param address IPv4 端点
         * @return SocketAddress 可直接交给套接字接口的地址
         */
        SocketAddress socketAddressOf(const sockaddr_in &address)
        {
            SocketAddress wrapped;
            std::memcpy(&wrapped.storage, &address, sizeof(address));
            wrapped.length = static_cast<socklen_t>(sizeof(address));
            return wrapped;
        }

        /**
         * @brief 有界等到一条报文（数据报侧的收法：非阻塞描述符要轮询）
         * @param descriptor 已绑定的数据报描述符
         * @param buffer 接收缓冲
         * @param capacity 缓冲容量
         * @return ssize_t 收到的字节数；期限到了仍没有报文则 -1
         */
        ssize_t receiveDatagramWithDeadline(const int descriptor, void *buffer, const std::size_t capacity)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kChannelWaitMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t received = ::recvfrom(descriptor, static_cast<char *>(buffer), static_cast<int>(capacity), 0, nullptr, nullptr);
                if (received >= 0)
                {
                    return received;
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
         * @brief 数一遍本进程当前打开的描述符（POSIX 的数法：/proc/self/fd 的条目数）
         * @return std::size_t 条目数；读不到目录时返回 0，由调用方按「没法判」跳过
         * @details 只用来判「拒一条移交时有没有留下活描述符」这种前后差值，绝对值不参与断言
         *          （本函数自己开的那份目录句柄在两侧计数里都在）。
         */
        std::size_t openDescriptorCount()
        {
            std::error_code error;
            std::size_t     entryCount = 0;
            for (const auto &entry: std::filesystem::directory_iterator("/proc/self/fd", error))
            {
                static_cast<void>(entry);
                ++entryCount;
            }
            return error ? 0 : entryCount;
        }

        /**
         * @brief 造一个已在 127.0.0.1 上监听、端口由内核挑的套接字
         * @param[out] port 实际端口
         * @return int 监听描述符；失败为 -1
         */
        int makeLoopbackListener(std::uint16_t &port)
        {
            port               = 0U;
            const int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (listener < 0)
            {
                return -1;
            }
            sockaddr_in address = loopbackAddress(0U);
            if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0)
            {
                FileDescriptor::close(listener);
                return -1;
            }
            sockaddr_in bound{};
            socklen_t   boundLength = static_cast<socklen_t>(sizeof(bound));
            if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0)
            {
                FileDescriptor::close(listener);
                return -1;
            }
            port = ntohs(bound.sin_port);
            return listener;
        }

        /**
         * @brief 造一对已连通的阻塞套接字，当移交通道
         * @param[out] sendingEnd 写端
         * @param[out] receivingEnd 读端
         * @return true 两端都建成并连通
         * @details 通道类型按平台分家，这不是冗余分支而是这条链的硬边界：POSIX 上内核只在 unix 域里
         *          随 SCM_RIGHTS 送描述符，拿 loopback TCP 当通道时 sendmsg/recvmsg 双双成功、字节一字
         *          不差，只有描述符被静默丢掉，收端只剩 EBADF 可报（实测两条通道各跑一遍对照过，
         *          见 Platform/IO/Socket.h 的 @note）。Windows 那边载荷本身就是一串字节
         *          （WSADuplicateSocket 换回的协议信息表），loopback TCP 正是它能用的通道。
         */
        bool makeBlockingChannel(int &sendingEnd, int &receivingEnd)
        {
            sendingEnd   = -1;
            receivingEnd = -1;

#if ASYN_PLATFORM_WIN32
            std::uint16_t channelPort = 0U;
            const int     listener    = makeLoopbackListener(channelPort);
            if (listener < 0)
            {
                return false;
            }

            const int writer = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (writer < 0)
            {
                FileDescriptor::close(listener);
                return false;
            }
            sockaddr_in peer = loopbackAddress(channelPort);
            if (::connect(writer, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer)) != 0)
            {
                FileDescriptor::close(writer);
                FileDescriptor::close(listener);
                return false;
            }
            receivingEnd = static_cast<int>(::accept(listener, nullptr, nullptr));
            FileDescriptor::close(listener);
            if (receivingEnd < 0)
            {
                FileDescriptor::close(writer);
                return false;
            }
            sendingEnd = writer;
            return true;
#else
            int pair[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
            {
                return false;
            }
            sendingEnd   = pair[0];
            receivingEnd = pair[1];
            return true;
#endif
        }

        /// 本轮用过的描述符，析构时统一关掉
        class DescriptorCleanup
        {
        public:
            void add(const int descriptor) noexcept
            {
                if (descriptor >= 0)
                {
                    m_descriptors.push_back(descriptor);
                }
            }

            ~DescriptorCleanup()
            {
                for (const int descriptor: m_descriptors)
                {
                    FileDescriptor::close(descriptor);
                }
            }

        private:
            std::vector<int> m_descriptors;
        };
    } // namespace

    /**
     * @brief 交出与收下是一对操作：收端拿到的必须是一个还能 accept 的监听套接字
     *
     * @details 这是零停机换代的核心性质——新进程接手的不该只是一个数字，而是「带着 backlog 的监听态」。
     *          通道用各平台真能送描述符的那条（见 makeBlockingChannel），因此这条同时钉住了线路格式
     *          （定长头 + 平台载体）与
     *          「收端重建的套接字确实能服务连接」两件事。
     * @note 通道上的读一律直接阻塞读，不用 TestSupport::waitForReadable：那个助手是真把字节读走再
     *       丢掉（它判的是「能不能读到」），先跑它就把移交消息的头吞了。第一次排查这条用例时正是
     *       它让收端读到一串全零，看着像格式错，其实是被截了头。
     * @note 目标进程取本进程：Windows 上 WSADuplicateSocket 按进程号认目标，交给自己也走同一条
     *       编码/传输/重建路径；换成正真另起一个进程只差目标号，那段路径由编排层用例覆盖。
     */
    TEST(SocketHandoff, DeliversAWorkingListeningSocketThroughTheChannel)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        std::uint16_t     listeningPort = 0U;
        const int         listener      = makeLoopbackListener(listeningPort);
        ASSERT_GE(listener, 0) << "没造出监听套接字，错误码 " << PlatformError::lastErrorCode();
        cleanup.add(listener);

        int channelWriter = -1;
        int channelReader = -1;
        ASSERT_TRUE(makeBlockingChannel(channelWriter, channelReader));
        cleanup.add(channelWriter);
        cleanup.add(channelReader);

        const auto selfProcessId = static_cast<std::uint64_t>(ProcessInfo::currentProcessId());
        ASSERT_TRUE(Socket::writeListeningSocketHandoff(channelWriter, listener, selfProcessId)) << "交出监听套接字失败，错误码 " << PlatformError::lastErrorCode();

        const int received = Socket::readListeningSocketHandoff(channelReader);
        ASSERT_GE(received, 0) << "收端没能重建监听套接字，错误码 " << PlatformError::lastErrorCode();
        cleanup.add(received);

        // 交来的套接字必须真的在那个端口上听着：连它，并在收端 accept 出一条会话
        const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(client, 0);
        cleanup.add(client);
        sockaddr_in serverEndpoint = loopbackAddress(listeningPort);
        ASSERT_EQ(::connect(client, reinterpret_cast<const sockaddr *>(&serverEndpoint), sizeof(serverEndpoint)), 0)
                << "端口 " << listeningPort << " 上没人听了——交来的套接字不带监听态";

        const int accepted = static_cast<int>(::accept(received, nullptr, nullptr));
        ASSERT_GE(accepted, 0) << "收端 accept 失败：交接过来的不是可用的监听套接字";
        cleanup.add(accepted);

        constexpr std::string_view kPing = "ping";
        ASSERT_EQ(::send(client, kPing.data(), static_cast<int>(kPing.size()), 0), static_cast<int>(kPing.size()));
        std::array<char, 8> arrived{};
        const int           arrivedLength = ::recv(accepted, arrived.data(), static_cast<int>(kPing.size()), 0);
        ASSERT_EQ(arrivedLength, static_cast<int>(kPing.size()));
        EXPECT_EQ(std::string_view(arrived.data(), arrivedLength), kPing) << "会话两端拿到的字节不同";

        // 原监听口仍然可用：换代是「接手」，交接本身不该让老监听口失效（回滚路径要靠它继续服务）
        const int secondClient = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(secondClient, 0);
        cleanup.add(secondClient);
        ASSERT_EQ(::connect(secondClient, reinterpret_cast<const sockaddr *>(&serverEndpoint), sizeof(serverEndpoint)), 0);
        const int stillWorking = static_cast<int>(::accept(listener, nullptr, nullptr));
        ASSERT_GE(stillWorking, 0) << "原监听口 accept 失败：它已经不再服务这个端口";
        cleanup.add(stillWorking);
    }

    /**
     * @brief 通道里只有半截消息、或载荷长度不像本平台的格式时，收端必须整体作废
     * @details 「失败」与「看起来成功但拿到半个套接字」的代价差得远：后者会一路走到 accept 才炸，
     *          那时已经分不清是移交坏了还是别的东西坏了。
     */
    TEST(SocketHandoff, RejectsIncompleteOrForeignMessagesInsteadOfHalfASocket)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        int               channelWriter = -1;
        int               channelReader = -1;
        ASSERT_TRUE(makeBlockingChannel(channelWriter, channelReader));
        cleanup.add(channelWriter);
        cleanup.add(channelReader);

        // 只写两个字节就把写端关掉：读端拿到的是「消息不完整」，不是可用套接字
        const std::array<char, 2> truncated{'A', 'B'};
        ASSERT_EQ(::send(channelWriter, truncated.data(), static_cast<int>(truncated.size()), 0), 2);
        FileDescriptor::close(channelWriter);
        EXPECT_EQ(Socket::readListeningSocketHandoff(channelReader), -1) << "半截消息被当成了有效移交";

        // 载荷长度不像本平台的载体：那是别的版本或别的平台写来的，不能猜着读完再建套接字。
        // 判据不是「返回 -1」（拿垃圾去建也会失败），而是「拒得很早」：收端一个字节的载荷都不该碰，
        // 所以那四个字节还原样躺在通道里等着被读出来。
        int secondWriter = -1;
        int secondReader = -1;
        ASSERT_TRUE(makeBlockingChannel(secondWriter, secondReader));
        cleanup.add(secondWriter);
        cleanup.add(secondReader);
        struct
        {
            std::uint16_t family;
            std::uint16_t socketType;
            std::uint32_t blobByteCount;
        } foreignHeader{AF_INET, SOCK_STREAM, 4U};
        ASSERT_EQ(::send(secondWriter, reinterpret_cast<const char *>(&foreignHeader), static_cast<int>(sizeof(foreignHeader)), 0), static_cast<int>(sizeof(foreignHeader)));
        ASSERT_EQ(::send(secondWriter, "WXYZ", 4, 0), 4);
        FileDescriptor::close(secondWriter);

        EXPECT_EQ(Socket::readListeningSocketHandoff(secondReader), -1) << "载荷长度不合本平台的格式，却被当成有效移交";
        std::array<char, 4> leftover{};
        ASSERT_EQ(::recv(secondReader, leftover.data(), static_cast<int>(leftover.size()), 0), 4) << "收端在读格式之前就把载荷吃掉了：那不是「拒绝」，是「猜着解」";
        EXPECT_EQ(std::string_view(leftover.data(), leftover.size()), "WXYZ") << "被读走的字节应当原样留在通道里";
    }

    /**
     * @brief 参数非法要在动通道之前就拒掉
     */
    TEST(SocketHandoff, RefusesInvalidArgumentsBeforeTouchingTheChannel)
    {
        ASSERT_TRUE(Socket::initialize());

        int channelWriter = -1;
        int channelReader = -1;
        ASSERT_TRUE(makeBlockingChannel(channelWriter, channelReader));
        DescriptorCleanup cleanup;
        cleanup.add(channelWriter);
        cleanup.add(channelReader);

        EXPECT_FALSE(Socket::writeListeningSocketHandoff(-1, channelReader, 1U));
        EXPECT_FALSE(Socket::writeListeningSocketHandoff(channelWriter, -1, 1U));
        EXPECT_EQ(Socket::readListeningSocketHandoff(-1), -1);
    }
    /**
     * @brief 有界等待：没人来连时必须在预算内报「没有」，而不是停在 accept 上不返回
     * @details 编排层拿它判「worker 起来了却没连通道」那条出口。写成无限阻塞的话，一个连不上
     *          通道的子进程会冻住整池的补位与收尾——而那正是这套编排要处理的场景。
     */
    TEST(SocketHandoff, WaitForAcceptReadyTimesOutWhenNobodyConnects)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        std::uint16_t     port     = 0U;
        const int         listener = makeLoopbackListener(port);
        ASSERT_GE(listener, 0) << "没造出监听套接字，错误码 " << PlatformError::lastErrorCode();

        const auto started = std::chrono::steady_clock::now();
        const auto pending = Socket::waitForAcceptReady(listener, std::chrono::milliseconds{200});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        ASSERT_TRUE(pending.has_value()) << "等待本身失败，错误码 " << pending.error().value();
        EXPECT_FALSE(*pending) << "没人连这条通道，却报成有连接 pending";
        // 上界给到 5 秒只用来分辨「按期返回」与「根本没返回」：这条要抓的是无限阻塞那种形状，
        // 不是替 200 毫秒计时
        EXPECT_LT(elapsed, std::chrono::seconds{5}) << "预算 200 毫秒却没按期返回：那是一次无限阻塞";

        // 0 预算是「只取当前状态」，同样不该被读成一次失败
        const auto sampled = Socket::waitForAcceptReady(listener, std::chrono::milliseconds::zero());
        ASSERT_TRUE(sampled.has_value()) << "零预算的一次取样被报成失败：它要的答案只是有没有";
        EXPECT_FALSE(*sampled);

        // 负数是写错了预算：不静默当成「无限等」，也不当成「零取样」，直接点名
        const auto invalid = Socket::waitForAcceptReady(listener, std::chrono::milliseconds{-1});
        ASSERT_FALSE(invalid.has_value());
        EXPECT_EQ(invalid.error().value(), static_cast<int>(std::errc::invalid_argument));
    }

    /**
     * @brief 已排队的连接要报得出「有」，而且紧接着的 accept 真拿得到它
     * @details 只报「有」而 accept 落空，等于把一次就绪读成一次失败；反过来 accept 能成而等待报
     *          「没有」，编排层就会把一个正常连上来的 worker 判死。
     */
    TEST(SocketHandoff, WaitForAcceptReadyReportsAPendingConnection)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        std::uint16_t     port     = 0U;
        const int         listener = makeLoopbackListener(port);
        ASSERT_GE(listener, 0) << "没造出监听套接字，错误码 " << PlatformError::lastErrorCode();
        cleanup.add(listener);

        const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(client, 0);
        cleanup.add(client);
        sockaddr_in endpoint = loopbackAddress(port);
        // connect() 在回环上返回成功就意味着三次握手已落进接受队列，等待不必再赌时序
        ASSERT_EQ(::connect(client, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)), 0) << "回环上连自己的监听口都连不上";

        const auto pending = Socket::waitForAcceptReady(listener, std::chrono::milliseconds{3000});
        ASSERT_TRUE(pending.has_value()) << "等待本身失败，错误码 " << pending.error().value();
        EXPECT_TRUE(*pending) << "已经排好队的连接没被报出来";

        const int accepted = Socket::acceptHandoffPeer(listener);
        ASSERT_GE(accepted, 0) << "报「有」之后 accept 拿不到东西，那次报读等于没报";
        cleanup.add(accepted);
    }
    /**
     * @brief 数据报套接字也走得通这条路：交出去的那一份仍替那个端口收报文
     * @details 移交机制本身不限套接字类型，Windows 的多进程 UDP 服务靠的就是这一点（那边没有
     *          SO_REUSEPORT，多个进程各自 bind 同一端口时内核只把报文给最后绑上的那个）。这条把
     *          「交出 → 收下 → 接管」三段串起来，判据是真发一条报文给那个端口、在接手方读到它。
     */
    TEST(SocketHandoff, DeliversAWorkingDatagramSocketThroughTheChannel)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        const int         receiver = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ASSERT_GE(receiver, 0);
        cleanup.add(receiver);
        sockaddr_in bound = loopbackAddress(0);
        ASSERT_EQ(::bind(receiver, reinterpret_cast<const sockaddr *>(&bound), sizeof(bound)), 0);
        socklen_t boundLength = static_cast<socklen_t>(sizeof(bound));
        ASSERT_EQ(::getsockname(receiver, reinterpret_cast<sockaddr *>(&bound), &boundLength), 0);
        const std::uint16_t port = ntohs(bound.sin_port);

        int channelWriter = -1;
        int channelReader = -1;
        ASSERT_TRUE(makeBlockingChannel(channelWriter, channelReader));
        cleanup.add(channelWriter);
        cleanup.add(channelReader);

        const auto selfProcessId = static_cast<std::uint64_t>(ProcessInfo::currentProcessId());
        ASSERT_TRUE(Socket::writeListeningSocketHandoff(channelWriter, receiver, selfProcessId)) << "交出数据报套接字失败，错误码 " << PlatformError::lastErrorCode();

        const int adoptedDescriptor = Socket::readListeningSocketHandoff(channelReader);
        ASSERT_GE(adoptedDescriptor, 0) << "收端没能重建数据报套接字，错误码 " << PlatformError::lastSocketErrorCode();
        // 接手侧要自己把继承位清掉：交出方的标志位传不过来（两侧都是新建的一份引用，POSIX 上内核
        // 给新描述符时把 FD_CLOEXEC 清成 0），不补就等于让新一代此后派生的每个子进程都替这个端口
        // 留一份持有
        EXPECT_TRUE(TestSupport::isNotInheritable(adoptedDescriptor)) << "接手来的套接字仍可随进程创建继承，端口关不干净";
        // 交接交回来的是一枚裸描述符：接管动作（判类型、判已 bind、置非阻塞）在 DatagramSocket 那侧
        auto adopted = DatagramSocket::adopt(adoptedDescriptor);
        if (!adopted.has_value())
        {
            // 接管失败时描述符仍归调用方，这时才需要夹具兜底；成功后它已由 adopted 的析构负责，
            // 再登记一份就是同枚描述符关两次（第二次会打到别人刚拿到的同号描述符上）
            cleanup.add(adoptedDescriptor);
        }
        ASSERT_TRUE(adopted.has_value()) << "接管失败，错误码 " << adopted.error().value();

        // 端口跟着过来：接手方问出来的端口就是交出方当初绑的那个
        sockaddr_in adoptedLocal{};
        socklen_t   adoptedLocalLength = static_cast<socklen_t>(sizeof(adoptedLocal));
        ASSERT_EQ(::getsockname(adoptedDescriptor, reinterpret_cast<sockaddr *>(&adoptedLocal), &adoptedLocalLength), 0);
        EXPECT_EQ(ntohs(adoptedLocal.sin_port), port) << "接手方的端口与交出方的不是一回事";

        const DatagramSocket sender = DatagramSocket::bindTo(socketAddressOf(loopbackAddress(0U)));
        ASSERT_TRUE(sender.isValid());
        constexpr std::string_view kMessage = "to-adopted";
        ASSERT_EQ(sender.send(socketAddressOf(loopbackAddress(port)), kMessage.data(), kMessage.size()), static_cast<ssize_t>(kMessage.size()));

        std::array<char, 32> buffer{};
        const ssize_t        arrived = receiveDatagramWithDeadline(adoptedDescriptor, buffer.data(), buffer.size());
        ASSERT_EQ(arrived, static_cast<ssize_t>(kMessage.size())) << "发往那个端口的报文没落到接手方手里：交过来的那份不收报文";
        EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(arrived)), kMessage);
    }

    /**
     * @brief 头里写的类型与实际交过来的套接字对不上时整体作废，而不是照建一个用
     * @details 头那两项是交出方从**同一个套接字**问出来的，所以对不上只可能是消息被写坏、版本不对
     *          或通道上跑的根本不是移交载荷。那种情况下重建出来的套接字收不到任何报文（数据报的口径
     *          与流的口径不是一回事），而调用方以为接手成功了。
     * @note 伪造方式按平台分两条：Windows 的载荷是普通字节，先做一次真移交、把协议信息原样搬过来
     *       再改头里的类型；POSIX 的描述符走控制消息，只能自己写头并随 SCM_RIGHTS 送一枚数据报
     *       描述符过去。两条都判同一件事：读端报 EINVAL 且不交回任何描述符。
     */
    TEST(SocketHandoff, RejectsAHandoffWhoseHeaderDisagreesWithTheSocket)
    {
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        const int         datagram = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ASSERT_GE(datagram, 0);
        cleanup.add(datagram);
        sockaddr_in bound = loopbackAddress(0);
        ASSERT_EQ(::bind(datagram, reinterpret_cast<const sockaddr *>(&bound), sizeof(bound)), 0);

        struct
        {
            std::uint16_t family;
            std::uint16_t socketType;
            std::uint32_t blobByteCount;
        } header{AF_INET, SOCK_STREAM, 0U};

        int channelWriter = -1;
        int channelReader = -1;
#if ASYN_PLATFORM_WIN32
        // 先取一份**真的**协议信息：它只能由交出方生成，伪造不了，所以伪造的只有头
        int realWriter = -1;
        int realReader = -1;
        ASSERT_TRUE(makeBlockingChannel(realWriter, realReader));
        cleanup.add(realWriter);
        cleanup.add(realReader);
        ASSERT_TRUE(Socket::writeListeningSocketHandoff(realWriter, datagram, static_cast<std::uint64_t>(ProcessInfo::currentProcessId())));
        std::array<char, sizeof(header)> realHeader{};
        ASSERT_EQ(::recv(realReader, realHeader.data(), static_cast<int>(realHeader.size()), 0), static_cast<int>(realHeader.size()));
        std::memcpy(&header, realHeader.data(), sizeof(header));
        std::vector<char> blob(header.blobByteCount);
        ASSERT_GT(blob.size(), 0U);
        ASSERT_EQ(::recv(realReader, blob.data(), static_cast<int>(blob.size()), 0), static_cast<int>(blob.size()));
        header.socketType = static_cast<std::uint16_t>(SOCK_STREAM) == header.socketType ? static_cast<std::uint16_t>(SOCK_DGRAM) : static_cast<std::uint16_t>(SOCK_STREAM);

        ASSERT_TRUE(makeBlockingChannel(channelWriter, channelReader));
        cleanup.add(channelWriter);
        cleanup.add(channelReader);
        ASSERT_EQ(::send(channelWriter, reinterpret_cast<const char *>(&header), static_cast<int>(sizeof(header)), 0), static_cast<int>(sizeof(header)));
        ASSERT_EQ(::send(channelWriter, blob.data(), static_cast<int>(blob.size()), 0), static_cast<int>(blob.size()));
#else
        int pair[2] = {-1, -1};
        // unix 流套接字才是这条通道能送描述符的形状（见 makeBlockingChannel 的那条 @details）
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0) << "造不出 unix 通道";
        channelWriter = pair[0];
        channelReader = pair[1];
        cleanup.add(channelWriter);
        cleanup.add(channelReader);

        header.socketType    = SOCK_STREAM;
        header.blobByteCount = 0U;
        iovec  dataVector{&header, sizeof(header)};
        char   control[CMSG_SPACE(sizeof(int))] = {};
        msghdr message{};
        message.msg_iov           = &dataVector;
        message.msg_iovlen        = 1;
        message.msg_control       = control;
        message.msg_controllen    = sizeof(control);
        cmsghdr *controlHeader    = CMSG_FIRSTHDR(&message);
        controlHeader->cmsg_level = SOL_SOCKET;
        controlHeader->cmsg_type  = SCM_RIGHTS;
        controlHeader->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(controlHeader), &datagram, sizeof(datagram));
        ASSERT_GE(::sendmsg(channelWriter, &message, 0), 0) << "伪造的移交消息没写进通道";
#endif

        const int rejected = Socket::readListeningSocketHandoff(channelReader);
        EXPECT_EQ(rejected, -1) << "头里写着流套接字、交过来的却是数据报，这样一份不可信的消息被当成了成功";
        // 读端报原因走的是 setLastErrorCode（Windows 上 socket 错误码是另一个槽位，只有系统调用
        // 自己写它），所以这里取 lastErrorCode 而不是 lastSocketErrorCode
        EXPECT_EQ(PlatformError::lastErrorCode(), EINVAL);
    }

    /**
     * @brief 拒掉一条不可信的移交时，也要把**已经装进本进程**的描述符关掉
     * @details recvmsg 一成功返回，内核就把 SCM_RIGHTS 里那枚描述符交到了本进程手上；此后每一条
     *          失败出口都得关它。换代是旧进程交给新进程，还要按 worker 补位重复许多轮，漏下来的就是
     *          每个槽位一枚永不回收的描述符——症状要等长跑撞到上限才看得见。
     * @note 只在 POSIX 上判：Windows 的载荷是普通字节，重建发生在所有格式判定之后，本就没有
     *       「先装进来再退回」这一步
     * @note 证伪：把顺序改回「先判格式、不过就 return -1」（描述符留在那儿没人关），本条红
     */
    TEST(SocketHandoff, ClosesTheInstalledDescriptorWhenTheHandoffFormatIsRejected)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "Windows 侧失败都在重建之前，没有需要回滚的描述符";
#else
        ASSERT_TRUE(Socket::initialize());

        DescriptorCleanup cleanup;
        const int         datagram = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ASSERT_GE(datagram, 0);
        cleanup.add(datagram);
        sockaddr_in bound = loopbackAddress(0);
        ASSERT_EQ(::bind(datagram, reinterpret_cast<const sockaddr *>(&bound), sizeof(bound)), 0);

        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0) << "造不出 unix 通道";
        cleanup.add(pair[0]);
        cleanup.add(pair[1]);

        // 头里写着一个不属于本平台的载荷长度（POSIX 上恒为 0）：字节与描述符都会正常到达，
        // 判据仍该整条作废——而作废就得把收到的那枚退回去
        struct
        {
            std::uint16_t family;
            std::uint16_t socketType;
            std::uint32_t blobByteCount;
        } header{AF_INET, SOCK_DGRAM, 4096U};

        iovec  dataVector{&header, sizeof(header)};
        char   control[CMSG_SPACE(sizeof(int))] = {};
        msghdr message{};
        message.msg_iov        = &dataVector;
        message.msg_iovlen     = 1;
        message.msg_control    = control;
        message.msg_controllen = sizeof(control);
        cmsghdr *controlHeader = CMSG_FIRSTHDR(&message);
        controlHeader->cmsg_level = SOL_SOCKET;
        controlHeader->cmsg_type  = SCM_RIGHTS;
        controlHeader->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(controlHeader), &datagram, sizeof(datagram));
        ASSERT_EQ(::sendmsg(pair[0], &message, 0), static_cast<ssize_t>(sizeof(header))) << "伪造的移交消息没写进通道";

        const std::size_t descriptorsBefore = openDescriptorCount();
        ASSERT_NE(descriptorsBefore, 0U) << "读不到 /proc/self/fd，这条判据没法成立";

        EXPECT_EQ(Socket::readListeningSocketHandoff(pair[1]), -1) << "格式不对的移交被当成了成功";
        EXPECT_EQ(openDescriptorCount(), descriptorsBefore) << "拒了一条移交，却把已经装进来的描述符留在了本进程里";
#endif
    }
} // namespace AsynGyanis::Platform
