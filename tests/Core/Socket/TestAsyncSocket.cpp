// AsyncSocket 单元测试：创建、移动语义、bind/listen 生命周期、地址查询、关闭唤醒与监听收口的引用边界
//
// 关闭唤醒那几条用例不引入事件循环线程：与 TestIoWatcher 同一手法，
// 事件分发与调度推进由测试自己在同一线程上完成，时序因此完全确定。

#include "Core/Socket/AsyncSocket.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/MemoryMappedFile.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include "CoreTestSupport.h"
#include "PlatformTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#if !ASYN_PLATFORM_WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::advanceUntil;
        using TestSupport::stepLoopOnce;
        using TestSupport::waitForCondition;

        /// 临时目录夹具在跨模块的那一份里（Core::TestSupport 只转发等待与泵循环的助手）
        using AsynGyanis::TestSupport::TemporaryDirectory;

        /// 每轮发送的负载长度：对端不读时，两侧缓冲加起来远小于这里一轮的量
        constexpr std::size_t kBlockingSendChunkLength = 64 * 1024;

        /// 「对端拒绝」在本机错误空间里的数值：Windows 是 WSAECONNREFUSED，POSIX 是 ECONNREFUSED。
        /// 两侧都按 std::system_category() 递交，因此数值可以直接对照
        constexpr int kExpectedRefusedErrorCode =
#if ASYN_PLATFORM_WIN32
            10061;
#else
            ECONNREFUSED;
