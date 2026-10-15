/**
 * @file MetricData.h
 * @brief 指标数据模型的公共件：直方图读数与标签键值
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 两条导出出口（`/metrics` 的 Prometheus 文本、`OtlpHttpMetricsExporter` 的 OTLP JSON）此前
 *          各自把同一个形状写了一遍：标签键值、直方图的档位换算（毫秒上界与微秒和都换成秒）、
 *          以及「每格条数 ↔ 累计条数」的折算。同一件事写两遍就会有一天只改一处——本文件把它收成
 *          一份数据模型：**谁采集谁负责折进模型，谁渲染谁从模型取值**，两条出口对着同一份模型编码。
 *
 * @note 这个头只放**已经在用的**形状，不预留「将来可能用得上」的字段：模型里的每个成员都有至少
 *       一处消费方（Prometheus 渲染与 OTLP 渲染各一处）。新的读数形态出现时再往里加，并在加的那
 *       一刻把两条出口都接上——只加字段不接出口，本层就又变回了「另一处没人在读的定义」。
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 一条标签键值：采集侧的维度
     *
     * @details 同一个键值对在两条出口里的编码不同（Prometheus 是 `key="value"`，OTLP 是
     *          `attributes` 数组里的一项），但**键名与取值的拼法必须同源**——两处各拼一遍，
     *          采集侧按标签聚合时就会在两套面板之间看到不同的维度名。
     */
    struct ASYN_CORE_API MetricLabel
    {
        std::string key;   ///< 标签键（如 `status_class`）
        std::string value; ///< 标签取值（如 `2xx`）
    };

    /**
     * @brief 一次抓取里的直方图读数：桶上界 + 每格条数 + 样本数 + 样本和
     *
     * @details 语义有三条，两条出口都按这一份理解：
     *          - `upperBoundsSeconds` 是各档**上界**（严格升序，单位秒、不含 `+Inf` 档）；
     *            `bucketCounts` 比它多一格，末格是溢出档（收纳超过最后一个上界的全部样本）。
     *          - `bucketCounts` 是**每格条数**（非累计）。Prometheus 展示格式要的是累计数，
     *            那属于渲染端的活：由 `cumulativeBucketCounts()` 现算，模型本身不存两份。
     *          - `sumSeconds` / `count` 是这一族样本的两个汇总，与分格计数同源同一次计入——
     *            只有每格条数时能算分位却算不出均值。
     *
     * @note 单位统一到**秒**：外部编码（Prometheus 的 `_sum`、OTLP 的 `sum`/`explicitBounds`）
     *       都用秒，采集端若按毫秒/微秒计数，在折进模型时换算一次，两条出口不再各换一遍。
     */
    struct ASYN_CORE_API MetricHistogramSample
    {
        std::vector<double>        upperBoundsSeconds; ///< 各档上界（严格升序，单位秒，不含 +Inf）
        std::vector<std::uint64_t> bucketCounts;       ///< 每格条数（**非累计**），末格溢出；长度 = 上界数 + 1
        std::uint64_t              count{0};           ///< 样本总数（与 bucketCounts 之和同解）
        double                     sumSeconds{0.0};    ///< 样本和，单位秒

        /// 分格是否与上界配套（长度 = 上界数 + 1）
        [[nodiscard]] bool hasConsistentBucketCount() const noexcept
        {
            return bucketCounts.size() == upperBoundsSeconds.size() + 1U;
        }
    };

    /**
     * @brief 把「每格条数」折算成 Prometheus 展示格式要的**累计**条数
     *
     * @details 展示格式的语义是「耗时不超过该上界的观测数」，而采集端记的是每格区间内的条数；
     *          两者只差一个前缀和，错了不会报错、只会让 `histogram_quantile()` 系统性偏小。
     *          折算放在本层一次做对，渲染端不各写一遍。
     * @param sample 直方图读数；`bucketCounts` 与上界不配套时按空结果处理（调用方应先经
     *        `hasConsistentBucketCount()` 判形，这里不抛——渲染端拿到的是一份只读快照）
     * @return std::vector<std::uint64_t> 与 `bucketCounts` 同长、同序的累计条数
     */
    [[nodiscard]] ASYN_CORE_API std::vector<std::uint64_t> cumulativeBucketCounts(const MetricHistogramSample &sample);

} // namespace AsynGyanis::Core
