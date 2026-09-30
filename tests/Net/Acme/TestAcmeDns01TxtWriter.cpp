// dns-01 的 TXT 动作面用例：记录名怎么算、阿里云的编码与签名对不对得上独立实现、
// 以及「凭据缺失」与「主域名推不出」这两条出口有没有各归其位。
// 签名的期望值一律由仓库外的两份实现（Node 的 crypto 与 Python 的 hmac/hashlib）算出后写死：
// 拿本仓库的 helper 算期望值等于让规范怎么改都绿。
#include "Net/Acme/AcmeAliyunDns01TxtWriter.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Acme/AcmeDns01TxtWriter.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "MetricsTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 阿里云的百分号编码形状：与通用 URL 编码那三处差别是签名能不能对上的关键
     */
    TEST(AliyunPercentEncode, MatchesRfc3986Shape)
    {
        EXPECT_EQ(aliyunPercentEncode(""), "");
        EXPECT_EQ(aliyunPercentEncode("a b"), "a%20b") << "空格要编成 %20，编成 + 是表单编码而不是 RFC 3986";
        EXPECT_EQ(aliyunPercentEncode("a*b"), "a%2Ab") << "星号必须编，阿里云那份规则专门点过它";
        EXPECT_EQ(aliyunPercentEncode("a~b"), "a~b") << "波浪号不编，编成 %7E 就对不上";
        EXPECT_EQ(aliyunPercentEncode("a+b"), "a%2Bb");
        EXPECT_EQ(aliyunPercentEncode("/"), "%2F");
        EXPECT_EQ(aliyunPercentEncode("100%"), "100%25");
        EXPECT_EQ(aliyunPercentEncode("a=b"), "a%3Db");
        EXPECT_EQ(aliyunPercentEncode("a b/c?d&e=f"), "a%20b%2Fc%3Fd%26e%3Df");
        EXPECT_EQ(aliyunPercentEncode("_acme-challenge.gyanis.space"), "_acme-challenge.gyanis.space") << "记录名本身不该被改动";
        // 非 ASCII 按 UTF-8 逐字节编：例=E4 BE 8B，え=E3 81 88
        EXPECT_EQ(aliyunPercentEncode("\xE4\xBE\x8B\xE3\x81\x88.jp"), "%E4%BE%8B%E3%81%88.jp");
        EXPECT_EQ(aliyunPercentEncode("\xE5\xAF\x86\xE9\x92\xA5&=x"), "%E5%AF%86%E9%92%A5%26%3Dx");
        // 十六进制是大写：小写的 %2f 在服务端那份串里不是同一个字符
        EXPECT_EQ(aliyunPercentEncode(std::string(1, '\x7F')), "%7F");
    }

    /**
     * @brief RPC 风格签名的三条独立算好的期望值
     * @details 覆盖面：一条是文档那个形状的完整参数表（含大小写混排的键，验证按 ASCII 序拼串），
     *          一条是空参数表配空密钥（最容易被「空串就走别的路径」写歪的一档），
     *          一条是值里带需要编码字符的（拼串与再编码的顺序错不了）
     */
    TEST(AliyunRpcSignature, MatchesIndependentlyComputedVectors)
    {
        const std::map<std::string, std::string> documentedCase{
                {"Action", "AddDomainRecord"},
                {"AccessKeyId", "testid"},
                {"DomainName", "gyanis.space"},
                {"Format", "JSON"},
                {"RR", "_acme-challenge"},
                {"RegionId", "cn-hangzhou"},
                {"SignatureMethod", "HMAC-SHA1"},
                {"SignatureNonce", "3ee8c1b8-93d1-4c37-a9c6-xxxxxxxx"},
                {"SignatureVersion", "1.0"},
                {"TTL", "600"},
                {"Timestamp", "2026-09-30T06:00:00Z"},
                {"Type", "TXT"},
                {"Value", "dX5E6p2g-s6gJ3lN0aQe1rTw8yU3iO4pA5sD6fG7hI0"},
                {"Version", "2015-01-09"},
        };
        EXPECT_EQ(aliyunRpcSignature("GET", documentedCase, "testsecret"), "/B2/v/mJB+jJeqGvWlPpx/sd5Mc=");

        EXPECT_EQ(aliyunRpcSignature("GET", {}, ""), "9uWlYLdCnrqTnAxZowDkuU1h2og=");

        const std::map<std::string, std::string> needsEncoding{
                {"Aa", "x*"}, {"Action", "DescribeDomainRecords"}, {"DomainName", "a b*c~d"}, {"Value", "100%.="}, {"Zz", "last"},
        };
        // 拼出来的规范串是 Aa=x%2A&Action=DescribeDomainRecords&DomainName=a%20b%2Ac~d&Value=100%25.%3D&Zz=last
        EXPECT_EQ(aliyunRpcSignature("GET", needsEncoding, "s e c"), "zqWsPanftH3JDsOD3naCTYznhtc=");
    }

    /**
     * @brief 通配符要先摘掉 "*."：它与裸域名是同一条 TXT
     */
    TEST(Dns01RecordName, StripsTheWildcardLabel)
    {
        EXPECT_EQ(dns01RecordName("gyanis.space"), "_acme-challenge.gyanis.space");
        EXPECT_EQ(dns01RecordName("*.gyanis.space"), "_acme-challenge.gyanis.space");
        EXPECT_EQ(dns01RecordName("api.gyanis.space"), "_acme-challenge.api.gyanis.space");
        EXPECT_EQ(dns01RecordName(""), "_acme-challenge.") << "空名字不该被当成一条合法记录，但拼接口本身是纯粹的";
    }

    /**
     * @brief 动作对的「可用」判据：两格都要填
     * @details 只填 publish 的对象能把签发带到「写出去了、撤不回来」那一步，
     *          而留下的 TXT 会挡死下一轮 —— 因此它一律按「没有 DNS-01 能力」处置
     */
    TEST(AcmeDns01TxtWriter, RequiresBothHandlers)
    {
        const AcmeDns01TxtWriter empty;
        EXPECT_FALSE(empty.isUsable());

        AcmeDns01TxtWriter halfFilled;
        halfFilled.publish = [](std::string, std::string) -> Core::Task<std::expected<void, std::string>> { co_return std::expected<void, std::string>{}; };
        EXPECT_FALSE(halfFilled.isUsable());

        halfFilled.withdraw = halfFilled.publish;
        EXPECT_TRUE(halfFilled.isUsable());
    }

    /**
     * @brief 阿里云那份实现把两格都填上
     */
    TEST(AliyunDns01TxtWriter, FactoryFillsBothHandlers)
    {
        Core::EventLoop loop;
        const auto      writer = makeAliyunDns01TxtWriter(loop, AliyunDns01Configuration{});
        EXPECT_TRUE(writer.isUsable());
    }

    namespace
    {
        using AsynGyanis::TestSupport::findRegistrySample;
    } // namespace

    /**
     * @brief 钉住：dns-01 的四条耗时读数在写入器构造时登记、析构时注销
     * @details 这四条读数是给「一次续期花掉十分钟」那种现场看的：没有出口时它和「卡住了」长得一样。
     *          用例只能钉住登记与注销这一段——publish 的自增要打到真的云解析控制面上，
     *          那不在回环上测得到的范围（凭据与 endpoint 都不在这份配置里可换）
     */
    TEST(AliyunDns01TxtWriterMetrics, RegistersFourCountersAndReleasesThemOnDestruction)
    {
        constexpr std::array<const char *, 4> kMetricNames = {"asyn_acme_dns01_records_published_total", "asyn_acme_dns01_publish_seconds_total",
                                                              "asyn_acme_dns01_quiet_waits_total", "asyn_acme_dns01_quiet_wait_seconds_total"};

        Core::EventLoop loop;
        for (const char *const name: kMetricNames)
        {
            ASSERT_FALSE(findRegistrySample(name).has_value()) << name << "：还没有写入器，导出里就先有这条读数";
        }

        {
            const auto writer = makeAliyunDns01TxtWriter(loop, AliyunDns01Configuration{});
            for (const char *const name: kMetricNames)
            {
                const auto lookup = findRegistrySample(name);
                ASSERT_TRUE(lookup.has_value()) << name;
                EXPECT_EQ(lookup->kind, Core::ProcessMetricKind::Counter) << name << "：这类量要做 rate()，登记成 gauge 就用错了";
                EXPECT_EQ(lookup->value, 0U) << name << "：一次 publish 都没发过就该是 0，带上一个来路不明的数是假读数";
            }
        }

        for (const char *const name: kMetricNames)
        {
            EXPECT_FALSE(findRegistrySample(name).has_value()) << name << "：写入器已析构，这条读数还挂在导出里";
        }
    }

    /**
     * @brief 钉住：同名重写的静默期按「旧记录的 TTL 等满」算，两端都夹住
     * @details 这条时长是「机构读到上一条答案」与「读到这一条」的分界：少等就红，
     *          多等到天量 TTL 又把一次签发挂死
     */
    TEST(AliyunDns01RewriteQuiet, WaitsOutTheRecordTtlAndStopsAtTheCap)
    {
        using namespace std::chrono_literals;

        // 刚撤完：等满整条 TTL
        EXPECT_EQ(aliyunRewriteQuietPeriod(0ms, 600U), 600s);
        // 等了一半：只剩一半
        EXPECT_EQ(aliyunRewriteQuietPeriod(240s, 600U), 360s);
        // 已经等过：不再等
        EXPECT_EQ(aliyunRewriteQuietPeriod(600s, 600U), 0ms);
        EXPECT_EQ(aliyunRewriteQuietPeriod(900s, 600U), 0ms);
        // 配成一天的 TTL 不该把签发挂在那儿，落在上限
        EXPECT_EQ(aliyunRewriteQuietPeriod(0ms, 86400U), kAliyunMaximumRewriteQuiet);
        // 时钟读反了按「刚撤完」处置：这里的偏差方向取宁可多等
        EXPECT_EQ(aliyunRewriteQuietPeriod(-30s, 600U), 600s);
    }

    /**
     * @brief 钉住：静默期不会超过上限本身，而不是「上限减已过时间」算成负数
     */
    TEST(AliyunDns01RewriteQuiet, RemainingNeverGoesNegativeNearTheCap)
    {
        using namespace std::chrono_literals;

        EXPECT_EQ(aliyunRewriteQuietPeriod(kAliyunMaximumRewriteQuiet - 1s, 86400U), 1s);
        EXPECT_EQ(aliyunRewriteQuietPeriod(kAliyunMaximumRewriteQuiet - 1s, 60U), 0ms);
    }

    /**
     * @brief 夹具：在一条真循环上跑一次动作，把成败与失败文案交回来
     * @details 这两条出口都在**任何网络动作之前**，因此不需要桩服务端：主域名解析排第一，
     *          凭据校验排第二，谁先红就说明走的是哪一道门
     */
    class AliyunDns01TxtWriterTest : public ::testing::Test
    {
    protected:
        /// 一次动作的结果
        struct Outcome
        {
            bool                       succeeded{false}; ///< 动作有没有报成功
            std::optional<std::string> failureMessage{}; ///< 失败时的中文文案
        };

        /// 在循环上跑一次 publish
        Outcome publish(AliyunDns01Configuration configuration, const std::string &recordName)
        {
            return drive(std::move(configuration), recordName, true);
        }

        /// 在循环上跑一次 withdraw
        Outcome withdraw(AliyunDns01Configuration configuration, const std::string &recordName)
        {
            return drive(std::move(configuration), recordName, false);
        }

    private:
        Core::Task<void> driveOnce(Core::EventLoop &loop, AliyunDns01Configuration configuration, std::string recordName, bool isPublish)
        {
            const auto writer  = makeAliyunDns01TxtWriter(loop, std::move(configuration));
            const auto handler = isPublish ? writer.publish : writer.withdraw;
            auto       result  = co_await handler(std::move(recordName), "dX5E6p2g-s6gJ3lN0aQe1rTw8yU3iO4pA5sD6fG7hI0");
            if (result.has_value())
            {
                m_outcome.succeeded = true;
            } else
            {
                m_outcome.failureMessage = result.error();
            }
            loop.stop();
        }

        Outcome drive(AliyunDns01Configuration configuration, const std::string &recordName, bool isPublish)
        {
            m_outcome = {};
            // 循环与协程帧同生共死：帧要在循环之前析构，故声明排在后面
            Core::EventLoop loop;
            auto            task = driveOnce(loop, std::move(configuration), recordName, isPublish);
            loop.scheduler().schedule(task.handle());
            loop.run();
            return m_outcome;
        }

        Outcome m_outcome{};
    };

    /**
     * @brief 钉住：凭据为空时两条动作都在任何出站请求之前就把原因说清楚，而不是静默成功
     */
    TEST_F(AliyunDns01TxtWriterTest, RefusesBeforeAnyRequestWhenCredentialsAreMissing)
    {
        AliyunDns01Configuration configuration;
        configuration.accessKeyId     = "";
        configuration.accessKeySecret = "";

        const auto published = publish(configuration, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(published.succeeded);
        ASSERT_TRUE(published.failureMessage.has_value());
        EXPECT_NE(published.failureMessage->find("ASYN_ACME_DNS_ACCESS_KEY_ID"), std::string::npos) << *published.failureMessage;
        EXPECT_EQ(published.failureMessage->find("推不出主域名"), std::string::npos) << "主域名推导已经过了，报的却不该是那条";

        const auto withdrawn = withdraw(configuration, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(withdrawn.succeeded);
        ASSERT_TRUE(withdrawn.failureMessage.has_value());
        EXPECT_NE(withdrawn.failureMessage->find("ASYN_ACME_DNS_ACCESS_KEY_SECRET"), std::string::npos) << *withdrawn.failureMessage;

        // 只填一条也算空：一半的凭据签不出任何请求
        configuration.accessKeyId = "LTAI5tExampleKeyId";
        const auto halfCredential = publish(configuration, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(halfCredential.succeeded);
        ASSERT_TRUE(halfCredential.failureMessage.has_value());
        EXPECT_NE(halfCredential.failureMessage->find("凭据是空的"), std::string::npos) << *halfCredential.failureMessage;
    }

    /**
     * @brief 钉住：主域名推不出时报的是「去配 zoneDomainName」，而不是凭据缺失那条
     */
    TEST_F(AliyunDns01TxtWriterTest, RefusesWhenTheZoneCannotBeDerived)
    {
        AliyunDns01Configuration configuration;
        configuration.accessKeyId     = "LTAI5tExampleKeyId";
        configuration.accessKeySecret = "ExampleSecret";

        // 没有点号的名字推不出「哪条主域名下的哪段主机记录」
        const auto noDots = publish(configuration, "_acme-challenge");
        ASSERT_FALSE(noDots.succeeded);
        ASSERT_TRUE(noDots.failureMessage.has_value());
        EXPECT_NE(noDots.failureMessage->find("zoneDomainName"), std::string::npos) << *noDots.failureMessage;

        // 只有一段的情况同样推不出（域名至少要有两截）
        const auto singleLabel = publish(configuration, "_acme-challenge.local");
        ASSERT_FALSE(singleLabel.succeeded);
        ASSERT_TRUE(singleLabel.failureMessage.has_value());
        EXPECT_NE(singleLabel.failureMessage->find("zoneDomainName"), std::string::npos) << *singleLabel.failureMessage;

        // 配了主域名而记录名不在它下面：不能静默写到别的区里去
        configuration.zoneDomainName = "other.example.com";
        const auto outsideTheZone    = publish(configuration, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(outsideTheZone.succeeded);
        ASSERT_TRUE(outsideTheZone.failureMessage.has_value());
        EXPECT_NE(outsideTheZone.failureMessage->find("zoneDomainName"), std::string::npos) << *outsideTheZone.failureMessage;

        // 撤回走同一条判据：撤不干净的那条也要说清是没配主域名
        const auto withdrawn = withdraw(configuration, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(withdrawn.succeeded);
        ASSERT_TRUE(withdrawn.failureMessage.has_value());
        EXPECT_NE(withdrawn.failureMessage->find("zoneDomainName"), std::string::npos) << *withdrawn.failureMessage;

        // 配了主域名且名字在它下面：这一关过了。凭据仍是空的，于是它停在「真发请求之前」那道门上——
        // 这里要的判据是「不再提主域名」，而不是去打一次云端的真接口
        AliyunDns01Configuration matched;
        matched.zoneDomainName   = "gyanis.space";
        const auto insideTheZone = publish(matched, "_acme-challenge.gyanis.space");
        ASSERT_FALSE(insideTheZone.succeeded);
        ASSERT_TRUE(insideTheZone.failureMessage.has_value());
        EXPECT_EQ(insideTheZone.failureMessage->find("zoneDomainName"), std::string::npos) << *insideTheZone.failureMessage;
        EXPECT_NE(insideTheZone.failureMessage->find("凭据是空的"), std::string::npos) << *insideTheZone.failureMessage;
    }
} // namespace AsynGyanis::Net
