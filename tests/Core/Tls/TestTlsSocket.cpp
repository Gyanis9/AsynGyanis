// TlsSocket 单元测试：构造、移动语义、安全关闭与地址查询，以及会话释放后的拒绝面（使用仓库预生成证书）
//
// 台账读数（HandshakeAllocationProfile，Release 形态、按两百次摊平）：
//   · 一次双向 TLS1.3 握手：11 块 / 640 B；
//   · 两侧接线（SSL 对象 + AsyncSocket + IoWatcher 构造析构）：0 块；两只立刻跑完的协程帧：0 块
//     ——帧确实从帧池拿。带 sanitizer 的构建按 CoroutinePool.h 的既定口径绕开池，那时每条形状
//     都会各多出一到两块（Debug 实测：握手 16、明文等待 7、两只帧 2）；
//   · 一次明文「向后端注册 + 等一次可读」：5 块 / 312 B。握手两侧各摊一次注册，也就是说
//     **TLS 自己这一层基本不花分配，读数的九成落在这条 I/O 等待与注册上**。别把它跟「每连接
//     注册成本台账」里 epoll 0 次、io_uring 3 次直接对照：那份量的是后端注册本身，这条还含着等待。

#include "Core/Tls/TlsSocket.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Exception/CoreException.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Platform/IO/FileDescriptor.h"

