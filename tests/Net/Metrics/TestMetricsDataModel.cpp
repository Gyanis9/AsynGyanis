// 指标数据模型公共件（台账 2.5）的判据：直方图折换算只在映射层做一次，两条出口渲染同一份模型。
//
// 判据分三处：映射层给的上界/条数/和与手算逐条对齐；两条出口（Prometheus 文本、OTLP 批）从同一份
// 快照出发必须对上——且对照的期望值在用例里**手工累加**得出，不复用被测实现的折算函数。

#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Metrics/HttpStatsOtlp.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using AsynGyanis::Core::cumulativeBucketCounts;
    using AsynGyanis::Core::MetricHistogramSample;
    using AsynGyanis::Net::buildOtlpMetricsFromHttpStats;
    using AsynGyanis::Net::formatPrometheusMetrics;
    using AsynGyanis::Net::httpLatencyHistogramSample;
    using AsynGyanis::Net::HttpServerStats;
    using AsynGyanis::Net::httpStatusClassCounts;
    using AsynGyanis::Net::kHttpStatusClassLabelKey;
    using AsynGyanis::Net::OtlpHistogramSample;
    using AsynGyanis::Net::OtlpMetricsBatch;

    /// 一条出口样本用的快照：分格刻意各不相等，前缀和与原始值一眼能分开
    HttpServerStats makeHistogramStats()
    {
        HttpServerStats stats;
        stats.status1xxCount           = 1U;
        stats.status2xxCount           = 80U;
        stats.status3xxCount           = 3U;
        stats.status4xxCount           = 5U;
        stats.status5xxCount           = 11U;
        stats.latencyBucketCounts      = {40U, 30U, 20U, 5U, 5U};
        stats.totalLatencyMicroseconds = 2'500'000U;
        return stats;
    }

    /**
     * @brief 取 Prometheus 文本里某一行的**数值文本**（行里最后一个空格之后、换行之前）
     * @details 对 `名字{le="…"} 值` 与 `名字 值` 两种行形都成立，因此桶、_sum 与 _count 共用一份取法
     * @param prometheusText 渲染结果
     * @param linePrefix 行首特征（含指标名与可选的标签前缀）
     * @return std::string 数值文本；找不到该行时为空
     */
    std::string valueTextOnLine(const std::string &prometheusText, const std::string_view linePrefix)
    {
        const std::size_t lineStart = prometheusText.find(linePrefix);
        if (lineStart == std::string::npos)
        {
            return {};
        }
        const std::size_t valueEnd  = prometheusText.find('\n', lineStart);
        const std::string lineText  = prometheusText.substr(lineStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - lineStart);
        const std::size_t lastSpace = lineText.rfind(' ');
        return lastSpace == std::string::npos ? std::string{} : lineText.substr(lastSpace + 1U);
    }
} // namespace

/**
 * @brief 钉住状态码分类取值表：五档、顺序与类名固定，取数与快照同值
 * @details 两条出口共用这一份取值表；类名或顺序被改过，采集侧就会在 Prometheus 与 OTLP 之间看到
 *          两套对不上的标签值。期望值是手写常量，不复用被测实现的拼法
 */
TEST(MetricsDataModel, StatusClassTableHasFiveFixedClasses)
{
    const HttpServerStats          stats    = makeHistogramStats();
    const auto                     classes  = httpStatusClassCounts(stats);
    const std::vector<std::string> expected = {"1xx", "2xx", "3xx", "4xx", "5xx"};
    ASSERT_EQ(classes.size(), expected.size());
    for (std::size_t index = 0; index < classes.size(); ++index)
    {
        EXPECT_EQ(classes[index].className, expected[index]) << "第 " << index << " 档类名不对";
    }
    EXPECT_EQ(classes[0].count, 1U);
    EXPECT_EQ(classes[1].count, 80U);
    EXPECT_EQ(classes[2].count, 3U);
    EXPECT_EQ(classes[3].count, 5U);
    EXPECT_EQ(classes[4].count, 11U);
    EXPECT_EQ(kHttpStatusClassLabelKey, "status_class") << "标签键名是跨出口的契约，不许改名";
}

/**
 * @brief 钉住直方图折换：上界按秒给出、分格保持每格条数、和与样本数按手算
 * @details 期望值全部手写：上界 {0.001, 0.005, 0.02, 0.1}（毫秒常量除以 1000 的十进制精确值）、
 *          和 2.5 秒（2'500'000 微秒）、样本数 100（40+30+20+5+5）
 */
TEST(MetricsDataModel, HistogramSampleConvertsUnitsExactlyOnce)
{
    const MetricHistogramSample sample = httpLatencyHistogramSample(makeHistogramStats());

    ASSERT_TRUE(sample.hasConsistentBucketCount()) << "分格数与上界数不配套";
    const std::vector<double> expectedBounds = {0.001, 0.005, 0.02, 0.1};
    ASSERT_EQ(sample.upperBoundsSeconds.size(), expectedBounds.size());
    for (std::size_t index = 0; index < expectedBounds.size(); ++index)
    {
        EXPECT_DOUBLE_EQ(sample.upperBoundsSeconds[index], expectedBounds[index]) << "第 " << index << " 档上界的秒值不对";
    }
    const std::vector<std::uint64_t> expectedCounts = {40U, 30U, 20U, 5U, 5U};
    EXPECT_EQ(sample.bucketCounts, expectedCounts) << "分格必须保持「每格条数」，不是累计值";
    EXPECT_EQ(sample.count, 100U) << "样本数应当是各格之和";
    EXPECT_DOUBLE_EQ(sample.sumSeconds, 2.5) << "和必须换算到秒";
}

