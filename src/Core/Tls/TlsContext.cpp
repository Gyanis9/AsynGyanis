#include "Core/Tls/TlsContext.h"
#include "Core/Exception/CoreException.h"

#include <openssl/ssl.h>

#include <csignal>
#include <cstring>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 服务端对外提供的 ALPN 协议名（注意：选择回调要的是裸协议名，不带长度前缀）
        constexpr unsigned char kHttp2ProtocolName[] = {'h', '2'};
        constexpr unsigned char kHttp11ProtocolName[] = {'h', 't', 't', 'p', '/', '1', '.', '1'};

        /**
         * @brief 一条 ALPN 偏好：本端支持的一个协议名
         */
        struct AlpnPreference
        {
            const unsigned char *protocolName;     ///< 协议名（裸名称，不带长度前缀）
            unsigned int protocolNameLength;       ///< 协议名字节数
        };

        /// 本端偏好顺序：h2 在前、http/1.1 在后（选择策略的唯一出处，新增协议按偏好插进这个表即可）
        constexpr AlpnPreference kAlpnPreferences[] = {
            {kHttp2ProtocolName, static_cast<unsigned int>(sizeof(kHttp2ProtocolName))},
            {kHttp11ProtocolName, static_cast<unsigned int>(sizeof(kHttp11ProtocolName))},
        };

        /// 显式排除弱算法：MD5/RC4/3DES/DES/导出级/匿名/PSK/SRP 一律不进服务端候选套件，
        /// 与安全等级 2 形成双保险（等级策略可能随发行版配置变化，这份列表不会）
        constexpr const char *kServerCipherList = "HIGH:!aNULL:!eNULL:!MD5:!RC4:!3DES:!DES:!EXPORT:!PSK:!SRP";

        /**
         * @brief ALPN 选择回调：在客户端提供的列表里按本端偏好选出协议名
         * @details 偏好顺序见 kAlpnPreferences（h2 优先、其次 http/1.1）。只能选客户端**提供过**的名字
         *          （RFC 7301 §3.2）：替对端选一个它没提过的名字会被严格的客户端直接拒绝。
         *          客户端没提供 ALPN 时返回 NOACK 让握手继续，不因为对端没提就拒绝连接。
         * @param outputProtocol 出参，被选中的协议名（裸名称，不带长度前缀）
         * @param outputLength 出参，协议名字节数
         * @param clientProtocols 客户端提供的协议列表（线上格式：长度字节 + 名称）
         * @param clientProtocolsLength 客户端列表总字节数
         * @return int SSL_TLSEXT_ERR_OK 选中了客户端提供过的协议名；SSL_TLSEXT_ERR_NOACK 未提供列表；
         *         SSL_TLSEXT_ERR_ALERT_FATAL 列表里没有本端支持的协议
         */
        int selectAlpnProtocol(SSL *, const unsigned char **outputProtocol, unsigned char *outputLength,
                               const unsigned char *clientProtocols, const unsigned int clientProtocolsLength, void *)
        {
            // 对端没提 ALPN：握手照常进行，双方都不使用 ALPN 协商结果
            if (clientProtocols == nullptr || clientProtocolsLength == 0)
            {
                return SSL_TLSEXT_ERR_NOACK;
            }

            // 外层按本端偏好、内层扫客户端列表：客户端列表是「单字节长度 + 协议名」的序列，
            // 同一条协议提几次都不改变它是否被支持，因此偏好顺序完全由本端这张表决定
            for (const AlpnPreference &preference: kAlpnPreferences)
            {
                unsigned int offset = 0;
                while (offset < clientProtocolsLength)
                {
                    const unsigned int protocolLength = clientProtocols[offset];
                    ++offset;

                    // 长度前缀为 0 或越出列表尾部都说明列表被截断，按「没有可选项」处理
                    if (protocolLength == 0 || offset + protocolLength > clientProtocolsLength)
                    {
                        break;
                    }

                    if (protocolLength == preference.protocolNameLength &&
                        std::memcmp(clientProtocols + offset, preference.protocolName, protocolLength) == 0)
                    {
                        *outputProtocol = preference.protocolName;
                        *outputLength = static_cast<unsigned char>(protocolLength);
                        return SSL_TLSEXT_ERR_OK;
                    }
                    offset += protocolLength;
                }
            }

            // 一条都不匹配（例如只提 http/1.0）：明确回 no_application_protocol，
            // 好过替对端选一个它根本没提供过的协议名
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
    } // namespace

    TlsContext::TlsContext()
    {
#ifndef _WIN32
        // OpenSSL 内部的 read()/write() 绕不开 MSG_NOSIGNAL：向已关闭的对端写数据
        // （握手失败发 alert、SSL_shutdown 发 close_notify）在 Linux 上会触发 SIGPIPE，
        // 默认动作是直接打死整个进程。忽略该信号后，写失败改以 EPIPE 错误码返回，
        // 交给既有错误路径处理——这也是服务器程序的标准做法（nginx、libuv 同样忽略）。
        // 该设置是进程级且幂等的：重复构造 TlsContext 没有额外影响
        std::signal(SIGPIPE, SIG_IGN);
#endif

        m_context = createHardenedContext();
    }

    SSL_CTX *TlsContext::createHardenedContext()
    {
        SSL_CTX *context = SSL_CTX_new(TLS_server_method());
        if (!context)
        {
            // SSL_CTX_new 只在内存不足或 OpenSSL 未被正确初始化时才返回空：
            // 这是不可恢复的启动期故障，因此直接抛出让调用方尽早失败
            throw CoreException("创建 TLS 上下文失败：SSL_CTX_new 返回空"
                                "（通常是内存不足，或 OpenSSL 库未正确初始化）");
        }

        // 最低协议限定 TLS 1.2：RFC 8996 已把 TLS 1.0/1.1 列为废弃，
        // 两者仍有已知攻击面（BEAST 等）与过时的算法组合，服务端不再接受
        if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 0)
        {
            // 失败路径先释放刚建的句柄再抛，否则漏掉一个 SSL_CTX。热轮换路径同样依赖这一点：
            // 由调用方决定是抛给上层还是退化成「这次轮换没成」
            SSL_CTX_free(context);
            throw CoreException("创建 TLS 上下文失败：无法把最低协议版本设为 TLS 1.2"
                                "（OpenSSL 可能未启用该版本，请检查库的编译配置）");
        }

        // 关闭压缩：压缩会引入 CRIME 侧信道，服务端一律不协商压缩
        SSL_CTX_set_options(context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);

        // 安全等级 2：拒绝 1024 位以下 RSA/DH 与 SHA-1 签名；本仓库夹具证书是
        // 2048 位 RSA + SHA-256，实测可在等级 2 下完成握手，故不降到等级 1
        SSL_CTX_set_security_level(context, 2);

        // 安全等级只管强度阈值，弱算法类别另由套件列表显式排除
        if (SSL_CTX_set_cipher_list(context, kServerCipherList) == 0)
        {
            SSL_CTX_free(context);
            throw CoreException("创建 TLS 上下文失败：套件列表 \"" + std::string(kServerCipherList) +
                                "\" 没有匹配到任何可用套件（OpenSSL 可能被编译成不含高强度算法，请检查库的编译配置）");
        }

        // 注册 ALPN 选择回调：选择策略见 selectAlpnProtocol()
        SSL_CTX_set_alpn_select_cb(context, selectAlpnProtocol, nullptr);

        SSL_CTX_set_mode(context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        return context;
    }

    TlsContext::~TlsContext()
    {
        if (m_context)
        {
            SSL_CTX_free(m_context);
            m_context = nullptr;
        }
    }

    bool TlsContext::installCertificate(SSL_CTX *context, const std::string &certificateFile, const std::string &keyFile)
    {
        if (SSL_CTX_use_certificate_file(context, certificateFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }

        if (SSL_CTX_use_PrivateKey_file(context, keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }

        if (SSL_CTX_check_private_key(context) != 1)
        {
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

    bool TlsContext::reloadCertificate()
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        // 还没加载过证书就谈不上轮换。这里不抛：其它加载接口同样用 false 表达「这次没成」，
        // 调用方（运维脚本）拿到 false 就继续用旧证书服务，比当场抛异常更符合轮换的语义
        if (m_certificateFile.empty() || m_keyFile.empty())
        {
            return false;
        }

        // 先清空错误栈：返回 false 时调用方读到的原因必须是本次留下的
        ERR_clear_error();

        // 关键顺序：**先把整台新上下文配置好，成功之后才换**。任何一步失败都直接返回 false，
        // 旧上下文一个字节都不动——轮换失败不能让正在服务的进程掉线
        SSL_CTX *newContext = nullptr;
        try
        {
            newContext = createHardenedContext();
        } catch (const CoreException &)
        {
            // 加固项在当前 OpenSSL 上无法生效：同上，本次轮换失败，旧证书继续服务
            return false;
        }

        if (!installCertificate(newContext, m_certificateFile, m_keyFile))
        {
            SSL_CTX_free(newContext);
            return false;
        }

        // 复现已加载的 mTLS 状态：漏掉这一步会让一次续期把对端证书校验悄悄关掉，
        // 那是「续期看起来成功、安全性反而降级」的典型形态
        if (!m_clientCertificateAuthorityFile.empty())
        {
            if (SSL_CTX_load_verify_locations(newContext, m_clientCertificateAuthorityFile.c_str(), nullptr) != 1)
            {
                SSL_CTX_free(newContext);
                return false;
            }
            if (m_clientCertificateRequired)
            {
                SSL_CTX_set_verify(newContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            }
        }

        SSL_CTX *previousContext = m_context;
        m_context                = newContext;

        // 旧上下文在这里只是引用计数减一：已建立的连接各自的 SSL 对象还持有它，
        // 因此握手中的连接与已通连的续传照旧走旧证书，最后一个引用消失时才真正释放
        SSL_CTX_free(previousContext);
        return true;
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
        m_clientCertificateAuthorityFile   = caFile;
        return true;
    }

    void TlsContext::setClientCertificateRequired(const bool required) const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        if (!required)
        {
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

        // FAIL_IF_NO_PEER_CERT：对端不出示证书时立即终止握手，而不是退化成「可选校验」
        SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        m_clientCertificateRequired = true;
    }

    SSL *TlsContext::createSSL(const int fileDescriptor) const
    {
        // 与 reloadCertificate() 互斥：SSL_new 会给上下文加一次引用，加引用之后本次换代就不会
        // 把它释放掉，因此后面的握手用的一定是本函数取到的这一代证书
        std::lock_guard<std::mutex> guard(m_contextMutex);

        SSL *ssl = SSL_new(m_context);
        if (!ssl)
        {
            throw CoreException("创建 TLS 会话失败：SSL_new 返回空（上下文无效或内存不足）");
        }
        if (SSL_set_fd(ssl, fileDescriptor) == 0)
        {
            // 绑定失败时先释放刚创建的会话再抛出：否则这次失败的调用会漏掉一个 SSL 对象
            SSL_free(ssl);
            throw CoreException("把套接字绑定到 TLS 会话失败：SSL_set_fd 返回失败"
                                "（文件描述符可能已关闭或不是套接字）");
        }
        return ssl;
    }

    SSL_CTX *TlsContext::nativeHandle() const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);
        return m_context;
    }

}
