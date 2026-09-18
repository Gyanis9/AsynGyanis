// TlsContext 单元测试：证书与 CA 加载、SSL 对象创建，以及协议加固后的握手行为（使用仓库预生成证书）

#include "Core/Tls/TlsContext.h"

#include "Base/Exception/Exception.h"
#include "Core/Exception/CoreException.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string_view>
#include <thread>

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
            std::string stapledOcspResponse;                ///< 客户端请求并收到的 OCSP 装订响应（未装订时为空串）
            bool        sessionApplied{false};              ///< SSL_set_session 登记成功（仅带会话重连时有意义）
            bool        clientReused{false};                ///< 客户端视角本次握手命中了会话恢复
            bool        serverReused{false};                ///< 服务端视角本次握手命中了会话恢复
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
         * @brief 交替推进服务端与客户端到终态，并把过程快照写进 outcome
         * @details 内存 BIO 对的写端不阻塞，因此两端各调一次 SSL_accept/SSL_connect 交替推进即可：
         *          返回 WANT_READ 只表示还在等对端产出数据，无需套接字与事件循环，也不受时序影响。
         * @param serverSsl 服务端 SSL 对象
         * @param clientSsl 客户端 SSL 对象
         * @param outcome 出参，写入两端的完成状态与错误文本
         */
        void driveBothToTerminal(SSL *serverSsl, SSL *clientSsl, HandshakeOutcome &outcome)
        {
            bool serverTerminal = false;
            bool clientTerminal = false;
            for (int round = 0; round < kMaximumHandshakeRounds && !(serverTerminal && clientTerminal); ++round)
            {
                if (!serverTerminal)
                {
                    const int result = SSL_accept(serverSsl);
                    if (result == 1)
                    {
                        outcome.serverCompleted = true;
                        serverTerminal          = true;
                    }
                    else if (const int error = SSL_get_error(serverSsl, result);
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
                    const int result = SSL_connect(clientSsl);
                    if (result == 1)
                    {
                        outcome.clientCompleted = true;
                        clientTerminal          = true;
                    }
                    else if (const int error = SSL_get_error(clientSsl, result);
                             error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                    {
                        outcome.clientErrorText   = lastOpenSslErrorText();
                        outcome.clientErrorReason = ERR_GET_REASON(ERR_peek_last_error());
                        ERR_clear_error();
                        clientTerminal = true;
                    }
                }
            }
        }

        /**
         * @brief 在单进程内用一对内存 BIO 驱动服务端与客户端完成一次握手。
         * @param serverContext 服务端上下文，通常是被测 TlsContext 的 nativeHandle()
         * @param clientContext 客户端上下文，协议版本与安全等级由调用方按场景设定
         * @param clientOffersAlpn true 时客户端登记 http/1.1，false 时完全不提供 ALPN
         * @param clientPresentsCertificate true 时客户端用仓库夹具证书/私钥作为客户端证书
         * @param clientAlpnWireFormat ALPN 线格式字节
         * @param clientAlpnWireFormatLength ALPN 线格式字节数
         * @param clientRequestsOcspStatus true 时客户端在 ClientHello 里请求 OCSP 状态（status_request）
         * @return HandshakeOutcome 两端的完成情况、错误文本与协商结果
         */
        HandshakeOutcome runInProcessHandshake(SSL_CTX *serverContext, SSL_CTX *clientContext,
                                              const bool clientOffersAlpn, const bool clientPresentsCertificate,
                                              const unsigned char *clientAlpnWireFormat = kHttp11AlpnWireFormat,
                                              const unsigned int clientAlpnWireFormatLength = sizeof(kHttp11AlpnWireFormat),
                                              const bool clientRequestsOcspStatus = false)
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
            if (clientRequestsOcspStatus)
            {
                // 请求 OCSP 状态：服务端应在 status_request 扩展的回应中携带装订响应
                SSL_set_tlsext_status_type(clientSsl.get(), TLSEXT_STATUSTYPE_ocsp);
            }

            driveBothToTerminal(serverSsl.get(), clientSsl.get(), outcome);

            if (clientRequestsOcspStatus)
            {
                // 取回装订响应：未装订时 OpenSSL 返回 -1，此时保持空串与「未收到」同义
                unsigned char *stapledResponse = nullptr;
                const long     stapledLength   = SSL_get_tlsext_status_ocsp_resp(clientSsl.get(), &stapledResponse);
                if (stapledLength > 0 && stapledResponse != nullptr)
                {
                    outcome.stapledOcspResponse.assign(reinterpret_cast<const char *>(stapledResponse),
                                                       static_cast<std::size_t>(stapledLength));
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
     * @brief 客户端只提供 h2 时协商结果就是 h2（本框架已支持 HTTP/2）
     * @details 钉住两点：h2 属于本端支持的协议，且选择只在客户端提供过的名字里进行。
     *          协商失败（例如只提 http/1.0）的情形由 AlpnRejectsClientOfferingOnlyUnsupportedProtocols 覆盖。
     */
    TEST(TlsContext, AlpnNegotiatesH2WhenClientOnlyOffersH2)
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
        ASSERT_TRUE(outcome.alpnListAccepted) << "客户端未能登记 ALPN 列表";
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.serverAlpn, "h2");
        EXPECT_EQ(outcome.clientAlpn, "h2");
    }

    /**
     * @brief 客户端把 h2 与 http/1.1 都提出来时优先选 h2，且与客户端的排列顺序无关
     * @details 钉住偏好顺序：本端按 h2 → http/1.1 的次序挑，不是「客户端先提谁就选谁」。
     */
    TEST(TlsContext, AlpnPrefersH2WhenClientOffersBoth)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        // 两种排列各跑一次：h2 在前与 http/1.1 在前都必须选出 h2
        const std::vector<std::vector<unsigned char>> alpnLists = {
            {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'},
            {8, 'h', 't', 't', 'p', '/', '1', '.', '1', 2, 'h', '2'},
        };
        for (const std::vector<unsigned char> &alpnList: alpnLists)
        {
            SslContextPointer clientContext = createClientContext();
            ASSERT_NE(clientContext, nullptr);

            const HandshakeOutcome outcome = runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), true, false,
                                                                  alpnList.data(), static_cast<unsigned int>(alpnList.size()));

            ASSERT_FALSE(outcome.setupFailed);
            ASSERT_TRUE(outcome.alpnListAccepted) << "客户端未能登记 ALPN 列表";
            EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
            EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
            EXPECT_EQ(outcome.serverAlpn, "h2") << "两个协议都提时必须优先 h2";
            EXPECT_EQ(outcome.clientAlpn, "h2");
        }
    }

    /**
     * @brief 客户端只提本端不支持的协议（http/1.0）时协商失败，且失败原因是没得选
     *
     * @details 钉住「只能选客户端提供过的名字」的另一半：一个都不匹配时必须回
     *          no_application_protocol 终止握手，而不是替对端选一个它没提过的协议名。
     */
    TEST(TlsContext, AlpnRejectsClientOfferingOnlyUnsupportedProtocols)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);

        // 线格式是「长度前缀 + 协议名」：这里只提供 http/1.0，本端不支持
        const unsigned char http10OnlyAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
        const HandshakeOutcome outcome = runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), true, false,
                                                              http10OnlyAlpn, static_cast<unsigned int>(sizeof(http10OnlyAlpn)));

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_FALSE(outcome.serverCompleted) << "服务端替客户端选了一个它没提供过的协议名";
        EXPECT_FALSE(outcome.clientCompleted) << "客户端接受了从未提供过的协议名";
        EXPECT_NE(outcome.clientAlpn, "http/1.1") << "协商结果不该是客户端没提过的 http/1.1";
    }
    // ============================================================================
    // 证书热轮换（TlsContext::reloadCertificate）
    // ============================================================================

    namespace
    {
        /// 临时文件名里的自增序号：同一进程内多次调用也不撞名
        std::atomic<unsigned> g_temporaryFileSequence{0};

        /**
         * @brief 造一个唯一的临时文件路径
         * @param tag 用途标签，便于在临时目录里辨认
         * @return std::filesystem::path 唯一的文件路径
         * @note 名字里必须带「唯一」的成分：用例是并行跑的，两个用例撞名就会互相覆盖文件
         *       （本仓库吃过这类亏），因此时间戳、线程号、自增序号三者都放进去
         */
        [[nodiscard]] std::filesystem::path makeUniqueTemporaryPath(const std::string_view tag)
        {
            const auto ticks        = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto threadHash   = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const unsigned sequence = g_temporaryFileSequence.fetch_add(1);
            return std::filesystem::temp_directory_path() /
                   ("asyngyanis_" + std::string(tag) + "_" + std::to_string(ticks) + "_" + std::to_string(threadHash) + "_" +
                    std::to_string(sequence) + ".pem");
        }

        /**
         * @brief 取上下文将要出示的证书的序列号（十六进制文本）
         * @param context 目标上下文
         * @return std::string 序列号；上下文里还没有证书时为空串
         * @note 读的是 SSL_CTX_get0_certificate()：新建 SSL 时会被装上去的就是这张证书，
         *       因此它变了就等于「新连接的握手换用另一张证书」
         */
        [[nodiscard]] std::string presentedCertificateSerialNumber(SSL_CTX *context)
        {
            X509 *certificate = SSL_CTX_get0_certificate(context);
            if (certificate == nullptr)
            {
                return {};
            }

            const std::unique_ptr<BIGNUM, decltype(&BN_free)> serialNumber(
                ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr), &BN_free);
            if (!serialNumber)
            {
                return {};
            }

            char *hexText = BN_bn2hex(serialNumber.get());
            std::string result(hexText != nullptr ? hexText : "");
            OPENSSL_free(hexText);
            return result;
        }

        /**
         * @brief FILE* 的关闭器类型
         * @note 显式写出生效类型而不是 decltype(&std::fclose)：fclose 带 nonnull 属性，
         *       GCC 会对「把带属性的函数指针当模板实参」报 -Wignored-attributes
         */
        using FileCloser = int (*)(FILE *);

        /**
         * @brief 打开一个 std::FILE*（OpenSSL 的 PEM 读写接口收 FILE*，用不了流式接口）
         * @param filePath 文件路径
         * @param mode fopen 模式串
         * @return FILE* 失败返回 nullptr
         * @note MSVC 在 /W4 下把 fopen 判为弃用（C4996），Windows 侧改用 fopen_s
         */
        FILE *openFileStream(const std::filesystem::path &filePath, const char *mode) noexcept
        {
#if ASYN_PLATFORM_WIN32
            FILE *fileStream = nullptr;
            if (::fopen_s(&fileStream, filePath.string().c_str(), mode) != 0)
            {
                return nullptr;
            }
            return fileStream;
#else
            return ::fopen(filePath.string().c_str(), mode);
#endif
        }

        /**
         * @brief 用给定私钥另造一张自签证书并写成 PEM
         * @param keyFile 既有私钥路径（复用夹具私钥：不同 OpenSSL 版本生成密钥对的接口不一致，没必要碰）
         * @param outputFile 证书输出路径
         * @param serialNumber 序列号，用来与夹具证书区分
         * @return bool 写成功
         */
        bool writeSelfSignedCertificate(const std::filesystem::path &keyFile, const std::filesystem::path &outputFile,
                                        const long serialNumber)
        {
            const std::unique_ptr<FILE, FileCloser> keyStream(openFileStream(keyFile.string().c_str(), "rb"), &std::fclose);
            if (!keyStream)
            {
                return false;
            }

            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> privateKey(
                PEM_read_PrivateKey(keyStream.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free);
            if (!privateKey)
            {
                return false;
            }

            const std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), &X509_free);
            if (!certificate)
            {
                return false;
            }

            // 版本号 2 对应 X.509 v3（OpenSSL 的版本号从 0 起算）
            X509_set_version(certificate.get(), 2);
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serialNumber);
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 24L * 60L * 60L);

            if (X509_set_pubkey(certificate.get(), privateKey.get()) != 1)
            {
                return false;
            }

            X509_NAME *subjectName           = X509_get_subject_name(certificate.get());
            const unsigned char commonName[] = "asyngyanis-reload";
            if (X509_NAME_add_entry_by_txt(subjectName, "CN", MBSTRING_ASC, commonName, -1, -1, 0) != 1)
            {
                return false;
            }
            // 自签：签发者就是自己
            X509_set_issuer_name(certificate.get(), subjectName);

            if (X509_sign(certificate.get(), privateKey.get(), EVP_sha256()) == 0)
            {
                return false;
            }

            const std::unique_ptr<FILE, FileCloser> certificateStream(openFileStream(outputFile.string().c_str(), "wb"),
                                                                                  &std::fclose);
            if (!certificateStream)
            {
                return false;
            }
            return PEM_write_X509(certificateStream.get(), certificate.get()) == 1;
        }

        /// 把仓库夹具证书复制到指定路径（模拟「证书就部署在这个路径上」的形态）
        bool copyFixtureCertificate(const std::filesystem::path &destination)
        {
            std::error_code errorCode;
            std::filesystem::copy_file(kTestCertificatePath, destination, std::filesystem::copy_options::overwrite_existing, errorCode);
            return !errorCode;
        }
    } // namespace

    /**
     * @brief 钉住：路径上的证书被换掉后 reloadCertificate() 换代成功，且出示的证书真的换了
     */
    TEST(TlsContext, ReloadCertificateSwapsThePresentedCertificate)
    {
        const std::filesystem::path certificatePath = makeUniqueTemporaryPath("reload_cert");
        ASSERT_TRUE(copyFixtureCertificate(certificatePath));

        TlsContext context;
        ASSERT_TRUE(context.loadCertificate(certificatePath.string(), kTestKeyPath.string()));

        SSL_CTX *const previousContext = context.nativeHandle();
        ASSERT_NE(previousContext, nullptr);
        const std::string previousSerialNumber = presentedCertificateSerialNumber(previousContext);
        ASSERT_FALSE(previousSerialNumber.empty()) << "夹具证书应能读出序列号";

        // 换代的语义要覆盖「在途连接」：先造一个绑定在旧上下文上的 SSL，稍后验证它没被牵连
        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));
        SSL *const inFlightSsl = context.createSSL(localDescriptor);

        // 覆盖到原路径：这正是 ACME 客户端/续期脚本的行为（路径不变，内容换新）
        ASSERT_TRUE(writeSelfSignedCertificate(kTestKeyPath, certificatePath, 0x5EEDL));

        EXPECT_TRUE(context.reloadCertificate()) << "路径上已是合法的新证书，轮换应当成功；OpenSSL 错误：" << lastOpenSslErrorText();

        // 换代而不是就地改：上下文必须换人，否则并发创建 SSL 时等于边改边用同一个 SSL_CTX
        EXPECT_NE(context.nativeHandle(), previousContext);
        EXPECT_NE(presentedCertificateSerialNumber(context.nativeHandle()), previousSerialNumber)
            << "换过之后出示的证书序列号应当变化";

        // 在途连接仍绑在旧上下文上（它的 SSL 持有旧上下文的引用），因此握手中与已通连的连接不受影响。
        // 紧接着的 SSL_free 会走到旧上下文上：若实现把旧上下文提前释放了，ASan 会在这里直接报出来
        EXPECT_EQ(SSL_get_SSL_CTX(inFlightSsl), previousContext);
        SSL_free(inFlightSsl);

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
        std::error_code errorCode;
        std::filesystem::remove(certificatePath, errorCode);
    }

    /**
     * @brief 钉住：新证书坏了就整体失败，旧证书继续服务（不能因为一次轮换失败而掉线）
     */
    TEST(TlsContext, ReloadCertificateKeepsServingOldCertificateWhenNewOneIsBroken)
    {
        const std::filesystem::path certificatePath = makeUniqueTemporaryPath("broken_cert");
        ASSERT_TRUE(copyFixtureCertificate(certificatePath));

        TlsContext context;
        ASSERT_TRUE(context.loadCertificate(certificatePath.string(), kTestKeyPath.string()));

        SSL_CTX *const previousContext         = context.nativeHandle();
        const std::string previousSerialNumber = presentedCertificateSerialNumber(previousContext);

        // 往证书路径写垃圾：模拟「续期只写了一半」「文件传坏」
        {
            const std::unique_ptr<FILE, FileCloser> stream(openFileStream(certificatePath.string().c_str(), "wb"), &std::fclose);
            ASSERT_TRUE(stream != nullptr);
            // 判据用 fwrite 的返回值：fputs 只承诺「非负」，MSVC 下成功也返回 0
            const char brokenCertificateText[] = "-----BEGIN CERTIFICATE-----\nnot a certificate\n";
            ASSERT_EQ(std::fwrite(brokenCertificateText, 1, sizeof(brokenCertificateText) - 1, stream.get()),
                      sizeof(brokenCertificateText) - 1);
        }

        EXPECT_FALSE(context.reloadCertificate()) << "加载不了的新证书必须让本次轮换整体失败";
        // 关键：失败不碰旧上下文——指针与证书都没变，正在服务的连接完全不受影响
        EXPECT_EQ(context.nativeHandle(), previousContext);
        EXPECT_EQ(presentedCertificateSerialNumber(context.nativeHandle()), previousSerialNumber);

        std::error_code errorCode;
        std::filesystem::remove(certificatePath, errorCode);
    }

    /**
     * @brief 钉住：从未加载过证书时轮换直接失败，而不是把上下文换成空壳
     */
    TEST(TlsContext, ReloadCertificateFailsBeforeAnyCertificateWasLoaded)
    {
        TlsContext context;

        EXPECT_FALSE(context.reloadCertificate());
        EXPECT_NE(context.nativeHandle(), nullptr) << "失败的轮换不该破坏原有上下文";
    }

    /**
     * @brief 钉住：轮换必须复现 mTLS 配置——不能因为续期把对端证书校验悄悄关掉
     */
    TEST(TlsContext, ReloadCertificatePreservesClientCertificateVerification)
    {
        const std::filesystem::path certificatePath = makeUniqueTemporaryPath("mtls_cert");
        ASSERT_TRUE(copyFixtureCertificate(certificatePath));

        TlsContext context;
        ASSERT_TRUE(context.loadCertificate(certificatePath.string(), kTestKeyPath.string()));
        // 自签证书本身就是可信锚点，拿它当校验对端证书的 CA 用
        ASSERT_TRUE(context.loadClientCertificateAuthority(kTestCertificatePath.string()));
        context.setClientCertificateRequired(true);

        const int verifyModeBeforeReload = SSL_CTX_get_verify_mode(context.nativeHandle());
        ASSERT_NE(verifyModeBeforeReload & SSL_VERIFY_PEER, 0);

        ASSERT_TRUE(writeSelfSignedCertificate(kTestKeyPath, certificatePath, 0x5EEEL));
        ASSERT_TRUE(context.reloadCertificate());

        // 复现到位：新上下文同样要求并校验对端证书，而不是退回「不校验」的默认模式
        const int verifyModeAfterReload = SSL_CTX_get_verify_mode(context.nativeHandle());
        EXPECT_NE(verifyModeAfterReload & SSL_VERIFY_PEER, 0) << "轮换后丢失了对端证书校验要求";
        EXPECT_NE(verifyModeAfterReload & SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 0) << "轮换后退化成了「对端可不带证书」";

        std::error_code errorCode;
        std::filesystem::remove(certificatePath, errorCode);
    }

    // ============================================================================
    // 会话恢复（session tickets / 内部缓存）
    // ============================================================================

    namespace
    {
        /// 会话恢复的一次尝试：同一客户端上下文跑两次握手，第二次携带第一次的会话
        struct ResumptionOutcome
        {
            HandshakeOutcome first;                        ///< 第一次（全量）握手
            HandshakeOutcome second;                       ///< 第二次（携带会话重连）握手
            bool             firstSessionResumable{false}; ///< 第一次握手后客户端取到了可恢复会话
            bool             firstSetupFailed{false};      ///< 第一次握手的环境失败（BIO/SSL 创建）
            bool             secondSetupFailed{false};     ///< 第二次握手的环境失败
        };

        /// 一次握手的结果：快照 + 可选的客户端会话（新引用，析构自动释放）
        struct HandshakeWithSession
        {
            HandshakeOutcome outcome;                 ///< 握手快照
            bool             setupFailed{false};      ///< BIO 对或 SSL 对象创建失败
            bool             sessionResumable{false}; ///< 客户端取到了可恢复会话
            std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session{nullptr, &SSL_SESSION_free}; ///< 客户端会话
        };

        /**
         * @brief 完成一次内存 BIO 握手，并在结束后取回客户端侧的可恢复会话
         * @details TLS 1.3 的 NewSessionTicket 在握手完成之后才到达，必须再读一轮让客户端把它
         *          处理掉，SSL_get1_session 拿到的会话才可恢复；BIO 写端不阻塞，读到 WANT_READ
         *          即表示待处理的记录已收齐。
         * @param serverContext 服务端上下文
         * @param clientContext 客户端上下文
         * @param clientPresentsCertificate 客户端是否出示证书（mTLS 场景）
         * @param sessionToResume 第二次握手时携带的会话；nullptr 表示全量握手
         * @return HandshakeWithSession 快照与会话；会话只在可恢复时非空
         */
        HandshakeWithSession completeHandshakeWithSession(SSL_CTX *serverContext, SSL_CTX *clientContext,
                                                          const bool clientPresentsCertificate,
                                                          SSL_SESSION *sessionToResume = nullptr)
        {
            HandshakeWithSession result;

            BIO *clientBio = nullptr;
            BIO *serverBio = nullptr;
            if (BIO_new_bio_pair(&clientBio, 0, &serverBio, 0) != 1)
            {
                result.setupFailed = true;
                return result;
            }

            const std::unique_ptr<SSL, SslDeleter> serverSsl(SSL_new(serverContext));
            const std::unique_ptr<SSL, SslDeleter> clientSsl(SSL_new(clientContext));
            if (serverSsl == nullptr || clientSsl == nullptr)
            {
                BIO_free(clientBio);
                BIO_free(serverBio);
                result.setupFailed = true;
                return result;
            }

            SSL_set_bio(serverSsl.get(), serverBio, serverBio);
            SSL_set_bio(clientSsl.get(), clientBio, clientBio);

            if (clientPresentsCertificate)
            {
                SSL_use_certificate_file(clientSsl.get(), kTestCertificatePath.string().c_str(), SSL_FILETYPE_PEM);
                SSL_use_PrivateKey_file(clientSsl.get(), kTestKeyPath.string().c_str(), SSL_FILETYPE_PEM);
            }
            if (sessionToResume != nullptr)
            {
                // 只是登记候选会话：命不命中由握手本身决定，没命中就退回一次全量握手
                result.outcome.sessionApplied = SSL_set_session(clientSsl.get(), sessionToResume) == 1;
            }

            driveBothToTerminal(serverSsl.get(), clientSsl.get(), result.outcome);

            // 恢复判定两端各看一次：只信一侧可能把「服务端发了票据但客户端没用上」误判成恢复
            result.outcome.clientReused = SSL_session_reused(clientSsl.get()) == 1;
            result.outcome.serverReused = SSL_session_reused(serverSsl.get()) == 1;
            result.outcome.protocolVersion = SSL_get_version(serverSsl.get());

            if (result.outcome.clientCompleted)
            {
                // 读一轮处理握手后的 NewSessionTicket；非应用数据被 SSL_read 就地消费，最后以 WANT_READ 收尾
                char      ignoredByte = 0;
                const int readResult  = SSL_read(clientSsl.get(), &ignoredByte, 1);
                (void) readResult;
                ERR_clear_error();

                // 礼仪式关闭：SSL_free 会把「未发过 close_notify」的连接当坏会话，顺手把其当前
                // 会话标记成不可恢复（ssl_clear_bad_session）。对「导出会话供下次连接复用」的用法，
                // 必须在销毁前发一次 close_notify——这也是真实客户端复用会话的标准姿势
                SSL_shutdown(clientSsl.get());
                ERR_clear_error();

                SSL_SESSION *session = SSL_get1_session(clientSsl.get());
                result.sessionResumable = session != nullptr && SSL_SESSION_is_resumable(session) == 1;
                if (!result.sessionResumable && session != nullptr)
                {
                    SSL_SESSION_free(session);
                    session = nullptr;
                }
                result.session.reset(session);
            }
            return result;
        }

        /**
         * @brief 同一客户端上下文连跑两次握手：第一次取会话，第二次带上会话验证恢复命中
         * @param serverContext 服务端上下文
         * @param clientContext 客户端上下文
         * @param clientPresentsCertificate 客户端是否出示证书（mTLS 场景）
         * @return ResumptionOutcome 两次握手的快照与恢复命中情况
         */
        ResumptionOutcome runResumptionHandshake(SSL_CTX *serverContext, SSL_CTX *clientContext,
                                                 const bool clientPresentsCertificate)
        {
            ResumptionOutcome result;

            HandshakeWithSession first = completeHandshakeWithSession(serverContext, clientContext, clientPresentsCertificate);
            result.first                = first.outcome;
            result.firstSetupFailed     = first.setupFailed;
            result.firstSessionResumable = first.sessionResumable;
            if (first.setupFailed || !first.sessionResumable)
            {
                return result; // 第二次无从进行：调用方先按第一次的断言定位
            }

            HandshakeWithSession second = completeHandshakeWithSession(serverContext, clientContext, clientPresentsCertificate,
                                                                       first.session.get());
            result.second            = second.outcome;
            result.secondSetupFailed = second.setupFailed;
            return result;
        }
    } // namespace

    /**
     * @brief 钉住：TLS 1.3 下第二次连接携带会话票据即命中恢复（两端视角都验证）
     */
    TEST(TlsContext, SessionResumptionHitsOnTls13)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        // 不限定版本：加固服务端与默认客户端会协商到 TLS 1.3（下面用前置断言钉住这一前提）
        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);

        const ResumptionOutcome outcome = runResumptionHandshake(tlsContext.nativeHandle(), clientContext.get(), false);

        ASSERT_FALSE(outcome.firstSetupFailed);
        ASSERT_TRUE(outcome.first.serverCompleted) << outcome.first.serverErrorText;
        ASSERT_TRUE(outcome.first.clientCompleted) << outcome.first.clientErrorText;
        ASSERT_EQ(outcome.first.protocolVersion, "TLSv1.3") << "本用例的前提是协商到 TLS 1.3";
        ASSERT_TRUE(outcome.firstSessionResumable) << "握手后客户端没有取到可恢复的会话票据";

        ASSERT_FALSE(outcome.secondSetupFailed);
        EXPECT_TRUE(outcome.second.sessionApplied) << "SSL_set_session 登记失败";
        ASSERT_TRUE(outcome.second.serverCompleted) << outcome.second.serverErrorText;
        ASSERT_TRUE(outcome.second.clientCompleted) << outcome.second.clientErrorText;
        EXPECT_TRUE(outcome.second.clientReused) << "第二次握手客户端视角没有命中恢复";
        EXPECT_TRUE(outcome.second.serverReused) << "第二次握手服务端视角没有命中恢复";
    }

    /**
     * @brief 钉住：TLS 1.2 下会话恢复（票据/内部缓存）同样命中
     */
    TEST(TlsContext, SessionResumptionHitsOnTls12)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const ResumptionOutcome outcome = runResumptionHandshake(tlsContext.nativeHandle(), clientContext.get(), false);

        ASSERT_FALSE(outcome.firstSetupFailed);
        ASSERT_TRUE(outcome.first.serverCompleted) << outcome.first.serverErrorText;
        ASSERT_TRUE(outcome.first.clientCompleted) << outcome.first.clientErrorText;
        ASSERT_EQ(outcome.first.protocolVersion, "TLSv1.2");
        ASSERT_TRUE(outcome.firstSessionResumable) << "握手后客户端没有取到可恢复的会话";

        ASSERT_FALSE(outcome.secondSetupFailed);
        EXPECT_TRUE(outcome.second.sessionApplied) << "SSL_set_session 登记失败";
        ASSERT_TRUE(outcome.second.serverCompleted) << outcome.second.serverErrorText;
        ASSERT_TRUE(outcome.second.clientCompleted) << outcome.second.clientErrorText;
        EXPECT_TRUE(outcome.second.clientReused) << "第二次握手客户端视角没有命中恢复";
        EXPECT_TRUE(outcome.second.serverReused) << "第二次握手服务端视角没有命中恢复";
    }

    /**
     * @brief 钉住：要求客户端证书的部署同样能恢复会话
     * @details OpenSSL 在启用对端校验（SSL_VERIFY_PEER）且未设置 session id context 时会拒绝
     *          TLS 1.2 的会话恢复；本用例的 reused 断言就是那条设置的判据——去掉它即变红
     *          （客户端会静默退回一次全量握手，两端 reused 均为假）。
     */
    TEST(TlsContext, SessionResumptionHitsWithRequiredClientCertificate)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadClientCertificateAuthority(kTestCertificatePath.string()));
        tlsContext.setClientCertificateRequired(true);

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        const ResumptionOutcome outcome = runResumptionHandshake(tlsContext.nativeHandle(), clientContext.get(), true);

        ASSERT_FALSE(outcome.firstSetupFailed);
        ASSERT_TRUE(outcome.first.serverCompleted) << outcome.first.serverErrorText;
        ASSERT_TRUE(outcome.first.clientCompleted) << outcome.first.clientErrorText;
        ASSERT_TRUE(outcome.firstSessionResumable) << "握手后客户端没有取到可恢复的会话";

        ASSERT_FALSE(outcome.secondSetupFailed);
        EXPECT_TRUE(outcome.second.sessionApplied) << "SSL_set_session 登记失败";
        ASSERT_TRUE(outcome.second.serverCompleted) << outcome.second.serverErrorText;
        ASSERT_TRUE(outcome.second.clientCompleted) << outcome.second.clientErrorText;
        EXPECT_TRUE(outcome.second.clientReused) << "mTLS 部署下第二次握手没有命中恢复（客户端视角）";
        EXPECT_TRUE(outcome.second.serverReused) << "mTLS 部署下第二次握手没有命中恢复（服务端视角）";
    }

    // ============================================================================
    // OCSP 装订（stapling）
    // ============================================================================

    namespace
    {
        /// 载入仓库夹具证书（OCSP 用例的签发者）
        std::unique_ptr<X509, decltype(&X509_free)> loadFixtureCertificate()
        {
            const std::unique_ptr<FILE, FileCloser> stream(openFileStream(kTestCertificatePath.string().c_str(), "rb"), &std::fclose);
            if (!stream)
            {
                return {nullptr, &X509_free};
            }
            return {PEM_read_X509(stream.get(), nullptr, nullptr, nullptr), &X509_free};
        }

        /// 载入仓库夹具私钥（复用为叶证书密钥与签名密钥，避免另生成密钥的版本差异）
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> loadFixturePrivateKey()
        {
            const std::unique_ptr<FILE, FileCloser> stream(openFileStream(kTestKeyPath.string().c_str(), "rb"), &std::fclose);
            if (!stream)
            {
                return {nullptr, &EVP_PKEY_free};
            }
            return {PEM_read_PrivateKey(stream.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free};
        }

        /// 载入任意 PEM 证书文件
        std::unique_ptr<X509, decltype(&X509_free)> loadCertificateFrom(const std::filesystem::path &file)
        {
            const std::unique_ptr<FILE, FileCloser> stream(openFileStream(file.string().c_str(), "rb"), &std::fclose);
            if (!stream)
            {
                return {nullptr, &X509_free};
            }
            return {PEM_read_X509(stream.get(), nullptr, nullptr, nullptr), &X509_free};
        }

        /**
         * @brief 造一张由夹具证书签发的叶证书并写成 PEM
         * @details 装订用例必须避开自签名的叶证书：OpenSSL 3.x 对自签名叶证书直接跳过 OCSP 装订
         *          （策略），而仓库夹具是自签的。这里复用夹具私钥、把 subject 换成叶名字、issuer
         *          指向夹具证书，得到一张「subject ≠ issuer」的叶证书。
         * @param outputFile 叶证书输出路径
         * @param serialNumber 序列号（换证用例靠它区分新旧）
         * @return bool 写成功
         */
        bool writeCaSignedLeafCertificate(const std::filesystem::path &outputFile, const long serialNumber)
        {
            const auto issuerCertificate = loadFixtureCertificate();
            const auto issuerKey         = loadFixturePrivateKey();
            if (!issuerCertificate || !issuerKey)
            {
                return false;
            }

            const std::unique_ptr<X509, decltype(&X509_free)> leaf(X509_new(), &X509_free);
            if (!leaf)
            {
                return false;
            }

            // 版本号 2 对应 X.509 v3（OpenSSL 的版本号从 0 起算）
            X509_set_version(leaf.get(), 2);
            ASN1_INTEGER_set(X509_get_serialNumber(leaf.get()), serialNumber);
            X509_gmtime_adj(X509_getm_notBefore(leaf.get()), 0);
            X509_gmtime_adj(X509_getm_notAfter(leaf.get()), 24L * 60L * 60L);

            if (X509_set_pubkey(leaf.get(), issuerKey.get()) != 1)
            {
                return false;
            }

            X509_NAME *subjectName           = X509_get_subject_name(leaf.get());
            const unsigned char commonName[] = "asyngyanis-leaf";
            if (X509_NAME_add_entry_by_txt(subjectName, "CN", MBSTRING_ASC, commonName, -1, -1, 0) != 1)
            {
                return false;
            }
            // 非自签的关键：issuer 是夹具证书，而不是叶证书自己
            if (X509_set_issuer_name(leaf.get(), X509_get_subject_name(issuerCertificate.get())) != 1)
            {
                return false;
            }
            if (X509_sign(leaf.get(), issuerKey.get(), EVP_sha256()) == 0)
            {
                return false;
            }

            const std::unique_ptr<FILE, FileCloser> stream(openFileStream(outputFile.string().c_str(), "wb"), &std::fclose);
            if (!stream)
            {
                return false;
            }
            return PEM_write_X509(stream.get(), leaf.get()) == 1;
        }

        /**
         * @brief 造一份与给定叶证书匹配的 OCSP 响应（DER 字节）
         * @details 服务端装订前会按「序列号 + 签发者名哈希」把响应与叶证书对上（OpenSSL 3.x），
         *          因此用例必须造真实匹配的响应；服务端不验签，但这里照样签名保持结构真实。
         * @param leafCertificate 叶证书
         * @param issuerCertificate 签发者证书
         * @param issuerKey 签发者私钥
         * @return std::string DER 字节；任一步失败返回空串
         */
        std::string makeMatchingOcspResponseDer(X509 *leafCertificate, X509 *issuerCertificate, EVP_PKEY *issuerKey)
        {
            const std::unique_ptr<OCSP_BASICRESP, decltype(&OCSP_BASICRESP_free)> basic(
                    OCSP_BASICRESP_new(), &OCSP_BASICRESP_free);
            const std::unique_ptr<OCSP_CERTID, decltype(&OCSP_CERTID_free)> certificateId(
                    OCSP_cert_to_id(EVP_sha1(), leafCertificate, issuerCertificate), &OCSP_CERTID_free);
            const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> thisUpdate(
                    ASN1_TIME_set(nullptr, std::time(nullptr)), &ASN1_TIME_free);
            const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> nextUpdate(
                    ASN1_TIME_adj(nullptr, std::time(nullptr), 0, 3600), &ASN1_TIME_free);
            if (!basic || !certificateId || !thisUpdate || !nextUpdate)
            {
                return {};
            }

            if (OCSP_basic_add1_status(basic.get(), certificateId.get(), V_OCSP_CERTSTATUS_GOOD, 0, nullptr,
                                       thisUpdate.get(), nextUpdate.get()) == nullptr)
            {
                return {};
            }
            if (OCSP_basic_sign(basic.get(), issuerCertificate, issuerKey, EVP_sha256(), nullptr, 0) != 1)
            {
                return {};
            }

            // OCSP_response_create 会把 basic 打包复制进响应，basic 的所有权仍在调用方
            const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> response(
                    OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic.get()), &OCSP_RESPONSE_free);
            if (!response)
            {
                return {};
            }

            unsigned char *der = nullptr;
            const int      derLength = i2d_OCSP_RESPONSE(response.get(), &der);
            if (derLength <= 0 || der == nullptr)
            {
                return {};
            }
            std::string bytes(reinterpret_cast<const char *>(der), static_cast<std::size_t>(derLength));
            OPENSSL_free(der);
            return bytes;
        }

        /// 把字节串写到指定路径（覆盖已有内容）；写失败返回 false
        bool overwriteBytes(const std::filesystem::path &path, const std::string &bytes)
        {
            std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(stream);
        }

        /**
         * @brief 把字节串写进唯一命名的临时文件（二进制）
         * @param tag 用途标签，便于在临时目录里辨认
         * @param bytes 文件内容
         * @return std::filesystem::path 文件路径；写失败返回空路径
         */
        std::filesystem::path writeTemporaryBytes(const std::string_view tag, const std::string &bytes)
        {
            const std::filesystem::path path = makeUniqueTemporaryPath(tag).replace_extension(".der");
            if (!overwriteBytes(path, bytes))
            {
                return {};
            }
            return path;
        }

        /// 一套装订测试材料：非自签叶证书与与之匹配的 OCSP 响应
        struct StaplingTestMaterial
        {
            std::filesystem::path leafCertificatePath; ///< 叶证书 PEM（非自签）
            std::filesystem::path responsePath;        ///< 匹配响应的 DER 文件
            std::string           responseBytes;       ///< 匹配响应的字节
        };

        /**
         * @brief 造一套装订测试材料（叶证书 + 与之匹配的 OCSP 响应文件）
         * @param tag 临时文件用途标签
         * @param serialNumber 叶证书序列号
         * @return StaplingTestMaterial 材料；任一步失败时路径为空
         */
        StaplingTestMaterial makeStaplingTestMaterial(const std::string_view tag, const long serialNumber)
        {
            StaplingTestMaterial material;
            material.leafCertificatePath = makeUniqueTemporaryPath(tag).replace_extension(".pem");
            if (!writeCaSignedLeafCertificate(material.leafCertificatePath, serialNumber))
            {
                return material;
            }

            const auto leaf      = loadCertificateFrom(material.leafCertificatePath);
            const auto issuer    = loadFixtureCertificate();
            const auto issuerKey = loadFixturePrivateKey();
            if (!leaf || !issuer || !issuerKey)
            {
                return material;
            }

            material.responseBytes = makeMatchingOcspResponseDer(leaf.get(), issuer.get(), issuerKey.get());
            if (material.responseBytes.empty())
            {
                return material;
            }
            material.responsePath = writeTemporaryBytes(tag, material.responseBytes);
            return material;
        }

        /// 请求 OCSP 状态跑一次 TLS 1.2 握手（OCSP 用例的公共形态）
        HandshakeOutcome runHandshakeRequestingOcspStatus(SSL_CTX *serverContext)
        {
            SslContextPointer clientContext = createClientContext();
            if (clientContext == nullptr)
            {
                HandshakeOutcome failed;
                failed.setupFailed = true;
                return failed;
            }
            SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION);
            return runInProcessHandshake(serverContext, clientContext.get(), false, false,
                                         kHttp11AlpnWireFormat, sizeof(kHttp11AlpnWireFormat), true);
        }
    } // namespace

    /**
     * @brief 钉住：加载的 OCSP 响应在客户端请求时被原样装订
     * @details 用非自签的叶证书（夹具 CA 签发）：OpenSSL 3.x 对自签名叶证书跳过装订
     */
    TEST(TlsContext, OcspStaplingDeliversLoadedResponse)
    {
        const StaplingTestMaterial material = makeStaplingTestMaterial("ocsp_ok", 197L);
        ASSERT_FALSE(material.responsePath.empty()) << "装订测试材料构造失败（叶证书或 OCSP 响应）";

        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(material.leafCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadOcspResponse(material.responsePath.string()));

        const HandshakeOutcome outcome = runHandshakeRequestingOcspStatus(tlsContext.nativeHandle());

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.stapledOcspResponse, material.responseBytes) << "装订响应与加载的字节不一致";

        std::error_code errorCode;
        std::filesystem::remove(material.leafCertificatePath, errorCode);
        std::filesystem::remove(material.responsePath, errorCode);
    }

    /**
     * @brief 钉住：客户端没有请求 OCSP 状态时服务端不装订
     */
    TEST(TlsContext, OcspStaplingIsSkippedWithoutClientRequest)
    {
        const StaplingTestMaterial material = makeStaplingTestMaterial("ocsp_noreq", 198L);
        ASSERT_FALSE(material.responsePath.empty()) << "装订测试材料构造失败（叶证书或 OCSP 响应）";

        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(material.leafCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(tlsContext.loadOcspResponse(material.responsePath.string()));

        SslContextPointer clientContext = createClientContext();
        ASSERT_NE(clientContext, nullptr);
        ASSERT_NE(SSL_CTX_set_max_proto_version(clientContext.get(), TLS1_2_VERSION), 0);

        // 不请求 status_request：装订数据在上下文中，但不应随响应下发
        const HandshakeOutcome outcome = runInProcessHandshake(tlsContext.nativeHandle(), clientContext.get(), false, false);

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_TRUE(outcome.stapledOcspResponse.empty()) << "没被请求却下发了装订响应";

        std::error_code errorCode;
        std::filesystem::remove(material.leafCertificatePath, errorCode);
        std::filesystem::remove(material.responsePath, errorCode);
    }

    /**
     * @brief 钉住：未加载响应时客户端请求也不导致失败，只是不装订（NOACK 路径）
     */
    TEST(TlsContext, OcspStaplingIsSkippedWhenNothingLoaded)
    {
        TlsContext tlsContext;
        ASSERT_TRUE(tlsContext.loadCertificate(kTestCertificatePath.string(), kTestKeyPath.string()));

        const HandshakeOutcome outcome = runHandshakeRequestingOcspStatus(tlsContext.nativeHandle());

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_TRUE(outcome.stapledOcspResponse.empty()) << "没有加载响应却装订了内容";
    }

    /**
     * @brief 拒绝面：路径不存在、内容为空或不是合法 DER 都返回 false
     */
    TEST(TlsContext, LoadOcspResponseRejectsMissingEmptyOrGarbageFile)
    {
        const TlsContext tlsContext;
        EXPECT_FALSE(tlsContext.loadOcspResponse("/nonexistent/ocsp.der"));

        const std::filesystem::path emptyPath = writeTemporaryBytes("ocsp_empty", std::string{});
        ASSERT_FALSE(emptyPath.empty());
        EXPECT_FALSE(tlsContext.loadOcspResponse(emptyPath.string()));

        // 加载即做 DER 解析校验：坏文件在配置阶段就被挡掉，而不是服务期间静默不装订
        const std::filesystem::path garbagePath = writeTemporaryBytes("ocsp_garbage", "this is not a DER OCSP response");
        ASSERT_FALSE(garbagePath.empty());
        EXPECT_FALSE(tlsContext.loadOcspResponse(garbagePath.string()));

        std::error_code errorCode;
        std::filesystem::remove(emptyPath, errorCode);
        std::filesystem::remove(garbagePath, errorCode);
    }

    /**
     * @brief 钉住：证书热轮换后装订照旧（换代按原路径重读响应文件）
     * @details 续期的真实形态是证书与响应一起更新：新叶证书有新序列号，响应必须随之匹配。
     *          若实现沿用旧字节而不重读，旧响应与新证书对不上，装订会缺席——本用例即变红。
     */
    TEST(TlsContext, ReloadCertificateKeepsOcspStapling)
    {
        StaplingTestMaterial material = makeStaplingTestMaterial("ocsp_reload", 0xC6L);
        ASSERT_FALSE(material.responsePath.empty()) << "装订测试材料构造失败（叶证书或 OCSP 响应）";

        TlsContext context;
        ASSERT_TRUE(context.loadCertificate(material.leafCertificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(context.loadOcspResponse(material.responsePath.string()));

        // 证书与响应一起更新到原路径
        ASSERT_TRUE(writeCaSignedLeafCertificate(material.leafCertificatePath, 0xC7L));
        const auto newLeaf   = loadCertificateFrom(material.leafCertificatePath);
        const auto issuer    = loadFixtureCertificate();
        const auto issuerKey = loadFixturePrivateKey();
        ASSERT_TRUE(newLeaf && issuer && issuerKey);
        const std::string newResponseBytes = makeMatchingOcspResponseDer(newLeaf.get(), issuer.get(), issuerKey.get());
        ASSERT_FALSE(newResponseBytes.empty());
        ASSERT_TRUE(overwriteBytes(material.responsePath, newResponseBytes));

        ASSERT_TRUE(context.reloadCertificate());

        const HandshakeOutcome outcome = runHandshakeRequestingOcspStatus(context.nativeHandle());

        ASSERT_FALSE(outcome.setupFailed);
        EXPECT_TRUE(outcome.serverCompleted) << outcome.serverErrorText;
        EXPECT_TRUE(outcome.clientCompleted) << outcome.clientErrorText;
        EXPECT_EQ(outcome.stapledOcspResponse, newResponseBytes) << "轮换后的上下文没有按原路径重读最新响应";

        std::error_code errorCode;
        std::filesystem::remove(material.leafCertificatePath, errorCode);
        std::filesystem::remove(material.responsePath, errorCode);
    }

    /**
     * @brief 钉住：轮换时 OCSP 文件不可读 → 本次轮换整体失败，旧上下文继续服务
     */
    TEST(TlsContext, ReloadCertificateFailsWhenOcspFileDisappeared)
    {
        const std::filesystem::path certificatePath = makeUniqueTemporaryPath("ocsp_gone_cert");
        ASSERT_TRUE(copyFixtureCertificate(certificatePath));

        // 响应只需是合法 DER（本用例只验证「重读失败让轮换整体失败」）；用夹具自签证书自配一份
        const auto issuer    = loadFixtureCertificate();
        const auto issuerKey = loadFixturePrivateKey();
        ASSERT_TRUE(issuer && issuerKey);
        const std::string responseBytes = makeMatchingOcspResponseDer(issuer.get(), issuer.get(), issuerKey.get());
        ASSERT_FALSE(responseBytes.empty());
        const std::filesystem::path responsePath = writeTemporaryBytes("ocsp_gone", responseBytes);
        ASSERT_FALSE(responsePath.empty());

        TlsContext context;
        ASSERT_TRUE(context.loadCertificate(certificatePath.string(), kTestKeyPath.string()));
        ASSERT_TRUE(context.loadOcspResponse(responsePath.string()));

        SSL_CTX *const    previousContext = context.nativeHandle();
        const std::string previousSerialNumber = presentedCertificateSerialNumber(previousContext);

        // 证书换新、但响应文件被清掉：重读失败必须让整次轮换失败，而不是悄悄装订一份过期响应
        ASSERT_TRUE(writeSelfSignedCertificate(kTestKeyPath, certificatePath, 0x5EED3L));
        std::error_code errorCode;
        std::filesystem::remove(responsePath, errorCode);

        EXPECT_FALSE(context.reloadCertificate()) << "OCSP 响应已不可读，轮换应当整体失败";
        EXPECT_EQ(context.nativeHandle(), previousContext);
        EXPECT_EQ(presentedCertificateSerialNumber(context.nativeHandle()), previousSerialNumber);

        std::filesystem::remove(certificatePath, errorCode);
    }
}
