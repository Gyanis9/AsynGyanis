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
