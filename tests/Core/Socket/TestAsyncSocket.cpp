/**
 * @file TestAsyncSocket.cpp
 * @brief AsyncSocket 单元测试：创建、移动语义、bind/listen 生命周期与地址查询
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Socket/AsyncSocket.h"

#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <array>
#include <coroutine>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    /**
     * @brief 验证 create() 直接交出可用的描述符（失败要在这里暴露，而不是拖到第一次收发）
     */
    TEST(AsyncSocket, CreateReturnsValidDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);

        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        asyncSocket.close();
    }

    /**
     * @brief 验证移动构造转移描述符所有权，源对象回到无效态
     *
     * @details 源对象必须被置空：两个对象持有同一个 fd 会各关一次，第二次关的是别人的描述符。
     */
    TEST(AsyncSocket, MoveConstructionTransfersDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket1 = AsyncSocket::create(loop);
        const int fileDescriptor1 = asyncSocket1.fileDescriptor();
        ASSERT_GE(fileDescriptor1, 0);

        const AsyncSocket asyncSocket2(std::move(asyncSocket1));

        EXPECT_EQ(asyncSocket2.fileDescriptor(), fileDescriptor1);
        EXPECT_EQ(asyncSocket1.fileDescriptor(), -1);
    }

    /**
     * @brief 验证移动赋值同样转移描述符并让源对象失效
     */
    TEST(AsyncSocket, MoveAssignmentTransfersDescriptor)
    {
        EventLoop loop;
        AsyncSocket asyncSocket1 = AsyncSocket::create(loop);
        AsyncSocket asyncSocket2 = AsyncSocket::create(loop);
        const int fileDescriptor1 = asyncSocket1.fileDescriptor();
        ASSERT_GE(fileDescriptor1, 0);

        asyncSocket2 = std::move(asyncSocket1);

        EXPECT_EQ(asyncSocket2.fileDescriptor(), fileDescriptor1);
        EXPECT_EQ(asyncSocket1.fileDescriptor(), -1);

        asyncSocket2.close();
    }

    /**
     * @brief 验证 close() 把描述符置回 -1，使「已关闭」状态可从外部判定
     */
    TEST(AsyncSocket, CloseResetsDescriptorToInvalid)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        asyncSocket.close();

        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    /**
     * @brief 验证重复 close() 幂等：第二次不会去关闭一个已经释放（可能已被复用）的描述符
     */
    TEST(AsyncSocket, DoubleCloseIsSafe)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);
        asyncSocket.close();

        EXPECT_NO_THROW(asyncSocket.close());
        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    /**
     * @brief 验证能绑到回环 + 端口 0（由内核分配空闲端口），这是测试里起临时服务的常规做法
     */
    TEST(AsyncSocket, BindToLoopbackEphemeralPortSucceeds)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));

        asyncSocket.close();
    }

    /**
     * @brief 验证绑定之后能进入监听状态（先 bind 再 listen 是套接字的使用次序前提）
     */
    TEST(AsyncSocket, ListenAfterBindSucceeds)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));
        ASSERT_TRUE(asyncSocket.listen(AsyncSocket::kDefaultListenBacklog));

        asyncSocket.close();
    }

    /**
     * @brief 验证 setSockOpt 真能把地址复用打开（服务重启时不被上一代的 TIME_WAIT 挡住）
     */
    TEST(AsyncSocket, SetSockOptEnablesAddressReuse)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);

        int optionValue = 1;
        ASSERT_TRUE(asyncSocket.setSockOpt(SOL_SOCKET, SO_REUSEADDR, &optionValue, sizeof(optionValue)));

        asyncSocket.close();
    }

    /**
     * @brief 验证 bind→listen→localAddress→close 整条生命周期可用，且能读回内核分配的真实端口
     */
    TEST(AsyncSocket, BindListenCloseLifecycleReportsEphemeralPort)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        const InetAddress address = InetAddress::localhost(0);

        ASSERT_TRUE(asyncSocket.bind(address));
        ASSERT_TRUE(asyncSocket.listen(AsyncSocket::kDefaultListenBacklog));

        const InetAddress localAddress = asyncSocket.localAddress();
        EXPECT_NE(localAddress.port(), 0);

        asyncSocket.close();
        EXPECT_EQ(asyncSocket.fileDescriptor(), -1);
    }

    /**
     * @brief 验证 localAddress() 读回的就是绑定的那个回环地址与端口（而不是 0.0.0.0 之类）
     */
    TEST(AsyncSocket, LocalAddressMatchesBoundLoopbackAddress)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_TRUE(asyncSocket.bind(InetAddress::localhost(0)));

        const InetAddress localAddress = asyncSocket.localAddress();
        EXPECT_EQ(localAddress.ip(), "127.0.0.1");
        EXPECT_NE(localAddress.port(), 0);

        asyncSocket.close();
    }

    /**
     * @brief 验证未连接时读对端地址必须抛错，而不是返回一个全零/垃圾地址
     */
    TEST(AsyncSocket, RemoteAddressThrowsOnUnconnectedSocket)
    {
        EventLoop loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        EXPECT_THROW(asyncSocket.remoteAddress(), Base::SystemException);

        asyncSocket.close();
    }

    /**
     * @brief 验证 asyncConnect 能真正建立连接：回环上监听 → 连接 → 对端端口可读回
     *
     * @details 惰性协程的入参接收方式是这条路径上最容易出错的地方：按 const 引用收时，
     *          传临时对象会静默悬垂，因此本接口按值接收。
     *          用例不引入事件循环线程：协程最多挂起一次，测试自己从 epoll 取出事件恢复它，
     *          因此时序完全确定。
     */
    TEST(AsyncSocket, AsyncConnectSucceedsAgainstLoopbackListener)
    {
        EventLoop loop;

        // 监听侧绑到回环的临时端口（端口 0 由内核分配）。内核在三次握手完成后会把连接
        // 放进 backlog，因此这里不需要调用 accept() 就能让连接建立成功
        AsyncSocket listener = AsyncSocket::create(loop);
        ASSERT_TRUE(listener.bind(InetAddress(static_cast<std::uint16_t>(0), "127.0.0.1")));
        ASSERT_TRUE(listener.listen(1));

        const std::uint16_t listeningPort = listener.localAddress().port();
        ASSERT_GT(listeningPort, 0);

        AsyncSocket client      = AsyncSocket::create(loop);
        Task<>      connectTask = client.asyncConnect(InetAddress("127.0.0.1", listeningPort));
        connectTask.handle().resume();

        // 回环连接可能立即成功，也可能返回 EINPROGRESS 而挂起等待可写：后者需要事件循环
        // 推进一步。这里按 EventLoop::run() 的方式分发事件——data.ptr 挂载的是常驻注册对象
        // 的地址（不再是协程句柄），由它决定恢复哪个等待者
        if (!connectTask.isReady())
        {
            for (const auto &event: loop.epoll().wait(2000))
            {
                if (event.data.ptr != nullptr)
                {
                    static_cast<IoWatcher *>(event.data.ptr)->handleEvents(event.events);
                }
            }
        }

        ASSERT_TRUE(connectTask.isReady()) << "连接未在预期内完成";
        EXPECT_NO_THROW(connectTask.handle().promise().result());

        // 连接确实建立在刚监听的那个端口上：对端地址可读回即为证据
        const InetAddress peerAddress = client.remoteAddress();
        EXPECT_EQ(peerAddress.port(), listeningPort);
        EXPECT_EQ(peerAddress.ip(), "127.0.0.1");

        client.close();
        listener.close();
    }

    /**
     * @brief 聚合发送把多段按顺序写成一个字节流，且短数据在回环上不挂起
     *
     * @details socketpair 造一条双向通道：一端包成 AsyncSocket 做聚合发送，另一端直接读。
     *          用例不引入事件循环线程——小数据一次就写完，不会挂起，时序完全确定。
     */
    TEST(AsyncSocket, AsyncSendVectoredDeliversSegmentsInOrder)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        AsyncSocket sender(loop, localDescriptor);

        const std::string_view first  = "GET /a HTTP/1.1\r\n";
        const std::string_view second = "Host: localhost\r\n";
        const std::string_view third  = "\r\nBODY";
        const Platform::Socket::WriteBuffer buffers[3] = {
                {first.data(), first.size()},
                {second.data(), second.size()},
                {third.data(), third.size()},
        };
        const std::size_t expectedLength = first.size() + second.size() + third.size();

        Task<ssize_t> sending = sender.asyncSendVectored(buffers, 3);
        sending.handle().resume();
        ASSERT_TRUE(sending.isReady()) << "回环上的小数据不应挂起";
        EXPECT_EQ(sending.handle().promise().result(), static_cast<ssize_t>(expectedLength));

        // 读侧看到的必须是三段按序拼接的结果
        std::string received(expectedLength, '\0');
        std::size_t receivedLength = 0;
        while (receivedLength < expectedLength)
        {
            const ssize_t readLength = Platform::FileDescriptor::read(
                    peerDescriptor, received.data() + receivedLength, expectedLength - receivedLength);
            ASSERT_GT(readLength, 0);
            receivedLength += static_cast<std::size_t>(readLength);
        }
        EXPECT_EQ(received, std::string(first) + std::string(second) + std::string(third));

        sender.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 段数为 0 或超过平台上限时当场抛错，而不是静默拆分或假装发出去
     */
    TEST(AsyncSocket, AsyncSendVectoredRejectsInvalidSegmentCount)
    {
        EventLoop   loop;
        AsyncSocket socket(loop, -1); // 描述符无效不影响本用例：参数校验先于任何 I/O

        const Platform::Socket::WriteBuffer single[1] = {{"x", 1}};

        Task<ssize_t> emptyTask = socket.asyncSendVectored(nullptr, 0);
        emptyTask.handle().resume();
        ASSERT_TRUE(emptyTask.isReady());
        EXPECT_THROW(emptyTask.handle().promise().result(), Base::SystemException);

        constexpr std::size_t                                          kTooManyCount = Platform::Socket::kMaximumVectorCount + 1;
        const std::array<Platform::Socket::WriteBuffer, kTooManyCount> tooManyBuffers{};
        Task<ssize_t> tooManyTask = socket.asyncSendVectored(tooManyBuffers.data(), kTooManyCount);
        tooManyTask.handle().resume();
        ASSERT_TRUE(tooManyTask.isReady());
        EXPECT_THROW(tooManyTask.handle().promise().result(), Base::SystemException);

        Task<ssize_t> singleTask = socket.asyncSendVectored(single, 1);
        singleTask.handle().resume();
        ASSERT_TRUE(singleTask.isReady());
        // 参数合法但描述符无效：以平台错误收场，而不是抛参数类异常
        EXPECT_THROW(singleTask.handle().promise().result(), Base::SystemException);
    }
} // namespace AsynGyanis::Core