/**
 * @brief 钉住累计折算：前缀和含溢出档，形不配套时交回空
 */
TEST(MetricsDataModel, CumulativeCountsArePrefixSumsIncludingTheOverflowBucket)
{
    MetricHistogramSample sample;
    sample.upperBoundsSeconds = {0.001, 0.005, 0.02, 0.1};
    sample.bucketCounts       = {40U, 30U, 20U, 5U, 5U}; // 末格是溢出档
    sample.count              = 100U;

    EXPECT_EQ(cumulativeBucketCounts(sample), (std::vector<std::uint64_t>{40U, 70U, 90U, 95U, 100U}));

    MetricHistogramSample mismatched;
    mismatched.upperBoundsSeconds = {0.001, 0.005};
    mismatched.bucketCounts       = {1U, 2U}; // 少一格溢出档
    EXPECT_TRUE(cumulativeBucketCounts(mismatched).empty()) << "形不配套时应当交回空，而不是算出一份缺一格的累计";
}

/**
 * @brief 钉住两条出口渲染同一份模型：同一份快照下，OTLP 的每格条数**手工累加**必须等于
 *        Prometheus 文本里的 `_bucket` 累计值，上界文本、和的换算、样本数与状态码标签逐一对上
 * @details 本用例的期望值在用例内手工累加得出（不调用 cumulativeBucketCounts），因此它同时钉住
 *          「折算规则是前缀和」与「两条出口同源」两件事：任何一处把语义换掉都会在这里分叉
 * @note 证伪：把 OTLP 侧的 bucketCounts 改成累计值（或让 Prometheus 侧不再累加），第一格之后立刻
 *       分叉，本用例红
 */
TEST(MetricsDataModel, BothExportsRenderTheSameHistogramAndLabels)
{
    const HttpServerStats  stats          = makeHistogramStats();
    const std::string      prometheusText = formatPrometheusMetrics(stats, "srv");
    const OtlpMetricsBatch otlpBatch      = buildOtlpMetricsFromHttpStats(stats, "srv");

    ASSERT_EQ(otlpBatch.histograms.size(), 1U);
    const OtlpHistogramSample &otlpHistogram = otlpBatch.histograms.front();
    ASSERT_EQ(otlpHistogram.bucketCounts.size(), 5U);
    ASSERT_EQ(otlpHistogram.explicitBoundsSeconds.size(), 4U);

    // 手写的上界文本与用例自己的累计账本；两者都不经过被测实现的折算函数
    const std::array<std::string, 4> expectedBoundTexts = {"0.001", "0.005", "0.02", "0.1"};
    std::uint64_t                    manualCumulative   = 0;
    for (std::size_t index = 0; index < otlpHistogram.bucketCounts.size(); ++index)
    {
        manualCumulative += otlpHistogram.bucketCounts[index];

        const bool        isOverflowBucket = index + 1U == otlpHistogram.bucketCounts.size();
        const std::string boundText        = isOverflowBucket ? std::string("+Inf") : expectedBoundTexts[index];
        const std::string linePrefix       = "srv_request_duration_seconds_bucket{le=\"" + boundText + "\"} ";

        const std::string valueText = valueTextOnLine(prometheusText, linePrefix);
        ASSERT_FALSE(valueText.empty()) << "Prometheus 文本里找不到这一条 _bucket：" << linePrefix;
        EXPECT_EQ(std::stoull(valueText), manualCumulative) << "第 " << index << " 格：两条出口的计数对不上";

        if (!isOverflowBucket)
        {
            // OTLP 侧同一档的秒值必须与 Prometheus 文本里的那个上界是同一个数
            EXPECT_DOUBLE_EQ(otlpHistogram.explicitBoundsSeconds[index], std::stod(expectedBoundTexts[index])) << "第 " << index << " 档的秒值不对";
        }
    }

    // 和的换算与样本数：Prometheus 的 _sum/_count 与 OTLP 的同源
    EXPECT_DOUBLE_EQ(std::stod(valueTextOnLine(prometheusText, "srv_request_duration_seconds_sum ")), otlpHistogram.sumSeconds);
    EXPECT_DOUBLE_EQ(otlpHistogram.sumSeconds, 2.5);
    EXPECT_EQ(std::stoull(valueTextOnLine(prometheusText, "srv_request_duration_seconds_count ")), otlpHistogram.count);
    EXPECT_EQ(otlpHistogram.count, 100U);

    // 状态码标签族：同一份取数、同一个键名与类名，两条出口逐点对上
    ASSERT_EQ(otlpBatch.labeled.size(), 1U);
    const auto statusClasses = httpStatusClassCounts(stats);
    ASSERT_EQ(otlpBatch.labeled.front().points.size(), statusClasses.size());
    for (std::size_t index = 0; index < statusClasses.size(); ++index)
    {
        const auto &attributes = otlpBatch.labeled.front().points[index].attributes;
        ASSERT_EQ(attributes.size(), 1U);
        EXPECT_EQ(attributes.front().first, std::string(kHttpStatusClassLabelKey));
        EXPECT_EQ(attributes.front().second, std::string(statusClasses[index].className));

        const std::string linePrefix = std::string("srv_responses_total{status_class=\"") + std::string(statusClasses[index].className) + "\"} ";
        EXPECT_EQ(std::stoull(valueTextOnLine(prometheusText, linePrefix)), otlpBatch.labeled.front().points[index].value)
                << "状态码类 " << statusClasses[index].className << " 两条出口的取值对不上";
    }
}