#endif

        /// 触发「等可写」的轮数上限：跑满说明本机没有构造出写阻塞，而不是实现出错
        constexpr int kBlockingSendRoundLimit = 4096;

        /// 灌满对端接收队列的单轮负载长度
        constexpr std::size_t kInboundFillChunkLength = 16 * 1024;

        /**
         * @brief 把调用方给出的套接字连到回环上的临时端口，并交出它的对端描述符
         * @details 对端刻意不做成 AsyncSocket：收发的另一端由测试线程自己读写，挂事件循环只会
         *          多引入一个线程而不会让断言更强。监听描述符在交出对端之后立即关闭——POSIX 上
         *          关闭监听不影响已建立的连接。
         * @param loop 驱动连接协程的事件循环
         * @param socket 已 create() 出来的套接字，本函数把它连出去
         * @return int 对端描述符（非阻塞）；建链或接受失败时返回 -1，由调用方断言
         */
        int connectAndAcceptPeer(EventLoop &loop, AsyncSocket &socket)
        {
            AsyncSocket listener = AsyncSocket::create(loop);
            if (!listener.bind(InetAddress::localhost(0)) || !listener.listen(4))
            {
                return -1;
            }

            Task<> connecting = socket.asyncConnect(InetAddress::localhost(listener.localAddress().port()));
            connecting.handle().resume();
            if (!advanceUntil(loop, [&connecting]()
            {
                return connecting.isReady();
            }))
            {
                return -1;
            }

            const int peerDescriptor = Platform::Socket::accept(listener.fileDescriptor(), nullptr, nullptr);
            listener.close();
            return peerDescriptor;
        }

        /**
         * @brief 从读取侧有界读满指定字节数
         * @details 对端已经写完才调用：此刻字节全在内核队列里，本函数只负责把它们取出来，
         *          因此不需要推进事件循环。
         * @param descriptor 读取侧描述符
         * @param expectedLength 期望读到的字节数
         * @return std::string 实际读到的字节；不足时由调用方按长度报红
         */
        std::string drainBytes(const int descriptor, const std::size_t expectedLength)
        {
            std::string received;
            received.reserve(expectedLength);
            std::array<char, 16 * 1024> buffer{};

            static_cast<void>(waitForCondition(
                    [&]()
                    {
                        const ssize_t readLength = Platform::FileDescriptor::read(descriptor, buffer.data(), buffer.size());
                        if (readLength > 0)
                        {
                            received.append(buffer.data(), static_cast<std::size_t>(readLength));
                            return received.size() >= expectedLength;
                        }
                        // 读到真错误就提前收手：继续等只会把「对端重置」拖成超时，报出来的原因也是错的
                        return readLength < 0 &&
                               Platform::PlatformError::lastSocketErrorCode() != Platform::PlatformError::kWouldBlock;
                    }));
            return received;
        }

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
     * @brief create() 交出的套接字不得随进程创建传给子进程
     * @details POSIX 侧由 SOCK_CLOEXEC 在建套接字时一并置入；Windows 的 ::socket 没有等价标志位，
     *          句柄默认就是可继承的，必须建完取消继承位。留着它，消费者一 spawn 就把监听端口与已建立
     *          的连接交了出去——父进程关掉描述符也无济于事，子进程那份引用还占着端口。
     */
    TEST(AsyncSocket, CreatedSocketIsNotInheritable)
    {
        EventLoop   loop;
        AsyncSocket asyncSocket = AsyncSocket::create(loop);
        ASSERT_GE(asyncSocket.fileDescriptor(), 0);

        EXPECT_TRUE(Platform::TestSupport::isNotInheritable(asyncSocket.fileDescriptor()))
                << "套接字可被继承，spawn 出去的子进程会替父进程占住这个端口";

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
     * @brief 验证监听套接字收口只关掉本端这一份引用，不把端点一起停掉
     * @details 零停机换代靠的就是「同一端点上有两份引用」：新一代从 SCM_RIGHTS 里收到的那一份，
     *          与 POSIX 上 dup() 出来的这一份同性质（指向同一个开放文件描述）。收口若照已建立连接的
     *          流程去 shutdown(SHUT_RDWR)，停的是**端点**而不是本端引用，另一份就再也接不到连接
     *          （父代收口后子代十次连接全失败的实测就是这么来的）。
     *          只在 POSIX 上断言：Windows 上 dup 不出「同一端点的第二份引用」，那句柄移交要跨进程
     *          才成立，那条链由 samples/core_upgrade 在真机上验。
     */
    TEST(AsyncSocket, CloseOfListeningSocketLeavesDuplicatedDescriptorAccepting)
    {
#if !ASYN_PLATFORM_WIN32
        EventLoop loop;
        AsyncSocket listener = AsyncSocket::create(loop);
        ASSERT_TRUE(listener.bind(InetAddress::localhost(0)));
        ASSERT_TRUE(listener.listen(4));
        const std::uint16_t port = listener.localAddress().port();

        const int adoptedDescriptor = ::dup(listener.fileDescriptor());
        ASSERT_GE(adoptedDescriptor, 0) << "dup 失败就构造不出「同一端点的第二份引用」";

        listener.close();

        const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(client, 0);
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = htons(port);
        const bool isConnected = ::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
        EXPECT_TRUE(isConnected) << "关掉本端引用把端点一起停了：换代时新一代接不到连接";
        if (isConnected)
        {
            // 连接已在队列里，另一份引用上应当直接 accept 得到它
            const int accepted = Platform::Socket::accept(adoptedDescriptor, nullptr, nullptr);
            EXPECT_GE(accepted, 0);
            if (accepted >= 0)
            {
                Platform::FileDescriptor::close(accepted);
            }
        }
        Platform::FileDescriptor::close(client);
        Platform::FileDescriptor::close(adoptedDescriptor);
#else
        GTEST_SKIP() << "Windows 上 dup 不出同一端点的第二份引用，这条判据在 POSIX 侧实测";
#endif
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
     * @brief 连接被拒要由 IO 后端报回来，而不是等上层的看门狗
     *
     * @details Windows 上这一条曾经走不通：非阻塞 `connect()` 之后 IOCP 永远不给可写事件——连接中的
     *          套接字上零字节 `WSASend` 连投递都上不去（实测 WSAENOTCONN 贯穿整个连接期），于是
     *          `asyncConnect` 只能靠时限收场，报出来的原因是「等待期间套接字被关闭」而不是「对端拒绝」。
     *          现在连接是一次真正的重叠操作：完成通知带着译回 Winsock 空间的错误码回来。
     *
     * @note 判据取错误身份而不是墙钟毫秒：回环上「被拒」的完成时刻由内核决定（实测约 2 秒），
     *       把毫秒钉进用例等于赌调度运气。
     */
    TEST(AsyncSocket, ReportsRefusedConnectThroughTheIoBackend)
    {
        EventLoop loop;

        AsyncSocket listener = AsyncSocket::create(loop);
        ASSERT_TRUE(listener.bind(InetAddress(static_cast<std::uint16_t>(0), "127.0.0.1")));
        ASSERT_TRUE(listener.listen(1));
        const std::uint16_t deadPort = listener.localAddress().port();
        ASSERT_GT(deadPort, 0);
        listener.close(); // 关掉之后这条端口没人听：连它会被内核拒绝

        AsyncSocket client     = AsyncSocket::create(loop);
        Task<>      connecting = client.asyncConnect(InetAddress("127.0.0.1", deadPort));
        connecting.handle().resume();

        ASSERT_TRUE(advanceUntil(loop, [&connecting]
        {
            return connecting.isReady();
        }, std::chrono::seconds{15}))
                << "连接被拒却没醒：等待方只能靠看门狗收场，这正是要修掉的形态";

        int  reportedCode = 0;
        bool isRefused    = false;
        try
        {
            connecting.handle().promise().result();
            FAIL() << "连一个没人听的端口居然成功了";
        } catch (const Base::SystemException &failure)
        {
            reportedCode = failure.nativeError();
            isRefused    = reportedCode == kExpectedRefusedErrorCode;
        }
        EXPECT_TRUE(isRefused) << "醒来是醒来了，但报的错误码不是「对端拒绝」：实测拿到 "
                               << reportedCode << "（期望 " << kExpectedRefusedErrorCode << "）";
        client.close();
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
        EXPECT_THROW(static_cast<void>(emptyTask.handle().promise().result()), Base::InvalidArgumentException);

        constexpr std::size_t                                          kTooManyCount = Platform::Socket::kMaximumVectorCount + 1;
        const std::array<Platform::Socket::WriteBuffer, kTooManyCount> tooManyBuffers{};
        Task<ssize_t> tooManyTask = socket.asyncSendVectored(tooManyBuffers.data(), kTooManyCount);
        tooManyTask.handle().resume();
        ASSERT_TRUE(tooManyTask.isReady());
        EXPECT_THROW(static_cast<void>(tooManyTask.handle().promise().result()), Base::InvalidArgumentException);

        Task<ssize_t> singleTask = socket.asyncSendVectored(single, 1);
        singleTask.handle().resume();
        ASSERT_TRUE(singleTask.isReady());
        // 参数合法但描述符无效：以平台错误收场，而不是抛参数类异常
        EXPECT_THROW(singleTask.handle().promise().result(), Base::SystemException);
    }

    /**
     * @brief 等待标记只报「真的有人在等」的那个方向
     * @details 这两个标记是 TLS 侧判断「能不能去抢另一方向的等待槽」的唯一依据：一个方向只允许
     *          一个等待者，抢槽会直接抛 LogicException。标记多报会让读侧白等一次定时让出，
     *          少报则把「槽位已被占」当成空位，于是把别人的等待者挤掉。
     */
    TEST(AsyncSocket, WaitingFlagsReportOnlyTheDirectionThatHasAWaiter)
    {
        EventLoop   loop;
        AsyncSocket socket = AsyncSocket::create(loop);
        const int   peerDescriptor = connectAndAcceptPeer(loop, socket);
        ASSERT_GE(peerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        // 注册对象是首次等待时才建的，之前两个方向都必须报「没人等」
        EXPECT_FALSE(socket.isWaitingReadable()) << "还没人读过这条连接，读方向就报在等";
        EXPECT_FALSE(socket.isWaitingWritable()) << "还没人写过这条连接，写方向就报在等";

        std::string      buffer(16, '\0');
        Task<ssize_t>    receiving = socket.asyncReceive(buffer.data(), buffer.size());
        receiving.handle().resume();
        ASSERT_FALSE(receiving.isReady()) << "对端没写，读却没挂到「等可读」上";
        EXPECT_TRUE(socket.isWaitingReadable()) << "读方向已挂上等待者却没被认出来";
        EXPECT_FALSE(socket.isWaitingWritable()) << "只有读方向有人在等，写方向却报了「在等」：TLS 会因此让出不必要的等待";

        ASSERT_EQ(Platform::FileDescriptor::write(peerDescriptor, "ping", 4), 4);
        ASSERT_TRUE(advanceUntil(loop, [&receiving]()
        {
            return receiving.isReady();
        })) << "对端已写入，读协程却没被叫醒";
        EXPECT_FALSE(socket.isWaitingReadable())
                << "等待者已被取走，标记却还留着：后续判断会以为这个槽位仍被占着";

        socket.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief releaseFileDescriptor() 交出所有权后，本对象既不再持有也不得关掉那个描述符
     * @details 这条通道用来把刚 accept 的连接转交给别的循环。误关的后果不是「连接断了」而是
     *          「别人的连接断了」：描述符号一还系统就可能被下一条 accept 复用，第二次关闭等于
     *          替别人掐线，且现场完全指不到本类。
     *          用例不用 fcntl 之类的方式问「还开着没」，而是在 client 析构之后再拿那个号写一次
     *          并从对端读回来：能被写、内容按序到达，才同时证明「没被误关」与「交出去的是可用的连接」。
     */
    TEST(AsyncSocket, ReleasedFileDescriptorStaysOpenAndUsableAfterTheSocketDies)
    {
        int releasedDescriptor = -1;
        int peerDescriptor     = -1;
        {
            EventLoop   loop;
            AsyncSocket client = AsyncSocket::create(loop);
            peerDescriptor     = connectAndAcceptPeer(loop, client);
            ASSERT_GE(peerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

            const int heldDescriptor = client.fileDescriptor();
            releasedDescriptor       = client.releaseFileDescriptor();
            EXPECT_EQ(releasedDescriptor, heldDescriptor) << "交出的不是本对象原先持有的那个号";
            EXPECT_EQ(client.fileDescriptor(), -1) << "交出之后本对象仍认为自己持有那个号：析构就会再关一次";

            // 出作用域：client 与 loop 依次析构。此刻交出去的描述符没有任何主人
        }

        ASSERT_EQ(Platform::FileDescriptor::write(releasedDescriptor, "x", 1), 1)
                << "持有者析构把已交出的描述符一起关掉了：那个号可能已被下一条连接复用";
        EXPECT_EQ(drainBytes(peerDescriptor, 1), "x") << "交出去的描述符写不进去，等于移交了一条坏连接";

        Platform::FileDescriptor::close(releasedDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
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

    /**
     * @brief 参数取值非法必须落在「用法错误」那条分支上，而不是运行期故障链
     * @details 明文套接字与 TLS 套接字在 HttpSession 里是可替换的（调用方用 requires 探测能力），
     *          同一条误用因此必须走同一条分支：TlsSocket 早就把这类拒绝放在 std::invalid_argument
     *          上，而本类原先混在 SystemException 里。落错分支的两种后果都对不上：只
     *          `catch (Base::Exception)` 的调用点会把用法错误当成「可恢复故障」重试一遍，而按
     *          运行期故障统计的地方又会让真正的坏参数在指标里隐身。
     * @note 参数取值的拒绝重试不会变好，这是它与「对端重置」这类运行期故障的分界
     */
    TEST(AsyncSocket, ParameterValueRejectionsUseTheLogicErrorBranch)
    {
        static_assert(!std::is_base_of_v<Base::Exception, Base::InvalidArgumentException>,
                      "用法错误一旦被并入运行期故障链，本用例的判据就失效了");

        EventLoop           loop;
        AsyncSocket         socket(loop, -1); // 描述符无效不影响本用例：这些判定都早于任何 I/O
        std::array<char, 8> buffer{};

        const auto runAndReportBranch = [](const std::function<Task<ssize_t>(void)> &start)
        {
            Task<ssize_t> pending = start();
            pending.handle().resume();
            try
            {
                static_cast<void>(pending.handle().promise().result());
                ADD_FAILURE() << "参数非法却没有抛错";
            } catch (const Base::Exception &)
            {
                ADD_FAILURE() << "误用被报成了运行期故障：只 catch Base::Exception 的调用点会把它当成可恢复故障";
            } catch (const std::logic_error &error)
            {
                // 只验分支：各条拒绝的具体文案由对应的用例逐条钉住
                EXPECT_FALSE(std::string_view(error.what()).empty()) << "报错文案是空的，运维无从定位是哪个参数";
            } catch (const std::exception &error)
            {
                ADD_FAILURE() << "既不在 logic_error 分支也不在框架异常链上：" << error.what();
            }
        };

        const std::size_t oversizedLength = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
        runAndReportBranch([&]() { return socket.asyncSend(buffer.data(), oversizedLength); });
        runAndReportBranch([&]() { return socket.asyncReceive(buffer.data(), oversizedLength); });
        runAndReportBranch([&]() { return socket.asyncSendVectored(nullptr, 0); });

        socket.close();
    }

    /**
     * @brief 接收返回对端写来的那一段字节
     * @details 明文 TCP 读路径的头号契约：返回实际字节数、且字节内容与对端写的一致。
     *          数据在 co_await 之前就已到达，因此这条走的是「一次 recv 就拿到」的快路径。
     */
    TEST(AsyncSocket, AsyncReceiveReturnsWhatThePeerWrote)
    {
        EventLoop   loop;
        AsyncSocket reader = AsyncSocket::create(loop);
        const int   peerDescriptor = connectAndAcceptPeer(loop, reader);
        ASSERT_GE(peerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        const std::string payload = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ASSERT_EQ(Platform::FileDescriptor::write(peerDescriptor, payload.data(), payload.size()),
                  static_cast<ssize_t>(payload.size())) << "对端没能把请求写进来";

        std::string buffer(256, '\0');
        Task<ssize_t> receiving = reader.asyncReceive(buffer.data(), buffer.size());
        receiving.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&receiving]()
        {
            return receiving.isReady();
        })) << "已到期的数据没能读完";

        const ssize_t receivedLength = receiving.handle().promise().result();
        ASSERT_GT(receivedLength, 0) << "对端明明写了数据，接收却报「没有」";
        EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(receivedLength)),
                  payload.substr(0, static_cast<std::size_t>(receivedLength)));

        reader.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 没有数据时协程挂到「等可读」上，对端随后写入才把它叫醒
     * @details 钉住 EAGAIN 分支：这条路径若把「等待失败」当成「读到了 0 字节」，或反过来在没数据时
     *          就地返回 0，表现都是连接被当成对端关闭而掐掉。用例先断言「确实挂住了」，再写入，
     *          因此唤醒与内容两件事都是用例自己造成的，不依赖调度运气。
     */
    TEST(AsyncSocket, AsyncReceiveWaitsUntilThePeerWrites)
    {
        EventLoop   loop;
        AsyncSocket reader = AsyncSocket::create(loop);
        const int   peerDescriptor = connectAndAcceptPeer(loop, reader);
        ASSERT_GE(peerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        std::string buffer(64, '\0');
        Task<ssize_t> receiving = reader.asyncReceive(buffer.data(), buffer.size());
        receiving.handle().resume();
        ASSERT_FALSE(receiving.isReady()) << "对端一个字都没写，接收却已经返回：没有真的挂到「等可读」上";

        const std::string payload = "PING\r\n";
        ASSERT_EQ(Platform::FileDescriptor::write(peerDescriptor, payload.data(), payload.size()),
                  static_cast<ssize_t>(payload.size()));

        ASSERT_TRUE(advanceUntil(loop, [&receiving]()
        {
            return receiving.isReady();
        })) << "对端已经写入，挂在等可读上的接收却没被叫醒";
        const ssize_t receivedLength = receiving.handle().promise().result();
        ASSERT_EQ(receivedLength, static_cast<ssize_t>(payload.size()));
        EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(receivedLength)), payload);

        reader.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 对端正常关闭（FIN）读成 0，而不是抛错也不是挂住
     * @details 「0 = 对端关闭」是整个读路径赖以收口的信号：读循环据此结束协程、据此把响应写干净。
     *          它若变成 EBADF 异常或永久挂起，管线化的最后一条请求就会以故障收场。
     */
    TEST(AsyncSocket, AsyncReceiveReportsPeerClosureAsZero)
    {
        EventLoop   loop;
        AsyncSocket reader = AsyncSocket::create(loop);
        const int   peerDescriptor = connectAndAcceptPeer(loop, reader);
        ASSERT_GE(peerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        // 先关掉对端：FIN 已在路上，随后的接收只会读到 EOF
        Platform::FileDescriptor::close(peerDescriptor);

        std::string buffer(64, '\0');
        Task<ssize_t> receiving = reader.asyncReceive(buffer.data(), buffer.size());
        receiving.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&receiving]()
        {
            return receiving.isReady();
        })) << "对端已经关闭，读侧却还在等一个永远不会来的可读";
        EXPECT_EQ(receiving.handle().promise().result(), 0) << "对端正常关闭必须读成 0";

        reader.close();
    }

    /**
     * @brief 长度为 0 的接收是「什么都不做」，且不得被当成对端关闭
     * @details 底层 recv(fd, buf, 0) 返回 0，而 0 正是「对端正常关闭」的编码值：两者不可区分。
     *          本方法因此在任何 I/O 之前短路掉，调用方绝不能拿「缓冲区剩余空间」当长度传进来
     *          （算出 0 就会被误读成 EOF）。这里用无效描述符证明短路真的先于任何系统调用。
     */
    TEST(AsyncSocket, AsyncReceiveWithZeroLengthShortCircuitsBeforeAnyIo)
    {
        EventLoop   loop;
        AsyncSocket socket(loop, -1); // 描述符无效：若真去 recv，只会拿到 EBADF 异常
        std::array<char, 8> buffer{};

        Task<ssize_t> receiving = socket.asyncReceive(buffer.data(), 0);
        receiving.handle().resume();
        ASSERT_TRUE(receiving.isReady()) << "长度为 0 的请求不该挂起";
        EXPECT_EQ(receiving.handle().promise().result(), 0) << "短路返回值必须是 0（调用方据此知道「没读」）";
    }

    /**
     * @brief 单次接收长度超过 INT_MAX 当场拒绝，而不是让底层静默窄化
     * @details recv 的长度形参是 int，强转过去会拿到一个可疑的负数（Windows 上更是直接进 WSA 参数）。
     *          这条拒绝先于任何系统调用，因此无效描述符也能验证到。
     */
    TEST(AsyncSocket, AsyncReceiveRejectsLengthBeyondSingleCallCeiling)
    {
        EventLoop   loop;
        AsyncSocket socket(loop, -1);
        std::array<char, 8> buffer{};

        const std::size_t oversizedLength = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
        Task<ssize_t>     receiving       = socket.asyncReceive(buffer.data(), oversizedLength);
        receiving.handle().resume();
        ASSERT_TRUE(receiving.isReady()) << "超限长度应当场失败，而不是挂起等一次永远不会来的可读";

        bool isRejectedWithReason = false;
        try
        {
            static_cast<void>(receiving.handle().promise().result());
        } catch (const Base::InvalidArgumentException &exception)
        {
            isRejectedWithReason = std::string_view(exception.what()).find("超过上限") != std::string_view::npos;
        } catch (const Base::Exception &)
        {
            isRejectedWithReason = false;
        }
        EXPECT_TRUE(isRejectedWithReason) << "没按「长度超限」的用法错误拒绝：调用方会以为可以换个缓冲区重试";
    }

    /**
     * @brief 本端已关闭时读的是「故障」，不能报成 0（那会被当成对端正常关闭）
     * @details 这两个 0 的含义正好相反：EOF 意味着「数据读完了，收口」，而本端关闭意味着
     *          「这条连接早就不在了」。把后者报成前者，上层会以为收到了一次干净的结束。
     */
    TEST(AsyncSocket, AsyncReceiveOnClosedSocketThrowsInsteadOfReportingEof)
    {
        EventLoop   loop;
        AsyncSocket socket = AsyncSocket::create(loop);
        ASSERT_GE(socket.fileDescriptor(), 0);
        socket.close();

        std::array<char, 16> buffer{};
        Task<ssize_t>        receiving = socket.asyncReceive(buffer.data(), buffer.size());
        receiving.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&receiving]()
        {
            return receiving.isReady();
        })) << "本端已关闭，接收却挂住了";

        bool isReportedAsFailure = false;
        try
        {
            const ssize_t receivedLength = receiving.handle().promise().result();
            isReportedAsFailure          = false;
            static_cast<void>(receivedLength);
        } catch (const Base::Exception &)
        {
            isReportedAsFailure = true;
        }
        EXPECT_TRUE(isReportedAsFailure) << "本端已关闭被报成了「读到 0 字节」：上层会把故障当成干净的 EOF";
    }

