/**
 * @file TestTlsContext.cpp
 * @brief TlsContext 单元测试：证书与 CA 加载、SSL 对象创建，以及协议加固后的握手行为（使用仓库预生成证书）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Tls/TlsContext.h"

#include "Base/Exception/Exception.h"
#include "Core/Exception/CoreException.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <filesystem>
#include <memory>
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

        /// 客户端提供 ALPN 时的线上格式：长度字节 + "http/1.1"
        constexpr unsigned char kHttp11AlpnWireFormat[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

        /// 交替推进两端的轮数上限：一次握手只要数个来回，超出上限说明是环境异常而非被测语义
        constexpr int kMaximumHandshakeRounds = 64;

        /// 握手结果快照：只保留断言需要的信息，不持有任何 OpenSSL 对象
        struct HandshakeOutcome
        {
            bool        serverCompleted{false};             ///< 服务端是否完成握手
            bool        clientCompleted{false};             ///< 客户端是否完成握手
            bool        setupFailed{false};                 ///< 内存 BIO 对或 SSL 对象创建失败（环境问题，非被测语义）
            bool        alpnListAccepted{false};            ///< 客户端是否成功登记了 ALPN 列表
            bool        clientCertificateInstalled{false};  ///< 客户端是否成功装载了证书与私钥
            std::string serverErrorText;                    ///< 服务端失败时的 OpenSSL 错误串
            std::string clientErrorText;                    ///< 客户端失败时的 OpenSSL 错误串
            int         clientErrorReason{0};               ///< 客户端失败原因码（ERR_GET_REASON）
            std::string protocolVersion;                    ///< 服务端视角协商出的协议版本
            std::string serverAlpn;                         ///< 服务端视角的 ALPN 协商结果
            std::string clientAlpn;                         ///< 客户端视角的 ALPN 协商结果
            long        serverVerifyResult{0};              ///< 服务端对客户端证书的校验结果（X509_V_OK 为 0）
        };

        /// SSL 对象释放器，供 unique_ptr 在断言提前返回时也不泄漏
        struct SslDeleter
        {
            void operator()(SSL *sslHandle) const noexcept
            {
                SSL_free(sslHandle);
            }
        };

        /**
         * @brief 取出 OpenSSL 错误栈里最近一条错误描述。
         * @return std::string 错误描述文本，错误栈为空时返回空串
         */
        std::string lastOpenSslErrorText()
        {
            if (ERR_peek_last_error() == 0)
            {
                return {};
            }
            char buffer[256];
            ERR_error_string_n(ERR_peek_last_error(), buffer, sizeof(buffer));
            return buffer;
        }

        /**
         * @brief 在单进程内用一对内存 BIO 驱动服务端与客户端完成一次握手。
         * @details 内存 BIO 对的写端不阻塞，因此两端各调一次 SSL_accept/SSL_connect 交替推进即可：
         *          返回 WANT_READ 只表示还在等对端产出数据，无需套接字与事件循环，也不受时序影响。
         * @param serverContext 服务端上下文，通常是被测 TlsContext 的 nativeHandle()
         * @param clientContext 客户端上下文，协议版本与安全等级由调用方按场景设定
         * @param clientOffersAlpn true 时客户端登记 http/1.1，false 时完全不提供 ALPN
         * @param clientPresentsCertificate true 时客户端用仓库夹具证书/私钥作为客户端证书
         * @return HandshakeOutcome 两端的完成情况、错误文本与协商结果
         */
        HandshakeOutcome runInProcessHandshake(SSL_CTX *serverContext, SSL_CTX *clientContext,
                                              const bool clientOffersAlpn, const bool clientPresentsCertificate,
                                              const unsigned char *clientAlpnWireFormat = kHttp11AlpnWireFormat,
                                              const unsigned int clientAlpnWireFormatLength = sizeof(kHttp11AlpnWireFormat))
        {
            HandshakeOutcome outcome;

            // 用内存 BIO 对代替真实套接字：用例不占端口、不依赖网络与事件循环，重复运行结果一致
            BIO *clientBio = nullptr;
            BIO *serverBio = nullptr;
            if (BIO_new_bio_pair(&clientBio, 0, &serverBio, 0) != 1)
            {
                outcome.setupFailed = true;
                return outcome;
            }

            const std::unique_ptr<SSL, SslDeleter> serverSsl(SSL_new(serverContext));
            const std::unique_ptr<SSL, SslDeleter> clientSsl(SSL_new(clientContext));
            if (serverSsl == nullptr || clientSsl == nullptr)
            {
                // SSL_new 失败时 BIO 还没交出去，这里自行释放，避免唯一一次失败的调用漏内存
                BIO_free(clientBio);
                BIO_free(serverBio);
                outcome.setupFailed = true;
                return outcome;
            }

            // BIO 所有权交给 SSL：同一个 BIO 兼作读/写端，传两次即可，由 SSL_free 释放
            SSL_set_bio(serverSsl.get(), serverBio, serverBio);
            SSL_set_bio(clientSsl.get(), clientBio, clientBio);

            if (clientOffersAlpn)
            {
                // 返回值 0 表示列表被接受；记录下来，避免「客户端其实没提 ALPN」被误判成协商成功
                outcome.alpnListAccepted = SSL_set_alpn_protos(clientSsl.get(), clientAlpnWireFormat, clientAlpnWireFormatLength) == 0;
            }
            if (clientPresentsCertificate)
            {
                // 客户端证书安装在 SSL 对象上：夹具证书自签且已在服务端信任库中，可被验通
                const int certificateResult =
                    SSL_use_certificate_file(clientSsl.get(), kTestCertificatePath.string().c_str(), SSL_FILETYPE_PEM);
                const int keyResult =
                    SSL_use_PrivateKey_file(clientSsl.get(), kTestKeyPath.string().c_str(), SSL_FILETYPE_PEM);
                // 记下装载结果：夹具缺失时让用例以「客户端根本没证书」失败，而不是伪装成服务端拒绝
                outcome.clientCertificateInstalled = certificateResult == 1 && keyResult == 1;
            }

            bool serverTerminal = false;
            bool clientTerminal = false;
            for (int round = 0; round < kMaximumHandshakeRounds && !(serverTerminal && clientTerminal); ++round)
            {
                if (!serverTerminal)
                {
                    const int result = SSL_accept(serverSsl.get());
                    if (result == 1)
                    {
                        outcome.serverCompleted = true;
                        serverTerminal          = true;
                    }
                    else if (const int error = SSL_get_error(serverSsl.get(), result);
                             error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                    {
                        // 非「等对端数据」的错误即为终态失败：记下原因，清栈以免污染后续断言
                        outcome.serverErrorText = lastOpenSslErrorText();
                        ERR_clear_error();
                        serverTerminal = true;
                    }
                }

                if (!clientTerminal)
                {
                    const int result = SSL_connect(clientSsl.get());
                    if (result == 1)
                    {
                        outcome.clientCompleted = true;
                        clientTerminal          = true;
                    }
                    else if (const int error = SSL_get_error(clientSsl.get(), result);
                             error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                    {
                        outcome.clientErrorText   = lastOpenSslErrorText();
                        outcome.clientErrorReason = ERR_GET_REASON(ERR_peek_last_error());
                        ERR_clear_error();
                        clientTerminal = true;
                    }
                }
            }

            outcome.protocolVersion = SSL_get_version(serverSsl.get());
            // 校验结果由服务端视角给出：mTLS 用例据此断言对端证书确实被验过
            outcome.serverVerifyResult = SSL_get_verify_result(serverSsl.get());

            const unsigned char *selectedProtocol = nullptr;
            unsigned int         selectedLength   = 0;
            SSL_get0_alpn_selected(serverSsl.get(), &selectedProtocol, &selectedLength);
            if (selectedProtocol != nullptr && selectedLength > 0)
            {
                outcome.serverAlpn.assign(reinterpret_cast<const char *>(selectedProtocol), selectedLength);
            }
            SSL_get0_alpn_selected(clientSsl.get(), &selectedProtocol, &selectedLength);
            if (selectedProtocol != nullptr && selectedLength > 0)
            {
                outcome.clientAlpn.assign(reinterpret_cast<const char *>(selectedProtocol), selectedLength);
            }

            return outcome;
        }

        /// SSL_CTX 释放器，客户端上下文在断言提前返回时也不会泄漏
        struct SslContextDeleter
        {
            void operator()(SSL_CTX *context) const noexcept
            {
                SSL_CTX_free(context);
            }
        };

        using SslContextPointer = std::unique_ptr<SSL_CTX, SslContextDeleter>;

        /**
         * @brief 创建一个默认设置的客户端上下文。
         * @return SslContextPointer 客户端上下文，协议版本、安全等级与套件由调用方按场景追加设定
         */
        SslContextPointer createClientContext()
        {
            return SslContextPointer(SSL_CTX_new(TLS_client_method()));
        }
    }

    /**
     * @brief 构造即创建好 SSL_CTX：nativeHandle() 非空，后续才能加载证书
     */
    TEST(TlsContext, ConstructionInitializesNativeHandle)
    {
        const TlsContext tlsContext;
        EXPECT_NE(tlsContext.nativeHandle(), nullptr);
    }

    /**
     * @brief 构造失败以句柄为空表达而非抛异常：本类不把构造路径作为错误上报口
     */
    TEST(TlsContext, ConstructionDoesNotThrow)
    {
        EXPECT_NO_THROW([]()
        {
            TlsContext tlsContext;
        }());
    }

    /**
     * @brief 仓库预生成的证书/私钥对能被加载并返回 true（夹具由 TEST_FIXTURES_DIR 提供，不依赖外部服务）
     */
    TEST(TlsContext, LoadCertificateAcceptsPreGeneratedFixturePair)
    {
        const TlsContext tlsContext;

        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath));
        ASSERT_TRUE(std::filesystem::exists(kTestKeyPath));
        EXPECT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
    }

    /**
     * @brief 拒绝面：证书或私钥文件不存在时返回 false，而不是崩溃或抛异常
     */
    TEST(TlsContext, LoadCertificateFailsWithNonexistentFiles)
    {
        const TlsContext tlsContext;
        EXPECT_FALSE(tlsContext.loadCertificate("/nonexistent/cert.pem", "/nonexistent/key.pem"));
    }

    /**
     * @brief 证书就绪后可为有效描述符创建 SSL 对象（非空），且创建出的对象由调用方负责 SSL_free
     */
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

    /**
     * @brief nativeHandle() 是纯访问器：多次调用返回同一个 SSL_CTX 指针，不会重建上下文
     */
    TEST(TlsContext, NativeHandleReturnsSamePointerAcrossCalls)
    {
        const TlsContext tlsContext;
        SSL_CTX *first = tlsContext.nativeHandle();
        SSL_CTX *second = tlsContext.nativeHandle();
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first, second);
    }

    /**
     * @brief 协议策略落在上下文本身上：最低版本 TLS 1.2、关闭压缩、安全等级至少 2
     * @details 三条加固项的配置面断言，握手面的行为由后续用例覆盖
     */
    TEST(TlsContext, ContextForbidsLegacyProtocolVersionsAndCompression)
    {
        const TlsContext tlsContext;
        SSL_CTX         *context = tlsContext.nativeHandle();
        ASSERT_NE(context, nullptr);

        // 最低版本 TLS 1.2：TLS 1.0/1.1 已由 RFC 8996 列为废弃
        EXPECT_EQ(SSL_CTX_get_min_proto_version(context), TLS1_2_VERSION);
        // CRIME 侧信道依赖压缩，服务端必须显式关闭压缩
        EXPECT_TRUE((SSL_CTX_get_options(context) & SSL_OP_NO_COMPRESSION) != 0UL);
        // 安全等级至少 2：拒绝 1024 位以下密钥与 SHA-1 签名（夹具证书为 2048 位 RSA + SHA-256）
        EXPECT_GE(SSL_CTX_get_security_level(context), 2);
    }

    /**
     * @brief 生效的候选套件里没有 3DES/RC4/MD5/NULL/单 DES/匿名/导出级套件，且列表不为空
     * @details 断言 ctx 里最终生效的套件列表：既挡住弱算法，也挡住「把列表配空」这种
     *          看似更严、实则连握手都做不成的写法
     */
    TEST(TlsContext, ContextCipherListExcludesWeakSuites)
    {
        const TlsContext      tlsContext;
        STACK_OF(SSL_CIPHER) *ciphers = SSL_CTX_get_ciphers(tlsContext.nativeHandle());
        ASSERT_NE(ciphers, nullptr);
        ASSERT_GT(sk_SSL_CIPHER_num(ciphers), 0);

        for (int index = 0; index < sk_SSL_CIPHER_num(ciphers); ++index)
        {
            const std::string cipherName = SSL_CIPHER_get_name(sk_SSL_CIPHER_value(ciphers, index));
            EXPECT_EQ(cipherName.find("3DES"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("CBC3"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("DES-CBC"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("RC4"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("MD5"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("NULL"), std::string::npos) << "弱套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("ADH"), std::string::npos) << "匿名套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("AECDH"), std::string::npos) << "匿名套件未排除：" << cipherName;
            EXPECT_EQ(cipherName.find("EXP"), std::string::npos) << "导出级套件未排除：" << cipherName;
        }
    }

    /**
     * @brief 加固后的上下文仍能与 TLS 1.2 客户端完成握手
     * @details 同时钉住「安全等级 2 不会拦掉 2048 位 RSA + SHA-256 的仓库夹具证书」，
     *          这正是安全等级取 2 而不降到 1 的依据
     */
    TEST(TlsContext, HardenedContextCompletesTls12Handshake)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.protocolVersion, "TLSv1.2");
    }

    /**
     * @brief 只支持 TLS 1.0 的客户端握手必须失败，且拒绝来自服务端
     * @details 客户端安全等级降到 0 才发得出 TLS 1.0 的 ClientHello，否则失败会发生在客户端本地，
     *          测不到服务端的最低版本策略；客户端收到 protocol_version 告警即证明是服务端拒绝的
     */
    TEST(TlsContext, Tls10OnlyClientHandshakeIsRejected)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        SSL_CTX_set_security_level(clientContext.get(), 0);
        ASSERT_NE(SSL_CTX_set_cipher_list(clientContext.get(), "DEFAULT:@SECLEVEL=0"), 0);
        ASSERT_NE(SSL_CTX_set_min_proto_version(clientContext.get(), TLS1_VERSION), 0);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_FALSE(outcome.serverCompleted) << "服务端不应接受只支持 TLS 1.0 的客户端";
        EXPECT_FALSE(outcome.serverErrorText.empty()) << "服务端应给出拒绝原因";
        EXPECT_FALSE(outcome.clientCompleted) << "客户端也不应完成握手";
        EXPECT_EQ(outcome.clientErrorReason, SSL_R_TLSV1_ALERT_PROTOCOL_VERSION) << outcome.clientErrorText;
    }

    /**
     * @brief 显式套件列表没有把协议版本封死在 1.2：TLS 1.3 客户端仍能完成握手
     */
    TEST(TlsContext, Tls13ClientHandshakeStillSucceeds)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_min_proto_version(clientContext.get(), TLS1_3_VERSION), 0);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_3_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.protocolVersion, "TLSv1.3");
    }

    /**
     * @brief 客户端提供 http/1.1 时 ALPN 协商结果就是 http/1.1（本框架不做 HTTP/2）
     */
    TEST(TlsContext, AlpnNegotiatesHttp11WhenClientOffersIt)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), true, false);

        ASSERT_FALSE(outcome.setupFailed);
        // 前提校验：客户端确实登记了 ALPN 列表，否则协商结果无从谈起
        ASSERT_TRUE(outcome.alpnListAccepted) << "客户端未能登记 ALPN 列表";
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.serverAlpn, "http/1.1");
        EXPECT_EQ(outcome.clientAlpn, "http/1.1");
    }

    /**
     * @brief 客户端不提供 ALPN 时握手照常完成，且两端都没有协商结果
     * @details 钉住 NOACK 分支：不能因为对端没提 ALPN 就拒绝连接
     */
    TEST(TlsContext, AlpnIsSkippedWhenClientDoesNotOfferIt)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_TRUE(outcome.serverAlpn.empty());
        EXPECT_TRUE(outcome.clientAlpn.empty());
    }

    /**
     * @brief CA 文件不存在时 loadClientCertificateAuthority() 返回 false，而不是抛异常或静默成功
     */
    TEST(TlsContext, LoadingClientCertificateAuthorityRejectsMissingFile)
    {
        const TlsContext tlsContext;
        EXPECT_FALSE(tlsContext.loadClientCertificateAuthority("/nonexistent/ca.pem"));
    }

    /**
     * @brief 加载仓库夹具证书作为 CA 成功后，可以开启客户端证书校验（校验模式被置位）
     */
    TEST(TlsContext, LoadingClientCertificateAuthorityAcceptsFixtureCertificate)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath));
        ASSERT_TRUE(tlsContext.loadClientCertificateAuthority(kTestCertificatePath.string()));

        EXPECT_NO_THROW(tlsContext.setClientCertificateRequired(true));
        EXPECT_EQ(SSL_CTX_get_verify_mode(tlsContext.nativeHandle()),
                  SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
    }

    /**
     * @brief 没有可用 CA 就要求客户端证书时必须拒绝，且文案指出修复方式
     * @details 覆盖两种「没有 CA」的情形：从未加载过，以及加载失败过（失败不算已加载）；
     *          传 false 关闭校验则不依赖 CA，任何状态下都接受
     */
    TEST(TlsContext, RequiringClientCertificateWithoutAuthorityThrows)
    {
        TlsContext tlsContext;

        try
        {
            tlsContext.setClientCertificateRequired(true);
            FAIL() << "未加载 CA 时要求客户端证书应当抛出 CoreException";
        }
        catch (const CoreException &exception)
        {
            // 文案要能指导修复：先加载 CA，或改传 false 关闭校验
            const std::string message = exception.what();
            EXPECT_NE(message.find("loadClientCertificateAuthority"), std::string::npos) << message;
            EXPECT_NE(message.find("setClientCertificateRequired(false)"), std::string::npos) << message;
        }

        // 异常类型要落在框架基类上，调用方能用一条 catch 兜住
        EXPECT_THROW(tlsContext.setClientCertificateRequired(true), Base::Exception);

        // 加载失败同样不算「已加载」：此后依然必须拒绝
        EXPECT_FALSE(tlsContext.loadClientCertificateAuthority("/nonexistent/ca.pem"));
        EXPECT_THROW(tlsContext.setClientCertificateRequired(true), CoreException);

        // 关闭校验不需要 CA：任何状态下调用都应当被接受
        EXPECT_NO_THROW(tlsContext.setClientCertificateRequired(false));
    }

    /**
     * @brief 要求客户端证书后，不带证书的连接在握手阶段就被拒绝
     */
    TEST(TlsContext, RequiredClientCertificateRejectsClientWithoutCertificate)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadClientCertificateAuthority(kTestCertificatePath.string()));
        tlsContext.setClientCertificateRequired(true);

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_FALSE(outcome.serverCompleted) << "要求客户端证书时不得放过未出示证书的连接";
        EXPECT_FALSE(outcome.serverErrorText.empty());
        EXPECT_FALSE(outcome.clientCompleted) << outcome.clientErrorText;
    }

    /**
     * @brief 要求客户端证书时，出示受信证书的连接握手成功，且服务端确实做了链校验
     * @details 夹具证书自签且已作为 CA 加载，因此对端出示它即可被验通；校验结果为
     *          X509_V_OK（0）说明走的是真实校验路径，而不是跳过校验放行
     */
    TEST(TlsContext, RequiredClientCertificateAcceptsClientPresentingTrustedCertificate)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadClientCertificateAuthority(kTestCertificatePath.string()));
        tlsContext.setClientCertificateRequired(true);

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, true);

        ASSERT_FALSE(outcome.setupFailed);
        // 前提校验：客户端确实装了证书与私钥，否则下面的校验结果无从谈起
        ASSERT_TRUE(outcome.clientCertificateInstalled) << "客户端未能装载夹具证书/私钥";
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.serverVerifyResult, X509_V_OK);
    }

    /**
     * @brief 显式关闭客户端证书校验后，即使 CA 已加载也不要求对端出示证书
     * @details 钉住「加载了 CA」与「开启了校验」是两件事：传 false 必须回到不校验的状态
     */
    TEST(TlsContext, DisablingClientCertificateRequirementAllowsAnonymousClient)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadClientCertificateAuthority(kTestCertificatePath.string()));
        tlsContext.setClientCertificateRequired(false);
        EXPECT_EQ(SSL_CTX_get_verify_mode(tlsContext.nativeHandle()), SSL_VERIFY_NONE);

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const HandshakeOutcome outcome =
            runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
    }

    /**
     * @brief 客户端只提供 h2 时不得以 http/1.1 完成握手
     *
     * @details 钉住的是可观测行为：这种连接不能协商成功、也不能出现 http/1.1 的协商结果。
     *          注意它**不能区分**两种实现——「服务端扫描列表后拒选」与「服务端乱选、
     *          客户端自己拒绝」都会让两端完不成握手，故选择规则本身靠代码评审与
     *          "客户端提供 http/1.1 时能协商成功"那条用例共同守住。
     */
    TEST(TlsContext, AlpnRejectsClientThatOnlyOffersH2)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);

        // 线格式是「长度前缀 + 协议名」：这里只提供 h2
        const unsigned char h2OnlyAlpn[] = {2, 'h', '2'};
        const HandshakeOutcome outcome = runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), true, false,
                                                              h2OnlyAlpn, static_cast<unsigned int>(sizeof(h2OnlyAlpn)));

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_FALSE(outcome.serverCompleted) << "服务端替客户端选了一个它没提供过的协议名";
        EXPECT_FALSE(outcome.clientCompleted) << "客户端接受了从未提供过的协议名";
        EXPECT_NE(outcome.clientAlpn, "http/1.1") << "协商结果不该是客户端没提过的 http/1.1";
    }
}
