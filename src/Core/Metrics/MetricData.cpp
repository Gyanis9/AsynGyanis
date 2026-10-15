/**
 * @file MetricData.cpp
 * @brief 指标数据模型的公共件实现（见头文件的形状说明）
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Metrics/MetricData.h"

namespace AsynGyanis::Core
{
    std::vector<std::uint64_t> cumulativeBucketCounts(const MetricHistogramSample &sample)
    {
        if (!sample.hasConsistentBucketCount())
        {
            return {};
        }

        std::vector<std::uint64_t> cumulativeCounts;
        cumulativeCounts.reserve(sample.bucketCounts.size());

        std::uint64_t runningTotal = 0;
        for (const std::uint64_t bucketCount: sample.bucketCounts)
        {
            runningTotal += bucketCount;
            cumulativeCounts.push_back(runningTotal);
        }
        return cumulativeCounts;
    }
} // namespace AsynGyanis::Core
