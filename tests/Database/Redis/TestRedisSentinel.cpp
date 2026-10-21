// Redis 哨兵的主库地址发现。
//
// 覆盖两半：
// 离线一组的三条配置错误都在发出任何字节之前判掉（空表、空监控名、地址不完整），
//   因此不需要服务端也永远可判——CI 上没有哨兵时这一组照常跑。
// 真机一组问一个被监控的主库：答得出的地址、监控名不存在时的终止、坏地址先跳过再问下一个、
//   全部问不到时把每一台的失败原因都留在文案里。
// 故障转移一组：真把主库杀掉，等哨兵把副本提升成新主库，再确认地址真的变了。这一条是
//   破坏性的（跑完旧主库已停机），因此单独由 `ASYN_REDIS_SENTINEL_TEST_FAILOVER=1` 门控，
//   不进默认套件；要在一次性环境里跑。
//
// 真机一组的门控：`ASYN_REDIS_SENTINEL_TEST_NAME` 未设置即整组 GTEST_SKIP（不给默认值，
// 免得在一台没有哨兵的机器上把「跳过」读成「通过」）。地址默认 127.0.0.1:26379，
// 可用 `ASYN_REDIS_SENTINEL_TEST_HOST` / `_PORT` 覆盖。
// 造这个形状的配方（主机端口映射；哨兵按 IP 监视，避免主库停掉后 DNS 解析失败把哨兵推进 tilt）：
//   主库：docker run -d --name asyn-sentinel-master -p 16379:6379 redis:7.4 \
//           redis-server --port 6379 --requirepass <pw>
//   副本：docker run -d --name asyn-sentinel-replica -p 16380:6380 redis:7.4 \
//           redis-server --port 6380 --requirepass <pw> --masterauth <pw> \
//           --replicaof <宿主IP> 16379 --replica-announce-ip <宿主IP> --replica-announce-port 16380
//   哨兵：docker run -d --name asyn-sentinel -p 26379:26379 -v <可写目录>:/sentinel redis:7.4 \
//           redis-server /sentinel/sentinel.conf --sentinel
// sentinel.conf 至少要有 `port 26379`、`dir /sentinel`、`sentinel monitor mymaster <宿主IP> 16379 1`、
// `sentinel auth-pass mymaster <pw>` 与 `sentinel down-after-milliseconds mymaster 2000`；
// 目录必须可写（哨兵要把状态回写进配置文件，没有可写的配置文件时它直接退出，实测原文是
// `Sentinel needs config file on disk to save state`）。
// 然后设 `ASYN_REDIS_SENTINEL_TEST_NAME=mymaster`、`ASYN_REDIS_SENTINEL_TEST_MASTER_PORT=16379`
// 跑 TestDatabase 即可；故障转移那条再加 `ASYN_REDIS_SENTINEL_TEST_FAILOVER=1` 与主库口令。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Redis/RedisConnection.h"
#include "Database/Redis/RedisSentinel.h"

