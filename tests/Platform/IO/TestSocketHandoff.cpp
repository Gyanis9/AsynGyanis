/**
 * @file TestSocketHandoff.cpp
 * @brief 监听套接字跨通道移交的用例：交出的一定要能在收端接着 accept，坏消息必须整体作废
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"
#include "PlatformTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
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
         * @brief 造一个已在 127.0.0.1 上监听、端口由内核挑的套接字
         * @param[out] port 实际端口
         * @return int 监听描述符；失败为 -1
         */
        int makeLoopbackListener(std::uint16_t &port)
        {
            port = 0U;
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
        ASSERT_TRUE(Socket::writeListeningSocketHandoff(channelWriter, listener, selfProcessId))
                << "交出监听套接字失败，错误码 " << PlatformError::lastErrorCode();

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
        ASSERT_EQ(::send(secondWriter, reinterpret_cast<const char *>(&foreignHeader), static_cast<int>(sizeof(foreignHeader)), 0),
                   static_cast<int>(sizeof(foreignHeader)));
        ASSERT_EQ(::send(secondWriter, "WXYZ", 4, 0), 4);
        FileDescriptor::close(secondWriter);

        EXPECT_EQ(Socket::readListeningSocketHandoff(secondReader), -1) << "载荷长度不合本平台的格式，却被当成有效移交";
        std::array<char, 4> leftover{};
        ASSERT_EQ(::recv(secondReader, leftover.data(), static_cast<int>(leftover.size()), 0), 4)
                << "收端在读格式之前就把载荷吃掉了：那不是「拒绝」，是「猜着解」";
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
} // namespace AsynGyanis::Platform
