#include "Net/Acme/AcmeAutomationConfig.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Net/Acme/AcmeAliyunDns01TxtWriter.h"
#include "Platform/System/ProcessInfo.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// acme 段直接支持的键（写了别的一律当场拒：证书自动化里一个拼错的键往往静默生效成「没配」）
        constexpr std::array<std::string_view, 13> kAcmeKeys{
                "enabled",
                "directory_url",
                "domains",
                "certificate_file",
                "private_key_file",
                "account_key_file",
                "account_state_file",
                "contact_email",
                "tos_accepted",
                "challenge",
                "renew_before_expiry_days",
                "renewal_check_interval_minutes",
                "dns",
        };

        /// acme.dns 子段支持的键
        constexpr std::array<std::string_view, 3> kAcmeDnsKeys{"provider", "domain", "record_ttl_seconds"};

        /// TXT 记录的 TTL 合法区间：下限取 10 秒（低于这个值的记录在机构取答案之前就可能过期），上限是各家共识的一天
        constexpr std::uint64_t kMinimumRecordTtlSeconds = 10U;
        constexpr std::uint64_t kMaximumRecordTtlSeconds = 86400U;

        /**
         * @brief 把允许的键名拼成一句人话，用在「未知键」的报错里
         */
        template<std::size_t Count>
        [[nodiscard]] std::string joinKeys(const std::array<std::string_view, Count> &keys)
        {
            std::string text;
            for (const std::string_view key: keys)
            {
                text += text.empty() ? "" : "、";
                text += key;
            }
            return text;
        }

        /**
         * @brief 拒掉段里出现过的未知键
         * @param node 段本身
         * @param known 允许的键名表
         * @param sectionPath 报错时写的键路径
         */
        template<std::size_t Count>
        void rejectUnknownKeys(const Base::ConfigValue &node, const std::array<std::string_view, Count> &known, const std::string &sectionPath)
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                const std::string_view name = it.key();
                if (std::find(known.begin(), known.end(), name) == known.end())
                {
                    throw Base::ConfigValidationException(sectionPath + "." + std::string(name), std::format("不是 {} 支持的键。支持的键：{}", sectionPath, joinKeys(known)));
                }
            }
        }

        /**
         * @brief 取一个必填的字符串值（空串按缺失处理）
         */
        [[nodiscard]] std::string requireString(const Base::ConfigValue &node, const std::string_view key, const std::string &sectionPath)
        {
            const std::string keyPath = sectionPath + "." + std::string(key);
            if (!node.contains(key))
            {
                throw Base::ConfigValidationException(keyPath, "开了 acme.enabled 就必须给这一项");
            }
            const Base::ConfigValue &value = node.at(key);
            if (!value.is_string())
            {
                throw Base::ConfigValidationException(keyPath, "必须是字符串");
            }
            std::string text = value.get<std::string>();
            if (text.empty())
            {
                throw Base::ConfigValidationException(keyPath, "不能是空串：空串等于没配，而这一项没有「留空即默认」的读法");
            }
            return text;
        }

        /**
         * @brief 取一个可选的字符串值
         */
        [[nodiscard]] std::string optionalString(const Base::ConfigValue &node, const std::string_view key, const std::string &sectionPath, std::string fallback = {})
        {
            if (!node.contains(key))
            {
                return fallback;
            }
            const std::string keyPath = sectionPath + "." + std::string(key);
            if (!node.at(key).is_string())
            {
                throw Base::ConfigValidationException(keyPath, "必须是字符串");
            }
            return node.at(key).get<std::string>();
        }

        /**
         * @brief 取一个布尔值（缺失时给默认）
         */
        [[nodiscard]] bool optionalBoolean(const Base::ConfigValue &node, const std::string_view key, const std::string &sectionPath, bool fallback)
        {
            if (!node.contains(key))
            {
                return fallback;
            }
            const Base::ConfigValue &value = node.at(key);
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(sectionPath + "." + std::string(key), "必须是 true 或 false（\"yes\"/1 这类写法不算）");
            }
            return value.get<bool>();
        }

        /**
         * @brief 取一个非负整数并判上界
         */
        [[nodiscard]] std::uint64_t optionalInteger(const Base::ConfigValue &node, const std::string_view key, const std::string &sectionPath, std::uint64_t fallback,
                                                    std::uint64_t upperBound)
        {
            if (!node.contains(key))
            {
                return fallback;
            }
            const std::string        keyPath = sectionPath + "." + std::string(key);
            const Base::ConfigValue &value   = node.at(key);
            if (!value.is_number_integer() && !value.is_number_unsigned())
            {
                throw Base::ConfigValidationException(keyPath, "必须是整数（不能写成字符串或小数）");
            }
            if (value.is_number_integer() && value.get<std::int64_t>() < 0)
            {
                throw Base::ConfigValidationException(keyPath, "不能是负数");
            }
            const std::uint64_t number = value.is_number_integer() ? static_cast<std::uint64_t>(value.get<std::int64_t>()) : value.get<std::uint64_t>();
            if (number > upperBound)
            {
                throw Base::ConfigValidationException(keyPath, std::format("必须在 0-{} 之间，当前是 {}", upperBound, number));
            }
            return number;
        }

        /**
         * @brief 把域名列表读进 vector；每一项都必须是非空字符串
         */
        [[nodiscard]] std::vector<std::string> requireDomainList(const Base::ConfigValue &node, const std::string &sectionPath)
        {
            const std::string keyPath = sectionPath + ".domains";
            if (!node.contains("domains"))
            {
                throw Base::ConfigValidationException(keyPath, "开了 acme.enabled 就必须列出要覆盖的域名");
            }
            const Base::ConfigValue &value = node.at("domains");
            if (!value.is_array() || value.empty())
            {
                throw Base::ConfigValidationException(keyPath, "必须是一个非空的字符串数组（一张证书至少要有一个名字）");
            }
            std::vector<std::string> domainNames;
            for (const Base::ConfigValue &element: value)
            {
                if (!element.is_string() || element.get<std::string>().empty())
                {
                    throw Base::ConfigValidationException(keyPath, "数组的每一项都必须是非空字符串");
                }
                domainNames.push_back(element.get<std::string>());
            }
            return domainNames;
        }

        /**
         * @brief 目录 URL 只看形状，不联网
         * @details 拼错的 URL 会在第一次签发时才失败，而那时日志里只剩一条「取目录失败」；
         *          形状在读配置时就能判，所以判在这里
         */
        void requireDirectoryUrlShape(const std::string &directoryUrl)
        {
            constexpr std::string_view kHttpsPrefix = "https://";
            constexpr std::string_view kHttpPrefix  = "http://";
            if (!directoryUrl.starts_with(kHttpsPrefix) && !directoryUrl.starts_with(kHttpPrefix))
            {
                throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".directory_url",
                                                      std::format("必须以 {} 或 {} 开头，当前是「{}」。生产一律 https", kHttpsPrefix, kHttpPrefix, directoryUrl));
            }
            if (directoryUrl.size() == kHttpsPrefix.size() || directoryUrl.size() == kHttpPrefix.size())
            {
                throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".directory_url", "前缀之后没有主机名");
            }
        }

        /**
         * @brief 联系邮箱：接受带不带 mailto: 两种写法，缺前缀时补上
         * @details 管理器要的是 RFC 8555 §7.3 的 `mailto:` 形式；让运维自己记得写前缀，
         *          结局通常是机构把账户联系人存成一个无法回话的字符串
         */
        [[nodiscard]] std::string normalizeContactEmail(std::string contactEmail)
        {
            constexpr std::string_view kMailtoPrefix = "mailto:";
            if (contactEmail.find('@') == std::string::npos)
            {
                throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".contact_email",
                                                      std::format("看着不是邮箱地址：「{}」。机构在证书到期或账户异常时靠它找到你", contactEmail));
            }
            if (!contactEmail.starts_with(kMailtoPrefix))
            {
                contactEmail.insert(0, std::string(kMailtoPrefix));
            }
            return contactEmail;
        }

        /**
         * @brief 读一条环境变量，缺失按空串
         * @details 走 Platform 那份而不是 `std::getenv`：Windows 侧要按代码页取宽字符环境块
         *          （`getenv` 既被判不安全弃用，也读不到非 ACP 的值），而这类读取只该有一处实现
         */
        [[nodiscard]] std::string environmentValue(const std::string_view name)
        {
            const std::optional<std::string> value = Platform::ProcessInfo::environmentVariable(std::string(name));
            return value.has_value() ? *value : std::string{};
        }
    } // namespace

    AcmeAutomationConfiguration readAcmeConfiguration(const Base::ConfigValue &configurationRoot)
    {
        AcmeAutomationConfiguration configuration;
        if (!configurationRoot.is_object() || !configurationRoot.contains(kAcmeConfigSection))
        {
            return configuration;
        }

        const Base::ConfigValue &section = configurationRoot.at(kAcmeConfigSection);
        const std::string        path    = std::string(kAcmeConfigSection);
        if (!section.is_object())
        {
            throw Base::ConfigValidationException(path, "必须是一个对象（形如 acme = { enabled = true, ... }）");
        }
        rejectUnknownKeys(section, kAcmeKeys, path);

        configuration.isEnabled = optionalBoolean(section, "enabled", path, false);
        if (!configuration.isEnabled)
        {
            // 关着的时候其余字段一概不校验：一份留着但关掉的管理器配置，不该在每次启动时挡路
            if (section.contains("dns"))
            {
                throw Base::ConfigValidationException(path + ".dns", "acme.enabled 是 false，这一子段没人读：要么把开关打开，要么把 dns 这一段删掉");
            }
            return configuration;
        }

        configuration.manager.directoryUrl = requireString(section, "directory_url", path);
        requireDirectoryUrlShape(configuration.manager.directoryUrl);
        configuration.manager.domainNames = requireDomainList(section, path);

        configuration.manager.certificateFile     = requireString(section, "certificate_file", path);
        configuration.manager.privateKeyFile      = requireString(section, "private_key_file", path);
        configuration.manager.accountKeyFile      = requireString(section, "account_key_file", path);
        configuration.manager.accountStateFile    = requireString(section, "account_state_file", path);
        configuration.manager.contactEmailAddress = normalizeContactEmail(requireString(section, "contact_email", path));
        // 接受条款是一个有法律含义的动作：不能由配置解析器替谁默认，也不能由「反正别处也是 true」带过去
        configuration.manager.isTermsOfServiceAccepted = optionalBoolean(section, "tos_accepted", path, false);
        if (!configuration.manager.isTermsOfServiceAccepted)
        {
            throw Base::ConfigValidationException(path + ".tos_accepted", "必须是 true：接受机构的服务条款是一个有法律含义的动作，得由部署的人显式写下来，库不替谁默认同意");
        }

        const std::string challenge = optionalString(section, "challenge", path, std::string{kAcmeChallengeHttp01});
        if (challenge == kAcmeChallengeHttp01)
        {
            configuration.challengeKind = AcmeChallengeKind::Http01;
        } else if (challenge == kAcmeChallengeDns01)
        {
            configuration.challengeKind = AcmeChallengeKind::Dns01;
        } else
        {
            throw Base::ConfigValidationException(path + ".challenge", std::format("只接受 {} 或 {}，当前是「{}」。第三种值不能悄悄按默认跑——那会把「以为在测 dns-01」读成走过",
                                                                                   kAcmeChallengeHttp01, kAcmeChallengeDns01, challenge));
        }

        const std::uint64_t renewDays = optionalInteger(section, "renew_before_expiry_days", path, 30U, 3650U);
        if (renewDays == 0U)
        {
            throw Base::ConfigValidationException(path + ".renew_before_expiry_days",
                                                  "不能是 0：到期前 0 天续期等于每次启动都重签一张，而机构的速率限制是按「每域名每周几张」算的");
        }
        configuration.manager.renewBeforeExpiry = std::chrono::hours{24 * static_cast<long>(renewDays)};

        const std::uint64_t checkMinutes = optionalInteger(section, "renewal_check_interval_minutes", path, 720U, 60U * 24U * 30U);
        if (checkMinutes == 0U)
        {
            throw Base::ConfigValidationException(path + ".renewal_check_interval_minutes", "不能是 0：零间隔等于让续期循环空转");
        }
        configuration.manager.renewalCheckInterval = std::chrono::minutes{static_cast<long>(checkMinutes)};

        if (const Base::ConfigValue *dns = section.contains("dns") ? &section.at("dns") : nullptr; dns != nullptr)
        {
            if (!dns->is_object())
            {
                throw Base::ConfigValidationException(path + ".dns", "必须是一个对象");
            }
            if (!configuration.usesDns01())
            {
                throw Base::ConfigValidationException(
                        path + ".dns", std::format("challenge 是 {}，这一子段没人读：要么把 challenge 改成 {}，要么删掉 dns 这一段", kAcmeChallengeHttp01, kAcmeChallengeDns01));
            }
            rejectUnknownKeys(*dns, kAcmeDnsKeys, path + ".dns");

            configuration.dnsProvider = requireString(*dns, "provider", path + ".dns");
            if (configuration.dnsProvider != kAcmeDnsProviderAliyun)
            {
                throw Base::ConfigValidationException(path + ".dns.provider",
                                                      std::format("目前只有 {} 这一家有实现，当前是「{}」", kAcmeDnsProviderAliyun, configuration.dnsProvider));
            }
            configuration.dnsZoneDomainName   = optionalString(*dns, "domain", path + ".dns");
            configuration.dnsRecordTtlSeconds = static_cast<std::uint32_t>(optionalInteger(*dns, "record_ttl_seconds", path + ".dns", 600U, kMaximumRecordTtlSeconds));
            if (configuration.dnsRecordTtlSeconds < kMinimumRecordTtlSeconds)
            {
                throw Base::ConfigValidationException(path + ".dns.record_ttl_seconds",
                                                      std::format("不得低于 {} 秒：低于这个值时，机构取答案的一刻记录可能已经过期", kMinimumRecordTtlSeconds));
            }
        } else if (configuration.usesDns01())
        {
            throw Base::ConfigValidationException(path + ".dns", std::format("challenge 是 {} 却没配 dns 段：没有提供方就没法把 TXT 写出去", kAcmeChallengeDns01));
        }

        return configuration;
    }

    AcmeDns01TxtWriter buildDns01TxtWriter(Core::EventLoop &loop, const AcmeAutomationConfiguration &configuration)
    {
        if (!configuration.usesDns01())
        {
            throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".challenge", "这份配置走的是 http-01，不需要 TXT 写入动作");
        }
        if (configuration.dnsProvider != kAcmeDnsProviderAliyun)
        {
            throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".dns.provider", std::format("不认识提供方「{}」", configuration.dnsProvider));
        }

        const std::string accessKeyId     = environmentValue(kDnsAccessKeyIdEnvironmentVariable);
        const std::string accessKeySecret = environmentValue(kDnsAccessKeySecretEnvironmentVariable);
        if (accessKeyId.empty() || accessKeySecret.empty())
        {
            throw Base::ConfigValidationException(std::string(kAcmeConfigSection) + ".dns",
                                                  std::format("缺环境变量 {} 或 {}（两者都非空才起步）。密钥刻意不给配置文件留位置：一份能改域名记录的钥匙"
                                                              "进了版本库等于把域名交出去，而命令行上的参数会进进程列表与 shell 历史",
                                                              kDnsAccessKeyIdEnvironmentVariable, kDnsAccessKeySecretEnvironmentVariable));
        }

        AliyunDns01Configuration dns;
        dns.accessKeyId      = accessKeyId;
        dns.accessKeySecret  = accessKeySecret;
        dns.zoneDomainName   = configuration.dnsZoneDomainName;
        dns.recordTtlSeconds = configuration.dnsRecordTtlSeconds;
        return makeAliyunDns01TxtWriter(loop, std::move(dns));
    }

    namespace
    {
        /**
         * @brief 把两条路径折成可比的形式
         * @details 逐字比会把「同一条路径的两种写法」判成不一致，从而挡下一次合法启动（配置里写
         *          `./certs/chain.pem`、命令行给 `certs/chain.pem` 是同一张）。规范化失败时退回原值：
         *          那种情况下逐字一致仍是充分的放行证据，不一致就照常报出来让人看真实的两条路径。
         */
        [[nodiscard]] std::filesystem::path comparablePath(const std::filesystem::path &candidate)
        {
            std::error_code             ignore;
            const std::filesystem::path normalized = std::filesystem::weakly_canonical(candidate, ignore);
            return ignore ? candidate : normalized;
        }

        /// 路径的给人读写法：空值要说清是「没给」而不是一条空路径
        [[nodiscard]] std::string describePath(const std::filesystem::path &candidate)
        {
            return candidate.empty() ? std::string{"（没给）"} : candidate.string();
        }
    } // namespace

    std::expected<void, std::string> validateAcmeAssembly(const AcmeAutomationConfiguration &configuration, const AcmeAssemblyFacts &facts)
    {
        if (!configuration.isEnabled)
        {
            return {};
        }

        if (facts.runsMultipleWorkerProcesses && !facts.picksUpCertificateFromDisk)
        {
            return std::unexpected(std::format("{} 在多 worker 进程里落不下去：签发只该归一个进程做（N 份管理器会各撞一次机构，速率限制按账户计，不按进程），"
                                               "而签发的那个进程改的是磁盘——本进程既不会装回自己签的那张，也不会盯住那张文件的变化，线上身份永远是旧的那份。"
                                               "给本进程装上跟随通道（Net::followCertificateRotation，按文件身份变化重装）或改为单进程跑",
                                               kAcmeConfigSection));
        }
        if (!facts.hasTlsListener)
        {
            return std::unexpected(std::format("{} 开了但本进程没有 TLS 监听器可装回：续期循环会因为「没有装回的对象」而自己收口，磁盘上换多少张都与线上身份无关。"
                                               "要么给这台服务配上 HTTPS（或 h3）监听器，要么把 {}.enabled 改回 false",
                                               kAcmeConfigSection, kAcmeConfigSection));
        }
        if (!configuration.usesDns01() && !facts.hasPublicPlaintextListener)
        {
            return std::unexpected(std::format("走 {} 需要一台公网可达的明文监听器（机构按 80 端口取 /.well-known/acme-challenge/ 下的令牌，只挂在 443 上等于没答）。"
                                               "本进程没有这样一台口：把 {}.challenge 改成 {}（不需要任何入站通路），或让明文 80 口与这台服务同处一个进程",
                                               kAcmeChallengeHttp01, kAcmeConfigSection, kAcmeChallengeDns01));
        }

        // 这条排在最后：前三条拒的是「根本没有可装回的对象」，那比路径写歪更要紧，报出来也更该先看见
        if (comparablePath(facts.listenerCertificateFile) != comparablePath(configuration.manager.certificateFile) ||
            comparablePath(facts.listenerPrivateKeyFile) != comparablePath(configuration.manager.privateKeyFile))
        {
            return std::unexpected(std::format("监听器加载的证书/私钥是 {} / {}，而 {} 把新证书写到 {} / {}：`reloadCertificate()` 按监听器原来那条路径重读，"
                                               "签完的那张永远装不上去。两边必须写成同一条路径（开了证书自动化之后，服务端证书路径应由 acme 的落点决定）",
                                               describePath(facts.listenerCertificateFile), describePath(facts.listenerPrivateKeyFile), kAcmeConfigSection,
                                               describePath(configuration.manager.certificateFile), describePath(configuration.manager.privateKeyFile)));
        }
        return {};
    }
} // namespace AsynGyanis::Net
