#include "Core/Tls/TlsPolicy.h"

#include "Core/Exception/CoreException.h"

namespace AsynGyanis::Core
{
    namespace
    {
        /// 把策略里的版本档位翻译成 OpenSSL 的常量
        [[nodiscard]] int toOpenSslVersion(const TlsPolicy::ProtocolVersion version) noexcept
        {
            return version == TlsPolicy::ProtocolVersion::Tls1_3 ? TLS1_3_VERSION : TLS1_2_VERSION;
        }

        /// 版本档位写成中文，供报错文案指认是哪一项（枚举值本身对使用者没有意义）
        [[nodiscard]] const char *protocolVersionName(const TlsPolicy::ProtocolVersion version) noexcept
        {
            return version == TlsPolicy::ProtocolVersion::Tls1_3 ? "TLS 1.3" : "TLS 1.2";
        }
    } // namespace

    void applyTlsPolicy(SSL_CTX *const context, const TlsPolicy &policy, const char *const builtInCipherList)
    {
        if (context == nullptr)
        {
            throw CoreException("施加 TLS 策略失败：目标上下文为空");
        }

        // 这两项在 OpenSSL 那边是「设了不报错、也没效果」的写法，只能在本层拦住：负的安全等级会被
        // 按无符号数解释，0 跳校验深度等于关掉链深度保护，两者都不会给出任何信号
        if (policy.securityLevel < 0)
        {
            throw CoreException("TLS 策略无效：安全等级不能为负（当前 " + std::to_string(policy.securityLevel)
                                + "）；要放宽请显式给 0 或 1");
        }
        if (policy.verifyDepth.has_value() && *policy.verifyDepth <= 0)
        {
            throw CoreException("TLS 策略无效：证书链校验深度必须是正数（当前 " + std::to_string(*policy.verifyDepth) + "）");
        }

        // 安全等级要先落地：它连带收窄可选套件的集合，之后再设版本与套件才不会出现「策略合法但
        // 与等级冲突」那种误导性报错
        SSL_CTX_set_security_level(context, policy.securityLevel);

        // 版本区间：只给下限时上限交给 OpenSSL，反之亦然。写反了由这里先给一句人话——
        // OpenSSL 只会回「operation not supported」，看不出是自己把区间填倒了
        if (policy.minimumProtocolVersion.has_value() && policy.maximumProtocolVersion.has_value()
            && *policy.minimumProtocolVersion > *policy.maximumProtocolVersion)
        {
            throw CoreException(std::string("TLS 策略无效：最低版本 ") + protocolVersionName(*policy.minimumProtocolVersion)
                                + " 高于最高版本 " + protocolVersionName(*policy.maximumProtocolVersion));
        }
        if (policy.minimumProtocolVersion.has_value()
            && SSL_CTX_set_min_proto_version(context, toOpenSslVersion(*policy.minimumProtocolVersion)) == 0)
        {
            throw CoreException(std::string("施加 TLS 策略失败：无法把最低协议版本设为 ")
                                + protocolVersionName(*policy.minimumProtocolVersion)
                                + "（OpenSSL 可能未启用该版本，请检查库的编译配置）");
        }
        if (policy.maximumProtocolVersion.has_value()
            && SSL_CTX_set_max_proto_version(context, toOpenSslVersion(*policy.maximumProtocolVersion)) == 0)
        {
            throw CoreException(std::string("施加 TLS 策略失败：无法把最高协议版本设为 ")
                                + protocolVersionName(*policy.maximumProtocolVersion)
                                + "（OpenSSL 可能未启用该版本，请检查库的编译配置）");
        }

        // 套件串不做预先解析：什么算合法由 OpenSSL 说了算（它随发行版与编译配置变化），
        // 自己先判一遍只会把它支持的写法误判成非法。失败时把原文带回报错，调用方一眼看出是哪一项
        if (!policy.cipherList.empty() || builtInCipherList != nullptr)
        {
            const std::string cipherList = policy.cipherList.empty() ? std::string(builtInCipherList) : policy.cipherList;
            if (SSL_CTX_set_cipher_list(context, cipherList.c_str()) == 0)
            {
                throw CoreException("施加 TLS 策略失败：TLS 1.2 及以下的套件列表 \"" + cipherList
                                    + "\" 没有匹配到任何可用套件（可能被当前安全等级或库的编译选项排除）");
            }
        }
        if (!policy.tls13CipherSuites.empty() && SSL_CTX_set_ciphersuites(context, policy.tls13CipherSuites.c_str()) == 0)
        {
            throw CoreException("施加 TLS 策略失败：TLS 1.3 套件列表 \"" + policy.tls13CipherSuites
                                + "\" 没有匹配到任何可用套件");
        }
        if (!policy.supportedGroups.empty() && SSL_CTX_set1_curves_list(context, policy.supportedGroups.c_str()) == 0)
        {
            throw CoreException("施加 TLS 策略失败：命名曲线/组列表 \"" + policy.supportedGroups
                                + "\" 无效（要用冒号分隔，且名字得是当前 OpenSSL 认得的）");
        }

        // 信任库：文件与目录可以同时给，OpenSSL 两处都查。只给一项时另一项传 nullptr，
        // 这不是「不加载」而是「按系统默认」的起点——调用方设了本项就是接管信任库，
        // 与客户端那句 SSL_CTX_set_default_verify_paths 的关系由调用方决定（见 HttpClient）
        if (!policy.certificateAuthorityFile.empty() || !policy.certificateAuthorityPath.empty())
        {
            const char *const caFile = policy.certificateAuthorityFile.empty() ? nullptr : policy.certificateAuthorityFile.c_str();
            const char *const caPath = policy.certificateAuthorityPath.empty() ? nullptr : policy.certificateAuthorityPath.c_str();
            if (SSL_CTX_load_verify_locations(context, caFile, caPath) != 1)
            {
                throw CoreException("施加 TLS 策略失败：CA 信任库加载不了（文件=\"" + policy.certificateAuthorityFile
                                    + "\"，目录=\"" + policy.certificateAuthorityPath + "\"）");
            }
        }
        if (policy.verifyDepth.has_value())
        {
            SSL_CTX_set_verify_depth(context, *policy.verifyDepth);
        }

        // 票据开关：SSL_OP_NO_TICKET 对 TLS 1.2 与 1.3 都管用（1.3 的 NewSessionTicket 一并停掉）。
        // 反过来要显式 clear：本函数可能被复用的上下文再调一次，只「不设」会留下上一次的 NO_TICKET
        if (policy.areSessionTicketsEnabled)
        {
            SSL_CTX_clear_options(context, SSL_OP_NO_TICKET);
        }
        else
        {
            SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
        }
    }
}