#include "DatabaseTestSupport.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        using TestSupport::containsLocalizedText;
        using TestSupport::readEnvironmentPortOrDefault;
        using TestSupport::readEnvironmentTextOrDefault;
        using TestSupport::readEnvironmentVariableText;

        /// 哨兵的默认端口：主库是 6379，填错这一格是最常见的一次「连不上但其实连错了」
        constexpr std::uint16_t kDefaultSentinelPort = 26379;

        /// 主库的默认端口；一次性环境常把它映射到别的宿主端口，因此断言读这一格而不是写死 6379
        constexpr std::uint16_t kDefaultMasterPort = 6379;

        /**
         * @brief 取真机用例要查的监控名；未设置时返回空串（用例据此跳过）
         * @return std::string 监控名，或空串
         */
        [[nodiscard]] std::string sentinelMasterName()
        {
            return readEnvironmentVariableText("ASYN_REDIS_SENTINEL_TEST_NAME");
        }

        /**
         * @brief 真机用例用的哨兵地址
         * @return RedisSentinelAddress 默认 127.0.0.1:26379，可由环境变量覆盖
         */
        [[nodiscard]] RedisSentinelAddress sentinelAddress()
        {
            return RedisSentinelAddress{readEnvironmentTextOrDefault("ASYN_REDIS_SENTINEL_TEST_HOST", "127.0.0.1"),
                                        readEnvironmentPortOrDefault("ASYN_REDIS_SENTINEL_TEST_PORT", kDefaultSentinelPort)};
        }

        /**
         * @brief 哨兵应当报出的主库端口
         * @details 一次性环境把主库映射到 16379 这类宿主端口时，哨兵报的是映射后的端口；
         *          断言读这一格，别把用例绑死在 6379 上
         * @return std::uint16_t 主库端口，可用 ASYN_REDIS_SENTINEL_TEST_MASTER_PORT 覆盖
         */
        [[nodiscard]] std::uint16_t expectedMasterPort()
        {
            return readEnvironmentPortOrDefault("ASYN_REDIS_SENTINEL_TEST_MASTER_PORT", kDefaultMasterPort);
        }

        /**
         * @brief 一条不带任何凭据的哨兵连接配置
         * @details 哨兵多数部署不设口令；主库那边有口令也与这一步无关（发现只问「主库在哪」）。
         *          超时不在这格配：本仓的口径是超时归 DatabaseConnection 的 setter
         *          （见 ConnectionConfig 的类注释），用默认值即可
         * @return ConnectionConfig 指向本机 Redis 默认档的配置
         */
        [[nodiscard]] ConnectionConfig sentinelConfiguration()
        {
            return ConnectionConfig::redisDefault();
        }
    } // namespace

    // ------------------------------------------------------------------------
    // 离线：三条配置错误都在发出任何字节之前判掉
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住空哨兵表被当场拒：连一个地址都没有，谈不上「问下一个」
     */
    TEST(RedisSentinel, RefusesAnEmptySentinelTable)
    {
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>{}, std::string_view{"mymaster"}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().message.find("没有给哨兵地址"), std::string::npos) << outcome.error().message;
        EXPECT_EQ(outcome.error().kind, RedisSentinelErrorKind::InvalidConfiguration) << "空表要在发出任何字节之前就判成配置错";
    }

    /**
     * @brief 钉住空监控名被当场拒，而不是让每个哨兵都回一句「没有这个名字」
     * @details 这两种空在文案上必须分得开：前者是调用方没填，后者是哨兵那边没配。
     *          混在一起会让人去查哨兵配置，而问题在自己的代码里。
     *          地址刻意给一个不通的（127.0.0.1:1）：这条要证的是「一次都没问出口」。
     *          上一版填的是真哨兵地址，突变「撤掉这道判据」之后它真连上了本机那台哨兵、
     *          并拿到一句意思相近的答复，于是红集为空——是判据抓不住，不是实现没问题
     */
    TEST(RedisSentinel, RefusesAnEmptyMasterNameBeforeAsking)
    {
        const std::vector<RedisSentinelAddress>           sentinels{RedisSentinelAddress{"127.0.0.1", 1}};
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view{}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().message.find("监控名"), std::string::npos) << outcome.error().message;
        EXPECT_EQ(outcome.error().message.find("连不上"), std::string::npos) << "这条请求走到了连线上，判据就分不出「没填名字」与「哨兵坏了」：" << outcome.error().message;
        EXPECT_EQ(outcome.error().kind, RedisSentinelErrorKind::InvalidConfiguration) << "没填监控名是调用方的配置错，不许报成哨兵的可用性故障";
    }

    /**
     * @brief 钉住地址不完整的那一格被跳过并把原因留在汇总里，而不是退化成「连 0 号端口碰碰运气」
     * @note 这里给的两条地址都不合法，因此一次网络都不碰：判据只可能来自这条路径本身
     */
    TEST(RedisSentinel, SkipsAddressesThatMissAHostOrAPort)
    {
        const std::vector<RedisSentinelAddress>           sentinels{RedisSentinelAddress{"", 26379}, RedisSentinelAddress{"127.0.0.1", 0}};
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view{"mymaster"}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().message.find("地址不完整"), std::string::npos) << outcome.error().message;
        EXPECT_NE(outcome.error().message.find("2 个哨兵"), std::string::npos) << "汇总没说清问了几台：" << outcome.error().message;
        EXPECT_TRUE(containsLocalizedText(outcome.error().message)) << outcome.error().message;
        EXPECT_EQ(outcome.error().kind, RedisSentinelErrorKind::InvalidConfiguration) << "整张表都没碰上线时，这是地址表的问题而不是哨兵坏了";
    }

    /**
     * @brief 钉住「真去连过却没人答」与「地址表本身不成形」分成两档
     * @details 端口 1 上没有监听（回环直接回拒），因此第一台一定碰了线；第二台主机名为空，只会被跳过。
     *          这条不需要任何哨兵在跑，判据只可能来自这一条路径本身——汇总那一档到底报哪一档，
     *          看的就是有没有格子真的碰过线，而不是有没有格子被跳过
     */
    TEST(RedisSentinel, SeparatesUnreachableSentinelsFromAnIncompleteTable)
    {
        const std::vector<RedisSentinelAddress>           reached{RedisSentinelAddress{"127.0.0.1", 1}, RedisSentinelAddress{"", 26379}};
        const RedisSentinelExpected<RedisSentinelAddress> mixed =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(reached), std::string_view{"mymaster"}, sentinelConfiguration());

        ASSERT_FALSE(mixed.has_value());
        EXPECT_NE(mixed.error().message.find("连不上"), std::string::npos) << mixed.error().message;
        EXPECT_EQ(mixed.error().kind, RedisSentinelErrorKind::NoSentinelReachable) << "只要真碰过线，汇总就该报可用性故障而不是配置错";
    }

    // ------------------------------------------------------------------------
    // 真机：向一个在跑的哨兵问主库
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住发现能拿到「主机 + 端口」，且端口就是主库那个数
     * @details 主机地址是容器网段的运行时事实，钉它就是把用例绑死在这一台机器上，因此判据落在
     *          「非空 + 端口对」这一格——那才是调用方真正要拿去连主库的两样东西
     */
    TEST(RedisSentinel, ResolvesTheMonitoredMasterAddress)
    {
        const std::string masterName = sentinelMasterName();
        if (masterName.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_NAME，跳过哨兵真机用例（造这个形状的配方见文件头）";
        }

        // 调用方手上那份配置通常是主库的：键空间编号有值、可能还带口令。哨兵不认 SELECT
        // （本机实测回 `ERR unknown command 'SELECT'`），发现路径必须把这一格清掉再连——
        // 不清就是每一次发现都在建连的最后一步失败，而原因看起来像「这个哨兵坏了」
        ConnectionConfig credentials = sentinelConfiguration();
        credentials.database         = "5";
        const std::vector<RedisSentinelAddress>           sentinels{sentinelAddress()};
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), credentials);

        ASSERT_HAS_VALUE(outcome) << outcome.error().message;
        EXPECT_FALSE(outcome->host.empty());
        EXPECT_EQ(outcome->port, expectedMasterPort()) << "哨兵报的主库端口不是预期的那个，发现链路可能连的是另一个实例";
    }

    /**
     * @brief 钉住「监控名不存在」是终止性的，而不是继续问下一个哨兵
     * @details 各哨兵的监控配置由集群自己同步，因此换一台也问不出同样的名字。把这种配置错误
     *          当成「这台没答，问下一台」会把一次填错说成一次可用性故障，还会白跑完整张表
     */
    TEST(RedisSentinel, StopsOnAMasterNameNoSentinelKnows)
    {
        const std::string masterName = sentinelMasterName();
        if (masterName.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_NAME，跳过哨兵真机用例";
        }

        const std::vector<RedisSentinelAddress>           sentinels{sentinelAddress()};
        const std::string                                 unknownName = masterName + "-没有这个名字";
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(unknownName), sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().message.find(unknownName), std::string::npos) << "原因里没点出是哪一个名字没被监控：" << outcome.error().message;
        EXPECT_NE(outcome.error().message.find("换下一个也问不出来"), std::string::npos) << "没说清这是配置错而不是可用性故障：" << outcome.error().message;
        EXPECT_EQ(outcome.error().kind, RedisSentinelErrorKind::MasterNotMonitored) << "名字没被监控要单独成一档：换下一个哨兵也一样问不出";
    }

    /**
     * @brief 钉住「坏地址先跳过、后面的哨兵照样问」这条续问路径
     * @details 127.0.0.1:1 上必然立刻被拒（未监听的回环端口直接回 RST），因此第一台一定失败；
     *          判据是第二台答出来了。只测「一台都好」的话，「第一台失败就整个放弃」那种写法照样绿
     */
    TEST(RedisSentinel, TriesTheNextSentinelWhenOneRefusesToAnswer)
    {
        const std::string masterName = sentinelMasterName();
        if (masterName.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_NAME，跳过哨兵真机用例";
        }

        const std::vector<RedisSentinelAddress>           sentinels{RedisSentinelAddress{"127.0.0.1", 1}, sentinelAddress()};
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());

        ASSERT_HAS_VALUE(outcome) << outcome.error().message;
        EXPECT_EQ(outcome->port, expectedMasterPort());
    }

    /**
     * @brief 钉住全部问不到时的汇总：每一台的失败原因都要留得下来
     * @details 只报「都失败了」等于让调用方逐台手连一遍才知道哪台坏了。这里给一台端口不存在的、
     *          再给一台主机名为空的，两条各属于自己的那一格原因
     */
    TEST(RedisSentinel, AggregatesEverySentinelThatFailedToAnswer)
    {
        const std::string masterName = sentinelMasterName();
        if (masterName.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_NAME，跳过哨兵真机用例";
        }

        const std::vector<RedisSentinelAddress>           sentinels{RedisSentinelAddress{"127.0.0.1", 1}, RedisSentinelAddress{"", 26379}};
        const RedisSentinelExpected<RedisSentinelAddress> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().message.find("问遍 2 个哨兵"), std::string::npos) << outcome.error().message;
        EXPECT_NE(outcome.error().message.find("连不上"), std::string::npos) << "第一台的原因没留在汇总里：" << outcome.error().message;
        EXPECT_NE(outcome.error().message.find("地址不完整"), std::string::npos) << "第二台的原因没留在汇总里：" << outcome.error().message;
        EXPECT_EQ(outcome.error().kind, RedisSentinelErrorKind::NoSentinelReachable) << "真连过第一台，汇总就该按可用性故障交回";
    }

    /**
     * @brief 真杀主库，等哨兵把副本提升，确认发现结果里的地址真的变了
     *
     * @details 这一条不是「问一次发现接口」：它先用发现结果连上当前主库、发 `SHUTDOWN NOSAVE`
     *          把它打掉，再轮询哨兵直到报回来的地址不再是原来那一对。判据落在端口变了——
     *          主库与副本在一次性环境里刻意用不同端口，地址没变就说明哨兵仍在报旧配置，
     *          故障转移根本没发生。最后再连一次新主库并 PING，证明报出来的地址是活的。
     *
     * @note 破坏性：跑完旧主库已停机，副本被提升。由 `ASYN_REDIS_SENTINEL_TEST_FAILOVER=1`
     *       单独门控，不进默认套件；主库口令取 `ASYN_REDIS_TEST_PASSWORD`（与其它真机用例同源）。
     * @note 轮询是「等条件成立」而不是固定睡眠：每 200ms 问一次，最多 90 秒——哨兵在宿主负载
     *       过高时会进入 30 秒的 tilt 模式并暂停故障转移，时限要留出一次恢复窗口；超时仍拿到
     *       旧地址就红在该判据上，不会把一次没发生的事件读成通过。
     */
    TEST(RedisSentinel, PromotesTheReplicaWhenTheMasterIsKilled)
    {
        const std::string masterName = sentinelMasterName();
        if (masterName.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_NAME，跳过哨兵真机用例";
        }
        if (readEnvironmentVariableText("ASYN_REDIS_SENTINEL_TEST_FAILOVER") != "1")
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_SENTINEL_TEST_FAILOVER=1，跳过破坏性的故障转移用例（跑完旧主库会停机）";
        }
        const std::string masterPassword = readEnvironmentVariableText("ASYN_REDIS_TEST_PASSWORD");
        if (masterPassword.empty())
        {
            GTEST_SKIP() << "未设置 ASYN_REDIS_TEST_PASSWORD：故障转移用例要连上旧主库发 SHUTDOWN，跳过";
        }

        const std::vector<RedisSentinelAddress>           sentinels{sentinelAddress()};
        const RedisSentinelExpected<RedisSentinelAddress> before =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());
        ASSERT_HAS_VALUE(before) << before.error().message;

        ConnectionConfig masterConfiguration = ConnectionConfig::redisDefault();
        masterConfiguration.host             = before->host;
        masterConfiguration.port             = before->port;
        masterConfiguration.password         = masterPassword;
        masterConfiguration.database.clear();

        {
            RedisConnection master(masterConfiguration);
            ASSERT_TRUE(master.connect()) << "杀主库之前连不上旧主库：" << master.lastError();
            // 服务端执行完就关连接，本次 execute 必然拿不到正常的回复；这里只要求命令发出去了，
            // 是否真的停机由下面的轮询判据证明
            static_cast<void>(master.execute("SHUTDOWN NOSAVE"));
        }

        std::optional<RedisSentinelAddress> after;
        std::string                         lastQueryError;
        const auto                          deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < deadline)
        {
            RedisSentinelExpected<RedisSentinelAddress> outcome =
                    resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());
            if (outcome.has_value())
            {
                if (outcome->host != before->host || outcome->port != before->port)
                {
                    after = *outcome;
                    break;
                }
            } else
            {
                lastQueryError = outcome.error().message;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        ASSERT_HAS_VALUE(after) << "90 秒内哨兵没有报出新主库；最后一次查询失败原因：" << lastQueryError;
        EXPECT_NE(after->port, before->port) << "旧主库已被杀掉，哨兵报回来的仍是同一个端口——地址没有真的变";

        ConnectionConfig promotedConfiguration = ConnectionConfig::redisDefault();
        promotedConfiguration.host             = after->host;
        promotedConfiguration.port             = after->port;
        promotedConfiguration.password         = masterPassword;
        promotedConfiguration.database.clear();
        RedisConnection promoted(promotedConfiguration);
        ASSERT_TRUE(promoted.connect()) << "哨兵报出的新主库连不上：" << promoted.lastError();
        EXPECT_NE(promoted.execute("PING"), nullptr) << "新主库连上了但不回 PING：" << promoted.lastError();
    }

} // namespace AsynGyanis::Database
