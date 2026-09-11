/**
 * @file PlatformTime.cpp
 * @brief 跨平台本地时间转换
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/System/PlatformTime.h"
#include "Platform/Platform.h"

namespace AsynGyanis::Platform
{
    std::tm PlatformTime::localTime(const std::time_t calendarTime) noexcept
    {
        std::tm result{};
#if ASYN_PLATFORM_WIN32
        ::localtime_s(&result, &calendarTime);
#else
        ::localtime_r(&calendarTime, &result);
#endif
        return result;
    }
} // namespace AsynGyanis::Platform
