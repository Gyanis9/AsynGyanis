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
