/**
 * @file MetricsTestSupport.h
 * @brief 进程级指标注册表的测试助手：按名字取读数、判有没有登记
 * @author Gyanis
 * @date 2026-09-30
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 「登记了吗 / 登记成什么类型 / 现在是多少」这三问在六个模块的用例里各写了一遍同一份遍历，
 *          收在这一处。判据本身不变：用例仍然自己点名要看的指标，助手只负责取。
 */

#pragma once

#include "Core/Metrics/ProcessMetricsRegistry.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace AsynGyanis::TestSupport
{
    /**
     * @brief 按名字取一条进程级读数
     * @param name 登记时给的完整指标名
     * @return std::optional<Core::ProcessMetricSample> 取不到即没登记过
     */
    [[nodiscard]] inline std::optional<Core::ProcessMetricSample> findRegistrySample(const std::string_view name)
    {
        for (const Core::ProcessMetricSample &sample: Core::ProcessMetricsRegistry::samples())
        {
            if (sample.name == name)
            {
                return sample;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 这个名字有没有登记过（区别于「登记了但读数是 0」）
     */
    [[nodiscard]] inline bool hasRegistrySample(const std::string_view name)
    {
        return findRegistrySample(name).has_value();
    }

    /**
     * @brief 按名字取读数；没登记过时交回 0
     * @details 只在「登记过」已由同一用例的另一条断言钉住的场合用，否则 0 会把「没登记」误报成「登记了是 0」
     */
    [[nodiscard]] inline std::uint64_t registryValue(const std::string_view name)
    {
        const auto sample = findRegistrySample(name);
        return sample.has_value() ? sample->value : 0U;
    }
} // namespace AsynGyanis::TestSupport
