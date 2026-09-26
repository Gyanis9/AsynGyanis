/**
 * @file RollingPeriod.h
 * @brief 按时间滚动时的周期换算：文件名时间后缀与下一个周期边界
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Sinks/RollingFileSink.h"
#include "Platform/System/PlatformTime.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <string>

namespace AsynGyanis::Base::Detail
{
    /// 一天的秒数，用于推算下一个整日边界
    inline constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

    /// 一小时的秒数，用于推算下一个整点边界
    inline constexpr std::int64_t kSecondsPerHour = 60 * 60;

    /**
     * @brief 取某个时刻所属周期的文件名后缀
     * @details 只用于 Daily / Hourly 两种策略（Size 策略的文件名不带后缀）。按**本地**日历换算，
     *          与运维在文件系统里看到的时间同一口径。
     * @param moment 时刻（epoch 秒）
     * @param policy 滚动策略
     * @return std::string Daily 为 `YYYY-MM-DD`，Hourly 为 `YYYY-MM-DD_HH`
     */
    [[nodiscard]] inline std::string rollingPeriodSuffix(const std::time_t moment, const RollingPolicy policy)
    {
        const std::tm localTime = Platform::PlatformTime::localTime(moment);
        if (policy == RollingPolicy::Daily)
        {
            return std::format("{:04d}-{:02d}-{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday);
        }
        return std::format("{:04d}-{:02d}-{:02d}_{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour);
    }

    /**
     * @brief 推算下一个周期边界的时刻
     * @details 边界恒**严格晚于**入参时刻（同一周期内不会重复触发格式化），且不超过一个完整周期。
     *          周期内已过秒数按本地日历字段算，因此半小时/45 分钟偏移的时区、以及夏令时切换那一天，
     *          边界可能估偏不足一小时——调用方以「后缀是否变化」作二次判据，估偏会在下一个周期自然收敛。
     * @param moment 当前时刻（epoch 秒）
     * @param policy 滚动策略；只用于 Daily / Hourly
     * @return std::time_t 下一个整日 / 整点边界
     */
    [[nodiscard]] inline std::time_t nextRollingPeriodBoundary(const std::time_t moment, const RollingPolicy policy) noexcept
    {
        const std::tm      localTime     = Platform::PlatformTime::localTime(moment);
        const bool         isDailyPolicy = policy == RollingPolicy::Daily;
        const std::int64_t periodSeconds = isDailyPolicy ? kSecondsPerDay : kSecondsPerHour;

        // 当前周期内已过的秒数：整日策略看时分秒，整点策略只看分秒
        // 当前周期内已过的秒数：整日策略看时分秒，整点策略只看分秒
        const std::int64_t elapsedSeconds = isDailyPolicy ? static_cast<std::int64_t>(localTime.tm_hour) * kSecondsPerHour + localTime.tm_min * 60 + localTime.tm_sec
                                                          : static_cast<std::int64_t>(localTime.tm_min) * 60 + localTime.tm_sec;

        // elapsedSeconds 最大为 periodSeconds - 1，因此结果恒落在 (moment, moment + periodSeconds] 内
        return moment + static_cast<std::time_t>(periodSeconds - elapsedSeconds);
    }
} // namespace AsynGyanis::Base::Detail
