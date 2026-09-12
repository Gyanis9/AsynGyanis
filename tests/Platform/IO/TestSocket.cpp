/**
 * @file TestSocket.cpp
 * @brief Socket 单元测试：Winsock 生命周期与跨平台 accept 语义
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 有界重试统一使用的最长等待毫秒数，避免任何一步无限阻塞
        constexpr int kWaitTimeoutMilliseconds = 2000;

        /**
         * @brief 创建并绑定一个回环监听 socket（已置为非阻塞）
         * @param assignedPort 输出参数，系统分配的监听端口
         * @return int 监听描述符，失败返回 FileDescriptor::kInvalid
         */
        int createLoopbackListener(std::uint16_t &assignedPort)
        {
            Socket::initialize();

            const int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (listener < 0)
            {
                return FileDescriptor::kInvalid;
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }

            socklen_t addressLength = sizeof(address);
            if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }
            assignedPort = ntohs(address.sin_port);

            if (::listen(listener, 1) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }

            // 监听端设为非阻塞，accept 才可安全地在有界循环里重试
            if (!FileDescriptor::setNonBlocking(listener))
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }
            return listener;
        }

        /**
         * @brief 向指定回环端口发起连接，成功后转为非阻塞
         * @param port 目标端口
         * @return int 客户端描述符，失败返回 FileDescriptor::kInvalid
         * @note 连接阶段保持阻塞（回环握手即刻完成），随后转非阻塞，
         *       这样轮询读取不会在 recv 中无限挂起。
         */
        int connectToLoopback(const std::uint16_t port)
        {
            const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (client < 0)
            {
                return FileDescriptor::kInvalid;
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = htons(port);
            if (::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            {
                FileDescriptor::close(client);
                return FileDescriptor::kInvalid;
            }

            if (!FileDescriptor::setNonBlocking(client))
            {
                FileDescriptor::close(client);
                return FileDescriptor::kInvalid;
            }
            return client;
        }

        /**
         * @brief 在有界时间内从监听描述符接受一条连接
         * @param listener 非阻塞监听描述符
         * @param address 可选输出参数，对端地址
         * @param addressLength 可选输入输出参数，address 缓冲区容量与实际长度
         * @return int 已连接描述符，超时返回 FileDescriptor::kInvalid
         */
        int acceptWithTimeout(const int listener, sockaddr *address, socklen_t *addressLength)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (const int accepted = Socket::accept(listener, address, addressLength); FileDescriptor::isValid(accepted))
                {
                    return accepted;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return FileDescriptor::kInvalid;
        }
    } // namespace

    TEST(Socket, InitializeSucceedsOnBothPlatforms)
    {
        EXPECT_TRUE(Socket::initialize());
        // 内部使用一次性初始化，重复调用同样成功
        EXPECT_TRUE(Socket::initialize());
    }

    TEST(Socket, AcceptOnInvalidDescriptorFails)
    {
        const int accepted = Socket::accept(FileDescriptor::kInvalid, nullptr, nullptr);

        EXPECT_FALSE(FileDescriptor::isValid(accepted));
    }

    TEST(Socket, AcceptReturnsConnectedDescriptorAndPeerAddress)
    {
        std::uint16_t port     = 0;
        const int     listener = createLoopbackListener(port);
        ASSERT_TRUE(FileDescriptor::isValid(listener));

        const int client = connectToLoopback(port);
        ASSERT_TRUE(FileDescriptor::isValid(client));

        sockaddr_in peerAddress{};
        socklen_t   peerAddressLength = sizeof(peerAddress);
        const int   accepted          = acceptWithTimeout(listener, reinterpret_cast<sockaddr *>(&peerAddress),
                                               &peerAddressLength);

        ASSERT_TRUE(FileDescriptor::isValid(accepted));
        EXPECT_EQ(peerAddress.sin_family, AF_INET);
        EXPECT_EQ(peerAddress.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
        EXPECT_GT(peerAddressLength, 0);

        // 两端均已非阻塞：已连接的描述符可双向收发
        constexpr char kmarker = 'S';
        const ssize_t  sent    = FileDescriptor::write(accepted, &kmarker, sizeof(kmarker));
        EXPECT_EQ(sent, 1);

        char       received = '\0';
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (received == '\0' && std::chrono::steady_clock::now() < deadline)
        {
            if (FileDescriptor::read(client, &received, sizeof(received)) > 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(received, 'S');

        FileDescriptor::close(accepted);
        FileDescriptor::close(client);
        FileDescriptor::close(listener);
    }

    TEST(Socket, AcceptedDescriptorIsNonBlocking)
    {
        std::uint16_t port     = 0;
        const int     listener = createLoopbackListener(port);
        ASSERT_TRUE(FileDescriptor::isValid(listener));

        const int client = connectToLoopback(port);
        ASSERT_TRUE(FileDescriptor::isValid(client));

        const int accepted = acceptWithTimeout(listener, nullptr, nullptr);
        ASSERT_TRUE(FileDescriptor::isValid(accepted));

        // 无数据时读取应立即失败而不是阻塞，说明两侧实现都置入了非阻塞标志
        char buffer[8];
        EXPECT_LT(FileDescriptor::read(accepted, buffer, sizeof(buffer)), 0);

        FileDescriptor::close(accepted);
        FileDescriptor::close(client);
        FileDescriptor::close(listener);
    }

    /**
     * @brief 聚合写把多段拼成一个字节流，并把非法参数当场挡下
     *
     * @details 分段提交的意义在于不必先把数据拼进同一块缓冲（省一次整体拷贝），
     *          因此这里必须验证两件事：线上字节流与「按序拼接」逐字节一致；
     *          段数/空数组这类越界参数当场失败而不是静默拆分或假装发出去。
     */
    TEST(Socket, WriteVectoredConcatenatesSegmentsAndRejectsInvalidArguments)
    {
        int readDescriptor  = -1;
        int writeDescriptor = -1;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const std::string_view first = "alpha";
        const std::string_view empty = first.substr(1, 0); // 长度为 0 但指针非空，两个平台都接受
        const std::string_view third = "beta";
        const Socket::WriteBuffer buffers[3] = {
                {first.data(), first.size()},
                {empty.data(), empty.size()},
                {third.data(), third.size()},
        };

        const ssize_t writtenLength = Socket::writeVectored(writeDescriptor, buffers, 3);
        ASSERT_EQ(writtenLength, static_cast<ssize_t>(first.size() + third.size()));

        char          received[16] = {};
        const ssize_t readLength   = FileDescriptor::read(readDescriptor, received, sizeof(received));
        ASSERT_EQ(readLength, writtenLength);
        EXPECT_EQ(std::string_view(received, static_cast<std::size_t>(readLength)), "alphabeta");

        // 非法参数：返回 -1 并置 kInvalidArgument，绝不静默拆分（拆分会让「一次系统调用」的收益
        // 悄悄消失）也不静默补齐（补齐会让调用方以为数据发出去了）
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, nullptr, 1), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, buffers, 0), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, buffers, Socket::kMaximumVectorCount + 1), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }
} // namespace AsynGyanis::Platform
