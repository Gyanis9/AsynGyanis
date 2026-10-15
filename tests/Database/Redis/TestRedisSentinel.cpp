// Redis 哨兵的主库地址发现（台账 3.5 的 SENTINEL 那一格）。
//
// 覆盖两半：
// - 离线一组的三条配置错误都在发出任何字节之前判掉（空表、空监控名、地址不完整），
//   因此不需要服务端也永远可判——CI 上没有哨兵时这一组照常跑。
// - 真机一组问一个被监控的主库：答得出的地址、监控名不存在时的终止、坏地址先跳过再问下一个、
//   全部问不到时把每一台的失败原因都留在文案里。
//
// 真机一组的门控：`ASYN_REDIS_SENTINEL_TEST_NAME` 未设置即整组 GTEST_SKIP（不给默认值，
// 免得在一台没有哨兵的机器上把「跳过」读成「通过」）。地址默认 127.0.0.1:26379，
// 可用 `ASYN_REDIS_SENTINEL_TEST_HOST` / `_PORT` 覆盖。
// CI 上要造出这个形状：先起一个主库（本仓其它真机用例已经在用的那台），再起一个哨兵指着它——
//   docker run -d --name asyn-sentinel --link <主库容器名>:asynmaster -p 26379:26379 \
//     -v <一个可写目录>:/sentinel redis:7 redis-server /sentinel/sentinel.conf --sentinel
// 其中 sentinel.conf 至少要有 `port 26379`、`sentinel monitor mymaster asynmaster 6379 1`、
// `sentinel resolve-hostnames yes` 与一个可写的 `dir /sentinel`（哨兵要把状态回写进配置文件，
// 没有可写的配置文件时它直接退出，实测原文是 `Sentinel needs config file on disk to save state`）。
// 然后设 `ASYN_REDIS_SENTINEL_TEST_NAME=mymaster` 跑 TestDatabase 即可。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Redis/RedisSentinel.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
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
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>{}, std::string_view{"mymaster"}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find("没有给哨兵地址"), std::string::npos) << outcome.error();
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
        const std::vector<RedisSentinelAddress>                sentinels{RedisSentinelAddress{"127.0.0.1", 1}};
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view{}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find("监控名"), std::string::npos) << outcome.error();
        EXPECT_EQ(outcome.error().find("连不上"), std::string::npos) << "这条请求走到了连线上，判据就分不出「没填名字」与「哨兵坏了」：" << outcome.error();
    }

    /**
     * @brief 钉住地址不完整的那一格被跳过并把原因留在汇总里，而不是退化成「连 0 号端口碰碰运气」
     * @note 这里给的两条地址都不合法，因此一次网络都不碰：判据只可能来自这条路径本身
     */
    TEST(RedisSentinel, SkipsAddressesThatMissAHostOrAPort)
    {
        const std::vector<RedisSentinelAddress>                sentinels{RedisSentinelAddress{"", 26379}, RedisSentinelAddress{"127.0.0.1", 0}};
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view{"mymaster"}, sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find("地址不完整"), std::string::npos) << outcome.error();
        EXPECT_NE(outcome.error().find("2 个哨兵"), std::string::npos) << "汇总没说清问了几台：" << outcome.error();
        EXPECT_TRUE(containsLocalizedText(outcome.error())) << outcome.error();
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

        // 调用方手上那份配置通常是**主库**的：键空间编号有值、可能还带口令。哨兵不认 SELECT
        // （本机实测回 `ERR unknown command 'SELECT'`），发现路径必须把这一格清掉再连——
        // 不清就是每一次发现都在建连的最后一步失败，而原因看起来像「这个哨兵坏了」
        ConnectionConfig credentials = sentinelConfiguration();
        credentials.database         = "5";
        const std::vector<RedisSentinelAddress>                sentinels{sentinelAddress()};
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), credentials);

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_FALSE(outcome->host.empty());
        EXPECT_EQ(outcome->port, 6379) << "哨兵报的主库端口不是 6379，发现链路可能连的是另一个实例";
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

        const std::vector<RedisSentinelAddress>                sentinels{sentinelAddress()};
        const std::string                                      unknownName = masterName + "-没有这个名字";
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(unknownName), sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find(unknownName), std::string::npos) << "原因里没点出是哪一个名字没被监控：" << outcome.error();
        EXPECT_NE(outcome.error().find("换下一个也问不出来"), std::string::npos) << "没说清这是配置错而不是可用性故障：" << outcome.error();
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

        const std::vector<RedisSentinelAddress>                sentinels{RedisSentinelAddress{"127.0.0.1", 1}, sentinelAddress()};
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(outcome->port, 6379);
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

        const std::vector<RedisSentinelAddress>                sentinels{RedisSentinelAddress{"127.0.0.1", 1}, RedisSentinelAddress{"", 26379}};
        const std::expected<RedisSentinelAddress, std::string> outcome =
                resolveSentinelMasterAddress(std::span<const RedisSentinelAddress>(sentinels), std::string_view(masterName), sentinelConfiguration());

        ASSERT_FALSE(outcome.has_value());
        EXPECT_NE(outcome.error().find("问遍 2 个哨兵"), std::string::npos) << outcome.error();
        EXPECT_NE(outcome.error().find("连不上"), std::string::npos) << "第一台的原因没留在汇总里：" << outcome.error();
        EXPECT_NE(outcome.error().find("地址不完整"), std::string::npos) << "第二台的原因没留在汇总里：" << outcome.error();
    }

} // namespace AsynGyanis::Database
