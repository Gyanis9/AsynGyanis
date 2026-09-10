/**
 * @file PlatformTime.h
 * @brief 跨平台本地时间转换
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <ctime>

namespace AsynGyanis::Platform
{
    /**
     * @brief 平台时间工具
     *
     * @details POSIX 提供线程安全的 localtime_r，MSVC 提供参数顺序相反的 localtime_s，
     *          两者都没有可直接使用的 std::localtime 线程安全版本。日志时间戳与
     *          滚动文件名后缀都依赖本地时间，统一由此类出口。
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
    };
} // namespace AsynGyanis::Platform
