#include "Core/Tls/TlsContext.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"
#include "Core/Tls/SessionTicketKeyRing.h"

#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 服务端对外提供的 ALPN 协议名（注意：选择回调要的是裸协议名，不带长度前缀）
        constexpr unsigned char kHttp2ProtocolName[]  = {'h', '2'};
        constexpr unsigned char kHttp11ProtocolName[] = {'h', 't', 't', 'p', '/', '1', '.', '1'};

        /**
         * @brief 一条 ALPN 偏好：本端支持的一个协议名
         */
        struct AlpnPreference
        {
            const unsigned char *protocolName;       ///< 协议名（裸名称，不带长度前缀）
            unsigned int         protocolNameLength; ///< 协议名字节数
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
         * @param outputProtocol 出参，被选中的协议名（裸名称，不带长度前缀）
         * @param outputLength 出参，协议名字节数
         * @param clientProtocols 客户端提供的协议列表（线上格式：长度字节 + 名称）
         * @param clientProtocolsLength 客户端列表总字节数
         * @return SSL_TLSEXT_ERR_OK 选中；SSL_TLSEXT_ERR_NOACK 客户端未提供列表；
         *         SSL_TLSEXT_ERR_ALERT_FATAL 列表里没有本端支持的协议
         * @note 只能选客户端**提供过**的名字（RFC 7301 §3.2）：替对端选一个它没提过的名字会被
         *       严格的客户端直接拒绝；偏好顺序见 kAlpnPreferences（h2 优先）
         */
        int selectAlpnProtocol(SSL *, const unsigned char **outputProtocol, unsigned char *outputLength, const unsigned char *clientProtocols,
                               const unsigned int clientProtocolsLength, void *)
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

                    if (protocolLength == preference.protocolNameLength && std::memcmp(clientProtocols + offset, preference.protocolName, protocolLength) == 0)
                    {
                        *outputProtocol = preference.protocolName;
                        *outputLength   = static_cast<unsigned char>(protocolLength);
                        return SSL_TLSEXT_ERR_OK;
                    }
                    offset += protocolLength;
                }
            }

            // 一条都不匹配（例如只提 http/1.0）：明确回 no_application_protocol，
            // 好过替对端选一个它根本没提供过的协议名
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }

        /**
         * @brief 按上下文存放的 OCSP 装订数据
         * @details 用 SSL_CTX 的 ex_data 而不是 TlsContext 成员存放：回调在握手线程上运行，
         *          解引用一个可能已析构的 TlsContext 会悬垂；持有者的生死跟随 SSL_CTX 本身。
         *          bytes 用原子 shared_ptr 快照，使 loadOcspResponse() 可在服务运行中安全替换。
         */
        struct StapledOcspResponse
        {
            std::atomic<std::shared_ptr<const std::string>> bytes{std::shared_ptr<const std::string>{}};
        };

        /**
         * @brief 取装订数据的 ex_data 下标（首次调用时注册，释放回调负责 delete 持有者）
         * @return int 下标；注册失败返回 -1，调用方据此跳过装订能力
         */
        int stapledResponseExDataIndex()
        {
            static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, [](void *, void *pointer, CRYPTO_EX_DATA *, int, long, void *)
                                                              { delete static_cast<StapledOcspResponse *>(pointer); });
            return index;
        }

        /**
         * @brief 读取整个文件为字节串
         * @param filePath 文件路径（二进制读取）
         * @param output 出参，文件内容
         * @return true 读取成功且文件非空
         */
        bool readFileBytes(const std::string &filePath, std::string &output)
        {
            std::ifstream stream(filePath, std::ios::in | std::ios::binary);
            if (!stream)
            {
                return false;
            }

            output.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return !output.empty();
        }

        /**
         * @brief 把一份 OCSP 响应挂到指定上下文（后一次调用覆盖前一次）
         * @param context 目标上下文
         * @param bytes 响应字节（DER）
         */
        void attachOcspResponse(SSL_CTX *context, std::string bytes)
        {
            const int index = stapledResponseExDataIndex();
            if (index < 0)
            {
                return;
            }

            auto *holder = static_cast<StapledOcspResponse *>(SSL_CTX_get_ex_data(context, index));
            if (holder == nullptr)
            {
                holder = new StapledOcspResponse();
                if (SSL_CTX_set_ex_data(context, index, holder) != 1)
                {
                    delete holder;
                    return;
                }
            }
            holder->bytes.store(std::make_shared<const std::string>(std::move(bytes)), std::memory_order_release);
        }

        /**
         * @brief OCSP 装订回调：客户端请求 status_request 时把本上下文带的响应交给 OpenSSL
         * @details 未配置响应（或配置后为空）时回 NOACK：握手照常、只是不装订——这正是
         *          「证书没有配套 OCSP 信息」的部署应得的行为。
         * @param ssl 当前握手对象
         * @return int SSL_TLSEXT_ERR_OK 已装订；SSL_TLSEXT_ERR_NOACK 无响应可装订
         */
        int stapleOcspResponse(SSL *ssl, void *)
        {
            SSL_CTX *context = SSL_get_SSL_CTX(ssl);
            if (context == nullptr)
            {
                return SSL_TLSEXT_ERR_NOACK;
            }

            const auto *holder = static_cast<const StapledOcspResponse *>(SSL_CTX_get_ex_data(context, stapledResponseExDataIndex()));
            if (holder == nullptr)
            {
                return SSL_TLSEXT_ERR_NOACK;
            }

            const std::shared_ptr<const std::string> snapshot = holder->bytes.load(std::memory_order_acquire);
            if (!snapshot || snapshot->empty())
            {
                return SSL_TLSEXT_ERR_NOACK;
            }

            // OpenSSL 3.x 的装订判定只看本 SSL 上的 OCSP_RESPONSE 对象栈（resp_ex）：旧的裸字节
            // 接口不再驱动它，因此这里把 DER 解析成对象再放进去；解析失败按「无响应」处理
            const unsigned char *cursor   = reinterpret_cast<const unsigned char *>(snapshot->data());
            OCSP_RESPONSE       *response = d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(snapshot->size()));
            if (response == nullptr)
            {
                ERR_clear_error();
                return SSL_TLSEXT_ERR_NOACK;
            }

            STACK_OF(OCSP_RESPONSE) *responses = sk_OCSP_RESPONSE_new_null();
            if (responses == nullptr || sk_OCSP_RESPONSE_push(responses, response) == 0)
            {
                OCSP_RESPONSE_free(response);
                sk_OCSP_RESPONSE_free(responses);
                ERR_clear_error();
                return SSL_TLSEXT_ERR_NOACK;
            }

            // set0 语义：SSL 接管整个对象栈（替换时连旧栈一起释放），此后不要再引用 responses
            SSL_set0_tlsext_status_ocsp_resp_ex(ssl, responses);
            return SSL_TLSEXT_ERR_OK;
        }

        /**
         * @brief SNI 换站点上下文失败的告警：只在首次失败时出声
         *
         * @details 触发输入是对端 ClientHello 里的 server_name，逐条告警等于把日志交给远端刷，
         *          因此取上升沿（与武装失败告警同一口径）。计数留在原子里供趋势取用。
         * @note 兜底告警自己也不许穿出去：内存耗尽时日志通路会抛，而那正是本函数要防的展开源，
         *          所以这里再套一层——此时只剩「不出声」这一条出路。
         */
        void reportServerNameSelectionFailure(const std::string_view reason) noexcept
        {
            static std::atomic<std::uint64_t> failureCount{0};
            if (failureCount.fetch_add(1, std::memory_order_relaxed) != 0)
            {
                return;
            }

            try
            {
                LOG_ERROR_FMT("TlsContext: 按 SNI 换站点上下文失败，本次握手交回默认证书（原因：{}）。同类失败此后不再逐条告警，仅在此出声一次", reason);
            } catch (...)
            {
                // 见 @note：日志通路自身失败已无处可报，绝不能让它穿过 C 帧
            }
        }

    } // namespace

    TlsContext::TlsContext(const TlsPolicy &policy, const Role role) : m_policy(policy), m_role(role)
    {
#ifndef _WIN32
        // OpenSSL 内部的 read()/write() 绕不开 MSG_NOSIGNAL：向已关闭的对端写数据
        // （握手失败发 alert、SSL_shutdown 发 close_notify）在 Linux 上会触发 SIGPIPE，
        // 默认动作是直接打死整个进程。忽略该信号后，写失败改以 EPIPE 错误码返回，
        // 交给既有错误路径处理——这也是服务器程序的标准做法（nginx、libuv 同样忽略）。
        // 该设置是进程级且幂等的：重复构造 TlsContext 没有额外影响
        std::signal(SIGPIPE, SIG_IGN);
#endif

        m_context = createHardenedContext(role, policy);

        // 策略里带了 CA 就等于「校验对端证书的信任库已就位」：setClientCertificateRequired(true)
        // 的判据据此放行，调用方不必再补一次 loadClientCertificateAuthority()——两处都能给信任库，
        // 但只有这一条路支持 CA 目录与校验深度
        if (!policy.certificateAuthorityFile.empty() || !policy.certificateAuthorityPath.empty())
        {
            m_clientCertificateAuthorityLoaded = true;
        }
    }

    SSL_CTX *TlsContext::createHardenedContext(const Role role, const TlsPolicy &policy)
    {
        SSL_CTX *context = SSL_CTX_new(role == Role::Server ? TLS_server_method() : TLS_client_method());
        if (!context)
        {
            // SSL_CTX_new 只在内存不足或 OpenSSL 未被正确初始化时才返回空：
            // 这是不可恢复的启动期故障，因此直接抛出让调用方尽早失败
            throw CoreException("创建 TLS 上下文失败：SSL_CTX_new 返回空"
                                "（通常是内存不足，或 OpenSSL 库未正确初始化）");
        }

        // 加固项里属于「策略」的那一批（安全等级、版本区间、套件与曲线、信任库、票据）交给
        // applyTlsPolicy，一处实现；服务端那一档默认值也在这里补进策略，而不是散在两段代码里：
        // 最低协议限定 TLS 1.2（RFC 8996 已把 1.0/1.1 列为废弃，两者仍有已知攻击面与过时算法组合）
        TlsPolicy effectivePolicy = policy;
        if (role == Role::Server && !effectivePolicy.minimumProtocolVersion.has_value())
        {
            effectivePolicy.minimumProtocolVersion = TlsPolicy::ProtocolVersion::Tls1_2;
        }

        // 关闭压缩：压缩会引入 CRIME 侧信道，两侧都不协商压缩
        SSL_CTX_set_options(context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);

        // 关掉服务端侧重协商：TLS 1.2 及更早的「客户端可要求重新握手」是一条按连接放大的 CPU 消耗
        // 通道——一条已建立的连接上反复请求重协商，服务端就反复做完整的密钥交换，而这只花客户端
        // 几个包。服务端主动发起的 HelloRequest 一并关掉（浏览器都不用这条路，post-handshake
        // 认证在 TLS 1.3 里另有机制，而 1.3 本身没有重协商）。行业默认（nginx/cloudflare）同样关闭
        SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION);

        try
        {
            // 服务端那份套件列表显式排除弱算法（MD5/RC4/3DES/DES/导出级/匿名/PSK/SRP），与安全等级
            // 形成双保险；客户端一侧不预设列表——原先就没设过，设了反而可能把本可以连上的服务器拒掉
            applyTlsPolicy(context, effectivePolicy, role == Role::Server ? kServerCipherList : nullptr);
        } catch (...)
        {
            SSL_CTX_free(context);
            throw;
        }

        if (role == Role::Server)
        {
            // 注册 ALPN 选择回调：选择策略见 selectAlpnProtocol()。客户端是提出协议名的一方，
            // 挂这个回调没有意义（OpenSSL 也不会调它）
            SSL_CTX_set_alpn_select_cb(context, selectAlpnProtocol, nullptr);

            // OCSP 装订：客户端在 ClientHello 里请求 status_request 时按需回应；未配置响应则不装订
            SSL_CTX_set_tlsext_status_cb(context, stapleOcspResponse);
        }

        // session id context：把「会话属于哪个应用」固定进恢复票据与缓存。OpenSSL 在启用
        // 客户端证书校验（SSL_VERIFY_PEER）时要求已设置它，否则会话恢复会被拒绝；非 mTLS
        // 部署也借此避免与同进程其它 TLS 用途串会话。10 字节，远低于 OpenSSL 的 32 字节上限
        static const unsigned char kSessionIdContext[] = {'A', 's', 'y', 'n', 'G', 'y', 'a', 'n', 'i', 's'};
        if (SSL_CTX_set_session_id_context(context, kSessionIdContext, sizeof(kSessionIdContext)) != 1)
        {
            SSL_CTX_free(context);
            throw CoreException("创建 TLS 上下文失败：无法设置 session id context（长度超出 OpenSSL 上限）");
        }

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
        // 先清空错误栈：头文件承诺「失败可由 OpenSSL 错误栈取到原因」，而调用方通常只读第一条。
        // 不清的话，前一次失败留下的条目还压在队首，日志就会把「私钥文件不存在」报成上次的
        // 「证书文件不存在」——同一份文案指向错的那个文件
        ERR_clear_error();

        // 按「链文件」而非「单证书文件」加载：首张证书作本机证书，其余逐张进链并随握手一并出示。
        // use_certificate_file 只读第一张，链上的中间 CA 会被静默丢掉 —— 部署里全链证书
        // （fullchain.pem）是常态，缺链时对端只信任根 CA 就无法把证书串到根，握手直接失败
        if (SSL_CTX_use_certificate_chain_file(context, certificateFile.c_str()) != 1)
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

    std::shared_ptr<SSL_CTX> TlsContext::adoptContext(SSL_CTX *context)
    {
        return std::shared_ptr<SSL_CTX>(context, [](SSL_CTX *held) { SSL_CTX_free(held); });
    }

    int TlsContext::virtualHostRegistryIndex()
    {
        // 与装订数据同一条理由：登记表的生死跟着 SSL_CTX 走，回调在握手线程上才不会碰到已析构的 TlsContext
        static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr,
                                                          [](void *, void *pointer, CRYPTO_EX_DATA *, int, long, void *) { delete static_cast<VirtualHostRegistry *>(pointer); });
        return index;
    }

    TlsContext::VirtualHostRegistry *TlsContext::registryOf(SSL_CTX *context)
    {
        if (context == nullptr)
        {
            return nullptr;
        }
        const int index = virtualHostRegistryIndex();
        if (index < 0)
        {
            return nullptr;
        }
        return static_cast<VirtualHostRegistry *>(SSL_CTX_get_ex_data(context, index));
    }

    TlsContext::VirtualHostRegistry *TlsContext::ensureRegistry(SSL_CTX *context)
    {
        if (VirtualHostRegistry *existing = registryOf(context); existing != nullptr)
        {
            return existing;
        }

        const int index = virtualHostRegistryIndex();
        if (index < 0)
        {
            return nullptr;
        }

        auto *registry = new VirtualHostRegistry();
        if (SSL_CTX_set_ex_data(context, index, registry) != 1)
        {
            delete registry;
            return nullptr;
        }

        // 回调只在真有登记表时挂：一个站点都没登记过的上下文，不该在每条握手里多走一次查表
        SSL_CTX_set_tlsext_servername_callback(context, &TlsContext::selectContextByServerName);
        return registry;
    }

    std::string TlsContext::serverNameKey(const std::string_view hostName)
    {
        // 先去掉两侧空白（配置文件里多打一个空格是常态），再折小写、去结尾根点。
        // 不处理端口：SNI 按 RFC 6066 不带端口，替调用方剥掉反而会把「填错了」这件事藏起来
        std::size_t beginIndex = 0;
        std::size_t endIndex   = hostName.size();
        while (beginIndex < endIndex && (hostName[beginIndex] == ' ' || hostName[beginIndex] == '\t'))
        {
            ++beginIndex;
        }
        while (endIndex > beginIndex && (hostName[endIndex - 1] == ' ' || hostName[endIndex - 1] == '\t'))
        {
            --endIndex;
        }
        const std::string_view trimmed = hostName.substr(beginIndex, endIndex - beginIndex);

        std::string key;
        key.reserve(trimmed.size());
        for (const char character: trimmed)
        {
            key.push_back((character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character);
        }
        while (key.size() > 1 && key.back() == '.')
        {
            key.pop_back();
        }
        return key;
    }

    int TlsContext::selectContextByServerName(SSL *ssl, int *, void *) noexcept
    {
        // 本函数由 OpenSSL 经 C 函数指针回调（注册点见上文 SSL_CTX_set_tlsext_servername_callback），
        // 而输入是远端 ClientHello 里的 server_name。栈从这里展开要穿过 C 帧，那是未定义行为而不是
        // 「异常被某个上层接住」，因此整份实现按 noexcept 写死，兜底的 catch 只能留在这里。
        // 失败时交回默认证书：与下面「没带 SNI」「名字没登记过」两条同一语义，握手照常继续。
        try
        {
            const char *serverName = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            if (serverName == nullptr || *serverName == '\0')
            {
                // 客户端没带 SNI（老客户端、直连 IP 的探活）：交回默认证书，与 nginx 的 default server 同形
                return SSL_TLSEXT_ERR_OK;
            }

            VirtualHostRegistry *registry = registryOf(SSL_get_SSL_CTX(ssl));
            if (registry == nullptr)
            {
                return SSL_TLSEXT_ERR_OK;
            }

            const std::string        key = serverNameKey(serverName);
            std::shared_ptr<SSL_CTX> target;
            {
                const std::shared_lock<std::shared_mutex> guard(registry->mutex);
                if (const auto found = registry->contexts.find(key); found != registry->contexts.end())
                {
                    target = found->second;
                }
            }
            if (target == nullptr)
            {
                // 名字没登记过同样不是错误：这类客户端要的就是那个「什么特别站点都不是」的默认身份
                return SSL_TLSEXT_ERR_OK;
            }

            // SSL_set_SSL_CTX 只改 ssl->ctx、不给新上下文加引用（它的返回值是换上去的那个上下文，不是成败码）：
            // 这条引用链由「SSL 持有主上下文、主上下文持有登记表、登记表持有站点上下文」三段共同保证，
            // 只要这条连接还认着主上下文，站点上下文就不会先被释放
            if (SSL_set_SSL_CTX(ssl, target.get()) == nullptr)
            {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            return SSL_TLSEXT_ERR_OK;
        } catch (const std::exception &failure)
        {
            // 能走到这里的现实原因是分配失败（站点键的字符串、shared_lock 都要分配），不是配置错
            reportServerNameSelectionFailure(failure.what());
            return SSL_TLSEXT_ERR_OK;
        } catch (...)
        {
            reportServerNameSelectionFailure("非标准异常");
            return SSL_TLSEXT_ERR_OK;
        }
    }

    std::shared_ptr<SSL_CTX> TlsContext::buildHostContext(const TlsPolicy &policy, const Role role, const std::string &certificateFile, const std::string &keyFile)
    {
        SSL_CTX *context = nullptr;
        try
        {
            // 按与主上下文同一份策略与角色建：漏掉策略，登记的站点就会用默认档跑，
            // 主上下文挡掉的弱套件换个域名照样能谈成
            context = createHardenedContext(role, policy);
        } catch (const CoreException &)
        {
            return nullptr;
        }

        if (!installCertificate(context, certificateFile, keyFile))
        {
            SSL_CTX_free(context);
            return nullptr;
        }
        return adoptContext(context);
    }

    bool TlsContext::loadCertificateForHost(const std::string &hostName, const std::string &certificateFile, const std::string &keyFile) const
    {
        if (m_role != Role::Server)
        {
            throw Base::InvalidArgumentException("TlsContext: 只有服务端上下文能登记站点证书——客户端一侧不对外出示身份，SNI 选证书没有对应的动作");
        }

        const std::string key = serverNameKey(hostName);
        // 空串、带通配符、带空格或冒号：都不是一条可比对的 SNI 键。收下等于登记一条永不命中的规则，
        // 而它的表现是「那个域名莫名拿到默认证书」，比当场拒绝难查得多
        if (key.empty() || key.find('*') != std::string::npos || key.find_first_of(" \t:") != std::string::npos)
        {
            throw Base::InvalidArgumentException("TlsContext: SNI 站点名不能是空串或「" + hostName + "」，需要 localhost 这样一个真实主机名（通配、端口与空白都不属于 SNI）");
        }

        std::lock_guard<std::mutex> guard(m_contextMutex);

        // 先把证书装进新上下文再动登记表：任一份读不出来，登记表都保持原样（与热轮换同一口径——
        // 配置错误不该让正在服务的站点表变成半份）
        std::shared_ptr<SSL_CTX> hostContext = buildHostContext(m_policy, m_role, certificateFile, keyFile);
        if (hostContext == nullptr)
        {
            return false;
        }

        VirtualHostRegistry *registry = ensureRegistry(m_context);
        if (registry == nullptr)
        {
            // ex_data 下标注册失败：本进程用不了 SNI 能力，如实报失败而不是悄悄只装好上下文
            return false;
        }

        {
            const std::unique_lock<std::shared_mutex> registryGuard(registry->mutex);
            // 同名重复登记就地替换：与 loadCertificate() 的「再调一次就是换一份身份」同一口径
            registry->contexts[key] = std::move(hostContext);
        }

        // 路径记录同样按名字去重，换代时以最后一份为准
        m_hostCertificateFiles.erase(
                std::remove_if(m_hostCertificateFiles.begin(), m_hostCertificateFiles.end(), [&key](const HostCertificateFiles &entry) { return entry.hostName == key; }),
                m_hostCertificateFiles.end());
        m_hostCertificateFiles.push_back(HostCertificateFiles{key, certificateFile, keyFile});
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
            // 按同一角色、同一策略重建：只复现证书会漏掉加固项，而漏掉策略等于一次续期把
            // 「最低版本/套件/曲线」悄悄换回默认档
            newContext = createHardenedContext(m_role, m_policy);
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

        // 复现「要求并校验对端证书」这一档：CA 已随策略在新上下文上就位（信任库、校验深度都在策略里），
        // 但校验模式是 setClientCertificateRequired() 单独设的，漏掉这一步会让一次续期把 mTLS 悄悄关掉——
        // 那是「续期看起来成功、安全性反而降级」的典型形态
        if (m_clientCertificateRequired)
        {
            SSL_CTX_set_verify(newContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }

        // 复现已加载的 OCSP 响应：续期通常把响应与证书一起更新，因此按原路径**重读**而不是沿用旧字节；
        // 重读失败与证书加载失败同语义——本次轮换整体失败，旧上下文继续服务
        if (!m_ocspResponseFile.empty())
        {
            std::string responseBytes;
            if (!readFileBytes(m_ocspResponseFile, responseBytes))
            {
                SSL_CTX_free(newContext);
                return false;
            }
            attachOcspResponse(newContext, std::move(responseBytes));
        }

        // 复现已装载的票据密钥：漏掉这一步会让一次证书续期把「跨进程共享的恢复能力」悄悄退回
        // 「每个上下文一份随机密钥」——多进程部署下的表现是续期之后恢复命中率掉到零，
        // 而且日志里一个字都没有。与 OCSP 同语义：按原路径重读，读不到就整次换代失败
        if (!m_sessionTicketKeyFiles.empty())
        {
            std::vector<std::string> ticketKeys;
            try
            {
                SessionTicketKeyRing::readKeyFiles(m_sessionTicketKeyFiles, ticketKeys);
            } catch (const CoreException &)
            {
                // 密钥文件在两次续期之间被删掉或换成了长度不对的内容：本次换代失败，旧上下文继续服务。
                // 异常不往运维线程外抛——reloadCertificate() 的失败语义是 false，不是抛
                SSL_CTX_free(newContext);
                return false;
            }
            SessionTicketKeyRing::install(newContext, std::move(ticketKeys));
        }

        // 复现按主机名登记的站点证书：漏掉这一步，一次续期就把 SNI 站点全部退回默认证书——
        // 客户端看到的是「与它请求的主机不匹配的一张证书」，比续期失败更难查
        if (!m_hostCertificateFiles.empty())
        {
            VirtualHostRegistry *newRegistry = ensureRegistry(newContext);
            if (newRegistry == nullptr)
            {
                SSL_CTX_free(newContext);
                return false;
            }
            for (const HostCertificateFiles &entry: m_hostCertificateFiles)
            {
                std::shared_ptr<SSL_CTX> hostContext = buildHostContext(m_policy, m_role, entry.certificateFile, entry.keyFile);
                if (hostContext == nullptr)
                {
                    // 某一份站点证书读不出来：整次换代作废，旧上下文连着旧的站点表继续服务
                    SSL_CTX_free(newContext);
                    return false;
                }
                const std::unique_lock<std::shared_mutex> registryGuard(newRegistry->mutex);
                newRegistry->contexts[entry.hostName] = std::move(hostContext);
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

    void TlsContext::enableClientPeerVerification() const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);

        // 策略没接管信任库时才补系统那一份：补在两处都会的情况下会把自己 CA 签的对端
        // 也一并放行（系统库里有公网根 CA），那等于「我只信我自己的 CA」这条要求被悄悄放宽
        if (m_policy.certificateAuthorityFile.empty() && m_policy.certificateAuthorityPath.empty())
        {
            SSL_CTX_set_default_verify_paths(m_context);
        }
        // 出站一侧恒要校验：不验对端等于任何受信 CA 给他域签的证书都能冒充目标主机
        SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER, nullptr);
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

    bool TlsContext::loadOcspResponse(const std::string &ocspResponseFile) const
    {
        // 先读文件再进锁：读盘不持锁，避免把文件 IO 拖进与 createSSL() 争用的关键区
        std::string responseBytes;
        if (!readFileBytes(ocspResponseFile, responseBytes))
        {
            return false;
        }

        // 装订侧只认可解析的 DER（见回调里的说明）：在加载时就把坏文件挡掉，
        // 好过服务期间每次握手都解析失败、装订悄悄缺席
        const unsigned char *cursor = reinterpret_cast<const unsigned char *>(responseBytes.data());
        OCSP_RESPONSE       *probe  = d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(responseBytes.size()));
        if (probe == nullptr)
        {
            ERR_clear_error();
            return false;
        }
        OCSP_RESPONSE_free(probe);

        std::lock_guard<std::mutex> guard(m_contextMutex);
        attachOcspResponse(m_context, std::move(responseBytes));
        // 记住路径：reloadCertificate() 按它重读，保证响应与证书一起换新
        m_ocspResponseFile = ocspResponseFile;
        return true;
    }

    void TlsContext::loadSessionTicketKeys(const std::vector<std::string> &keyFiles) const
    {
        // 先读文件再进锁：读盘不持锁，避免把文件 IO 拖进与 createSSL() 争用的关键区。
        // 列表为空、读不出来、长度不合法三种都在这里当场抛，消息点名是哪一份文件
        std::vector<std::string> keys;
        SessionTicketKeyRing::readKeyFiles(keyFiles, keys);

        std::lock_guard<std::mutex> guard(m_contextMutex);
        SessionTicketKeyRing::install(m_context, std::move(keys));
        // 记住路径：reloadCertificate() 按它重读，保证密钥与证书一起换代
        m_sessionTicketKeyFiles = keyFiles;
    }

    SSL_CTX *TlsContext::nativeHandle() const
    {
        std::lock_guard<std::mutex> guard(m_contextMutex);
        return m_context;
    }

} // namespace AsynGyanis::Core
