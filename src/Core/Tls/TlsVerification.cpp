#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsVerification.h"

#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 判断字符串是否是 64 位小写十六进制的 SHA-256 指纹（名单格式的单一判据）
         * @param text 待判断的文本
         * @return true 形态合法
         */
        [[nodiscard]] bool isSha256HexFingerprint(const std::string_view text) noexcept
        {
            if (text.size() != 64)
            {
                return false;
            }
            return std::ranges::all_of(text, [](const char character) noexcept { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
        }
    } // namespace

    namespace
    {
        /**
         * @brief 取 OpenSSL 错误栈里最靠外那条的原因文本
         * @details 出口只有 bool 的场合，这是唯一能把「读不到文件 / 不是合法 PEM / 密钥不配对」
         *          分开的途径；错误栈空时如实说明，免得给出一条看不出名目的空文本
         * @return std::string 形如 "error:02001003:system library:fopen:Permission denied"，或空栈说明
         */
        [[nodiscard]] std::string openSslReasonText()
        {
            const unsigned long queueEntry = ERR_get_error();
            if (queueEntry == 0)
            {
                return "（错误栈为空，调用方未记下原因）";
            }
            char buffer[256];
            ERR_error_string_n(queueEntry, buffer, sizeof(buffer));
            return std::string(buffer);
        }
    } // namespace


    bool TlsContext::installCertificate(SSL_CTX *context, const std::string &certificateFile, const std::string &keyFile)
    {
        // 先清空错误栈：头文件承诺「失败可由 OpenSSL 错误栈取到原因」，而调用方通常只读第一条。
        // 不清的话，前一次失败留下的条目还压在队首，日志就会把「私钥文件不存在」报成上次的
        // 「证书文件不存在」——同一份文案指向错的那个文件
        ERR_clear_error();

        // 三步各自出声：本函数原先三处都只 `return false`，而这三件事对运维完全不同——
        // 「文件读不到」「不是合法 PEM」「证书与私钥不配对」的处置手法不一样，而最常见的正是第三种。
        // 出口是个 bool，OpenSSL 错误栈又只在同线程的下一次调用前有效，因此在这里当场抄回
        // 按「链文件」而非「单证书文件」加载：首张证书作本机证书，其余逐张进链并随握手一并出示。
        // use_certificate_file 只读第一张，链上的中间 CA 会被静默丢掉 —— 部署里全链证书
        // （fullchain.pem）是常态，缺链时对端只信任根 CA 就无法把证书串到根，握手直接失败
        if (SSL_CTX_use_certificate_chain_file(context, certificateFile.c_str()) != 1)
        {
            LOG_ERROR_FMT("TlsContext: 证书链装载失败，文件「{}」。OpenSSL 报的是：{}", certificateFile, openSslReasonText());
            return false;
        }

        if (SSL_CTX_use_PrivateKey_file(context, keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            LOG_ERROR_FMT("TlsContext: 私钥装载失败，文件「{}」（证书那张已经读进来了）。OpenSSL 报的是：{}", keyFile, openSslReasonText());
            return false;
        }

        if (SSL_CTX_check_private_key(context) != 1)
        {
            LOG_ERROR_FMT("TlsContext: 证书与私钥不配对——证书来自「{}」而私钥来自「{}」。OpenSSL 报的是：{}", certificateFile, keyFile, openSslReasonText());
            return false;
        }
        return true;
    }

    bool TlsContext::loadCertificate(const std::string &certificateFile, const std::string &keyFile) const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        if (!installCertificate(m_context, certificateFile, keyFile))
        {
            return false;
        }

        // 记住路径：热轮换按「新证书覆盖到原路径」的约定工作，没有这份记录就无从找起
        m_certificateFile = certificateFile;
        m_keyFile         = keyFile;
        return true;
    }

    int TlsContext::peerFingerprintRegistryIndex()
    {
        // 与 SNI 登记同一条理由：登记的生死跟着 SSL_CTX 走，回调在握手线程上才不会碰到已析构的 TlsContext
        static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, [](void *, void *pointer, CRYPTO_EX_DATA *, int, long, void *)
                                                          { delete static_cast<PeerFingerprintRegistry *>(pointer); });
        return index;
    }

    TlsContext::PeerFingerprintRegistry *TlsContext::peerFingerprintRegistryOf(SSL_CTX *context)
    {
        if (context == nullptr)
        {
            return nullptr;
        }
        const int index = peerFingerprintRegistryIndex();
        if (index < 0)
        {
            return nullptr;
        }
        return static_cast<PeerFingerprintRegistry *>(SSL_CTX_get_ex_data(context, index));
    }

    TlsContext::PeerFingerprintRegistry *TlsContext::ensurePeerFingerprintRegistry(SSL_CTX *context)
    {
        if (PeerFingerprintRegistry *existing = peerFingerprintRegistryOf(context); existing != nullptr)
        {
            return existing;
        }

        const int index = peerFingerprintRegistryIndex();
        if (index < 0)
        {
            return nullptr;
        }

        auto *registry = new PeerFingerprintRegistry();
        if (SSL_CTX_set_ex_data(context, index, registry) != 1)
        {
            delete registry;
            return nullptr;
        }
        return registry;
    }


    bool TlsContext::loadClientCertificateAuthority(const std::string &caFile) const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        // 先清空错误栈：返回 false 时调用方读到的原因必须是本次加载留下的，而不是上一次的残留
        ERR_clear_error();

        if (SSL_CTX_load_verify_locations(m_context, caFile.c_str(), nullptr) != 1)
        {
            // 加载失败不置位：此后 setClientCertificateRequired(true) 仍必须拒绝，
            // 绝不出现「要求校验却没有 CA」这种配置
            return false;
        }

        // CA 就绪才允许开启对端校验，这个标志就是 setClientCertificateRequired() 的判据
        m_clientCertificateAuthorityLoaded = true;
        // 路径记进策略而不是另开一个成员：换代时的复现只有一条路（createHardenedContext 里
        // 按策略施加），两处各记一份迟早会有一份忘了被复现
        m_policy.certificateAuthorityFile = caFile;
        return true;
    }

    void TlsContext::setClientCertificateRequired(const bool required) const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        if (!required)
        {
            // 已设指纹名单时不允许关闭：关掉校验，名单就成了一份永远不会执行的判据——
            // 那种「配置看着还在、判据已经没了」的状态比拒绝更坏，因此在这里当场说不
            if (!m_policy.allowedPeerSha256Fingerprints.empty())
            {
                throw CoreException("关闭客户端证书校验失败：本上下文已设置对端指纹名单，关掉校验会让名单变成一份永远不会执行的判据。请先调用 setPeerFingerprintAllowlist({}) "
                                    "清空名单，再关闭校验");
            }
            // 关闭校验：回到不要求、也不校验对端证书的默认模式，此时 CA 有没有加载都无所谓
            SSL_CTX_set_verify(m_context, SSL_VERIFY_NONE, nullptr);
            m_clientCertificateRequired = false;
            return;
        }

        // 要求校验却没有 CA：调用方用错了顺序，直接拒绝而不是放行一条必失败的配置
        if (!m_clientCertificateAuthorityLoaded)
        {
            throw CoreException("启用客户端证书校验失败：尚未加载用于校验对端证书的 CA"
                                "（请先用 loadClientCertificateAuthority() 加载 CA 文件，"
                                "或改用 setClientCertificateRequired(false) 关闭该校验）");
        }

        // FAIL_IF_NO_PEER_CERT：对端不出示证书时立即终止握手，而不是退化成「可选校验」；
        // 指纹名单（若已设）由同一处 helper 挂上，两条路径不会分叉
        applyPeerVerification(m_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
        m_clientCertificateRequired = true;
    }

    void TlsContext::enableClientPeerVerification() const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        // 出站一侧恒要校验：不验对端等于任何受信 CA 给他域签的证书都能冒充目标主机；
        // 指纹名单（若已设）由同一处 helper 挂上
        applyPeerVerification(m_context, SSL_VERIFY_PEER);
    }

    void TlsContext::applyPeerVerification(SSL_CTX *const context, const int mode) const
    {
        // 客户端校验的信任锚：策略没接管信任库时补系统那一份。轮换与各个 setter 都经这里，
        // 因此新上下文不会出现「校验模式在、信任锚空」的状态
        if (m_role == Role::Client && m_policy.certificateAuthorityFile.empty() && m_policy.certificateAuthorityPath.empty())
        {
            static_cast<void>(SSL_CTX_set_default_verify_paths(context));
        }
        // 名单搬进 SSL_CTX 的共享登记：回调只碰登记、不解引用宿主对象（回调在握手线程上跑，
        // 宿主可能已析构），替换持独占锁——与 VirtualHostRegistry 同一条理由、同一个形状
        if (PeerFingerprintRegistry *const registry = ensurePeerFingerprintRegistry(context); registry != nullptr)
        {
            const std::lock_guard<std::shared_mutex> guard(registry->mutex);
            registry->fingerprints = m_policy.allowedPeerSha256Fingerprints;
        }
        SSL_CTX_set_verify(context, mode, m_policy.allowedPeerSha256Fingerprints.empty() ? nullptr : &TlsContext::verifyPeerAllowlistCallback);
    }

    int TlsContext::verifyPeerAllowlistCallback(const int preverifyOK, X509_STORE_CTX *const storeContext) noexcept
    {
        // 默认链校验的结论先算数：名单是「在链验通之上再点名」，不是替代
        if (preverifyOK == 0)
        {
            return 0;
        }
        // 只对叶子证书判名单：链上中间 CA 在不在名单里不是调用方要表达的事
        if (X509_STORE_CTX_get_error_depth(storeContext) != 0)
        {
            return 1;
        }

        // 取回本上下文挂的共享登记：取不到就拒绝（fail-closed）——宁可不放行，也不静默退回只看链
        SSL *const                           ssl      = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(storeContext, SSL_get_ex_data_X509_STORE_CTX_idx()));
        X509 *const                          leaf     = X509_STORE_CTX_get_current_cert(storeContext);
        const PeerFingerprintRegistry *const registry = ssl != nullptr ? peerFingerprintRegistryOf(SSL_get_SSL_CTX(ssl)) : nullptr;
        if (registry == nullptr || leaf == nullptr)
        {
            return 0;
        }

        unsigned char digest[EVP_MAX_MD_SIZE] = {};
        unsigned int  digestLength            = 0;
        if (X509_digest(leaf, EVP_sha256(), digest, &digestLength) != 1 || digestLength != 32U)
        {
            return 0;
        }
        std::string fingerprint;
        fingerprint.reserve(64);
        constexpr char kHexDigits[] = "0123456789abcdef";
        for (unsigned int index = 0; index < digestLength; ++index)
        {
            fingerprint.push_back(kHexDigits[digest[index] >> 4U]);
            fingerprint.push_back(kHexDigits[digest[index] & 0x0FU]);
        }
        // 读持共享锁：setter 在配置线程上持独占锁整体替换，两面不再裸读同一份 vector
        const std::shared_lock<std::shared_mutex> guard(registry->mutex);
        return std::ranges::find(registry->fingerprints, fingerprint) != registry->fingerprints.end() ? 1 : 0;
    }

    void TlsContext::setPeerFingerprintAllowlist(std::vector<std::string> sha256HexFingerprints) const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        for (const std::string &fingerprint: sha256HexFingerprints)
        {
            if (!isSha256HexFingerprint(fingerprint))
            {
                throw CoreException("设置对端证书指纹名单失败：「" + fingerprint +
                                    "」不是 64 位小写十六进制的 SHA-256 指纹"
                                    "（可用 `openssl x509 -noout -fingerprint -sha256` 取得）");
            }
        }

        // 服务端角色：名单挂在「要求并校验客户端证书」上，校验没开时设名单＝埋一份不会执行的判据；
        // 顺序判据与 setClientCertificateRequired(true) 缺 CA 那条同一口径：用错了顺序当场说、不留半成品
        if (m_role == Role::Server && !m_clientCertificateRequired && !sha256HexFingerprints.empty())
        {
            throw CoreException("设置对端证书指纹名单失败：服务端尚未要求客户端证书（请先调用 setClientCertificateRequired(true)，"
                                "并要求此前成功加载过 CA）；名单挂在证书校验上，校验没开它一次都不会执行");
        }

        m_policy.allowedPeerSha256Fingerprints = std::move(sha256HexFingerprints);

        // 立即作用到当前上下文（下一次握手就按新名单判）：只对已启用的校验模式重挂——
        // 客户端角色在 enableClientPeerVerification() 那一刻随策略装上，此处不替它提前开启校验。
        // 换代由 reloadCertificate() 按策略自动复现，此处的即时生效只管当前上下文
        if (const int currentMode = SSL_CTX_get_verify_mode(m_context); currentMode != SSL_VERIFY_NONE)
        {
            applyPeerVerification(m_context, currentMode);
        }

        // 站点上下文各有一份登记；切换后回调读的是站点那份，故这里一并刷新
        if (VirtualHostRegistry *hostRegistry = registryOf(m_context); hostRegistry != nullptr)
        {
            const std::shared_lock<std::shared_mutex> hostGuard(hostRegistry->mutex);
            for (const auto &entry: hostRegistry->contexts)
            {
                if (PeerFingerprintRegistry *registry = peerFingerprintRegistryOf(entry.second.get()); registry != nullptr)
                {
                    const std::lock_guard<std::shared_mutex> registryGuard(registry->mutex);
                    registry->fingerprints = m_policy.allowedPeerSha256Fingerprints;
                }
            }
        }
    }

    namespace
    {
        /// OCSP_RESPONSE 的 RAII 释放器
        struct OcspResponseDeleter
        {
            void operator()(OCSP_RESPONSE *response) const noexcept
            {
                OCSP_RESPONSE_free(response);
            }
        };

        /// OCSP_BASICRESP 的 RAII 释放器
        struct OcspBasicResponseDeleter
        {
            void operator()(OCSP_BASICRESP *basic) const noexcept
            {
                OCSP_BASICRESP_free(basic);
            }
        };

        /// OCSP_CERTID 的 RAII 释放器
        struct OcspCertificateIdDeleter
        {
            void operator()(OCSP_CERTID *certificateId) const noexcept
            {
                OCSP_CERTID_free(certificateId);
            }
        };

        /**
         * @brief 取走 OpenSSL 错误栈里最靠外的一条并清空队列
         * @return std::string 错误原文；队列为空时返回空串
         */
        [[nodiscard]] std::string takeOpenSslDiagnosticText()
        {
            std::string detail;
            if (const unsigned long queueEntry = ERR_get_error(); queueEntry != 0)
            {
                char buffer[256];
                ERR_error_string_n(queueEntry, buffer, sizeof(buffer));
                detail = buffer;
            }
            ERR_clear_error();
            return detail;
        }

        /**
         * @brief 以统一的「校验对端装订的 OCSP 响应失败」前缀抛出，并清空 OpenSSL 错误队列
         * @param reason 具体是哪一项没通过
         */
        [[noreturn]] void throwOcspVerificationFailure(const std::string &reason)
        {
            const std::string detail = takeOpenSslDiagnosticText();
            if (detail.empty())
            {
                throw CoreException("校验对端装订的 OCSP 响应失败：" + reason);
            }
            throw CoreException("校验对端装订的 OCSP 响应失败：" + reason + "（OpenSSL 报：" + detail + "）");
        }

        /**
         * @brief 把叶证书的 OCSP 状态写成报错文案
         * @param status V_OCSP_CERTSTATUS_* 取值
         * @return const char* 状态名；无法识别时给出说明
         */
        [[nodiscard]] const char *certificateStatusName(const int status) noexcept
        {
            switch (status)
            {
                case V_OCSP_CERTSTATUS_GOOD:
                    return "GOOD";
                case V_OCSP_CERTSTATUS_REVOKED:
                    return "REVOKED";
                case V_OCSP_CERTSTATUS_UNKNOWN:
                    return "UNKNOWN";
                default:
                    return "无法识别的状态";
            }
        }
    } // namespace


    void verifyStapledOcspResponse(SSL *const ssl)
    {
        if (ssl == nullptr)
        {
            ERR_clear_error();
            throw CoreException("校验对端装订的 OCSP 响应失败：TLS 会话为空");
        }

        // 先清空错误栈：失败文案里的 OpenSSL 原文必须来自本次校验，而不是更早的调用
        ERR_clear_error();

        unsigned char *responseBytes  = nullptr;
        const long     responseLength = SSL_get_tlsext_status_ocsp_resp(ssl, &responseBytes);
        if (responseLength <= 0 || responseBytes == nullptr)
        {
            // 服务端没有装订（或本端没有请求）：不强制，握手照常完成
            ERR_clear_error();
            return;
        }

        const unsigned char                                      *cursor = responseBytes;
        const std::unique_ptr<OCSP_RESPONSE, OcspResponseDeleter> response(d2i_OCSP_RESPONSE(nullptr, &cursor, responseLength));
        if (response == nullptr)
        {
            throwOcspVerificationFailure("装订内容不是合法的 DER 编码");
        }

        const int responseStatus = OCSP_response_status(response.get());
        if (responseStatus != OCSP_RESPONSE_STATUS_SUCCESSFUL)
        {
            throwOcspVerificationFailure("响应状态不是 successful（当前 " + std::to_string(responseStatus) + "）");
        }

        const std::unique_ptr<OCSP_BASICRESP, OcspBasicResponseDeleter> basic(OCSP_response_get1_basic(response.get()));
        if (basic == nullptr)
        {
            throwOcspVerificationFailure("响应里没有 basicResponse");
        }

        // 签名校验与 CertID 匹配都要用到握手时已经验过的链：叶证书在第 0 位，签发者在第 1 位
        STACK_OF(X509) *const verifiedChain = SSL_get0_verified_chain(ssl);
        if (verifiedChain == nullptr || sk_X509_num(verifiedChain) < 2)
        {
            throwOcspVerificationFailure("拿不到已校验的证书链（客户端未开启对端证书校验，或链不完整）");
        }
        X509 *const leaf   = sk_X509_value(verifiedChain, 0);
        X509 *const issuer = sk_X509_value(verifiedChain, 1);

        SSL_CTX *const    sslContext = SSL_get_SSL_CTX(ssl);
        X509_STORE *const store      = sslContext != nullptr ? SSL_CTX_get_cert_store(sslContext) : nullptr;
        if (store == nullptr)
        {
            throwOcspVerificationFailure("拿不到上下文的证书存储");
        }
        if (OCSP_basic_verify(basic.get(), verifiedChain, store, 0) != 1)
        {
            throwOcspVerificationFailure("响应的签名链校验不通过");
        }

        const std::unique_ptr<OCSP_CERTID, OcspCertificateIdDeleter> certificateId(OCSP_cert_to_id(nullptr, leaf, issuer));
        if (certificateId == nullptr)
        {
            throwOcspVerificationFailure("无法为叶证书构造 CertID");
        }

        int                   certificateStatus = V_OCSP_CERTSTATUS_UNKNOWN;
        int                   revocationReason  = 0;
        ASN1_GENERALIZEDTIME *revocationTime    = nullptr;
        ASN1_GENERALIZEDTIME *thisUpdate        = nullptr;
        ASN1_GENERALIZEDTIME *nextUpdate        = nullptr;
        if (OCSP_resp_find_status(basic.get(), certificateId.get(), &certificateStatus, &revocationReason, &revocationTime, &thisUpdate, &nextUpdate) != 1)
        {
            throwOcspVerificationFailure("响应里没有当前叶证书的状态");
        }
        if (OCSP_check_validity(thisUpdate, nextUpdate, 300, -1) != 1)
        {
            throwOcspVerificationFailure("响应的有效期不合法或已过期");
        }
        if (certificateStatus != V_OCSP_CERTSTATUS_GOOD)
        {
            throwOcspVerificationFailure(std::string("叶证书状态不是 GOOD（当前 ") + certificateStatusName(certificateStatus) + "）");
        }

        // 校验通过也不把中间尝试留下的错误条目带去后续调用
        ERR_clear_error();
    }
} // namespace AsynGyanis::Core
