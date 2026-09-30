// acme 段的读取用例：形状与交叉判据都在读配置这一刻判掉，不留到第一次签发才报「取目录失败」。
// 每一条拒绝都写成「配了但不会生效」的形状——那类偏差在没人手跑签发的机器上是静默的。
#include "Net/Acme/AcmeAutomationConfig.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#if ASYN_PLATFORM_WIN32
#define ASYN_TEST_SET_ENV(name, value) _putenv_s((name), (value))
#define ASYN_TEST_UNSET_ENV(name) _putenv_s((name), "")
#else
#define ASYN_TEST_SET_ENV(name, value) ::setenv((name), (value), 1)
#define ASYN_TEST_UNSET_ENV(name) ::unsetenv((name))
#endif

namespace AsynGyanis::Net
{
    namespace
    {
        [[nodiscard]] Base::ConfigValue object(Base::ConfigObject members)
        {
            return Base::ConfigValue(std::move(members));
        }

        /// 一份「开了且合法」的 http-01 段，各用例在它上面改一处
        [[nodiscard]] Base::ConfigValue validSection(Base::ConfigObject overrides = {})
        {
            Base::ConfigObject section{
                    {"enabled", true},
                    {"directory_url", "https://acme-v02.api.letsencrypt.org/directory"},
                    {"domains", Base::ConfigValue(std::vector<std::string>{"gyanis.space"})},
                    {"certificate_file", "/etc/certs/chain.pem"},
                    {"private_key_file", "/etc/certs/domain-key.pem"},
                    {"account_key_file", "/var/lib/acme/account-key.pem"},
                    {"account_state_file", "/var/lib/acme/account.json"},
                    {"contact_email", "ops@gyanis.space"},
                    {"tos_accepted", true},
            };
            for (auto &[name, value]: overrides)
            {
                section[name] = std::move(value);
            }
            return object(std::move(section));
        }

        [[nodiscard]] Base::ConfigValue documentWith(const Base::ConfigValue &acmeSection)
        {
            return object({{"acme", acmeSection}});
        }

        /// 断言「读这份文档会抛」并把抛出的消息交回来，供逐条点名判据
        [[nodiscard]] std::string rejectionMessage(const Base::ConfigValue &document)
        {
            try
            {
                static_cast<void>(readAcmeConfiguration(document));
            } catch (const Base::ConfigValidationException &failure)
            {
                return failure.what();
            }
            return {};
        }

        void expectRejected(const Base::ConfigValue &document, const std::string &needle)
        {
            const std::string message = rejectionMessage(document);
            EXPECT_FALSE(message.empty()) << "这份配置本该被拒，却读成功了";
            EXPECT_NE(message.find(needle), std::string::npos) << "拒绝原因没点名「" << needle << "」，实际是：" << message;
        }
    } // namespace

    /**
     * @brief 钉住：没有 acme 段时整段按关闭处理，其余取默认
     */
    TEST(AcmeAutomationConfig, AbsentSectionLeavesAutomationDisabled)
    {
        const auto configuration = readAcmeConfiguration(object({{"server", object({})}}));
        EXPECT_FALSE(configuration.isEnabled);
        EXPECT_FALSE(configuration.usesDns01());
        EXPECT_TRUE(configuration.manager.directoryUrl.empty());
    }

    /**
     * @brief 钉住：关着的段不校验其余项，但也不允许留一份没人读的子段
     */
    TEST(AcmeAutomationConfig, DisabledSectionSkipsValidationButNotOrphanSubsections)
    {
        const auto off = readAcmeConfiguration(documentWith(object({{"enabled", false}})));
        EXPECT_FALSE(off.isEnabled);
        EXPECT_TRUE(off.manager.certificateFile.empty()) << "关着时不该替谁把路径填上";

        expectRejected(documentWith(object({{"enabled", false}, {"dns", object({{"provider", "aliyun"}})}})), "acme.dns");
    }

    /**
     * @brief 钉住：一份完整的 http-01 段被逐字搬到管理器配置上
     * @details 特别钉联系人的 `mailto:` 补全与两个时长：这两处是「配置读到了但意思变了」的高发点
     */
    TEST(AcmeAutomationConfig, ReadsHttp01SectionIntoManagerConfiguration)
    {
        const auto configuration = readAcmeConfiguration(documentWith(validSection()));

        ASSERT_TRUE(configuration.isEnabled);
        EXPECT_EQ(configuration.manager.directoryUrl, "https://acme-v02.api.letsencrypt.org/directory");
        ASSERT_EQ(configuration.manager.domainNames.size(), 1U);
        EXPECT_EQ(configuration.manager.domainNames.at(0), "gyanis.space");
        EXPECT_EQ(configuration.manager.certificateFile.string(), "/etc/certs/chain.pem");
        EXPECT_EQ(configuration.manager.privateKeyFile.string(), "/etc/certs/domain-key.pem");
        EXPECT_EQ(configuration.manager.accountKeyFile.string(), "/var/lib/acme/account-key.pem");
        EXPECT_EQ(configuration.manager.accountStateFile.string(), "/var/lib/acme/account.json");
        EXPECT_EQ(configuration.manager.contactEmailAddress, "mailto:ops@gyanis.space");
        EXPECT_TRUE(configuration.manager.isTermsOfServiceAccepted);
        EXPECT_FALSE(configuration.usesDns01());
        // 没写的两项取内置默认：30 天窗口与 12 小时节拍（与 Let's Encrypt 的 90 天寿命相配）
        EXPECT_EQ(configuration.manager.renewBeforeExpiry, std::chrono::hours{24 * 30});
        EXPECT_EQ(configuration.manager.renewalCheckInterval, std::chrono::minutes{720});
    }

