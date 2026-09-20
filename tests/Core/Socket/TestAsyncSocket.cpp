// AsyncSocket 单元测试：创建、移动语义、bind/listen 生命周期、地址查询与关闭唤醒
//
// 关闭唤醒那几条用例不引入事件循环线程：与 TestIoWatcher 同一手法，
// 事件分发与调度推进由测试自己在同一线程上完成，时序因此完全确定。

#include "Core/Socket/AsyncSocket.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::advanceUntil;
        using TestSupport::waitForCondition;

        /// 每轮发送的负载长度：对端不读时，两侧缓冲加起来远小于这里一轮的量
        constexpr std::size_t kBlockingSendChunkLength = 64 * 1024;

        /// 触发「等可写」的轮数上限：跑满说明本机没有构造出写阻塞，而不是实现出错
        constexpr int kBlockingSendRoundLimit = 4096;

        /// 灌满对端接收队列的单轮负载长度
        constexpr std::size_t kInboundFillChunkLength = 16 * 1024;

        /// 灌满接收队列的轮数上限：跑满说明本机的缓冲大到构造不出「有未读数据」
        constexpr int kInboundFillRoundLimit = 4096;

        /// 服务端接收缓冲的目标大小：显式设置会关掉内核的自动扩窗，灌满只需几十 KB
        constexpr int kSmallReceiveBufferBytes = 8 * 1024;

        /**
         * @brief 写阻塞用例的观测结果
         */
        struct SendObservation
        {
            bool isFailureObserved       = false; ///< asyncSend 是否如实报告了「套接字已不可用」
            bool isClosedFailureObserved = false; ///< 失败原因是否为「等待可写期间套接字被关闭」
            int  observedErrorCode       = 0;     ///< 异常携带的原生错误号（必须取自 socket 空间）
            int  completedRoundCount     = 0;     ///< 失败之前完整提交出去的轮数
        };

        /**
         * @brief 一直发送到内核发送缓冲写满为止
         * @details asyncSend 在首次成功提交后即返回，因此要靠外层循环逐步填满缓冲，
         *          最后一次才会落在内部的「等可写」上。返回时若协程尚未结束，
         *          就说明它正挂在那里——这正是本组用例要构造的状态
         * @param socket 目标套接字
         * @param payload 每轮发送的负载，必须在本次 co_await 恢复之前一直有效
         * @param observation 观测结果出参
         */
        Task<> sendUntilBlocked(AsyncSocket &socket, const std::string &payload, SendObservation &observation)
        {
            for (int round = 0; round < kBlockingSendRoundLimit; ++round)
            {
                try
                {
                    const ssize_t sentBytes = co_await socket.asyncSend(payload.data(), payload.size());
                    // -1 是对端已关闭那条路径的约定返回值（见 asyncSend 的说明），这里同样算观察到失败
                    if (sentBytes <= 0)
                    {
                        observation.completedRoundCount = round;
                        observation.isFailureObserved  = true;
                        co_return;
                    }
                } catch (const Base::SystemException &exception)
                {
                    // 等待可写期间套接字被关闭：await_resume 交回「未就绪」，asyncSend 据此抛错。
                    // 原因与错误号都记下来：用例据此确认「被关闭」与「事件就绪」可区分，
                    // 且错误号取自 socket 空间（winsock 失败不写 errno，读 errno 只会拿到陈旧值）
                    observation.completedRoundCount = round;
                    observation.isFailureObserved  = true;
                    observation.isClosedFailureObserved =
                            std::string_view(exception.what()).find("等待可写期间套接字被关闭") != std::string_view::npos;
                    observation.observedErrorCode = exception.nativeError();
                    co_return;
                } catch (const Base::Exception &)
                {
                    observation.completedRoundCount = round;
                    observation.isFailureObserved  = true;
                    co_return;
                }
            }
        }
    } // namespace

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
     * @brief 验证 close() 会先丢干净内核里没人读的入站字节，使对端收到 EOF 而不是连接重置
     * @details 接收队列非空时关闭，栈会改发 RST：对端把自己已收到、还没来得及读的响应一并丢掉，
     *          管线化场景里表现为「响应凭空消失」。用例不赌时序——先在对端写到 EAGAIN，此刻本端
     *          接收队列必然是满的且从未被读，再关闭本端，然后有界轮询对端的读结果。
     * @note 必须走真实的回环 TCP：收口形态是 TCP 的语义，POSIX 侧的 socketpair 是 AF_UNIX，
     *       它不遵守 FIN/RST 这套规则，用它测出来的是另一个协议的行为。
     */
    TEST(AsyncSocket, CloseDiscardsUnreadInboundDataSoPeerSeesEof)
    {
        EventLoop loop;

        AsyncSocket listener = AsyncSocket::create(loop);
        ASSERT_TRUE(listener.bind(InetAddress::localhost(0)));
        ASSERT_TRUE(listener.listen(1));

        AsyncSocket client      = AsyncSocket::create(loop);
        Task<>      connectTask = client.asyncConnect(InetAddress::localhost(listener.localAddress().port()));
        connectTask.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&connectTask] { return connectTask.isReady(); }))
                << "回环连接没能在时限内完成";
        EXPECT_NO_THROW(connectTask.handle().promise().result());

        // 服务端这一端要交给 AsyncSocket 包装，因此直接走平台层接受（它已经把非阻塞置好了）
        const int acceptedDescriptor = Platform::Socket::accept(listener.fileDescriptor(), nullptr, nullptr);
        ASSERT_TRUE(Platform::FileDescriptor::isValid(acceptedDescriptor)) << "回环连接没有被接受";
        // 先把服务端接收缓冲压小，客户端此刻还什么都没写：不压的话内核自动扩窗，灌满要写几十 MB
        ASSERT_TRUE(Platform::Socket::setReceiveBufferSize(acceptedDescriptor, kSmallReceiveBufferBytes));
        AsyncSocket server(loop, acceptedDescriptor);

        const int clientDescriptor = client.fileDescriptor();

        // 客户端不收，写到 EAGAIN 就说明服务端的接收队列里已经堆了没人读的字节
        std::array<char, kInboundFillChunkLength> payload{};
        std::int64_t submittedByteCount = 0;
        bool isQueueFilledObservably    = false;
        for (int roundCount = 0; roundCount < kInboundFillRoundLimit; ++roundCount)
        {
            const ssize_t writtenBytes =
                Platform::FileDescriptor::write(clientDescriptor, payload.data(), payload.size());
            if (writtenBytes > 0)
            {
                submittedByteCount += writtenBytes;
                continue;
            }
            if (writtenBytes < 0 &&
                Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                isQueueFilledObservably = true;
                break;
            }
            break;
        }
        ASSERT_TRUE(isQueueFilledObservably)
                << "没能在 " << kInboundFillRoundLimit << " 轮内把服务端接收队列灌到 EAGAIN，条件没构造出来";
        EXPECT_GT(submittedByteCount, 0) << "一个字节都没写出去，谈不上「有未读数据」";

        server.close();

        // 收口报文到达前客户端读到的是 EWOULDBLOCK：等到出现 0（EOF）或真正的错误为止
        std::array<char, 64> probeBuffer{};
        ssize_t readResult        = -2;
        int     observedErrorCode = Platform::PlatformError::kWouldBlock;
        const bool isClosureObserved = waitForCondition(
            [&]
            {
                readResult = Platform::FileDescriptor::read(clientDescriptor, probeBuffer.data(), probeBuffer.size());
                observedErrorCode =
                    readResult < 0 ? Platform::PlatformError::lastSocketErrorCode() : 0;
                return readResult == 0 || observedErrorCode != Platform::PlatformError::kWouldBlock;
            });

        ASSERT_TRUE(isClosureObserved) << "关闭服务端后客户端既没读到 EOF 也没报错，收口报文没有到达";
        EXPECT_EQ(readResult, 0) << "客户端读到错误码 " << observedErrorCode
                                 << "：RST 会让它丢掉已收到但还没读的响应";
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

        // 回环连接可能立即成功，也可能返回 EINPROGRESS 而挂起等待可写：后者要靠事件循环推进
        ASSERT_TRUE(advanceUntil(loop, [&connectTask] { return connectTask.isReady(); }))
                << "连接未在预期内完成";
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

    /**
     * @brief 关闭套接字必须唤醒正卡在「等可写」上的协程，并让它观察到「已关闭」而不是「就绪」
     *
     * @details 钉住收尾语义对**写方向**同样成立：对端不读，发送缓冲被填满后协程落在
     *          co_await asyncSend(...) 内部的「等可写」上；随后在同一个事件循环线程上关闭该
     *          套接字（清扫协程正是这么做的）。关闭必须把挂起的协程摘下来投回调度器并以
     *          「未就绪」唤醒它——关闭描述符本身不会让内核唤醒它，少了这一步，
     *          这帧协程连同它持有的连接与缓冲会一直滞留到进程退出。
     */
    TEST(AsyncSocket, CloseWakesCoroutineBlockedOnSend)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        AsyncSocket sender(loop, localDescriptor);
        // 想方设法把发送缓冲压到最小，让填满所需的字节数不要太大（内核会自行上调到一个下限）
        int sendBufferLength = 4096;
        [[maybe_unused]] const bool isSendBufferSet =
                sender.setSockOpt(SOL_SOCKET, SO_SNDBUF, &sendBufferLength, sizeof(sendBufferLength));

        // 对端全程不读：payload 与观测结果都必须活到协程恢复之后
        const std::string payload(kBlockingSendChunkLength, 'x');
        SendObservation   observation;

        Task<> sending = sendUntilBlocked(sender, payload, observation);
        sending.handle().resume();
        ASSERT_FALSE(sending.isReady()) << "对端不读，发送却没有落到「等可写」上：本机构造不出该场景";

        // 与清扫协程同一做法：在事件循环线程上关闭套接字
        sender.close();

        // 唤醒是投递到调度队列的（注册对象正在析构，不能就地恢复）
        loop.scheduler().runAll();

        EXPECT_TRUE(sending.isReady()) << "关闭套接字之后，卡在等可写上的协程仍未被唤醒";
        EXPECT_TRUE(observation.isFailureObserved)
                << "协程虽然被唤醒，却把「套接字已关闭」当成了一次就绪：await_resume 没有交回失败";
        EXPECT_TRUE(observation.isClosedFailureObserved)
                << "被唤醒后拿到的是「发送失败」而不是「等待可写期间套接字被关闭」："
                   "等待结果无法区分「被关闭」与「事件就绪」";
        // 错误号必须来自 socket 空间：按 errno 构造只会带上一个与本次失败无关的陈旧值
        EXPECT_EQ(observation.observedErrorCode, Platform::PlatformError::kConnectionAborted)
                << "异常携带的错误号不是 socket 空间的「本端中止连接」：多半又去读了 errno";
        EXPECT_LT(observation.completedRoundCount, kBlockingSendRoundLimit)
                << "对端从未读过，写侧却宣称把负载全部提交成功了";

        Platform::FileDescriptor::close(peerDescriptor);
    }
} // namespace AsynGyanis::Core
