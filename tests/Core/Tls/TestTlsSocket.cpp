// TlsSocket 单元测试：构造、移动语义、安全关闭与地址查询，以及会话释放后的拒绝面（使用仓库预生成证书）

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

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <cstdint>
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

} // namespace AsynGyanis::Core
