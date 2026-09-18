// AsyncUdpSocket 单元测试：整条收发的字节与来源地址、等就绪路径、失败面
//
// 与 TestTimer 同一手法：循环由测试自己按步推进，时序完全由测试安排，不依赖调度运气。
// 协程里的异常在协程内部捕获后记进观测结构，断言在推进结束之后做。

#include "Core/Socket/AsyncUdpSocket.h"

#include "Base/Exception/Exception.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::advanceUntil;
        using TestSupport::stepLoopOnce;

        /**
         * @brief 造一个回环 IPv4 地址
         * @param port 端口；0 表示由内核分配
         * @return Platform::SocketAddress 地址值
         */
        Platform::SocketAddress makeLoopbackAddress(const std::uint16_t port)
        {
            Platform::SocketAddress address;
            sockaddr_in             addressV4{};
            addressV4.sin_family      = AF_INET;
            addressV4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addressV4.sin_port        = htons(port);
            std::memcpy(&address.storage, &addressV4, sizeof(addressV4));
            address.length = sizeof(addressV4);
            return address;
        }

        /**
         * @brief 在给定循环上绑一个回环数据报套接字
         * @param loop 所属事件循环
         * @return AsyncUdpSocket 已绑定的套接字（绑定失败时 isValid() 为 false）
         */
        AsyncUdpSocket bindLoopbackSocket(EventLoop &loop)
        {
            Platform::DatagramSocket platformSocket = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
            return AsyncUdpSocket(loop, std::move(platformSocket));
        }

        /**
         * @brief 收发场景的观测结果
         */
        struct TransferObservation
        {
            std::optional<ssize_t>  sentByteCount;     ///< 发送返回的字节数
            std::optional<ssize_t>  receivedByteCount; ///< 接收返回的字节数；空表示还没收到
            std::string             receivedPayload;   ///< 收到的内容
            Platform::SocketAddress peerAddress;       ///< 收到报文的来源地址
            std::string             failureMessage;    ///< 协程内捕获到的异常文本；空表示没出异常
        };

        /**
         * @brief 「先发后收」场景：两条报文都用异步接口，全部记进观测结构
         * @param sender 发送套接字
         * @param receiver 接收套接字
         * @param payload 报文内容
         * @param observation 观测结果
         */
        Task<void> sendThenReceiveTask(AsyncUdpSocket &sender, AsyncUdpSocket &receiver, std::string payload,
                                       TransferObservation &observation)
        {
            try
            {
                observation.sentByteCount = co_await sender.asyncSendTo(receiver.localAddress(), payload.data(), payload.size());

                std::array<char, 256> buffer{};
                // 结果按值回来：字节数与来源地址一起拿到（惰性协程不往调用方的引用里写）
                const AsyncUdpSocket::DatagramReceiveResult received =
                        co_await receiver.asyncReceiveFrom(buffer.data(), buffer.size());
                const ssize_t receivedByteCount = received.receivedByteCount;
                observation.peerAddress         = received.peerAddress;
                observation.receivedByteCount   = receivedByteCount;
                if (receivedByteCount > 0)
                {
                    observation.receivedPayload.assign(buffer.data(), static_cast<std::size_t>(receivedByteCount));
                }
            } catch (const Base::Exception &exception)
            {
                observation.failureMessage = exception.what();
            }
            co_return;
        }

        /**
         * @brief 只收一条报文（用于「报文后到」的场景）
         * @param receiver 接收套接字
         * @param observation 观测结果
         */
        Task<void> receiveOnlyTask(AsyncUdpSocket &receiver, TransferObservation &observation)
        {
            try
            {
                std::array<char, 256> buffer{};
                // 结果按值回来：字节数与来源地址一起拿到（惰性协程不往调用方的引用里写）
                const AsyncUdpSocket::DatagramReceiveResult received =
                        co_await receiver.asyncReceiveFrom(buffer.data(), buffer.size());
                const ssize_t receivedByteCount = received.receivedByteCount;
                observation.peerAddress         = received.peerAddress;
                observation.receivedByteCount   = receivedByteCount;
                if (receivedByteCount > 0)
                {
                    observation.receivedPayload.assign(buffer.data(), static_cast<std::size_t>(receivedByteCount));
                }
            } catch (const Base::Exception &exception)
            {
                observation.failureMessage = exception.what();
            }
            co_return;
        }
    } // namespace

    /**
     * @brief 一条报文的字节与来源地址都要如实交回：接收方据此把报文分派给对应连接
     */
    TEST(AsyncUdpSocket, ReceivesDatagramWithPeerAddress)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket receiver = bindLoopbackSocket(loop);
        ASSERT_TRUE(receiver.isValid()) << "接收端绑定失败，套接字错误码 " << Platform::PlatformError::lastSocketErrorCode();
        AsyncUdpSocket sender = bindLoopbackSocket(loop);
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败";

        constexpr std::string_view kPayload = "asyn-quic-datagram";
        TransferObservation        observation;

        // Task 只能在协程里推进：整段场景做成一个协程，由测试按步推到收完
        Task<void> scenario = sendThenReceiveTask(sender, receiver, std::string(kPayload), observation);
        loop.scheduler().schedule(scenario.handle());
        ASSERT_TRUE(advanceUntil(loop, [&observation] { return observation.receivedByteCount.has_value(); }))
                << "没有在时限内收到报文";

        ASSERT_TRUE(observation.failureMessage.empty()) << "场景里抛了异常：" << observation.failureMessage;
        ASSERT_TRUE(observation.sentByteCount.has_value()) << "发送没有走到";
        EXPECT_EQ(*observation.sentByteCount, static_cast<ssize_t>(kPayload.size())) << "发送返回的字节数应当是整条报文长度";
        ASSERT_TRUE(observation.receivedByteCount.has_value());
        EXPECT_EQ(*observation.receivedByteCount, static_cast<ssize_t>(kPayload.size()));
        EXPECT_EQ(observation.receivedPayload, kPayload) << "收到的内容与发出的不一致";

        // 来源地址必须是发送端自己的地址：一个端口服务多条连接全靠这一点
        const Platform::SocketAddress senderAddress = sender.localAddress();
        EXPECT_GT(observation.peerAddress.length, 0U) << "没有交回来源地址";
        EXPECT_EQ(observation.peerAddress.length, senderAddress.length);
        EXPECT_EQ(std::memcmp(&observation.peerAddress.storage, &senderAddress.storage, observation.peerAddress.length), 0)
                << "来源地址不是发送端的地址";
    }

    /**
     * @brief 零长数据报可以发出去、接收侧照收（RFC 768 允许空报文）
     * @details 空报文常用作保活探测。此前发送侧把 length == 0 当成参数错误拒掉，而接收侧一直
     *          照收——同一件事在两个方向上行为不一致
     */
    TEST(AsyncUdpSocket, SendsAndReceivesEmptyDatagram)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket receiver = bindLoopbackSocket(loop);
        ASSERT_TRUE(receiver.isValid()) << "接收端绑定失败";
        AsyncUdpSocket sender = bindLoopbackSocket(loop);
        ASSERT_TRUE(sender.isValid()) << "发送端绑定失败";

        TransferObservation observation;
        Task<void>          scenario = sendThenReceiveTask(sender, receiver, std::string{}, observation);
        loop.scheduler().schedule(scenario.handle());
        ASSERT_TRUE(advanceUntil(loop, [&observation] { return observation.receivedByteCount.has_value(); }))
                << "空报文没有被交付";

        ASSERT_TRUE(observation.failureMessage.empty()) << "场景里抛了异常：" << observation.failureMessage;
        ASSERT_TRUE(observation.sentByteCount.has_value()) << "发送没有走到";
        EXPECT_EQ(*observation.sentByteCount, 0) << "空报文应当原样发出";
        ASSERT_TRUE(observation.receivedByteCount.has_value());
        EXPECT_EQ(*observation.receivedByteCount, 0);
        EXPECT_TRUE(observation.receivedPayload.empty());
    }

    /**
     * @brief 报文晚于接收挂起到达时，等就绪能把协程唤醒并把报文交回来
     * @details 这一条才真正走到 IoWatcher 的等待路径：先把接收摊到挂起，再从另一条套接字发报文。
     *          「推进一轮后仍未完成」本身就是「它挂在了等可读上」的证据——此刻一个字节都还没发
     */
    TEST(AsyncUdpSocket, ReceivesDatagramThatArrivesAfterTheWaitStarts)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket receiver = bindLoopbackSocket(loop);
        ASSERT_TRUE(receiver.isValid());

        // 发送方用平台层同步发（不受本用例考察），少一条协程就少一处时序假设
        const Platform::DatagramSocket sender = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
        ASSERT_TRUE(sender.isValid());

        constexpr std::string_view kPayload = "late-datagram";
        TransferObservation        observation;

        Task<void> scenario = receiveOnlyTask(receiver, observation);
        loop.scheduler().schedule(scenario.handle());

        // 推进一轮：没有报文可收，接收只能挂在等可读上
        stepLoopOnce(loop);
        ASSERT_FALSE(observation.receivedByteCount.has_value()) << "还没发报文就收到了：等就绪路径没有被走到";
        ASSERT_TRUE(observation.failureMessage.empty()) << "等待之前就抛了异常：" << observation.failureMessage;

        ASSERT_EQ(sender.send(receiver.localAddress(), kPayload.data(), kPayload.size()), static_cast<ssize_t>(kPayload.size()))
                << "发送失败，套接字错误码 " << Platform::PlatformError::lastSocketErrorCode();

        ASSERT_TRUE(advanceUntil(loop, [&observation] { return observation.receivedByteCount.has_value(); }))
                << "报文到达后等待没有被唤醒";
        ASSERT_TRUE(observation.failureMessage.empty()) << "场景里抛了异常：" << observation.failureMessage;
        EXPECT_EQ(*observation.receivedByteCount, static_cast<ssize_t>(kPayload.size()));
        EXPECT_EQ(observation.receivedPayload, kPayload) << "等就绪醒来后拿到的内容不对";
    }

    /**
     * @brief 被移动走的套接字按无效处理，观察接口一律安全失败
     */
    TEST(AsyncUdpSocket, MovedFromSocketReportsInvalid)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket source = bindLoopbackSocket(loop);
        ASSERT_TRUE(source.isValid());
        const int boundFileDescriptor = source.fileDescriptor();

        AsyncUdpSocket target = std::move(source);
        EXPECT_TRUE(target.isValid()) << "移动之后目标对象应当可用";
        EXPECT_EQ(target.fileDescriptor(), boundFileDescriptor) << "移动不该换掉描述符";
        EXPECT_FALSE(source.isValid()) << "移动之后源对象仍自称有效";
        EXPECT_LT(source.fileDescriptor(), 0);
        EXPECT_EQ(source.localAddress().length, 0U);
    }

    /**
     * @brief 无效套接字上收报文抛异常并给出中文原因，而不是静默返回 -1 或挂住
     */
    TEST(AsyncUdpSocket, ReceiveOnMovedFromSocketThrowsWithChineseReason)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket source = bindLoopbackSocket(loop);
        ASSERT_TRUE(source.isValid());
        AsyncUdpSocket target = std::move(source);
        ASSERT_TRUE(target.isValid());

        TransferObservation observation;
        Task<void>          scenario = receiveOnlyTask(source, observation);
        loop.scheduler().schedule(scenario.handle());
        ASSERT_TRUE(advanceUntil(loop, [&observation] { return !observation.failureMessage.empty(); }))
                << "无效套接字上收报文既没抛异常也没完成：调用方会被挂住";

        // 异常文本形如「[异常] 数据报接收失败：...」，因此只判「里面说了是哪一步」
        EXPECT_NE(observation.failureMessage.find("数据报接收失败"), std::string::npos)
                << "异常文本应当说明是哪一步失败的：" << observation.failureMessage;
        EXPECT_FALSE(observation.receivedByteCount.has_value()) << "失败时不该给出接收结果";
    }
} // namespace AsynGyanis::Core
