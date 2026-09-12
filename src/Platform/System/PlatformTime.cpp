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
