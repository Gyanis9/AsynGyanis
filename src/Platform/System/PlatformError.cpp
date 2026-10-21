#include "Platform/System/PlatformError.h"

#include <cstdio>
#include <cstring>
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

    void PlatformError::describeErrno(const int errorNumber, char *const output, const std::size_t outputCapacity) noexcept
    {
        if (output == nullptr || outputCapacity == 0)
        {
            return;
        }
#if ASYN_PLATFORM_WIN32
        static_cast<void>(::strerror_s(output, outputCapacity, errorNumber));
#else
        std::snprintf(output, outputCapacity, "%s", std::strerror(errorNumber));
#endif
    }
} // namespace AsynGyanis::Platform
