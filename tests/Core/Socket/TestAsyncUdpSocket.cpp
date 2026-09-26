// AsyncUdpSocket 单元测试：整条收发的字节与来源地址、等就绪路径、失败面
//
// 与 TestTimer 同一手法：循环由测试自己按步推进，时序完全由测试安排，不依赖调度运气。
// 协程里的异常在协程内部捕获后记进观测结构，断言在推进结束之后做。

#include "Core/Socket/AsyncUdpSocket.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
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
            int receiveErrorCode{0};                  ///< 没收到字节时的平台错误码；0 表示无码收场
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
                observation.receiveErrorCode    = received.socketErrorCode;
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
        // 本端拿不到描述符是「对象已被移动走」，与底层的 EINVAL 相比这才是可操作的原因
        EXPECT_NE(observation.failureMessage.find("套接字无效"), std::string::npos)
                << "无效套接字要把本端原因说清，而不是只把底层错误码翻译一遍：" << observation.failureMessage;
        EXPECT_FALSE(observation.receivedByteCount.has_value()) << "失败时不该给出接收结果";
    }

    /**
     * @brief 缓冲容量为 0 时要在交给系统调用之前拒掉，并把原因指到缓冲上
     * @details 底层对空缓冲回的是 EINVAL，顺着错误码翻译出来的文案是「Invalid argument」，
     *          后面还跟着一句「对端不可达」的提示——两样都不指向真实起因（调用方给的容量是 0），
     *          而这一类错属于用法错误，应当落在 InvalidArgument 这一支，让调用方知道改参数而不是重试
     */
    TEST(AsyncUdpSocket, ZeroCapacityReceiveRejectsWithTheBufferAsTheReason)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket socket = bindLoopbackSocket(loop);
        ASSERT_TRUE(socket.isValid());

        std::array<char, 8> buffer{};
        Task<AsyncUdpSocket::DatagramReceiveResult> receiveTask = socket.asyncReceiveFrom(buffer.data(), 0);
        receiveTask.handle().resume();
        ASSERT_TRUE(receiveTask.isReady()) << "参数在交给系统调用之前就该被拒掉，不该挂起等报文";

        bool        isInvalidArgument = false;
        std::string failureText;
        try
        {
            static_cast<void>(receiveTask.handle().promise().result());
        } catch (const Base::InvalidArgumentException &rejection)
        {
            isInvalidArgument = true;
            failureText       = rejection.what();
        } catch (const std::exception &other)
        {
            failureText = other.what();
        }
        EXPECT_TRUE(isInvalidArgument) << "零容量是调用方写错了，要落在 InvalidArgument 这一支：" << failureText;
        EXPECT_NE(failureText.find("缓冲"), std::string::npos) << "原因要指到缓冲上：" << failureText;
        EXPECT_EQ(failureText.find("对端不可达"), std::string::npos)
                << "参数错误不该带上「对端不可达」这类无关提示：" << failureText;
    }

    /**
     * @brief 单条报文超过上限时直接拒绝，并把上限数字写进文案
     * @details 交给系统调用只会拿到平台各自的 EMSGSIZE/WSAEMSGSIZE，两端文案不一样且都不说上限是多少
     */
    TEST(AsyncUdpSocket, OverlongDatagramSendIsRejectedWithTheLimitInText)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket socket = bindLoopbackSocket(loop);
        ASSERT_TRUE(socket.isValid());

        const std::size_t oversizedByteCount = Platform::DatagramSocket::kMaximumDatagramBytes + 1;
        const std::string payload(oversizedByteCount, 'x');
        Task<ssize_t>     sendTask = socket.asyncSendTo(socket.localAddress(), payload.data(), payload.size());
        sendTask.handle().resume();
        ASSERT_TRUE(sendTask.isReady()) << "超限的报文应当在进入系统调用之前就被拒掉";

        bool        isInvalidArgument = false;
        std::string failureText;
        try
        {
            static_cast<void>(sendTask.handle().promise().result());
        } catch (const Base::InvalidArgumentException &rejection)
        {
            isInvalidArgument = true;
            failureText       = rejection.what();
        } catch (const std::exception &other)
        {
            failureText = other.what();
        }
        EXPECT_TRUE(isInvalidArgument) << "长度超限是调用方参数问题：" << failureText;
        EXPECT_NE(failureText.find("上限"), std::string::npos) << failureText;
        EXPECT_NE(failureText.find(std::to_string(Platform::DatagramSocket::kMaximumDatagramBytes)), std::string::npos)
                << "文案要写出上限是多少，调用方据此决定分片大小：" << failureText;
    }

    /**
     * @brief 上限长度本身的另一侧：正好等于上限的报文必须放行，并且整条交给内核
     * @details 只测拒绝面容易把判界写成「达到上限即拒」，这条用例钉住合法那一侧
     */
    TEST(AsyncUdpSocket, DatagramExactlyAtTheMaximumIsAccepted)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket socket = bindLoopbackSocket(loop);
        ASSERT_TRUE(socket.isValid());

        const std::size_t maximumByteCount = Platform::DatagramSocket::kMaximumDatagramBytes;
        const std::string payload(maximumByteCount, 'x');
        Task<ssize_t>     sendTask = socket.asyncSendTo(socket.localAddress(), payload.data(), payload.size());
        sendTask.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&sendTask]
                                 {
                                     return sendTask.isReady();
                                 }))
                << "上限长度的发送没有收尾：既没交出去也没报错";

        ssize_t sentByteCount = -1;
        EXPECT_NO_THROW(sentByteCount = sendTask.handle().promise().result());
        EXPECT_EQ(sentByteCount, static_cast<ssize_t>(maximumByteCount)) << "整条报文应当被内核整条接下";
    }

    /**
     * @brief 目标地址要按值收进协程帧：临时量在首次恢复之前就已亡故，也不该读到野内存
     * @details 本仓库的规范硬性要求「惰性启动的接口不得按引用/视图收数据」——协程到首次 resume
     *          才执行函数体，而实参的临时量在那之前早就离开了作用域。这里刻意先把 Task 存下来、
     *          再恢复，就是那个现场（地址改成按引用收时，容器里的 ASan 报 stack-use-after-scope，
     *          读点落在 Platform::DatagramSocket::send 判地址那一步）
     */
    TEST(AsyncUdpSocket, SendReadsThePeerAddressCopyHeldByTheFrameRatherThanTheTemporary)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        EventLoop      loop;
        AsyncUdpSocket sender   = bindLoopbackSocket(loop);
        AsyncUdpSocket receiver = bindLoopbackSocket(loop);
        ASSERT_TRUE(sender.isValid());
        ASSERT_TRUE(receiver.isValid());

        const std::string payload = "lifetime";
        Task<ssize_t>     sendTask = sender.asyncSendTo(receiver.localAddress(), payload.data(), payload.size());
        // 这一行之前，asyncSendTo 的实参临时量已经亡故：协程还一行没跑
        sendTask.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&sendTask]
                                 {
                                     return sendTask.isReady();
                                 }))
                << "发送既没交出去也没报错：调用方拿不到结论";

        ssize_t sentByteCount = -1;
        EXPECT_NO_THROW(sentByteCount = sendTask.handle().promise().result());
        EXPECT_EQ(sentByteCount, static_cast<ssize_t>(payload.size())) << "按值收的地址应当与原临时量等价";

        // 等价性不只看返回码：报文要真能落到那个地址上
        std::array<char, 32> receiveBuffer{};
        Task<AsyncUdpSocket::DatagramReceiveResult> receiveTask =
                receiver.asyncReceiveFrom(receiveBuffer.data(), receiveBuffer.size());
        receiveTask.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&receiveTask]
                                 {
                                     return receiveTask.isReady();
                                 }))
                << "发送成功而接收没等到报文：地址在传递途中被改写了";
        const AsyncUdpSocket::DatagramReceiveResult received = receiveTask.handle().promise().result();
        EXPECT_EQ(received.receivedByteCount, static_cast<ssize_t>(payload.size()));
        EXPECT_EQ(std::string(receiveBuffer.data(), static_cast<std::size_t>(received.receivedByteCount)), payload);
    }

    /**
     * @brief 对端消失带回来的 socket 错误要按「-1 + 错误码」交出，**不能抛**
     * @details 这条契约是给 QUIC 监听循环定的：被调度器恢复的协程抛异常，在本框架里只会被记一行
     *          「协程有异常没人接住」然后丢弃——正在 await 它的那层连通知都收不到，于是整个收包循环
     *          静默消失（端口还在、进程还在，只是不再有任何响应）。触发方式是确定的：把套接字 connect
     *          到一个刚被关掉的回环端口再发一条，内核自己就会把「端口不可达」递到自己的下次读数上。
     *          改回「按硬失败抛出」，本条立刻红（异常文本落进 failureMessage）。
     */
    TEST(AsyncUdpSocket, ReportsPeerUnreachableReceiveErrorAsACodeInsteadOfThrowing)
    {
        ASSERT_TRUE(Platform::Socket::initialize());

        // 先占一个回环端口再关掉：之后的发送就有了一个确定「无人接收」的目标
        std::uint16_t deadPort = 0U;
        {
            const Platform::DatagramSocket occupied = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
            ASSERT_TRUE(occupied.isValid());
            const Platform::SocketAddress occupiedAddress = occupied.localAddress();
            deadPort = ntohs(reinterpret_cast<const sockaddr_in *>(&occupiedAddress.storage)->sin_port);
        } // 作用域结束即关闭，这个端口此后没人监听

        EventLoop      loop;
        AsyncUdpSocket socket = bindLoopbackSocket(loop);
        ASSERT_TRUE(socket.isValid());

        // 这里先 connect 是为了让「端口不可达」确定地递到自己的下次读数上（三大平台一致：Windows
        // 的 10054、Linux 与 macOS 的 ECONNREFUSED）。真实服务端并不 connect（一条套接字对所有来源），
        // Windows 上同样会递 10054——那正是它整个监听循环死掉的现场
        const Platform::SocketAddress deadAddress = makeLoopbackAddress(deadPort);
        ASSERT_EQ(::connect(socket.fileDescriptor(), reinterpret_cast<const sockaddr *>(&deadAddress.storage),
                            deadAddress.length),
                  0)
                << "connect 没成：这条用例需要一个会收 ICMP 的已连接套接字";
        ASSERT_GT(::send(socket.fileDescriptor(), "x", 1, 0), 0) << "发往死端口这一步本身就该成功";

        // 先等那条 ICMP 真递到本端：select 只问「可读吗」，不会把错误吃掉（挂着错误本身就算可读事件），
        // 于是接下来那次异步读数拿到的就是它。不等这一步，读数可能先撞上「暂无数据」而转去等可读，
        // 本条就在两个分支之间漂移（判据只能落在确实取到错误的那一次上）
        const int fileDescriptor = socket.fileDescriptor();
        fd_set    readSet{};
        FD_ZERO(&readSet);
        FD_SET(fileDescriptor, &readSet);
        timeval waitTime{2, 0};
        ASSERT_GT(::select(fileDescriptor + 1, &readSet, nullptr, nullptr, &waitTime), 0)
                << "对端不可达的错误两秒内没递到本端：平台行为与预期不符，下面的判据无从建立";

        TransferObservation observation;
        Task<void>          scenario = receiveOnlyTask(socket, observation);
        loop.scheduler().schedule(scenario.handle());
        ASSERT_TRUE(advanceUntil(loop, [&observation]
                                 {
                                     return observation.receivedByteCount.has_value() || !observation.failureMessage.empty();
                                 }))
                << "一次读数既不返回也不抛：调用方会被永久挂住";

        EXPECT_TRUE(observation.failureMessage.empty())
                << "对端不可达这类错误不该抛出（它会让正在 await 的循环静默消失）：" << observation.failureMessage;
        ASSERT_TRUE(observation.receivedByteCount.has_value());
        EXPECT_LT(*observation.receivedByteCount, 0) << "错误已经到位，读数却没报出失败：那递给调用方的是什么";
        EXPECT_NE(observation.receiveErrorCode, 0)
                << "报「没收到」却不给错误码：调用方分不开「套接字已不可用」与「对端不在了」";
        EXPECT_TRUE(socket.isValid()) << "递回一个对端错误不该把本端套接字一起判死";
    }

} // namespace AsynGyanis::Core