#include "CoreTestSupport.h"
#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::advanceUntil;

        /// 仓库内预生成的自签测试证书（CN=asyngyanis-test，有效期至 2036）
        const std::filesystem::path kTestCertificatePath =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

        /// 仓库内预生成的配套私钥
        const std::filesystem::path kTestKeyPath =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";
    }

    /**
     * @brief 构造后 fileDescriptor() 就是被包装的那个描述符，close() 由本对象负责收尾
     */
    TEST(TlsSocket, ConstructionWrapsDescriptor)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));
        EXPECT_EQ(tlsSocket.fileDescriptor(), localDescriptor);

        tlsSocket.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 移动构造把描述符整体交给新对象：新对象暴露的描述符等于原值，不重建连接
     */
    TEST(TlsSocket, MoveConstructionTransfersDescriptor)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket1(ssl, loop, AsyncSocket(loop, localDescriptor));
        const int wrappedDescriptor = tlsSocket1.fileDescriptor();

        TlsSocket tlsSocket2(std::move(tlsSocket1));
        EXPECT_EQ(tlsSocket2.fileDescriptor(), wrappedDescriptor);

        tlsSocket2.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 移动赋值同样转移描述符：不会出现两个 TlsSocket 管同一个 fd（也不会漏关旧的那一个）
     */
    TEST(TlsSocket, MoveAssignmentTransfersDescriptor)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int firstDescriptor = -1;
        int firstPeerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(firstDescriptor, firstPeerDescriptor));
        int secondDescriptor = -1;
        int secondPeerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(secondDescriptor, secondPeerDescriptor));

        SSL *firstSsl = tlsContext.createSSL(firstDescriptor);
        SSL *secondSsl = tlsContext.createSSL(secondDescriptor);
        ASSERT_NE(firstSsl, nullptr);
        ASSERT_NE(secondSsl, nullptr);

        TlsSocket tlsSocket1(firstSsl, loop, AsyncSocket(loop, firstDescriptor));
        TlsSocket tlsSocket2(secondSsl, loop, AsyncSocket(loop, secondDescriptor));

        const int transferredDescriptor = tlsSocket1.fileDescriptor();
        tlsSocket2 = std::move(tlsSocket1);

        EXPECT_EQ(tlsSocket2.fileDescriptor(), transferredDescriptor);

        tlsSocket2.close();
        Platform::FileDescriptor::close(firstPeerDescriptor);
        Platform::FileDescriptor::close(secondPeerDescriptor);
    }

    /**
     * @brief 重复 close() 幂等且安全：第二次不会重复释放底层资源，也不抛异常
     */
    TEST(TlsSocket, DoubleCloseIsSafe)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));
        tlsSocket.close();

        // 双重关闭应该是安全的
        EXPECT_NO_THROW(tlsSocket.close());

        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 未握手时 ALPN 协商结果为空串：调用方据此知道「此刻读到的空结果不代表没协商」
     * @details 与 h2 分流直接相关：会话必须等握手完成之后再读这个值，握手前读到的一律是空串。
     */
    TEST(TlsSocket, SelectedAlpnProtocolIsEmptyBeforeHandshake)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));
        EXPECT_TRUE(tlsSocket.selectedAlpnProtocol().empty()) << "握手尚未发生，不该有 ALPN 协商结果";

        tlsSocket.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 「上下文建的 SSL + 描述符对」这条组合路径自洽：暴露的描述符与传入的一致
     * @details 与 ConstructionWrapsDescriptor 的区别是本用例显式断言包装结果，覆盖 SSL 对象归属的完整链路
     */
    TEST(TlsSocket, WrapsContextCreatedSslOverSocketPair)
    {
        EventLoop loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));
        ASSERT_EQ(tlsSocket.fileDescriptor(), localDescriptor);

        tlsSocket.close();
        Platform::FileDescriptor::close(peerDescriptor);
    }

    namespace
    {
        /// 客户端一侧裸上下文的释放器：Core::TlsContext 只有服务端形态（TLS_server_method）
        inline void destroyClientContext(SSL_CTX *const context) noexcept
        {
            SSL_CTX_free(context);
        }

        using ClientContextPointer = std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)>;

        /**
         * @brief 造一个「只信那张自签测试证书」的客户端上下文
         * @param certificatePath 同时当作 CA 用的证书路径
         * @return ClientContextPointer 建好的上下文；建不出来时为空
         */
        ClientContextPointer makeClientContext(const std::filesystem::path &certificatePath)
        {
            ClientContextPointer context(SSL_CTX_new(TLS_client_method()), &destroyClientContext);
            if (!context)
            {
                return context;
            }
            static_cast<void>(SSL_CTX_load_verify_locations(context.get(), certificatePath.string().c_str(), nullptr));
            SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
            return context;
        }
    } // namespace

    /**
     * @brief 析构（而不是显式 close()）也必须先把 close_notify 送出去、再关描述符
     * @details m_ssl 声明在 m_socket 之前，交给默认成员析构就是「先关描述符、后 SSL_shutdown」：
     *          那次写入落在已经关闭、编号还能被别的线程立刻复用的描述符上。对端因此收不到
     *          close_notify，只能看到一次断线（asyncReceive 抛 CoreException 而不是返回 0），
     *          更糟的是那几个字节的 TLS 告警记录会灌进复用同一编号的陌生连接。
     *          这里让客户端只走作用域退出，用服务端的读数把顺序钉住。
     */
    TEST(TlsSocket, DestructorSendsCloseNotifyBeforeClosingTheDescriptor)
    {
        EventLoop loop;
        TlsContext serverContext;
        ASSERT_TRUE(serverContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int serverDescriptor = -1;
        int clientDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(serverDescriptor, clientDescriptor));

        SSL *serverHandle = serverContext.createSSL(serverDescriptor);
        ASSERT_NE(serverHandle, nullptr);
        TlsSocket serverSocket(serverHandle, loop, AsyncSocket(loop, serverDescriptor));

        auto  clientContext = makeClientContext(kTestCertificatePath);
        ASSERT_NE(clientContext, nullptr);
        SSL  *clientHandle  = SSL_new(clientContext.get());
        ASSERT_NE(clientHandle, nullptr);
        ASSERT_NE(SSL_set_fd(clientHandle, clientDescriptor), 0);

        {
            TlsSocket clientSocket(clientHandle, loop, AsyncSocket(loop, clientDescriptor), TlsSocket::Role::Client);

            Task<> serverHandshake = serverSocket.handshake();
            Task<> clientHandshake = clientSocket.handshake();
            serverHandshake.handle().resume();
            clientHandshake.handle().resume();
            ASSERT_TRUE(TestSupport::advanceUntil(loop, [&serverHandshake, &clientHandshake]
                                                  {
                                                      return serverHandshake.isReady() && clientHandshake.isReady();
                                                  }))
                << "两侧握手没有在时限内跑完，后面的读数说明不了任何问题";
        }   // ← 客户端在这里析构：必须已经在描述符还开着时发出过 close_notify

        std::uint8_t readBuffer[8]{};
        Task<ssize_t> readTask = serverSocket.asyncReceive(readBuffer, sizeof(readBuffer));
        readTask.handle().resume();
        ASSERT_TRUE(TestSupport::advanceUntil(loop, [&readTask]
                                             {
                                                 return readTask.isReady();
                                             }))
            << "服务端连结束都没读到：它挂在了一个不会再有事件的等待上";

        ssize_t     receivedByteCount = -1;
        std::string failureText;
        bool        isReadSucceeded = false;
        try
        {
            receivedByteCount = readTask.handle().promise().result();
            isReadSucceeded   = true;
        } catch (const std::exception &readFailure)
        {
            // 顺序写反时这里收到的是「对端非正常关闭」那一类协议错误，而不是干净的结束
            failureText = readFailure.what();
        }
        EXPECT_TRUE(isReadSucceeded) << "对端析构后服务端报的是协议错误而不是干净结束，说明 close_notify 没能在描述符关闭前发出："
                                     << failureText;
        EXPECT_EQ(receivedByteCount, 0) << "干净结束时的应用数据读数应为 0";

        serverSocket.close();
    }

    /**
     * @brief 地址查询透传到被包装的套接字：两端都在回环上，且关闭后按契约抛出
     * @details 不用 createPair：它只在 Windows 上造 loopback TCP，在 Linux/macOS 上产出的是
     *          AF_UNIX 描述符对（没有 IP 地址）。这里改用「回环监听 → 连接」两步建一条带真实
     *          地址的 TCP 通道，两端地址语义与 Windows 侧对等；这一层不自己记地址，地址问不出来
     *          就说明通道已不可用，调用方据此判失败而不是拿空地址。
     */
    TEST(TlsSocket, ExposesUnderlyingSocketAddresses)
    {
        EventLoop  loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

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

        SSL *ssl = tlsContext.createSSL(client.fileDescriptor());
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, std::move(client));

        const InetAddress peerAddress = tlsSocket.remoteAddress();
        const InetAddress ownAddress  = tlsSocket.localAddress();
        EXPECT_EQ(peerAddress.ip(), "127.0.0.1") << "对端地址没有从被包装的套接字上读到";
        EXPECT_EQ(ownAddress.ip(), "127.0.0.1") << "本端地址没有从被包装的套接字上读到";
        EXPECT_NE(peerAddress.port(), 0) << "对端端口为 0：地址没有真正读出来";
        EXPECT_NE(ownAddress.port(), 0) << "本端端口为 0：地址没有真正读出来";

        // 拒绝面：描述符被关掉之后再问地址，只能失败（静默返回空地址会让调用方以为拿到了对端）
        tlsSocket.close();
        EXPECT_THROW(static_cast<void>(tlsSocket.remoteAddress()), Base::SystemException);

        listener.close();
    }

    /**
     * @brief 拒绝面：对端在握手前关闭时握手必须以 CoreException 失败收场（可被 Base::Exception 统一捕获），而不是挂起等待
     * @details 对端已关 → SSL_accept 立刻读到 EOF，不进 WANT_READ 分支，因此单次 resume 就能走到抛出点，不依赖时序
     */
    TEST(TlsSocket, HandshakeAgainstClosedPeerFailsWithCoreException)
    {
        EventLoop  loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));

        // 对端在握手前就关闭：SSL_accept 会立刻读到 EOF 并失败，
        // 不会进入 WANT_READ 分支挂起，因此单次 resume 就能走到抛出点（不依赖时序）
        Platform::FileDescriptor::close(peerDescriptor);

        Task<> handshakeTask = tlsSocket.handshake();
        handshakeTask.handle().resume();
        ASSERT_TRUE(handshakeTask.isReady()) << "握手应当已失败返回，而不是挂起等待";

        // 握手失败必须落在 CoreException 上，且能被框架的异常基类统一捕获
        auto &promise = handshakeTask.handle().promise();
        EXPECT_THROW(promise.result(), CoreException);
        EXPECT_THROW(promise.result(), Base::Exception);

        tlsSocket.close();
    }

    namespace
    {
        /**
         * @brief 一条已终结协程的失败观测：异常是不是框架的运行期故障，以及它的文案
         */
        struct FailureObservation
        {
            bool isCoreException{false}; ///< 是否落在 CoreException 这一支（调用方可用框架基类统一捕获）
            std::string text{};          ///< 异常文案，用于核对「哪条操作失败」的前缀
        };

        /**
         * @brief 取出已终结协程里的异常，一次观测同时给出类型归属与文案
         * @details 这三条路径都在单次 resume 内就走到抛出点，因此不需要轮询也不需要等 I/O
         */
        template <typename TaskType>
        FailureObservation observeFailure(TaskType &task)
        {
            try
            {
                static_cast<void>(task.handle().promise().result());
            } catch (const CoreException &error)
            {
                return FailureObservation{true, std::string{error.what()}};
            } catch (const std::exception &error)
            {
                return FailureObservation{false, std::string{error.what()}};
            }
            return FailureObservation{};
        }
    }

    /**
     * @brief 会话已释放之后三条入口都报出「本端」原因，而不是 OpenSSL 的空错误加对端猜测
     * @details 摘掉闸门时实测到的文案是 `TLS 写入失败：error:00000000:lib(0)::reason(0)`
     *          外加「多半已被对端关闭」——OpenSSL 3 对空指针返回失败而不崩溃，于是排查的人会被
     *          一条没有内容的错误和一句指错方向的原因带走。因此这里既断言操作前缀（写错的分支
     *          会把「写入」报成「读取」），也断言闸门那句本端原因
     */
    TEST(TlsSocket, EverySslEntryPointRejectsAReleasedSession)
    {
        EventLoop  loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));
        // 先释放会话：此后 m_ssl 为空，三条入口都必须在本端把原因说清楚
        tlsSocket.close();

        char buffer[8]{};

        Task<> handshakeTask = tlsSocket.handshake();
        handshakeTask.handle().resume();
        ASSERT_TRUE(handshakeTask.isReady()) << "会话已释放，握手不该挂起等待";
        const FailureObservation handshakeFailure = observeFailure(handshakeTask);
        EXPECT_TRUE(handshakeFailure.isCoreException) << handshakeFailure.text;
        EXPECT_NE(handshakeFailure.text.find("TLS 握手失败："), std::string::npos) << handshakeFailure.text;
        EXPECT_NE(handshakeFailure.text.find("本端 TLS 会话已释放"), std::string::npos) << handshakeFailure.text;

        Task<ssize_t> receiveTask = tlsSocket.asyncReceive(buffer, sizeof(buffer));
        receiveTask.handle().resume();
        ASSERT_TRUE(receiveTask.isReady()) << "会话已释放，读取不该挂起等待";
        const FailureObservation receiveFailure = observeFailure(receiveTask);
        EXPECT_TRUE(receiveFailure.isCoreException) << receiveFailure.text;
        EXPECT_NE(receiveFailure.text.find("TLS 读取失败："), std::string::npos) << receiveFailure.text;
        EXPECT_NE(receiveFailure.text.find("本端 TLS 会话已释放"), std::string::npos) << receiveFailure.text;

        Task<ssize_t> sendTask = tlsSocket.asyncSend(buffer, sizeof(buffer));
        sendTask.handle().resume();
        ASSERT_TRUE(sendTask.isReady()) << "会话已释放，写入不该挂起等待";
        const FailureObservation sendFailure = observeFailure(sendTask);
        EXPECT_TRUE(sendFailure.isCoreException) << sendFailure.text;
        EXPECT_NE(sendFailure.text.find("TLS 写入失败："), std::string::npos) << sendFailure.text;
        EXPECT_NE(sendFailure.text.find("本端 TLS 会话已释放"), std::string::npos) << sendFailure.text;
    }

    /**
     * @brief 对端在握手中途断开时要把「TCP 突然断了」说出来，而不是把空错误队列当原因
     * @details SSL_ERROR_SYSCALL 一类失败在 OpenSSL 的错误队列里常常什么都没有，此时原因在
     *          返回值与套接字错误码里。照队列原文打印会得到 `error:00000000:lib(0)::reason(0)`
     *          再配一句「多半是证书/协议不匹配」，把人引向对端的配置而不是断线本身。
     */
    TEST(TlsSocket, HandshakeAgainstAbruptlyClosedPeerReportsTheTcpBreakNotAnEmptyQueue)
    {
        EventLoop  loop;
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        ASSERT_NE(ssl, nullptr);

        TlsSocket tlsSocket(ssl, loop, AsyncSocket(loop, localDescriptor));

        // 对端先消失：SSL_accept 第一次读就到文件尾，单次 resume 即可走到抛出点（不依赖时序）
        Platform::FileDescriptor::close(peerDescriptor);

        Task<> handshakeTask = tlsSocket.handshake();
        handshakeTask.handle().resume();
        ASSERT_TRUE(handshakeTask.isReady()) << "对端已关闭，握手不该继续挂起等待";

        const FailureObservation failure = observeFailure(handshakeTask);
        EXPECT_TRUE(failure.isCoreException) << failure.text;
        EXPECT_EQ(failure.text.find("00000000"), std::string::npos) << "空的错误队列不能当成失败原因：" << failure.text;
        EXPECT_NE(failure.text.find("关闭通知"), std::string::npos) << "要指明这是对端没走 TLS 关闭握手的断线：" << failure.text;
        // 原因已经确定是「对端断了 TCP」，此时再列「证书不受信」那类猜测就是把人往对端配置上引
        EXPECT_EQ(failure.text.find("证书不受信"), std::string::npos) << "已判定为断线时不该再给证书猜测：" << failure.text;

        tlsSocket.close();
    }

    /**
     * @brief 会话建好之后对端不走 close_notify 就断开：读侧要说清「TCP 突然断了」，而不是报一个空错误
     * @details 这是服务器上最常见的 TLS 失败（对端进程被杀、中间设备掐断链路）。OpenSSL 在这一路
     *          通常什么都不往错误队列里放，只照抄队列就会打印出 `error:00000000:lib(0)::reason(0)`，
     *          再配一句「多半已被对端关闭或会话失效」的猜测——两句都没有可操作信息。
     *          客户端用 quiet shutdown 模式关掉：SSL_shutdown 发不出 close_notify，
     *          留下的就是「描述符正常关闭、TLS 层没有告警」这一形态。
     */
    TEST(TlsSocket, ReceiveAfterPeerBreaksConnectionWithoutCloseNotifyNamesTheBreak)
    {
        EventLoop loop;
        TlsContext serverContext;
        ASSERT_TRUE(serverContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int serverDescriptor = -1;
        int clientDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(serverDescriptor, clientDescriptor));

        SSL *serverHandle = serverContext.createSSL(serverDescriptor);
        ASSERT_NE(serverHandle, nullptr);
        TlsSocket serverSocket(serverHandle, loop, AsyncSocket(loop, serverDescriptor));

        auto clientContext = makeClientContext(kTestCertificatePath);
        ASSERT_NE(clientContext, nullptr);
        SSL *clientHandle = SSL_new(clientContext.get());
        ASSERT_NE(clientHandle, nullptr);
        ASSERT_NE(SSL_set_fd(clientHandle, clientDescriptor), 0);
        // 静默关闭：释放客户端会话时不发 close_notify，对端看到的就是一次不合规格的断线
        SSL_set_quiet_shutdown(clientHandle, 1);

        {
            TlsSocket clientSocket(clientHandle, loop, AsyncSocket(loop, clientDescriptor), TlsSocket::Role::Client);

            Task<> serverHandshake = serverSocket.handshake();
            Task<> clientHandshake = clientSocket.handshake();
            serverHandshake.handle().resume();
            clientHandshake.handle().resume();
            ASSERT_TRUE(TestSupport::advanceUntil(loop, [&serverHandshake, &clientHandshake]
                                                  {
                                                      return serverHandshake.isReady() && clientHandshake.isReady();
                                                  }))
                << "两侧握手没有在时限内跑完，后面的读数说明不了任何问题";

            // 客户端在这里关闭：会话先释放（不发告警），随后描述符关闭 —— 服务端只看到断线
        }

        std::uint8_t readBuffer[8]{};
        Task<ssize_t> readTask = serverSocket.asyncReceive(readBuffer, sizeof(readBuffer));
        readTask.handle().resume();
        ASSERT_TRUE(TestSupport::advanceUntil(loop, [&readTask]
                                             {
                                                 return readTask.isReady();
                                             }))
            << "服务端连断线都没读到：它挂在了一个不会再有事件的等待上";

        const FailureObservation failure = observeFailure(readTask);
        EXPECT_TRUE(failure.isCoreException) << "非正常关闭必须报错，而不是被当成干净结束：" << failure.text;
        EXPECT_NE(failure.text.find("TLS 读取失败："), std::string::npos) << failure.text;
        EXPECT_EQ(failure.text.find("00000000"), std::string::npos) << "空的错误队列不能当成失败原因：" << failure.text;
        EXPECT_NE(failure.text.find("关闭通知"), std::string::npos) << "要指明这是对端没走 TLS 关闭握手的断线：" << failure.text;

        // 同一条断线再写一次：写侧走的是另一个 SSL 入口，文案也要给出同一水准的原因
        Task<ssize_t> writeTask = serverSocket.asyncSend(readBuffer, sizeof(readBuffer));
        writeTask.handle().resume();
        ASSERT_TRUE(TestSupport::advanceUntil(loop, [&writeTask]
                                             {
                                                 return writeTask.isReady();
                                             }))
            << "写侧既没成功也没失败：它挂在了一个不会再有事件的等待上";
        std::string writeFailureText;
        try
        {
            static_cast<void>(writeTask.handle().promise().result());
        } catch (const std::exception &writeFailure)
        {
            writeFailureText = writeFailure.what();
        }
        // 断线之后写不下去时，报的必须是被断的原因，而不是「多半已被对端关闭」这类没有信息量的猜测
        EXPECT_EQ(writeFailureText.find("多半已被对端关闭"), std::string::npos) << writeFailureText;
        EXPECT_EQ(writeFailureText.find("00000000"), std::string::npos) << writeFailureText;

        serverSocket.close();
    }

    namespace
    {
        /**
         * @brief 只回一个数的协程：用来分清「帧本身」与「握手路径」各占几次分配
         * @param marker 写一个标记，证明它真的跑过
         * @return Task<int> 固定 1
         */
        Task<int> trivialHandshakeShape(int &marker)
        {
            marker += 1;
            co_return 1;
        }

        /**
         * @brief 等一次可读的协程：把「注册 + 等可读」这条 I/O 路径包成可驱动的形状
         * @param socket 目标套接字
         * @return Task<bool> 等待是否被叫醒（false 表示注册失效）
         */
        Task<bool> waitOnceReadable(AsyncSocket &socket)
        {
            co_return co_await socket.waitReadable();
        }

        /**
         * @brief 对照组二：明文走一次「向后端注册 + 等可读」，完全不碰 TLS
         * @details IoWatcher 是首次等待才向后端注册的，握手形状里也含着这一次注册。把它单独量出来，
         *          才知道 TLS 台账里那几块有几张属于「每连接的 I/O 注册记账」而不是 TLS 本身
         * @param loop 承载等待的循环
         * @return true 等待被就绪事件叫醒
         */
        bool runOnePlaintextIoWait(EventLoop &loop)
        {
            int readDescriptor = -1;
            int writeDescriptor = -1;
            if (!Platform::FileDescriptor::createPair(readDescriptor, writeDescriptor))
            {
                return false;
            }

            AsyncSocket reader(loop, readDescriptor);
            AsyncSocket writer(loop, writeDescriptor);
            // 先放一个字节再等：等可读这一步必然被叫醒，不会把「没人叫醒」当成读数
            static constexpr char kMarker = 'x';
            if (Platform::FileDescriptor::write(writeDescriptor, &kMarker, 1) != 1)
            {
                return false;
            }

            Task<bool> waitTask = waitOnceReadable(reader);
            static_cast<void>(waitTask.handle().resume());
            if (!advanceUntil(loop, [&waitTask]
                              {
                                  return waitTask.isReady();
                              }))
            {
                return false;
            }
            return waitTask.handle().promise().result();
        }
    } // namespace

    namespace
    {
        /**
         * @brief 走一次完整的 TLS 握手：服务端与客户端各一条协程，同一个循环上驱动
         * @param loop 承载两条协程的事件循环
         * @param serverContext 已装好证书与私钥的服务端上下文
         * @param clientContext 只信那张自签测试证书的客户端上下文
         * @return true 两侧握手都成功完成；任何一步没做成都是 false
         */
        bool runOneTlsHandshake(EventLoop &loop, TlsContext &serverContext, SSL_CTX &clientContext)
        {
            int serverDescriptor = -1;
            int clientDescriptor = -1;
            if (!Platform::FileDescriptor::createPair(serverDescriptor, clientDescriptor))
            {
                return false;
            }

            SSL *const serverHandle = serverContext.createSSL(serverDescriptor);
            if (serverHandle == nullptr)
            {
                Platform::FileDescriptor::close(serverDescriptor);
                Platform::FileDescriptor::close(clientDescriptor);
                return false;
            }

            SSL *const clientHandle = SSL_new(&clientContext);
            if (clientHandle == nullptr || SSL_set_fd(clientHandle, clientDescriptor) == 0)
            {
                // 还没交给 TlsSocket 接管：这一侧的 SSL 与两个描述符都由本函数自己归还
                static_cast<void>(SSL_free(clientHandle));
                static_cast<void>(SSL_free(serverHandle));
                Platform::FileDescriptor::close(serverDescriptor);
                Platform::FileDescriptor::close(clientDescriptor);
                return false;
            }

            {
                TlsSocket serverSocket(serverHandle, loop, AsyncSocket(loop, serverDescriptor));
                TlsSocket clientSocket(clientHandle, loop, AsyncSocket(loop, clientDescriptor), TlsSocket::Role::Client);

                Task<> serverHandshake = serverSocket.handshake();
                Task<> clientHandshake = clientSocket.handshake();
                static_cast<void>(serverHandshake.handle().resume());
                static_cast<void>(clientHandshake.handle().resume());
                if (!advanceUntil(loop, [&serverHandshake, &clientHandshake]
                                  {
                                      return serverHandshake.isReady() && clientHandshake.isReady();
                                  }))
                {
                    return false;
                }

                try
                {
                    static_cast<void>(serverHandshake.handle().promise().result());
                    static_cast<void>(clientHandshake.handle().promise().result());
                } catch (...)
                {
                    return false;
                }
            }   // 两条协程帧先退，再退两个 TlsSocket：反过来就是对已释放帧的收尾
            return true;
        }

        /**
         * @brief 对照组：把两侧 TlsSocket 建起来又销毁，但一次握手都不做
         * @details 要减掉的是「我们这一层的接线成本」：SSL 对象、AsyncSocket、IoWatcher 注册
         *          （IOCP 上还要发探针）都在这一步发生。不减掉就会全记到握手本身头上。
         * @return true 这一步做完了（对照组只关心「有没有空转」）
         */
        bool runOneTlsSessionSetup(EventLoop &loop, TlsContext &serverContext, SSL_CTX &clientContext)
        {
            int serverDescriptor = -1;
            int clientDescriptor = -1;
            if (!Platform::FileDescriptor::createPair(serverDescriptor, clientDescriptor))
            {
                return false;
            }

            SSL *const serverHandle = serverContext.createSSL(serverDescriptor);
            if (serverHandle == nullptr)
            {
                Platform::FileDescriptor::close(serverDescriptor);
                Platform::FileDescriptor::close(clientDescriptor);
                return false;
            }

            SSL *const clientHandle = SSL_new(&clientContext);
            if (clientHandle == nullptr || SSL_set_fd(clientHandle, clientDescriptor) == 0)
            {
                static_cast<void>(SSL_free(clientHandle));
                static_cast<void>(SSL_free(serverHandle));
                Platform::FileDescriptor::close(serverDescriptor);
                Platform::FileDescriptor::close(clientDescriptor);
                return false;
            }

            {
                TlsSocket serverSocket(serverHandle, loop, AsyncSocket(loop, serverDescriptor));
                TlsSocket clientSocket(clientHandle, loop, AsyncSocket(loop, clientDescriptor), TlsSocket::Role::Client);
                static_cast<void>(serverSocket.fileDescriptor());
            }   // 两侧都在这里析构：close_notify 的写入属于接线成本，不属于握手
            return true;
        }
    } // namespace

    /**
     * @brief 一次完整 TLS 握手的分配画像（Core 里第一份 TLS 台账）
     * @details 口径同 tests/Net/Http 那套：把「建描述符对 → 两侧握手 → 关」连跑两百次摊平。
     *          三条对照（只接线不起协程、只起两只会立刻跑完的协程、一次明文「注册 + 等可读」）用来把
     *          TLS 自己的份额从帧、接线与后端记账里分出来。OpenSSL 走它自己的分配器、这台探针看不见，
     *          因此读数只归我们这一层；判据只在 Release 下钉，Debug 仍跑同样形状并打出直方图供对照。
     */
    TEST(TlsSocket, HandshakeAllocationProfile)
    {
        using AsynGyanis::TestSupport::measureOperations;

        // 一次握手要毫秒级（Debug+ASan 下 ECDSA 双端验签与密钥派生都在跑），一千次会吃掉整份用例
        // 的时限；摊平读数只需要次数够抵消顺序噪声
        constexpr std::uint64_t kMeasurementRounds = 200U;

        EventLoop  loop;
        TlsContext serverContext;
        ASSERT_TRUE(serverContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
        auto clientContext = makeClientContext(kTestCertificatePath);
        ASSERT_NE(clientContext, nullptr);

        // 先热一次身：OpenSSL 的进程内一次性初始化（错误队列、随机池、算法表）不能记到握手头上
        ASSERT_TRUE(runOneTlsHandshake(loop, serverContext, *clientContext)) << "本机跑不成 TLS 握手，量不了";

        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto profile = measureOperations(
                kMeasurementRounds,
                [&]
                {
                    return runOneTlsHandshake(loop, serverContext, *clientContext) ? 1U : 0U;
                });
        const auto handshakeHistogram = AsynGyanis::TestSupport::snapshotAllocationHistogram();

        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto baseline = measureOperations(
                kMeasurementRounds,
                [&]
                {
                    return runOneTlsSessionSetup(loop, serverContext, *clientContext) ? 1U : 0U;
                });
        const auto setupHistogram = AsynGyanis::TestSupport::snapshotAllocationHistogram();

        // 第三条对照：只起两条会立刻跑完的协程。它量的是「握手用的那两只帧」本身——帧走帧池时
        // 这里应为 0，握手读数里的 12~16 次就都不在帧上
        AsynGyanis::TestSupport::resetAllocationHistogram();
        int marker = 0;
        const auto frameBody = [&marker]
        {
            Task<int> first  = trivialHandshakeShape(marker);
            Task<int> second = trivialHandshakeShape(marker);
            static_cast<void>(first.handle().resume());
            static_cast<void>(second.handle().resume());
            return (first.isReady() && second.isReady()) ? 1U : 0U;
        };
        const auto frames = measureOperations(kMeasurementRounds, frameBody);
        // 第二趟读数用来分清「帧池一次性扩块」与「每次真的各走一次全局 new」：
        // 池在回收的话第二趟应接近 0，仍然按次涨就是没回收
        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto secondFramePass = measureOperations(kMeasurementRounds, frameBody);

        AsynGyanis::TestSupport::resetAllocationHistogram();
        const auto ioWait = measureOperations(kMeasurementRounds, [&loop]
        {
            return runOnePlaintextIoWait(loop) ? 1U : 0U;
        });

        std::printf("tls-handshake total=%llu bytes=%llu\n", static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        std::printf("tls-session-setup total=%llu bytes=%llu\n", static_cast<unsigned long long>(baseline.totalAllocations),
                    static_cast<unsigned long long>(baseline.totalBytes));
        std::printf("tls-two-frames total=%llu bytes=%llu\n", static_cast<unsigned long long>(frames.totalAllocations),
                    static_cast<unsigned long long>(frames.totalBytes));
        std::printf("tls-two-frames-second-pass total=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(secondFramePass.totalAllocations),
                    static_cast<unsigned long long>(secondFramePass.totalBytes));
        std::printf("tls-plaintext-iowait total=%llu bytes=%llu\n", static_cast<unsigned long long>(ioWait.totalAllocations),
                    static_cast<unsigned long long>(ioWait.totalBytes));
        for (std::size_t bucket = 0; bucket < handshakeHistogram.size(); ++bucket)
        {
            if (handshakeHistogram[bucket] != 0)
            {
                std::printf("   handshake bucket=%zu bytes=%zu count=%llu\n", bucket,
                            bucket * AsynGyanis::TestSupport::kAllocationHistogramBucketBytes,
                            static_cast<unsigned long long>(handshakeHistogram[bucket]));
            }
        }
        for (std::size_t bucket = 0; bucket < setupHistogram.size(); ++bucket)
        {
            if (setupHistogram[bucket] != 0)
            {
                std::printf("   setup bucket=%zu bytes=%zu count=%llu\n", bucket,
                            bucket * AsynGyanis::TestSupport::kAllocationHistogramBucketBytes,
                            static_cast<unsigned long long>(setupHistogram[bucket]));
            }
        }

        EXPECT_EQ(profile.resultSum, kMeasurementRounds) << "握手没有全部做成，读数没有意义";
        EXPECT_EQ(baseline.resultSum, kMeasurementRounds) << "对照组在空转，减不出归属";
        EXPECT_EQ(frames.resultSum, kMeasurementRounds) << "帧对照组没跑起来，它那份读数不作数";
        EXPECT_GT(profile.totalAllocations, baseline.totalAllocations)
                << "握手比「只接线不握手」还省？说明被测形状没有真的跑握手";
        // 带 sanitizer 的构建里帧池按 CoroutinePool.h 的既定口径被绕开（要让 ASan 能报出帧上的
        // use-after-free），所以这里的读数含「每帧一次全局 new」，比生产形态高。这一侧只拦量级
        EXPECT_LE(profile.totalAllocations, kMeasurementRounds * 40ULL)
                << "每次握手的分配数越过量级上界，检查握手路径上新增的缓冲与闭包";
        EXPECT_EQ(ioWait.resultSum, kMeasurementRounds) << "明文等待没有每次都被叫醒，那份对照读数不作数";
#ifdef NDEBUG
        // 生产形态（Release、帧池生效）实测：两侧接线 0 块、两只协程帧 0 块，一次双向 TLS1.3
        // 握手 11 块 / 640 B；而「一条套接字注册 + 等一次可读」的明文形状就要 5 块 / 312 B，
        // 握手两侧各摊一次 ≈ 10 块——**TLS 自己这一层几乎不再花分配**，剩下的都在后端的每连接注册记账上
        EXPECT_EQ(frames.totalAllocations, 0U) << "协程帧没有从帧池拿到：池的接线被改坏了";
        EXPECT_LE(profile.totalAllocations, kMeasurementRounds * 16ULL)
                << "每次握手的堆块数越界：TLS 这条路上多半又多了一次分配";
#endif
    }

} // namespace AsynGyanis::Core
