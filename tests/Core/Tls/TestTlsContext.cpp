/**
 * @file TestTlsContext.cpp
 * @brief TlsContext 单元测试：证书/私钥加载与 SSL 对象创建（使用仓库预生成证书）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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

    TEST(TlsContext, ConstructionInitializesNativeHandle)
    {
        const TlsContext tlsContext;
        EXPECT_NE(tlsContext.nativeHandle(), nullptr);
    }

    TEST(TlsContext, ConstructionDoesNotThrow)
    {
        EXPECT_NO_THROW([]()
        {
            TlsContext tlsContext;
        }());
    }

    TEST(TlsContext, LoadCertificateAcceptsPreGeneratedFixturePair)
    {
        const TlsContext tlsContext;

        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath));
        ASSERT_TRUE(std::filesystem::exists(kTestKeyPath));
        EXPECT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
    }

    TEST(TlsContext, LoadCertificateFailsWithNonexistentFiles)
    {
        const TlsContext tlsContext;
        EXPECT_FALSE(tlsContext.loadCertificate("/nonexistent/cert.pem", "/nonexistent/key.pem"));
    }

    TEST(TlsContext, CreateSslReturnsNonNullForValidDescriptor)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath));
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        int localDescriptor = -1;
        int peerDescriptor = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        SSL *ssl = tlsContext.createSSL(localDescriptor);
        EXPECT_NE(ssl, nullptr);

        SSL_free(ssl);
        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(TlsContext, NativeHandleReturnsSamePointerAcrossCalls)
    {
        const TlsContext tlsContext;
        SSL_CTX *first = tlsContext.nativeHandle();
        SSL_CTX *second = tlsContext.nativeHandle();
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first, second);
    }
}
