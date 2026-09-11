/**
 * @file PlatformError.cpp
 * @brief 跨平台错误码常量与最近一次系统错误的读取
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/System/PlatformError.h"

#include <system_error>

namespace AsynGyanis::Platform
{
    int PlatformError::lastErrorCode() noexcept
    {
        return errno;
    }

    int PlatformError::lastSocketErrorCode() noexcept
    {
#if ASYN_PLATFORM_WIN32
        return ::WSAGetLastError();
#else
        return errno;
#endif
    }

    void PlatformError::setLastErrorCode(const int errorCode) noexcept
    {
#if ASYN_PLATFORM_WIN32
        ::WSASetLastError(errorCode);
#endif
        errno = errorCode;
    }

    std::string PlatformError::message(const int errorCode)
    {
        return std::system_category().message(errorCode);
    }
} // namespace AsynGyanis::Platform
