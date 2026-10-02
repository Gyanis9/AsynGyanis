#include "Core/Metrics/ProcessMetricsRegistry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 取某个名字当前的登记条数（按实例数，不是按名字）：注销类判据要数它
        [[nodiscard]] std::size_t entryCountFor(const std::string &name)
        {
            std::size_t count = 0;
            for (const ProcessMetricSample &sample: ProcessMetricsRegistry::samples())
            {
                if (sample.name == name)
                {
                    ++count;
                }
            }
            return count;
        }

        /// 按名字取一条读数；没登记过返回 nullopt 语义用 hasValue 表达
        struct SampleLookup
        {
            bool                found{false};
            ProcessMetricSample sample{};
        };

        [[nodiscard]] SampleLookup findByName(const std::string &name)
        {
            for (const ProcessMetricSample &sample: ProcessMetricsRegistry::samples())
            {
                if (sample.name == name)
                {
                    return SampleLookup{true, sample};
                }
            }
            return {};
        }
    } // namespace

    /**
     * @brief 钉住：登记一次就多一条读数，把手析构就注销
     * @details 「注销靠 RAII 而不是记得调 unregister」是本层的核心约定：漏一次注销，
     *          导出的就是谁也不持有的假数
     */
    TEST(ProcessMetricsRegistry, RegistersAndDeregistersWithTheHandleLifetime)
    {
        std::atomic<std::uint64_t> value{7};
        const std::size_t          namesBefore = ProcessMetricsRegistry::nameCount();

        {
            const auto handle = ProcessMetricsRegistry::registerMetric("asyn_test_probe_total", "一条测试用的计数", ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                                       [&value] { return value.load(); });
            EXPECT_TRUE(handle.isValid());
            EXPECT_EQ(ProcessMetricsRegistry::nameCount(), namesBefore + 1U);

            const auto lookup = findByName("asyn_test_probe_total");
            ASSERT_TRUE(lookup.found);
            EXPECT_EQ(lookup.sample.value, 7U);
            EXPECT_EQ(lookup.sample.kind, ProcessMetricKind::Counter);
            EXPECT_EQ(lookup.sample.help, "一条测试用的计数");
        }

        EXPECT_EQ(ProcessMetricsRegistry::nameCount(), namesBefore);
        EXPECT_FALSE(findByName("asyn_test_probe_total").found);
    }

    /**
     * @brief 钉住：值每次抓取现取，不是登记那一刻的快照
     * @details 登记时把值定死，导出的就是一张不会动的死数——那条读数看起来在，其实什么都没说
     */
    TEST(ProcessMetricsRegistry, ReadsTheValueOnEveryScrape)
    {
        std::atomic<std::uint64_t> value{1};
        const auto                 handle = ProcessMetricsRegistry::registerMetric("asyn_test_probe_live_total", "现取的计数", ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                                                   [&value] { return value.load(); });

        EXPECT_EQ(findByName("asyn_test_probe_live_total").sample.value, 1U);
        value.store(42);
        EXPECT_EQ(findByName("asyn_test_probe_live_total").sample.value, 42U);
    }

    /**
     * @brief 钉住：同名两条实例按 Sum 并成一个数（多监听器/多对象的进程口径）
     */
    TEST(ProcessMetricsRegistry, SumsInstancesSharingAName)
    {
        const auto first  = ProcessMetricsRegistry::registerMetric("asyn_test_probe_sum_total", "两路相加", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 3U; });
        const auto second = ProcessMetricsRegistry::registerMetric("asyn_test_probe_sum_total", "两路相加", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 5U; });

        const auto lookup = findByName("asyn_test_probe_sum_total");
        ASSERT_TRUE(lookup.found);
        EXPECT_EQ(lookup.sample.value, 8U);
        // 按名字去重后只占一条：导出里同一名字出现两行是坏格式
        EXPECT_EQ(entryCountFor("asyn_test_probe_sum_total"), 1U);
    }

    /**
     * @brief 钉住：到期时刻这类取最小，而不是把两张证书的时间戳加成第三条证书
     * @details 求和会把「90 天后到期」与「89 天后到期」并成一个谁也没配的数；最早那张才是会先出事的
     */
    TEST(ProcessMetricsRegistry, TakesTheEarliestValueWhenMergingWithMin)
    {
        const auto later =
                ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_seconds", "最早的期限", ProcessMetricKind::Gauge, ProcessMetricMerge::Min, [] { return 9'000'000U; });
        const auto sooner =
                ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_seconds", "最早的期限", ProcessMetricKind::Gauge, ProcessMetricMerge::Min, [] { return 1'000U; });

        EXPECT_EQ(findByName("asyn_test_probe_min_seconds").sample.value, 1'000U);
    }

    /**
     * @brief 钉住：`MinNonZero` 里 0 不参与求早——「读不出」不是「最早」
     * @details 证书到期时刻那一格，0 的含义是这条路径上没有读得出的证书。让它按 Min 参与合并，一个
     *          还没签出的实例就会把另一个「还有八十天」的真值压成 0，报出一条假告警；两边都读不出时
     *          才该交回 0。登记顺序两个方向都要试：先 0 后真值、先真值后 0。
     */
    TEST(ProcessMetricsRegistry, SkipsUnreadableSlotsWhenMergingWithMinNonZero)
    {
        const auto unknown = ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_nonzero_seconds", "跳零求早", ProcessMetricKind::Gauge, ProcessMetricMerge::MinNonZero,
                                                                    [] { return 0U; });
        const auto known   = ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_nonzero_seconds", "跳零求早", ProcessMetricKind::Gauge, ProcessMetricMerge::MinNonZero,
                                                                    [] { return 4'200U; });
        EXPECT_EQ(findByName("asyn_test_probe_min_nonzero_seconds").sample.value, 4'200U) << "先登记的 0 把真值盖掉了";

        const auto laterUnknown = ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_nonzero_seconds", "跳零求早", ProcessMetricKind::Gauge,
                                                                         ProcessMetricMerge::MinNonZero, [] { return 0U; });
        EXPECT_EQ(findByName("asyn_test_probe_min_nonzero_seconds").sample.value, 4'200U) << "后登记的 0 也不该改答案";
    }

    /**
     * @brief 钉住：`MinNonZero` 两边都读不出时交回 0，而不是编出一个数
     */
    TEST(ProcessMetricsRegistry, KeepsZeroWhenNoInstanceCanReadAMinNonZeroMetric)
    {
        const auto firstUnknown  = ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_nonzero_empty_seconds", "全为空", ProcessMetricKind::Gauge,
                                                                          ProcessMetricMerge::MinNonZero, [] { return 0U; });
        const auto secondUnknown = ProcessMetricsRegistry::registerMetric("asyn_test_probe_min_nonzero_empty_seconds", "全为空", ProcessMetricKind::Gauge,
                                                                          ProcessMetricMerge::MinNonZero, [] { return 0U; });
        EXPECT_EQ(findByName("asyn_test_probe_min_nonzero_empty_seconds").sample.value, 0U);
    }

    /**
     * @brief 钉住：把手被移走之后只有一份注销责任，移动赋值会先放掉原来那条
     */
    TEST(ProcessMetricsRegistry, MovingAHandleKeepsExactlyOneOwner)
    {
        auto handle = ProcessMetricsRegistry::registerMetric("asyn_test_probe_move_total", "移动测试", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 1U; });
        auto other  = ProcessMetricsRegistry::registerMetric("asyn_test_probe_move_other_total", "移动测试的目标", ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                             [] { return 1U; });

        handle = std::move(other);

        // 原主被赋值时注销的是自己那条；接收来的那条还活着
        EXPECT_FALSE(findByName("asyn_test_probe_move_total").found);
        EXPECT_TRUE(findByName("asyn_test_probe_move_other_total").found);
        EXPECT_FALSE(other.isValid());
    }

    /**
     * @brief 钉住：名字不合法、回调为空、以及同名登记类型不一致，都在登记那一刻当场拒
     * @details 这三类都是「登记了但导出是坏的」：坏格式、恒 0 假读数、一份文本里两条矛盾的 TYPE
     */
    TEST(ProcessMetricsRegistry, RefusesBadRegistrationsAtRegistrationTime)
    {
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("has a space", "空格不合法", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 0U; })),
                     std::invalid_argument);
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("9starts-with-digit", "首位不能是数字", ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                                              [] { return 0U; })),
                     std::invalid_argument);
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("asyn_test_probe_empty", "空回调", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, nullptr)),
                     std::invalid_argument);

        const auto handle =
                ProcessMetricsRegistry::registerMetric("asyn_test_probe_conflict_total", "先登记的说明", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 1U; });
        // 类型不同
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("asyn_test_probe_conflict_total", "先登记的说明", ProcessMetricKind::Gauge, ProcessMetricMerge::Sum,
                                                                              [] { return 1U; })),
                     std::invalid_argument);
        // 并法不同
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("asyn_test_probe_conflict_total", "先登记的说明", ProcessMetricKind::Counter, ProcessMetricMerge::Min,
                                                                              [] { return 1U; })),
                     std::invalid_argument);
        // 说明不同
        EXPECT_THROW(static_cast<void>(ProcessMetricsRegistry::registerMetric("asyn_test_probe_conflict_total", "换了个说法", ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                                              [] { return 1U; })),
                     std::invalid_argument);
        // 三项一致的重复登记是支持的（多台同类对象各登记一份），把手握着才会在本条结束时注销
        const auto duplicate =
                ProcessMetricsRegistry::registerMetric("asyn_test_probe_conflict_total", "先登记的说明", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 1U; });
        EXPECT_TRUE(duplicate.isValid());
    }

    /**
     * @brief 钉住：说明里的换行在登记时被归一化，不会把 `# HELP` 那一行切开
     */
    TEST(ProcessMetricsRegistry, FlattensNewlinesInTheHelpText)
    {
        const auto handle =
                ProcessMetricsRegistry::registerMetric("asyn_test_probe_multiline_total", "第一行\n第二行", ProcessMetricKind::Counter, ProcessMetricMerge::Sum, [] { return 1U; });
        const auto lookup = findByName("asyn_test_probe_multiline_total");
        ASSERT_TRUE(lookup.found);
        EXPECT_EQ(lookup.sample.help, "第一行 第二行");
        EXPECT_EQ(lookup.sample.help.find('\n'), std::string::npos);
    }
} // namespace AsynGyanis::Core