#if ASYN_PLATFORM_WIN32
    namespace
    {
        /**
         * @brief 等一次描述符可读
         * @details Windows 上对监听描述符走这一步的意义不止是「等」：接受探针（AcceptEx）要到
         *          首次等待才投出去，不先等一次就永远不会有连接可取。
         * @param socket 目标套接字
         * @return Task<bool> 惰性协程；true 表示事件就绪
         */
        Task<bool> waitReadableOnce(AsyncSocket &socket)
        {
            co_return co_await socket.waitReadable();
        }
    } // namespace

    /**
     * @brief Windows 的接受路径只能从后端取连接，且一条只交一次
     * @details AcceptEx 完成时连接已被摘下并接进后端自己的接受套接字，`::accept()` 看不到它，
     *          取不到就会一直「在监听却不应答」。这条通道此前零直测（只有 TcpAcceptor 在用），
     *          而它最容易出的两种错都能在这里现形：没有连接时凭空的「取到一个」（交出去的是
     *          垃圾句柄），以及同一条连接被交两次（两条会话共用一个 socket，数据互相串台）。
     * @note 取到的句柄要能用：客户端写两个字节、从这个句柄读回来，才算证明交出的就是那条连接。
     */
    TEST(AsyncSocket, AcceptedConnectionsAreHandedOutOneAtATime)
    {
        EventLoop   loop;
        AsyncSocket listener = AsyncSocket::create(loop);
        ASSERT_TRUE(listener.bind(InetAddress::localhost(0)));
        ASSERT_TRUE(listener.listen(4));
        const std::uint16_t listeningPort = listener.localAddress().port();
        ASSERT_GT(listeningPort, 0U);

        EXPECT_FALSE(listener.takeAcceptedConnection().has_value())
                << "一条连接都没到，后端却报「取到一个」：交出去的会是垃圾句柄";

        // 先武装接受探针，再让客户端连上来：顺序反过来就可能永远等不到完成通知
        Task<bool> arming = waitReadableOnce(listener);
        arming.handle().resume();
        ASSERT_FALSE(arming.isReady()) << "刚武装就拿到事件：本用例没测到「等一条连接到达」";

        AsyncSocket client      = AsyncSocket::create(loop);
        Task<>      connecting  = client.asyncConnect(InetAddress::localhost(listeningPort));
        connecting.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&connecting, &arming]()
        {
            return connecting.isReady() && arming.isReady();
        })) << "客户端连上之后，监听描述符上的接受探针没被叫醒";
        EXPECT_NO_THROW(connecting.handle().promise().result()) << "回环上的连接没能建立成功";

        ASSERT_TRUE(arming.handle().promise().result()) << "接受探针交回的是「未就绪」";
        const std::optional<int> acceptedDescriptor = listener.takeAcceptedConnection();
        ASSERT_TRUE(acceptedDescriptor.has_value()) << "AcceptEx 完成之后必须能取到已接入的连接";

        EXPECT_FALSE(listener.takeAcceptedConnection().has_value())
                << "同一条连接被交出去两次：两条会话会共用同一个 socket";

        const std::string payload = "hi";
        ASSERT_EQ(Platform::FileDescriptor::write(client.fileDescriptor(), payload.data(), payload.size()),
                  static_cast<ssize_t>(payload.size()));
        EXPECT_EQ(drainBytes(*acceptedDescriptor, payload.size()), payload)
                << "取到的句柄读不到客户端写的字节：交出的不是那条连接";

        client.close();
        listener.close();
        Platform::FileDescriptor::close(*acceptedDescriptor);
    }