    /**
     * @brief 钉住：dns-01 段的提供方口径读到位
     */
    TEST(AcmeAutomationConfig, ReadsDns01SubSection)
    {
        const auto configuration = readAcmeConfiguration(documentWith(validSection({
                {"challenge", "dns-01"},
                {"dns", object({{"provider", "aliyun"}, {"domain", "gyanis.space"}, {"record_ttl_seconds", 120}})},
        })));

        EXPECT_TRUE(configuration.usesDns01());
        EXPECT_EQ(configuration.dnsProvider, "aliyun");
        EXPECT_EQ(configuration.dnsZoneDomainName, "gyanis.space");
        EXPECT_EQ(configuration.dnsRecordTtlSeconds, 120U);
    }

    /**
     * @brief 钉住：未知键当场拒，并把允许的键列出来
     * @details 证书自动化里一个拼错的键几乎总是静默生效成「没配」——比如 `certificate_path`
     *          拼错时端点仍会去写默认的空路径，症状要到第一次续期才暴露
     */
    TEST(AcmeAutomationConfig, RejectsUnknownKeyAndListsSupportedKeys)
    {
        const std::string message = rejectionMessage(documentWith(validSection({{"certificate_path", "/etc/certs/chain.pem"}})));
        EXPECT_NE(message.find("certificate_path"), std::string::npos) << message;
        EXPECT_NE(message.find("certificate_file"), std::string::npos) << "报错要把允许的键一起交出来：" << message;
    }

    /**
     * @brief 钉住：服务条款没显式接受就拒，缺省与 false 同义
     */
    TEST(AcmeAutomationConfig, RejectsUnlessTermsOfServiceExplicitlyAccepted)
    {
        Base::ConfigObject withoutTos = {};
        auto               section    = validSection();
        section.erase("tos_accepted");
        expectRejected(documentWith(section), "tos_accepted");

        expectRejected(documentWith(validSection({{"tos_accepted", false}})), "tos_accepted");
        static_cast<void>(withoutTos);
    }

    /**
     * @brief 钉住：challenge 只认两个值，第三种写法不能悄悄按默认跑
     */
    TEST(AcmeAutomationConfig, RejectsUnknownChallengeKind)
    {
        expectRejected(documentWith(validSection({{"challenge", "tls-alpn-01"}})), "tls-alpn-01");
        expectRejected(documentWith(validSection({{"challenge", "dns"}})), "只接受 http-01 或 dns-01");
    }

    /**
     * @brief 钉住：dns 子段与 challenge 必须成对，两个方向都拒
     */
    TEST(AcmeAutomationConfig, RejectsMismatchedChallengeAndDnsSubSection)
    {
        // 配了 dns 段却走 http-01：那一份提供方口径永远不会被读
        expectRejected(documentWith(validSection({{"dns", object({{"provider", "aliyun"}})}})), "challenge 是 http-01");
        // 走了 dns-01 却没有提供方：TXT 无处可写
        expectRejected(documentWith(validSection({{"challenge", "dns-01"}})), "没配 dns 段");
    }

    /**
     * @brief 钉住：提供方只有一家有实现，写别的一律拒而不是回落
     * @details 回落到「猜一家」会把一份配错的管理器变成往陌生服务商发凭据的客户端
     */
    TEST(AcmeAutomationConfig, RejectsUnsupportedDnsProvider)
    {
        expectRejected(documentWith(validSection({
                               {"challenge", "dns-01"},
                               {"dns", object({{"provider", "cloudflare"}})},
                       })),
                       "cloudflare");
    }

    /**
     * @brief 钉住：两个 0 都拒，各自的理由不同
     */
    TEST(AcmeAutomationConfig, RejectsZeroIntervalsWithTheirOwnReasons)
    {
        expectRejected(documentWith(validSection({{"renew_before_expiry_days", 0}})), "每次启动都重签");
        expectRejected(documentWith(validSection({{"renewal_check_interval_minutes", 0}})), "空转");
    }

