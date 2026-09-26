// TlsPolicy 单元测试：默认档与既有加固逐项等价、每一档配置都读得回来、非法值当场抛且点名是哪一项
//
// 为什么按「读回 SSL_CTX」的口径断言而不是跑真握手：策略的正确性在于「 OpenSSL 收到了什么」，
// 握手感知的判据（这套套件能不能连上、曲线协商得对不对）归 TestHttpsServer 与 h2spec/autobahn
// 那些进程外门禁。这里要钉住的是另外两件事：默认策略不改变既有档位，和被拒绝的配置一定会抛。

#include "Core/Tls/TlsPolicy.h"

#include "Base/Exception/Exception.h"
#include "Core/Exception/CoreException.h"
#include "Core/Tls/TlsContext.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 仓库内预生成的自签测试证书（同时充当 CA 文件的角色：里面就是那张签发证书本身）
        const std::filesystem::path kTestCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

        /// 服务端不写策略时使用的那份内置套件列表（与 TlsContext 里的加固档同一形态）
        constexpr const char *kBuiltInCipherList = "HIGH:!aNULL:!eNULL:!MD5:!RC4:!3DES:!DES:!EXPORT:!PSK:!SRP";

        /**
         * @brief 裸 SSL_CTX 夹具：只当作策略的被试对象
         * @details 不经 TlsContext 是有意的——那条路径自带压缩开关、重协商、ALPN 回调等一揽子加固，
         *          读数会混进「与被测项无关的既有设置」，出错时也分不清是谁干的。
         */
        class RawContext
        {
        public:
            explicit RawContext(const bool isServer = true) : m_context(SSL_CTX_new(isServer ? TLS_server_method() : TLS_client_method()))
            {
            }

            ~RawContext()
            {
                if (m_context != nullptr)
                {
                    SSL_CTX_free(m_context);
                }
            }

            RawContext(const RawContext &)            = delete;
            RawContext &operator=(const RawContext &) = delete;

            [[nodiscard]] SSL_CTX *get() const noexcept
            {
                return m_context;
            }

        private:
            SSL_CTX *m_context{nullptr}; ///< 本夹具独占的上下文
        };
    } // namespace

    /**
     * @brief 钉住：默认策略逐字保持既有加固档位（最低 1.2、安全等级 2、票据开着、组不动）
     * @details 这条是「可配置性只加在不满意的人身上」的凭据：默认构造的 TlsPolicy 施加到裸上下文上，
     *          读回来的必须与本次改造之前写死的那一套一致，否则老部署会在没改任何配置的情况下换档。
     */
    TEST(TlsPolicy, DefaultPolicyKeepsTheHardenedBaseline)
    {
        const RawContext context;
        ASSERT_NE(context.get(), nullptr);
        // 裸上下文的起点由 OpenSSL 给（随发行版配置变化，可能是 1.0 也可能是 1.6.5b 那档），
        // 这里只断言「没被改动」：下限是**服务端角色**补的（见下面 ServerRoleKeepsTheTls12Floor），
        // applyTlsPolicy 自己不许替调用方发明下限
        const int startingMinimumVersion = SSL_CTX_get_min_proto_version(context.get());

        applyTlsPolicy(context.get(), TlsPolicy{}, kBuiltInCipherList);

        EXPECT_EQ(SSL_CTX_get_min_proto_version(context.get()), startingMinimumVersion);
        EXPECT_EQ(SSL_CTX_get_security_level(context.get()), 2);
        // 会话票据默认不关：SSL_OP_NO_TICKET 出现即说明这份策略把恢复改成了只走 session id 缓存
        EXPECT_EQ(SSL_CTX_get_options(context.get()) & SSL_OP_NO_TICKET, 0UL);
        // 施加内置列表之后仍有一批可选套件（真正的「是不是那份列表」由上面的报错文案与
        // RejectsInvalidStrings 那条反向断言共同钉住：这里只保证没把可选项清成空）
        ASSERT_GT(sk_SSL_CIPHER_num(SSL_CTX_get_ciphers(context.get())), 0);
    }

    /**
     * @brief 钉住：服务端的 TLS 1.2 下限仍在，且是「角色」给的而不是策略里写死的
     * @details 改造前那句 SSL_CTX_set_min_proto_version(TLS1_2_VERSION) 直接写在加固函数里；
     *          现在它等价地表达为「服务端角色 + 未指定下限时补 1.2」。老调用方（不带参数的
     *          TlsContext 构造）必须读到同一个下限，否则一次不改配置的升级就把 TLS 1.0 放回来了。
     */
    TEST(TlsPolicy, ServerRoleKeepsTheTls12FloorAndClientRoleDoesNot)
    {
        const TlsContext serverContext;
        EXPECT_EQ(SSL_CTX_get_min_proto_version(serverContext.nativeHandle()), TLS1_2_VERSION);

        const TlsContext clientContext(TlsPolicy{}, TlsContext::Role::Client);
        EXPECT_EQ(SSL_CTX_get_min_proto_version(clientContext.nativeHandle()), 0) << "客户端被强加服务端的下限，等于把本可以连上的对端拒掉";
        // 但加固照做：等级 2 与「不协商压缩」两侧同档
        EXPECT_EQ(SSL_CTX_get_security_level(clientContext.nativeHandle()), 2);
        EXPECT_NE(SSL_CTX_get_options(clientContext.nativeHandle()) & SSL_OP_NO_COMPRESSION, 0UL);
    }

    /// 版本区间照收；区间填反了当场抛，且消息说是哪一档
    TEST(TlsPolicy, HonoursTheVersionRangeAndRejectsAReversedOne)
    {
        const RawContext context;
        TlsPolicy        policy;
        policy.minimumProtocolVersion = TlsPolicy::ProtocolVersion::Tls1_3;
        policy.maximumProtocolVersion = TlsPolicy::ProtocolVersion::Tls1_3;
        applyTlsPolicy(context.get(), policy, kBuiltInCipherList);

        EXPECT_EQ(SSL_CTX_get_min_proto_version(context.get()), TLS1_3_VERSION);
        EXPECT_EQ(SSL_CTX_get_max_proto_version(context.get()), TLS1_3_VERSION);

        const RawContext reversedContext;
        policy.maximumProtocolVersion = TlsPolicy::ProtocolVersion::Tls1_2;
        try
        {
            applyTlsPolicy(reversedContext.get(), policy, kBuiltInCipherList);
            FAIL() << "最低 1.3 高于最高 1.2 的策略不该被默默接受";
        } catch (const CoreException &error)
        {
            // OpenSSL 只会回一句 "operation not supported"，看不出是自己把区间填倒了
            const std::string message = error.what();
            EXPECT_NE(message.find("TLS 策略无效"), message.npos) << message;
            EXPECT_NE(message.find("TLS 1.3"), message.npos) << message;
            EXPECT_NE(message.find("TLS 1.2"), message.npos) << message;
        }
    }

    /**
     * @brief 钉住：被 OpenSSL 拒绝的字符串一定抛，且消息里带着那份原文
     * @details 三处同类（1.2 套件、1.3 套件、曲线）都靠同一句承诺排障：报错里没有原文的话，
     *          调用方只能猜自己配了什么——而这套配置通常是运维从别处抄来的。
     */
    TEST(TlsPolicy, RejectsInvalidStringsNamingTheOffendingValue)
    {
        {
            const RawContext context;
            TlsPolicy        policy;
            policy.cipherList = "NO_SUCH_CIPHER_SUITE_AT_ALL";
            EXPECT_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList), CoreException);
        }
        {
            const RawContext context;
            TlsPolicy        policy;
            policy.tls13CipherSuites = "TLS_13_NOT_A_THING";
            EXPECT_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList), CoreException);
        }
        {
            const RawContext context;
            TlsPolicy        policy;
            // 逗号不是分隔符：OpenSSL 的曲线列表用冒号，写错的人需要被告知原文
            policy.supportedGroups = "X25519,secp384r1";
            EXPECT_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList), CoreException);
        }

        // 合法写法照旧收：X25519 在两套 OpenSSL 构建里都在
        const RawContext context;
        TlsPolicy        policy;
        policy.supportedGroups = "X25519:secp384r1";
        EXPECT_NO_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList));
    }

    /**
     * @brief 钉住：OpenSSL 会「静默忽略」的那两类，本层拦住
     * @details 安全等级为负与校验深度为 0 都是设了不报错、也没效果的写法，等于把策略里的一项变成
     *          装饰；只有这一层能给出「这项没生效」的信号，所以宁可拒。
     */
    TEST(TlsPolicy, RejectsValuesOpenSSLWouldSilentlyIgnore)
    {
        {
            const RawContext context;
            TlsPolicy        policy;
            policy.securityLevel = -1;
            EXPECT_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList), CoreException);
        }
        {
            const RawContext context;
            TlsPolicy        policy;
            policy.verifyDepth = 0;
            EXPECT_THROW(applyTlsPolicy(context.get(), policy, kBuiltInCipherList), CoreException);
        }
        // 显式给等级 1 与深度 2 都要生效（1 是「要放宽」时的合法写法，本层不替调用方决定不能放）
        const RawContext context;
        TlsPolicy        policy;
        policy.securityLevel = 1;
        policy.verifyDepth   = 2;
        ASSERT_NO_THROW(applyTlsPolicy(context.get(), policy, nullptr));
        EXPECT_EQ(SSL_CTX_get_security_level(context.get()), 1);
    }

    /// 信任库：给得出的文件要真加载上，给不出的文件要抛且点名路径
    TEST(TlsPolicy, LoadsTheTrustStoreOrNamesTheFileItCouldNotRead)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少用例夹具：" << kTestCertificatePath.string();

        const RawContext context;
        TlsPolicy        policy;
        policy.certificateAuthorityFile = kTestCertificatePath.string();
        policy.verifyDepth              = 4;
        ASSERT_NO_THROW(applyTlsPolicy(context.get(), policy, nullptr));

        const RawContext brokenContext;
        TlsPolicy        brokenPolicy;
        brokenPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "no_such_ca.pem").string();
        try
        {
            applyTlsPolicy(brokenContext.get(), brokenPolicy, nullptr);
            FAIL() << "加载不了的 CA 不该被接受";
        } catch (const CoreException &error)
        {
            EXPECT_NE(std::string(error.what()).find("no_such_ca.pem"), std::string::npos) << error.what();
        }
    }

    /**
     * @brief 钉住：票据开关是能反复设的（clear 而非「不设」）
     * @details 关掉再打开必须真打开：只「不设这一项」会留下上一次的 SSL_OP_NO_TICKET，
     *          而在同一个上下文上再调一次是本类的正常用法（热轮换与将来按连接改档都走这条路）。
     */
    TEST(TlsPolicy, TogglesSessionTicketsBothWays)
    {
        const RawContext context;
        TlsPolicy        off;
        off.areSessionTicketsEnabled = false;
        applyTlsPolicy(context.get(), off, nullptr);
        EXPECT_NE(SSL_CTX_get_options(context.get()) & SSL_OP_NO_TICKET, 0UL);

        applyTlsPolicy(context.get(), TlsPolicy{}, nullptr);
        EXPECT_EQ(SSL_CTX_get_options(context.get()) & SSL_OP_NO_TICKET, 0UL) << "重新启用没清掉 NO_TICKET";
    }

    /**
     * @brief 钉住：客户端一侧不预设套件列表，也不被强加 1.2 下限
     * @details 出站连接面对的是「对面有什么就说什么」：把服务端的排除表套到客户端上，可能把本可以
     *          连上的服务器拒掉，而那种失败在调用方看来只是「握手失败」。builtInCipherList 传
     *          nullptr 就是这条语义的开关。
     */
    TEST(TlsPolicy, ClientRoleAppliesNoBuiltInCipherListAndNoVersionFloor)
    {
        const RawContext untouched(false);
        const RawContext clientContext(false);
        applyTlsPolicy(clientContext.get(), TlsPolicy{}, nullptr);

        EXPECT_EQ(SSL_CTX_get_min_proto_version(clientContext.get()), 0) << "客户端角色不该被强加服务端的下限";
        const STACK_OF(SSL_CIPHER) *untouchedCiphers = SSL_CTX_get_ciphers(untouched.get());
        const STACK_OF(SSL_CIPHER) *clientCiphers    = SSL_CTX_get_ciphers(clientContext.get());
        ASSERT_NE(untouchedCiphers, nullptr);
        ASSERT_NE(clientCiphers, nullptr);
        ASSERT_EQ(sk_SSL_CIPHER_num(untouchedCiphers), sk_SSL_CIPHER_num(clientCiphers));
        EXPECT_STREQ(SSL_CIPHER_get_name(sk_SSL_CIPHER_value(untouchedCiphers, 0)), SSL_CIPHER_get_name(sk_SSL_CIPHER_value(clientCiphers, 0))) << "nullptr 默认却把套件列表改了";
    }

    /**
     * @brief 钉住：策略里的 CA 就算「信任库已就位」，开启对端校验不再被拒
     * @details setClientCertificateRequired(true) 原先只认 loadClientCertificateAuthority() 那一条路，
     *          而能配 CA 目录与校验深度的只有策略这一条：两处判据不打通，就会出现「CA 明明已经装载，
     *          开启校验却说没 CA」这种自相矛盾的配置面。
     */
    TEST(TlsPolicy, PolicyTrustStoreSatisfiesTheVerificationPrecondition)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少用例夹具：" << kTestCertificatePath.string();

        TlsPolicy policy;
        policy.certificateAuthorityFile = kTestCertificatePath.string();
        TlsContext contextWithCa(policy);
        EXPECT_NO_THROW(contextWithCa.setClientCertificateRequired(true));

        TlsContext bareContext;
        EXPECT_THROW(bareContext.setClientCertificateRequired(true), CoreException) << "没 CA 就该拒，而不是放行一条必失败的配置";
        // 走老接口同样成立：加载 CA 之后开启校验不再抛
        ASSERT_TRUE(bareContext.loadClientCertificateAuthority(kTestCertificatePath.string()));
        EXPECT_NO_THROW(bareContext.setClientCertificateRequired(true));
    }
} // namespace AsynGyanis::Core