#endif

#if !ASYN_PLATFORM_WIN32
    namespace
    {
        /// 普通样本文件长度：够大到必须分块才能读完，又小到默认套接字缓冲能整块吞下（不挂起）
        constexpr std::size_t kSampleFileLength = 64 * 1024;

        /// 背压用例的负载长度：必须远大于「压小之后的发送队列 + 接收队列」，才会写到一半停手
        constexpr std::size_t kBackpressureFileLength = 512 * 1024;

        /// 压窗口的目标字节数：内核会把它向上钳到自身下限，实际值比这里大，但仍是十几 KB 量级
        constexpr int kTinySocketBufferBytes = 4096;

        /// 背压用例里读取侧的接收缓冲：留出余量，好让窗口腾开时不必等内核的零窗口探测重传定时器
        constexpr int kReaderReceiveBufferBytes = 64 * 1024;

        /// 一次从读取侧取走的字节数上限
        constexpr std::size_t kDrainChunkLength = 32 * 1024;

        /// 空转轮数：不读对端时先把发送窗口写满，靠这个轮数把「挂起在等可写上」构造出来
        constexpr int kIdlePumpRoundCount = 50;

        /**
         * @brief 生成「字节值 = 下标 mod 251」的样本内容
         * @details 内容必须随位置变化：整个文件填同一个字节时，错位、重复、少发一截都核对不出来
         * @param length 内容长度
         * @return std::string 与文件字节逐位相同的样本
         */
        std::string makeSampleContent(const std::size_t length)
        {
            std::string content(length, '\0');
            for (std::size_t index = 0; index < length; ++index)
            {
                content[index] = static_cast<char>(static_cast<unsigned char>(index % 251));
            }
            return content;
        }

        /**
         * @brief 在一个短时间窗内观察读取侧收到了多少字节
         * @details 「什么都没有发出去」这类否定断言不能靠默认超时来等：等满 5 秒既拖慢用例，
         *          也只是把「没有」读成「还没到」。回环上的字节若会到达，微秒级就到了。
         * @param descriptor 读取侧描述符
         * @param observationWindow 观察窗口
         * @return std::size_t 窗口内读到的字节数
         */
        std::size_t readWithinWindow(const int descriptor, const std::chrono::milliseconds observationWindow)
        {
            std::size_t              receivedLength = 0;
            std::array<char, 1024>   buffer{};
            const auto               deadline = std::chrono::steady_clock::now() + observationWindow;
            while (std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t readLength = Platform::FileDescriptor::read(descriptor, buffer.data(), buffer.size());
                if (readLength > 0)
                {
                    receivedLength += static_cast<std::size_t>(readLength);
                    continue;
                }
                // 读到真错误就此收手：它同样说明没有正文字节上线，继续轮询只会把结论拖得更模糊
                if (readLength < 0 &&
                    Platform::PlatformError::lastSocketErrorCode() != Platform::PlatformError::kWouldBlock)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return receivedLength;
        }

        /**
         * @brief 驱动一个零拷贝任务到结束，交出它抛出的异常文案
         * @param loop 驱动协程的事件循环
         * @param sending 待驱动的零拷贝发送任务（尚未 resume）
         * @return std::optional<std::string> 抛错时给出文案；正常返回结果时为空
         */
        std::optional<std::string> driveToExceptionText(EventLoop &loop, Task<ssize_t> &sending)
        {
            sending.handle().resume();
            const bool isTaskSettled = advanceUntil(loop, [&sending]()
            {
                return sending.isReady();
            });
            EXPECT_TRUE(isTaskSettled) << "零拷贝任务既没成功也没报错，一直挂在那里";

            try
            {
                static_cast<void>(sending.handle().promise().result());
            } catch (const std::exception &exception)
            {
                // 两条分支都收：参数非法走 logic_error，发送故障走框架的运行期故障链
                return std::string(exception.what());
            }
            return std::nullopt;
        }
    } // namespace

    /**
     * @brief 源描述符非法与待发字节数为 0 都在当场被拒绝，而不是发到内核再去猜
     * @details 这两种入参交给底层会折成同一个 kInvalidArgument，调用方无从分辨是自己传错了
     *          哪一项。用例刻意给「长度为 0」配一个**有效**的文件描述符：它仍须按「字节数为 0」
     *          报错，说明这条判定不是靠无效描述符顺带蒙对的。
     */
    TEST(AsyncSocket, AsyncSendFileRejectsInvalidSourceDescriptorAndEmptyLength)
    {
        EventLoop                loop;
        AsyncSocket              socket(loop, -1); // 描述符无效不影响本用例：参数校验早于任何 I/O
        TemporaryDirectory          directory("AsyncSendFile");
        ASSERT_TRUE(directory.writeFile("sample.bin", makeSampleContent(1024)));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        Task<ssize_t> invalidDescriptor = socket.asyncSendFile(-1, 0, 1024);
        invalidDescriptor.handle().resume();
        ASSERT_TRUE(invalidDescriptor.isReady());
        EXPECT_THROW(static_cast<void>(invalidDescriptor.handle().promise().result()), Base::InvalidArgumentException)
                << "源描述符非法是传进来的取值不对，必须落在用法错误那条分支上";

        Task<ssize_t> emptyLength = socket.asyncSendFile(mappedFile.nativeFileDescriptor(), 0, 0);
        const std::optional<std::string> emptyFailure = driveToExceptionText(loop, emptyLength);
        ASSERT_TRUE(emptyFailure.has_value()) << "待发字节数为 0 时被静默当成「已经发完」了";
        EXPECT_NE(emptyFailure->find("待发字节数为 0"), std::string::npos)
                << "报错没有点明是长度为 0：文案为 " << *emptyFailure;

        socket.close();
    }

    /**
     * @brief 零拷贝只发送请求的那一段，且不动源描述符自身的读写偏移
     * @details 第二条用例钉的是「同一个文件被多条响应共用」：Range 响应各取一段，若 sendfile
     *          改用了描述符自带的偏移，第二条就会从第一条停下的位置接着读，正文错位却不报错。
     * @note 走真实回环 TCP：POSIX 侧的 socketpair 是 AF_UNIX，sendfile 对它直接 EINVAL。
     */
    TEST(AsyncSocket, AsyncSendFileDeliversRequestedRangeWithoutMovingFileOffset)
    {
        EventLoop                loop;
        AsyncSocket              sender = AsyncSocket::create(loop);
        const int                readerDescriptor = connectAndAcceptPeer(loop, sender);
        ASSERT_GE(readerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        constexpr std::uint64_t sliceOffset        = 4096;
        constexpr std::size_t   sliceLength        = 8192;
        constexpr std::size_t   followUpLength     = 1024;

        TemporaryDirectory          directory("AsyncSendFileRange");
        const std::string               sampleContent = makeSampleContent(kSampleFileLength);
        ASSERT_TRUE(directory.writeFile("sample.bin", sampleContent));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        // 先中段、再开头：顺序本身就是判据，第二次的起点若被第一次推进过，收到的就不是 content[0..]
        Task<ssize_t> firstSlice = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), sliceOffset, sliceLength);
        firstSlice.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&firstSlice]()
        {
            return firstSlice.isReady();
        })) << "中段切片没能跑完";
        EXPECT_EQ(firstSlice.handle().promise().result(), static_cast<ssize_t>(sliceLength));

        Task<ssize_t> headSlice = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), 0, followUpLength);
        headSlice.handle().resume();
        ASSERT_TRUE(advanceUntil(loop, [&headSlice]()
        {
            return headSlice.isReady();
        })) << "文件头切片没能跑完";
        EXPECT_EQ(headSlice.handle().promise().result(), static_cast<ssize_t>(followUpLength));

        const std::string expected =
                sampleContent.substr(static_cast<std::size_t>(sliceOffset), sliceLength) +
                sampleContent.substr(0, followUpLength);
        EXPECT_EQ(drainBytes(readerDescriptor, expected.size()), expected)
                << "收到的正文与「按位置取的切片」不一致：偏移要么被动过，要么发错了段";

        sender.close();
        Platform::FileDescriptor::close(readerDescriptor);
    }

    /**
     * @brief 发送窗口写满之后协程挂起，对端一边读一边续发直到全部发完
     * @details 钉住 EAGAIN 分支：这条路径若写成「等可写之后就地返回」或「不重发已算过的偏移」，
     *          表现是大文件响应缺一截或永久挂起，而这正是慢消费者最常见的情形。
     *          先空转若干轮、期间一次都不读，用「此时仍未结束」证明本机确实构造出了写阻塞；
     *          随后每推进一轮循环就取走一批字节，进度由测试线程自己造成，不依赖任何调度运气。
     */
    TEST(AsyncSocket, AsyncSendFileResumesAndCompletesAfterTheSendWindowFillsUp)
    {
        EventLoop                loop;
        AsyncSocket              sender = AsyncSocket::create(loop);
        const int                readerDescriptor = connectAndAcceptPeer(loop, sender);
        ASSERT_GE(readerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        // 发送队列压到内核下限、接收侧留 64 KiB：总容量远小于正文，写到 EAGAIN 必然发生。
        // 接收侧不能再压小——两侧都压到下限会让对端窗口彻底关死，此后每腾一次窗口都要等内核的
        // 零窗口探测定时器（实测 512 KiB 要花 9.5 秒，而 copy 路径同样 9.6 秒：那是内核的粒度，
        // 不是本方法的速度）
        int sendBufferLength = kTinySocketBufferBytes;
        ASSERT_TRUE(sender.setSockOpt(SOL_SOCKET, SO_SNDBUF, &sendBufferLength, sizeof(sendBufferLength)));
        ASSERT_TRUE(Platform::Socket::setReceiveBufferSize(readerDescriptor, kReaderReceiveBufferBytes));

        TemporaryDirectory          directory("AsyncSendFileBackpressure");
        const std::string               sampleContent = makeSampleContent(kBackpressureFileLength);
        ASSERT_TRUE(directory.writeFile("sample.bin", sampleContent));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        Task<ssize_t> sending = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), 0, kBackpressureFileLength);
        sending.handle().resume();
        for (int round = 0; round < kIdlePumpRoundCount; ++round)
        {
            stepLoopOnce(loop, 0);
        }
        ASSERT_FALSE(sending.isReady())
                << "对端一次都没读，512 KB 正文却已经报称发完：本机构造不出写阻塞，断言失去意义";

        std::string               received;
        received.reserve(kBackpressureFileLength);
        std::array<char, kDrainChunkLength> readBuffer{};
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (received.size() < kBackpressureFileLength && std::chrono::steady_clock::now() < drainDeadline)
        {
            // 先推进循环再读：内核刚腾出的窗口要在这一轮就用掉，反序会让每轮都白等一拍
            stepLoopOnce(loop, 0);
            const ssize_t readLength = Platform::FileDescriptor::read(readerDescriptor, readBuffer.data(), readBuffer.size());
            if (readLength > 0)
            {
                received.append(readBuffer.data(), static_cast<std::size_t>(readLength));
            }
        }

        ASSERT_TRUE(advanceUntil(loop, [&sending]()
        {
            return sending.isReady();
        })) << "对端持续在读，零拷贝发送却始终没有收尾";
        EXPECT_EQ(sending.handle().promise().result(), static_cast<ssize_t>(kBackpressureFileLength))
                << "挂起续发之后把返回值算错了：部分写没有被累计";
        EXPECT_EQ(received.size(), kBackpressureFileLength) << "读取侧没有收满：正文在窗口恢复时被丢了一段";
        EXPECT_EQ(received, sampleContent);

        sender.close();
        Platform::FileDescriptor::close(readerDescriptor);
    }

    /**
     * @brief 起始偏移已经在文件末尾之后：报错要说清是「起点越界」，不是「文件被中途改写」
     * @details 这是调用方自己算错偏移的情形（一次都没发出去、零字节上线）。此时把原因归给
     *          「服务期间有人改写了静态目录」会把排查支到完全相反的方向——日志里那条文案
     *          就是运维唯一的线索。
     */
    TEST(AsyncSocket, AsyncSendFileReportsOffsetPastEndOfFileAsStartOutOfRange)
    {
        EventLoop                loop;
        AsyncSocket              sender = AsyncSocket::create(loop);
        const int                readerDescriptor = connectAndAcceptPeer(loop, sender);
        ASSERT_GE(readerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        TemporaryDirectory          directory("AsyncSendFilePastEnd");
        ASSERT_TRUE(directory.writeFile("sample.bin", makeSampleContent(4096)));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        Task<ssize_t> sending = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), 4096, 1024);
        const std::optional<std::string> failureText = driveToExceptionText(loop, sending);
        ASSERT_TRUE(failureText.has_value()) << "起点已经在文件末尾之后，却被当成发成功了";
        EXPECT_NE(failureText->find("起点已在源文件末尾之后"), std::string::npos)
                << "偏移越界没有被如实报出来：文案为 " << *failureText;
        EXPECT_EQ(readWithinWindow(readerDescriptor, std::chrono::milliseconds(200)), 0u)
                << "既然一次都没发出去，读取侧不该收到任何字节";

        sender.close();
        Platform::FileDescriptor::close(readerDescriptor);
    }

    /**
     * @brief 请求长度越过文件末尾：报错，但已经发出去的前缀确实上线了
     * @details 钉两件事：①到达文件末尾按失败收口（不能悄悄只发一半就报成功）；②失败会留下部分
     *          字节——这正是调用方「失败后不得整块重发」的依据，否则前缀会在流里重复一遍。
     */
    TEST(AsyncSocket, AsyncSendFileThrowsWhenLengthRunsPastEndOfFileKeepingThePrefixOnTheWire)
    {
        EventLoop                loop;
        AsyncSocket              sender = AsyncSocket::create(loop);
        const int                readerDescriptor = connectAndAcceptPeer(loop, sender);
        ASSERT_GE(readerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        TemporaryDirectory          directory("AsyncSendFileOverLength");
        const std::string               sampleContent = makeSampleContent(8192);
        ASSERT_TRUE(directory.writeFile("sample.bin", sampleContent));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        Task<ssize_t> sending = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), 0, 8192 + 4096);
        const std::optional<std::string> failureText = driveToExceptionText(loop, sending);
        ASSERT_TRUE(failureText.has_value()) << "长度超出文件末尾时被当成发送成功，缺的那一截不会有人知道";
        EXPECT_NE(failureText->find("到达文件末尾"), std::string::npos)
                << "报的不是「文件比请求的短」：文案为 " << *failureText;

        // 前缀必须已经在线：这条断言是「调用方不得整块重发」这条契约唯一的实证
        EXPECT_EQ(drainBytes(readerDescriptor, sampleContent.size()), sampleContent)
                << "抛错之前发出去的前缀没有出现在读取侧：部分写的后果无法被判定了";

        sender.close();
        Platform::FileDescriptor::close(readerDescriptor);
    }

    /**
     * @brief 挂在「等可写」上的零拷贝发送被 close() 唤醒时，报的是「套接字已关闭」
     * @details 与 asyncSend 同一条收尾契约：关闭描述符不会让内核唤醒等待者，必须靠注销注册时
     *          主动投递的那次唤醒；少了它，这帧协程连同它持有的映射会一直留到进程退出。
     *          区分「被关闭」与「事件就绪」也在这里：两者都让协程往下走，只有报错原因不同。
     */
    TEST(AsyncSocket, AsyncSendFileReportsClosureWhileWaitingForWritable)
    {
        EventLoop                loop;
        AsyncSocket              sender = AsyncSocket::create(loop);
        const int                readerDescriptor = connectAndAcceptPeer(loop, sender);
        ASSERT_GE(readerDescriptor, 0) << "回环连接没有建起来，本用例的前置条件不成立";

        int sendBufferLength = kTinySocketBufferBytes;
        ASSERT_TRUE(sender.setSockOpt(SOL_SOCKET, SO_SNDBUF, &sendBufferLength, sizeof(sendBufferLength)));
        ASSERT_TRUE(Platform::Socket::setReceiveBufferSize(readerDescriptor, kTinySocketBufferBytes));

        TemporaryDirectory          directory("AsyncSendFileClosed");
        ASSERT_TRUE(directory.writeFile("sample.bin", makeSampleContent(kBackpressureFileLength)));
        const Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(directory.path() / "sample.bin");
        ASSERT_TRUE(mappedFile.isValid());

        Task<ssize_t> sending = sender.asyncSendFile(mappedFile.nativeFileDescriptor(), 0, kBackpressureFileLength);
        sending.handle().resume();
        for (int round = 0; round < kIdlePumpRoundCount; ++round)
        {
            stepLoopOnce(loop, 0);
        }
        ASSERT_FALSE(sending.isReady()) << "没能让发送挂到「等可写」上，本用例没有测到关闭唤醒";

        // 与连接清扫同一做法：在事件循环线程上关闭，唤醒由注册对象的析构投给调度器
        sender.close();
        loop.scheduler().runAll();

        ASSERT_TRUE(sending.isReady()) << "关闭套接字之后，挂在等可写上的零拷贝协程仍未被唤醒";
        std::optional<std::string> failureText;
        try
        {
            static_cast<void>(sending.handle().promise().result());
        } catch (const Base::Exception &exception)
        {
            failureText = std::string(exception.what());
        }
        ASSERT_TRUE(failureText.has_value()) << "被唤醒之后当成「写好了」继续跑，等于向已关闭的连接发数据";
        EXPECT_NE(failureText->find("等待可写期间套接字被关闭"), std::string::npos)
                << "报的不是关闭而是别的失败：文案为 " << *failureText;

        Platform::FileDescriptor::close(readerDescriptor);
    }
#endif
} // namespace AsynGyanis::Core
