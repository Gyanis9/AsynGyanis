#include "Core/Tls/TlsContext.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Exception/CoreException.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
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
            // 校验回调挂在 SSL 对象上，切到站点上下文后它按新上下文查登记；把主上下文
            // 当前的名单复制过去，否则名单会把所有 SNI 连接拒掉
            if (const PeerFingerprintRegistry *sourceRegistry = peerFingerprintRegistryOf(SSL_get_SSL_CTX(ssl)); sourceRegistry != nullptr)
            {
                if (PeerFingerprintRegistry *targetRegistry = peerFingerprintRegistryOf(target.get()); targetRegistry != nullptr)
                {
                    const std::shared_lock<std::shared_mutex> sourceGuard(sourceRegistry->mutex);
                    const std::lock_guard<std::shared_mutex>  targetGuard(targetRegistry->mutex);
                    targetRegistry->fingerprints = sourceRegistry->fingerprints;
                }
            }
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
        } catch (const CoreException &failure)
        {
            // 建站点上下文失败的原因（策略参数、算法不被支持）只有这一处持有：调用方
            // loadCertificateForHost 只能回报「这个站点没装上」。虚拟主机登记发生在
            // 配置装载与证书续期，不在握手热路径上，逐条告警不会淹
            LOG_ERROR_EXCEPTION(failure, "TlsContext: 建立虚拟主机上下文失败，该站点不会进登记表（原因：{}）", failure.what());
            return nullptr;
        }

        if (!installCertificate(context, certificateFile, keyFile))
        {
            SSL_CTX_free(context);
            return nullptr;
        }

        // 指纹回调在 SNI 切换后按站点上下文取登记：每份站点上下文都先建好登记（名单可以为空），
        // 切换时只复制内容，不必在握手线程上创建共享状态
        PeerFingerprintRegistry *registry = ensurePeerFingerprintRegistry(context);
        if (registry == nullptr)
        {
            SSL_CTX_free(context);
            return nullptr;
        }
        {
            const std::lock_guard<std::shared_mutex> guard(registry->mutex);
            registry->fingerprints = policy.allowedPeerSha256Fingerprints;
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
} // namespace AsynGyanis::Core