    /**
     * @brief 钉住：TTL 的两端都判，且类型严格
     */
    TEST(AcmeAutomationConfig, BoundsAndTypesOnRecordTtl)
    {
        expectRejected(documentWith(validSection({
                               {"challenge", "dns-01"},
                               {"dns", object({{"provider", "aliyun"}, {"record_ttl_seconds", 5}})},
                       })),
                       "不得低于 10 秒");
        expectRejected(documentWith(validSection({
                               {"challenge", "dns-01"},
                               {"dns", object({{"provider", "aliyun"}, {"record_ttl_seconds", "600"}})},
                       })),
                       "必须是整数");
    }

    /**
     * @brief 钉住：目录 URL 的形状在读配置时就判
     * @details 拼错的 URL 到第一次签发才失败，而那时日志里只剩一条「取 ACME 目录失败」
     */
    TEST(AcmeAutomationConfig, RejectsMalformedDirectoryUrl)
    {
        expectRejected(documentWith(validSection({{"directory_url", "acme-v02.api.letsencrypt.org/directory"}})), "必须以 https:// 或 http:// 开头");
        expectRejected(documentWith(validSection({{"directory_url", "https://"}})), "没有主机名");
    }

    /**
     * @brief 钉住：域名列表与四个落点都是必填，空值不静默取默认
     */
    TEST(AcmeAutomationConfig, RejectsMissingDomainsAndPaths)
    {
        expectRejected(documentWith(validSection({{"domains", Base::ConfigValue(Base::ConfigObject{})}})), "domains");
        expectRejected(documentWith(validSection({{"certificate_file", ""}})), "不能是空串");
        auto section = validSection();
        section.erase("account_state_file");
        expectRejected(documentWith(section), "account_state_file");
    }

    /**
     * @brief 钉住：联系人长得像邮箱才收，且补全 mailto: 前缀
     */
    TEST(AcmeAutomationConfig, NormalizesOrRejectsContactEmail)
    {
        EXPECT_EQ(readAcmeConfiguration(documentWith(validSection({{"contact_email", "mailto:ops@gyanis.space"}}))).manager.contactEmailAddress, "mailto:ops@gyanis.space");
        expectRejected(documentWith(validSection({{"contact_email", "ops-gyanis"}})), "不是邮箱地址");
    }

    /**
     * @brief 钉住：TXT 动作对的凭据只从环境进，缺任一条当场拒
     */
    TEST(AcmeDns01TxtWriterBuilder, RequiresBothCredentialsFromEnvironment)
    {
        const auto dnsConfiguration = readAcmeConfiguration(documentWith(validSection({
                {"challenge", "dns-01"},
                {"dns", object({{"provider", "aliyun"}})},
        })));

        Core::EventLoop loop;
        ASYN_TEST_UNSET_ENV("ASYN_ACME_DNS_ACCESS_KEY_ID");
        ASYN_TEST_UNSET_ENV("ASYN_ACME_DNS_ACCESS_KEY_SECRET");
        try
        {
            static_cast<void>(buildDns01TxtWriter(loop, dnsConfiguration));
            ADD_FAILURE() << "凭据两条都缺时本该拒，却造出了动作对";
        } catch (const Base::ConfigValidationException &failure)
        {
            EXPECT_NE(std::string(failure.what()).find("ASYN_ACME_DNS_ACCESS_KEY_ID"), std::string::npos) << failure.what();
        }

        ASYN_TEST_SET_ENV("ASYN_ACME_DNS_ACCESS_KEY_ID", "LTAI-example");
        try
        {
            static_cast<void>(buildDns01TxtWriter(loop, dnsConfiguration));
            ADD_FAILURE() << "只有一条凭据时同样该拒：一半凭据签不出任何请求";
        } catch (const Base::ConfigValidationException &failure)
        {
            EXPECT_NE(std::string(failure.what()).find("ASYN_ACME_DNS_ACCESS_KEY_SECRET"), std::string::npos) << failure.what();
        }

        ASYN_TEST_SET_ENV("ASYN_ACME_DNS_ACCESS_KEY_SECRET", "secret-example");
        const auto writer = buildDns01TxtWriter(loop, dnsConfiguration);
        EXPECT_TRUE(writer.isUsable()) << "两条都齐时该交回两格填满的动作对";

        ASYN_TEST_UNSET_ENV("ASYN_ACME_DNS_ACCESS_KEY_ID");
        ASYN_TEST_UNSET_ENV("ASYN_ACME_DNS_ACCESS_KEY_SECRET");
    }

    /**
     * @brief 钉住：走 http-01 的配置来要 TXT 动作对，按用法错误拒
     */
    TEST(AcmeDns01TxtWriterBuilder, RefusesForHttp01Configuration)
    {
        const auto      httpConfiguration = readAcmeConfiguration(documentWith(validSection()));
        Core::EventLoop loop;
        try
        {
            static_cast<void>(buildDns01TxtWriter(loop, httpConfiguration));
            ADD_FAILURE() << "http-01 的配置不该能造出 TXT 动作对";
        } catch (const Base::ConfigValidationException &failure)
        {
            EXPECT_NE(std::string(failure.what()).find("http-01"), std::string::npos) << failure.what();
        }
    }
} // namespace AsynGyanis::Net
