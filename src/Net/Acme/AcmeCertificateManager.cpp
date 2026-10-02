#include "Net/Acme/AcmeCertificateManager.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Acme/AcmeClient.h"
#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/IO/FileContents.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 机构给的 `Retry-After` 的上限（秒）
         * @details 刻意的不信任：RFC 8555 §6.8 要求客户端照办，但没说可以照办到多久。一个坏掉、
         *          被换掉或胡乱答复的机构如果能把门槛推到一年，续期就永远不会再发生，而证书到期
         *          是几周之后才暴露的事。夹在 24 小时：足以躲过机构常见的「每小时/每分钟」窗口，
         *          又保证一天之内一定再试一次。
         */
        constexpr long long kMaximumRemoteRetryAfterSeconds = 24 * 60 * 60;

        /**
         * @brief 续期循环一次停放的分片长度
         * @details 取 1 秒：相对默认 12 小时的检查间隔，每进程一次的这一拍唤醒成本可以忽略，
         *          而它把「叫停到退出」的延迟从一整拍压成不超过这一片。要比这更快只有让叫停去
         *          叫醒这条帧——那需要跨线程取消定时器，而定时器只能由循环线程碰（见类注释的线程契约）
         */
        constexpr std::chrono::milliseconds kStopNoticeSlice{1000};

        /// X509 与 BIO 的归还动作
        struct X509Deleter
        {
            void operator()(X509 *certificate) const noexcept
            {
                X509_free(certificate);
            }
        };

        struct BioDeleter
        {
            void operator()(BIO *bio) const noexcept
            {
                BIO_free(bio);
            }
        };

        /**
         * @brief 把 tm 按 UTC 折成 Unix 秒
         * @details 两侧的函数名不同（MSVC 是 _mkgmtime），而到期判据要的必须是同一个数：
         *          证书里的 notAfter 本来就是 UTC，按本地时区折会差出一个时区，续期时机跟着歪
         */
        [[nodiscard]] long long toUtcUnixSeconds(struct tm &broken) noexcept
        {
#ifdef _WIN32
            return static_cast<long long>(_mkgmtime(&broken));
#else
            return static_cast<long long>(timegm(&broken));
#endif
        }

        /**
         * @brief 挂出去的令牌的作用域守卫：无论从哪条出口离开，令牌都从自证仓库里撤掉
         * @details 一次签发的出口比入口多（每条失败都要提前返回），手写 withdraw 迟早会漏一条；
         *          漏掉的那条会让一个一次性令牌永久可取
         */
        class PresentedToken
        {
        public:
            PresentedToken(AcmeHttp01ChallengeStore &store, std::string token, const std::string &accountKeyThumbprint) : m_store(store), m_token(std::move(token))
            {
                m_store.present(m_token, accountKeyThumbprint);
            }

            PresentedToken(const PresentedToken &) = delete;

            PresentedToken &operator=(const PresentedToken &) = delete;

            ~PresentedToken()
            {
                m_store.withdraw(m_token);
            }

        private:
            AcmeHttp01ChallengeStore &m_store;
            std::string               m_token;
        };
    } // namespace

    std::optional<std::chrono::system_clock::time_point> readCertificateExpiry(const std::filesystem::path &certificateFile)
    {
        std::error_code      sizeFailure;
        const std::uintmax_t fileSize = std::filesystem::file_size(certificateFile, sizeFailure);
        if (sizeFailure)
        {
            return std::nullopt;
        }

        const auto contents = Platform::readFileContents(certificateFile, 0U, static_cast<std::size_t>(fileSize));
        if (!contents.has_value())
        {
            return std::nullopt;
        }

        const std::unique_ptr<BIO, BioDeleter> memory(BIO_new_mem_buf(contents->data(), static_cast<int>(contents->size())));
        if (!memory)
        {
            return std::nullopt;
        }
        // 只读第一张：链里首张是叶证书，到期判据看的就是它的 notAfter
        const std::unique_ptr<X509, X509Deleter> certificate(PEM_read_bio_X509(memory.get(), nullptr, nullptr, nullptr));
        if (!certificate)
        {
            return std::nullopt;
        }

        struct tm notAfterTime{};
        if (ASN1_TIME_to_tm(X509_get0_notAfter(certificate.get()), &notAfterTime) != 1)
        {
            return std::nullopt;
        }
        return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(toUtcUnixSeconds(notAfterTime)));
    }

    AcmeCertificateManager::AcmeCertificateManager(Core::EventLoop &loop, Configuration configuration, ReloadHandler reloadHandler, AcmeDns01TxtWriter dns01TxtWriter) :
        m_loop(loop), m_configuration(std::move(configuration)), m_reloadHandler(std::move(reloadHandler)), m_dns01TxtWriter(std::move(dns01TxtWriter))
    {
        // 三条读数在构造时就挂上，不等第一次签发：常驻进程里它们长期为 0 就是要报的事——
        // 「自动化没跑成」与「自动化还没跑」从外面看得是同一个形状，得让面板能分辨有没有登记过
        m_metricHandles = {
                Core::ProcessMetricsRegistry::registerMetric("asyn_acme_certificate_expiry_seconds",
                                                             "磁盘上那张证书的到期时刻（Unix 秒，进程内多个管理器取最早的那张）；0 表示那条路径上没有读得出的证书",
                                                             Core::ProcessMetricKind::Gauge, Core::ProcessMetricMerge::Min, [this]
                                                             { return static_cast<std::uint64_t>(std::max<long long>(0, m_expiryUnixSeconds.load(std::memory_order_relaxed))); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_acme_issuances_total", "成功签发或续期并原子落盘的轮次数（进程累计）", Core::ProcessMetricKind::Counter,
                                                             Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_issuanceCount.load(std::memory_order_relaxed)); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_acme_failures_total", "证书自动化失败的轮次数：含被拒的配置、机构判 invalid、装回动作失败",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return static_cast<std::uint64_t>(m_failureCount.load(std::memory_order_relaxed)); }),
        };

        // 到期时刻先按磁盘上那张现有证书填一次，不等第一次签发。少了这一步，每次重启后面板都会读到
        // 一段 0 并且报成「证书没了」，而那个文件其实一直在那里
        if (const auto expiryOnDisk = readExpiryFromDisk(); expiryOnDisk.has_value())
        {
            m_expiryUnixSeconds.store(std::chrono::duration_cast<std::chrono::seconds>(expiryOnDisk->time_since_epoch()).count(), std::memory_order_relaxed);
        }
    }

    AcmeManagerStatus AcmeCertificateManager::status() const
    {
        AcmeManagerStatus snapshot;
        snapshot.issuanceCount                = m_issuanceCount.load(std::memory_order_relaxed);
        snapshot.failureCount                 = m_failureCount.load(std::memory_order_relaxed);
        snapshot.certificateExpiryUnixSeconds = m_expiryUnixSeconds.load(std::memory_order_relaxed);
        snapshot.backoffUntilUnixSeconds      = backoffGateUnixSeconds();
        std::lock_guard guard(m_lastFailureMutex);
        snapshot.lastFailureMessage = m_lastFailureMessage;
        return snapshot;
    }

    const AcmeHttp01ChallengeStore &AcmeCertificateManager::challengeStore() const noexcept
    {
        return m_challengeStore;
    }

    void AcmeCertificateManager::registerChallengeRoutes(Router &router)
    {
        m_challengeStore.registerRoutes(router);
    }

    void AcmeCertificateManager::stopRenewalLoop() noexcept
    {
        m_isStopping.store(true, std::memory_order_release);
    }

    void AcmeCertificateManager::recordFailure(std::string message, const std::optional<std::chrono::seconds> remoteRetryAfter) noexcept
    {
        m_failureCount.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard guard(m_lastFailureMutex);
            m_lastFailureMessage = message;
        }
        // 三条渠道都写：日志给半夜只看得到一行的人，计数给面板，expected 给就在等的调用方
        LOG_ERROR_FMT("AcmeCertificateManager: {}", message);
        m_notBeforeNextAttemptUnix.store(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
                                                 remoteOrLocalDelaySeconds(remoteRetryAfter) - 1,
                                         std::memory_order_relaxed);
    }

    long long AcmeCertificateManager::remoteOrLocalDelaySeconds(const std::optional<std::chrono::seconds> remoteRetryAfter) const noexcept
    {
        // 退避门槛有两个来源，合成一个正数：本地的最小重试间隔（机构按「每域名每周几张」限流，
        // 一小时都不该再撞一次——这一项以前虽然写着，却从没被读过，配成 5 分钟和配成一天是同一个行为），
        // 以及机构在 429/503 上明说的秒数（RFC 8555 §6.8 要求客户端必须照办）。
        // 机构的说法只被 24 小时这个上限夹住：那是刻意的不信任——一个坏掉、被换掉或胡乱答复的机构
        // 不该能把续期一推再推，推到证书真的过期。取两者中较大的，谁也不覆盖谁
        const long long localMinimumSeconds = std::chrono::duration_cast<std::chrono::seconds>(m_configuration.minimumRetryInterval).count();
        const long long configuredSeconds   = std::max<long long>(1, localMinimumSeconds);
        if (!remoteRetryAfter.has_value())
        {
            return configuredSeconds;
        }
        const long long cappedRemoteSeconds = std::clamp<long long>(remoteRetryAfter->count(), 0, kMaximumRemoteRetryAfterSeconds);
        return std::max(configuredSeconds, cappedRemoteSeconds);
    }

    long long AcmeCertificateManager::backoffGateUnixSeconds() const noexcept
    {
        // 记的是「失败那一刻」，门槛取它的下一秒：同一秒内再进来一次不该被放行两次
        const long long failedAt = m_notBeforeNextAttemptUnix.load(std::memory_order_relaxed);
        return failedAt == 0 ? 0 : failedAt + 1;
    }

    AcmeError AcmeCertificateManager::failWith(const AcmeErrorKind kind, std::string message)
    {
        recordFailure(message);
        return AcmeError{kind, std::move(message)};
    }

    AcmeError AcmeCertificateManager::notedFailure(AcmeError error)
    {
        // 机构的 Retry-After 只有经这条路才进得了退避门槛：RateLimited 那一档带着它，其余档为空
        recordFailure(error.message, error.retryAfter);
        return error;
    }

    std::optional<std::string> AcmeCertificateManager::readPersistedAccountUrl() const
    {
        std::error_code      sizeFailure;
        const std::uintmax_t fileSize = std::filesystem::file_size(m_configuration.accountStateFile, sizeFailure);
        if (sizeFailure)
        {
            // 没有状态文件就是第一次跑，不是事故
            return std::nullopt;
        }

        const auto contents = Platform::readFileContents(m_configuration.accountStateFile, 0U, static_cast<std::size_t>(fileSize));
        if (!contents.has_value())
        {
            return std::nullopt;
        }
        const auto parsed = Base::parseConfigValue(*contents);
        if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("accountUrl"))
        {
            return std::nullopt;
        }
        return Base::configValueAs<std::string>(parsed->at("accountUrl"));
    }

    std::optional<AcmeError> AcmeCertificateManager::persistAccountState(const std::string &accountUrl) const
    {
        Base::ConfigValue state = Base::ConfigValue::object();
        state["accountUrl"]     = accountUrl;
        const auto text         = Base::serializeConfigValue(state);
        if (!text.has_value())
        {
            return AcmeError{AcmeErrorKind::FileSystem, "账户状态没能序列化成 JSON"};
        }

        std::string writeFailure;
        if (!Platform::AtomicFileWriter::writeText(m_configuration.accountStateFile, *text, std::nullopt, &writeFailure))
        {
            return AcmeError{AcmeErrorKind::FileSystem, std::format("账户状态落盘到 {} 失败：{}", m_configuration.accountStateFile.string(), writeFailure)};
        }
        return std::nullopt;
    }

    std::optional<std::chrono::system_clock::time_point> AcmeCertificateManager::readExpiryFromDisk() const
    {
        return readCertificateExpiry(m_configuration.certificateFile);
    }

    std::expected<AcmeKeyPair *, AcmeError> AcmeCertificateManager::ensureAccountKey()
    {
        // 已经就位就复用：账户密钥换一次就意味着在机构那边换了个人，之前的速率与授权历史都不跟着走
        if (m_accountKey.has_value())
        {
            return &*m_accountKey;
        }

        std::error_code existenceFailure;
        const bool      exists = std::filesystem::exists(m_configuration.accountKeyFile, existenceFailure);
        if (exists)
        {
            auto loaded = AcmeKeyPair::loadFromFile(m_configuration.accountKeyFile);
            if (!loaded.has_value())
            {
                return std::unexpected(loaded.error());
            }
            m_accountKey = std::move(*loaded);
            return &*m_accountKey;
        }

        auto generated = AcmeKeyPair::generate(m_configuration.accountKeyAlgorithm);
        if (!generated.has_value())
        {
            return std::unexpected(generated.error());
        }
        auto saved = generated->saveToFile(m_configuration.accountKeyFile);
        if (!saved.has_value())
        {
            return std::unexpected(saved.error());
        }
        m_accountKey = std::move(*generated);
        return &*m_accountKey;
    }

    Core::Task<std::optional<AcmeError>> AcmeCertificateManager::authorizeDns01(AcmeClient &client, const AcmeAuthorization &authorization)
    {
        const std::string recordName       = dns01RecordName(authorization.identifier);
        const std::string keyAuthorization = authorization.dns01->token + "." + m_accountKey->jsonWebKeyThumbprint();
        const std::string recordValue      = dns01ValidationText(keyAuthorization);

        if (auto published = co_await m_dns01TxtWriter.publish(recordName, recordValue); !published.has_value())
        {
            // 写失败也要撤：「控制面收下之后才失败」那一类（确认阶段超时）其实已经写成了，
            // 留下的那条 TXT 会让下一轮签发在同一名字上读到两条不同答案，而机构的原文不会说这一点
            if (auto rollback = co_await m_dns01TxtWriter.withdraw(recordName, recordValue); !rollback.has_value())
            {
                LOG_WARN_FMT("AcmeCertificateManager: 域名 {} 的 dns-01 记录 {} 写入失败之后的撤回也没成功：{}。"
                             "请手工到 DNS 控制台删掉这条 TXT，否则下一轮签发会在同一个名字上看到两条答案",
                             authorization.identifier, recordName, rollback.error());
            }
            co_return failWith(AcmeErrorKind::DnsRecordRejected, std::format("域名 {} 的 dns-01 记录 {} 没能写入：{}", authorization.identifier, recordName, published.error()));
        }

        std::optional<AcmeError> solveFailure;
        if (auto solved = co_await client.solveChallenge(authorization.dns01->challengeUrl, m_configuration.challengePollInterval, m_configuration.issuanceTimeout);
            !solved.has_value())
        {
            solveFailure = notedFailure(solved.error());
        }

        // 撤这一步在任何一条自证出口之后都要跑到：先跑完再决定报哪条失败
        auto withdrawn = co_await m_dns01TxtWriter.withdraw(recordName, recordValue);
        if (solveFailure.has_value())
        {
            co_return *solveFailure;
        }
        if (!withdrawn.has_value())
        {
            co_return failWith(AcmeErrorKind::DnsRecordRejected, std::format("域名 {} 的 dns-01 记录 {} 没能撤掉：{}。自证已经过了，但那条 TXT 会留在同一个名字上，请尽快手工删掉",
                                                                             authorization.identifier, recordName, withdrawn.error()));
        }
        co_return std::nullopt;
    }

    Core::Task<std::expected<AcmeIssuedCertificate, AcmeError>> AcmeCertificateManager::issueIfRequired()
    {
        // 配置判据排在最前：这一层最常见的失败是路径或域名没填，而它比任何网络错误都更该先说
        if (m_configuration.domainNames.empty())
        {
            co_return std::unexpected(failWith(AcmeErrorKind::InvalidConfiguration, "ACME 配置里没有要覆盖的域名。把这台服务对外的域名填进 domainNames"));
        }
        if (m_configuration.certificateFile.empty() || m_configuration.privateKeyFile.empty() || m_configuration.accountKeyFile.empty())
        {
            co_return std::unexpected(failWith(AcmeErrorKind::InvalidConfiguration,
                                               "ACME 配置需要证书链、域名私钥与账户私钥三个落点，其中有空的。证书路径还要与服务加载的那条一致，"
                                               "否则续期写到了别处、服务一直在用旧的那份"));
        }
        if (m_configuration.renewBeforeExpiry <= std::chrono::milliseconds::zero())
        {
            co_return std::unexpected(failWith(AcmeErrorKind::InvalidConfiguration,
                                               "renewBeforeExpiry 必须是个正数：它为 0 就等于每次调用都重签一张，机构的速率限制是按「每域名每周几张」算的，"
                                               "那种写法不是激进而是把自己锁死"));
        }

        const auto      now         = std::chrono::system_clock::now();
        const auto      existing    = readExpiryFromDisk();
        const auto      graceWindow = m_configuration.renewBeforeExpiry;
        std::error_code keyExistenceFailure;
        const bool      isPrivateKeyPresent = std::filesystem::exists(m_configuration.privateKeyFile, keyExistenceFailure);
        if (existing.has_value() && isPrivateKeyPresent && *existing - now > graceWindow)
        {
            // 还有富余：一次请求都不发。读不出到期时刻（文件坏或不是 X509）不进这一支,
            // 于是坏文件会被新签的那张覆盖掉
            m_expiryUnixSeconds.store(std::chrono::duration_cast<std::chrono::seconds>(existing->time_since_epoch()).count(), std::memory_order_relaxed);
            co_return AcmeIssuedCertificate{m_configuration.certificateFile, m_configuration.privateKeyFile, *existing, false};
        }

        auto accountKey = ensureAccountKey();
        if (!accountKey.has_value())
        {
            co_return std::unexpected(notedFailure(accountKey.error()));
        }

        // 域名密钥：文件里有就沿用。换密钥不是续期必需的（LE 的建议是换，但它也在换不了时照常工作），
        // 而每次续期都换会让「私钥被谁拿走」这件事更难对齐；要换就删掉那个文件，这是运维的显式动作
        std::expected<AcmeKeyPair, AcmeError> domainKey = [this]() -> std::expected<AcmeKeyPair, AcmeError>
        {
            std::error_code existenceFailure;
            if (std::filesystem::exists(m_configuration.privateKeyFile, existenceFailure))
            {
                return AcmeKeyPair::loadFromFile(m_configuration.privateKeyFile);
            }
            auto generated = AcmeKeyPair::generate(m_configuration.domainKeyAlgorithm);
            if (!generated.has_value())
            {
                return std::unexpected(generated.error());
            }
            auto saved = generated->saveToFile(m_configuration.privateKeyFile);
            if (!saved.has_value())
            {
                return std::unexpected(saved.error());
            }
            return generated;
        }();
        if (!domainKey.has_value())
        {
            co_return std::unexpected(notedFailure(domainKey.error()));
        }

        AcmeClient::Configuration clientConfiguration;
        clientConfiguration.directoryUrl             = m_configuration.directoryUrl;
        clientConfiguration.contactEmailAddress      = m_configuration.contactEmailAddress;
        clientConfiguration.isTermsOfServiceAccepted = m_configuration.isTermsOfServiceAccepted;
        clientConfiguration.externalAccountKeyId     = m_configuration.externalAccountKeyId;
        clientConfiguration.externalAccountKeySecret = m_configuration.externalAccountKeySecret;
        clientConfiguration.requestTimeout           = m_configuration.issuanceTimeout;
        // 通道由「有没有 TXT 写入动作」定，一次签发只走一条：挑了 dns-01 就得有撤的能力，
        // 而只填了一格的动作对按「没有 DNS-01 能力」处置，不会因为 publish 在而 withdraw 不在就跑一半
        const bool isDns01Channel         = m_dns01TxtWriter.isUsable();
        clientConfiguration.challengeKind = isDns01Channel ? AcmeChallengeKind::Dns01 : AcmeChallengeKind::Http01;

        AcmeClient client(m_loop, std::move(clientConfiguration), *accountKey.value());

        if (auto prepared = co_await client.prepareAccount(readPersistedAccountUrl()); !prepared.has_value())
        {
            co_return std::unexpected(notedFailure(prepared.error()));
        }

        auto order = co_await client.createOrder(m_configuration.domainNames);
        if (!order.has_value())
        {
            co_return std::unexpected(notedFailure(order.error()));
        }

        for (const std::string &authorizationUrl: order->authorizationUrls)
        {
            auto authorization = co_await client.fetchAuthorization(authorizationUrl);
            if (!authorization.has_value())
            {
                co_return std::unexpected(notedFailure(authorization.error()));
            }
            // 机构会复用已经 valid 的授权（Boulder 与 Pebble 都会，Pebble 默认按概率复用）。
            // 这时候挂令牌再 POST 会被拒 ——「Cannot update challenge with status valid, only status pending」
            // 是 Boulder 系的原文，所以授权本身已经 valid 就等于这一格自证完成，直接进下一条
            if (authorization->status == "valid")
            {
                continue;
            }
            if (isDns01Channel)
            {
                if (const auto failure = co_await authorizeDns01(client, *authorization); failure.has_value())
                {
                    co_return std::unexpected(*failure);
                }
                continue;
            }
            // 令牌挂出与撤走成对：PresentedToken 的析构负责每条提前返回的出口
            [[maybe_unused]] const PresentedToken guard(m_challengeStore, authorization->http01->token, m_accountKey->jsonWebKeyThumbprint());
            auto solved = co_await client.solveChallenge(authorization->http01->challengeUrl, m_configuration.challengePollInterval, m_configuration.issuanceTimeout);
            if (!solved.has_value())
            {
                co_return std::unexpected(notedFailure(solved.error()));
            }
        }

        auto encodedRequest = domainKey->createCertificateSigningRequest(m_configuration.domainNames);
        if (!encodedRequest.has_value())
        {
            co_return std::unexpected(notedFailure(encodedRequest.error()));
        }

        auto finalized = co_await client.finalizeOrder(*order, *encodedRequest, m_configuration.challengePollInterval, m_configuration.issuanceTimeout);
        if (!finalized.has_value())
        {
            co_return std::unexpected(notedFailure(finalized.error()));
        }

        auto chain = co_await client.fetchCertificateChain(finalized->certificateUrl);
        if (!chain.has_value())
        {
            co_return std::unexpected(notedFailure(chain.error()));
        }

        // 先记账户 URL 再写证书：这两步都幂等，且账户先落账的话，中途崩了下一次不会重复建号
        if (const auto stateFailure = persistAccountState(std::string(client.accountUrl())); stateFailure.has_value())
        {
            co_return std::unexpected(notedFailure(*stateFailure));
        }

        // 先写私钥（域名密钥若刚生成过，saveToFile 已经写过一次，这里不重复覆盖），再原子替换证书链：
        // 中途失败最坏是旧证书配新私钥，服务加载时配对校验会当场拒绝并说清原因，而不是留下身份不明的组合
        std::string writeFailure;
        if (!Platform::AtomicFileWriter::writeText(m_configuration.certificateFile, *chain, std::nullopt, &writeFailure))
        {
            co_return std::unexpected(failWith(AcmeErrorKind::FileSystem, std::format("证书链落盘到 {} 失败：{}", m_configuration.certificateFile.string(), writeFailure)));
        }

        const auto expiry = readExpiryFromDisk();
        if (!expiry.has_value())
        {
            co_return std::unexpected(failWith(AcmeErrorKind::UnexpectedResponse,
                                               std::format("证书写到 {} 之后读不回到期时刻：机构交回的正文可能不是一份合法 PEM", m_configuration.certificateFile.string())));
        }
        m_expiryUnixSeconds.store(std::chrono::duration_cast<std::chrono::seconds>(expiry->time_since_epoch()).count(), std::memory_order_relaxed);

        // 装回服务：这一步没做成就等于「磁盘新、内存旧」，必须按失败交回而不是只在日志里说一句
        if (m_reloadHandler)
        {
            const auto applied = m_reloadHandler();
            if (!applied.has_value())
            {
                co_return std::unexpected(failWith(AcmeErrorKind::ReloadRejected, std::format("证书已写到 {}，但装回服务这一步失败了：{}。线上仍在用旧的那张",
                                                                                              m_configuration.certificateFile.string(), applied.error())));
            }
        } else
        {
            LOG_WARN_FMT("AcmeCertificateManager: 证书已写到 {}，但没有设置装回服务的动作，运行中的服务仍在用旧的那一份。"
                         "首签引导（先签再构造服务器）可以忽略这条，常驻续期则必须设置 reloadHandler",
                         m_configuration.certificateFile.string());
        }

        m_issuanceCount.fetch_add(1, std::memory_order_relaxed);
        m_notBeforeNextAttemptUnix.store(0, std::memory_order_relaxed);
        std::string joinedDomainNames;
        for (const std::string &domainName: m_configuration.domainNames)
        {
            joinedDomainNames += joinedDomainNames.empty() ? "" : ",";
            joinedDomainNames += domainName;
        }
        LOG_INFO_FMT("AcmeCertificateManager: 已为 {} 备好证书，到期时刻的 Unix 秒是 {}（本次是新签发）", joinedDomainNames,
                     std::chrono::duration_cast<std::chrono::seconds>(expiry->time_since_epoch()).count());
        co_return AcmeIssuedCertificate{m_configuration.certificateFile, m_configuration.privateKeyFile, *expiry, true};
    }

    Core::Task<void> AcmeCertificateManager::runRenewalLoop()
    {
        if (!m_reloadHandler)
        {
            recordFailure("续期循环需要「把新证书装回服务」的那个动作：没有它，循环只会每月把新证书写到磁盘，"
                          "线上身份永远是旧的那张。首签引导请直接用 issueIfRequired()，常驻续期要设 reloadHandler");
            co_return;
        }

        Core::Timer timer(m_loop);
        while (!m_isStopping.load(std::memory_order_acquire))
        {
            const auto issued = co_await issueIfRequired();
            // 失败已在 expected 通道里报过、也记进 status()：这里只决定下一轮什么时候再来
            static_cast<void>(issued);

            const long long nowUnix      = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const long long intervalUnix = std::chrono::duration_cast<std::chrono::seconds>(m_configuration.renewalCheckInterval).count();
            // 失败退避与查到期节拍取更晚的那个：机构侧按「每域名每周几张」限流，
            // 把检查间隔调得很小不会更快拿到证书，只会把额度耗光
            const long long wakeAtUnix = std::max(nowUnix + intervalUnix, backoffGateUnixSeconds());
            const auto      delay      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(std::max<long long>(1, wakeAtUnix - nowUnix)));
            // 停放按 kStopNoticeSlice 切片：叫醒这条帧的唯一办法是让它自己醒来，而 stopRenewalLoop()
            // 只落一个标志——整段睡下去时「叫停」要等到下一拍（默认可长达 12 小时）。这期间按文档
            // 等循环停下再拆对象的调用方会一直等（停机挂住），不等就拆对象的调用方会在几小时后被一条
            // 帧踩在 freed memory 上。切片只改醒来频次，签发尝试的节拍仍由 renewalCheckInterval 决定
            for (std::chrono::milliseconds elapsed{0}; elapsed < delay && !m_isStopping.load(std::memory_order_acquire); elapsed += kStopNoticeSlice)
            {
                co_await timer.waitFor(std::min(kStopNoticeSlice, delay - elapsed));
            }
        }
    }
} // namespace AsynGyanis::Net
