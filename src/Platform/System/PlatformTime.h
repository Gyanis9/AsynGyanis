/**
 * @file PlatformTime.h
 * @brief 跨平台本地时间转换
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <ctime>

namespace AsynGyanis::Platform
{
    /**
     * @brief UTC 日历字段分解结果
     *
     * @details 字段按人类可读口径给出：年份是完整年份（如 1994）、月份 1~12、日 1~31，
     *          与 std::tm 的 tm_year（自 1900 起）和 tm_mon（自 0 起）刻意不同，
     *          避免每个调用方各自做一次偏移换算而写错。
     */
    struct UtcTimeFields
    {
        int year{0};    ///< 完整年份，如 1994；转换失败时为 0
        int month{0};   ///< 月份 1~12；转换失败时为 0
        int day{0};     ///< 日 1~31；转换失败时为 0
        int hour{0};    ///< 时 0~23
        int minute{0};  ///< 分 0~59
        int second{0};  ///< 秒 0~60（闰秒可能出现 60）
        int weekday{0}; ///< 星期 0~6，0 表示周日（与 std::tm::tm_wday 同约定）
    };

    /**
     * @brief 平台时间工具
     *
     * @details POSIX 提供线程安全的 localtime_r / gmtime_r，MSVC 提供参数顺序相反的
     *          localtime_s / gmtime_s，两者都没有可直接使用的 std::localtime / std::gmtime
     *          线程安全版本。日志时间戳与滚动文件名后缀依赖本地时间，HTTP 日期头依赖 UTC，
     *          统一由此类出口。
     */
    class PlatformTime
    {
    public:
        /**
         * @brief 将 UTC 秒数转换为本地时区日历时间
         * @param calendarTime 自 Unix 纪元起的秒数
         * @return std::tm 本地时间分解结果；转换失败时返回零值结构
         */
        static std::tm localTime(std::time_t calendarTime) noexcept;

        /**
         * @brief 将 UTC 秒数转换为 UTC 日历字段
         * @details 收在 Platform 层是因为只有这一层能按平台选对 gmtime_r / gmtime_s；
         *          直接用 std::gmtime 会复用静态缓冲，多线程下互相覆盖。
         * @param calendarTime 自 Unix 纪元起的 UTC 秒数
         * @return UtcTimeFields UTC 分解结果；转换失败时返回零值结构（各字段为 0）
         */
        static UtcTimeFields utcTime(std::time_t calendarTime) noexcept;
    };
} // namespace AsynGyanis::Platform
