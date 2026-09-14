/**
 * @file TestDatagramSocket.cpp
 * @brief DatagramSocket 单元测试：绑定、收发（带对端地址）、无数据与非法参数的错误面
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/DatagramSocket.h"

#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
} // namespace AsynGyanis::Platform
