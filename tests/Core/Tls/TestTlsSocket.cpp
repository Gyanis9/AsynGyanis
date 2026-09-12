/**
 * @file TestTlsSocket.cpp
 * @brief TlsSocket 单元测试：构造、移动语义与安全关闭（使用仓库预生成证书）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Tls/TlsSocket.h"

#include "Base/Exception/Exception.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Exception/CoreException.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Tls/TlsContext.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <filesystem>
#include <string>

namespace AsynGyanis::Core
{
    namespace
    {
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
} // namespace AsynGyanis::Core
