#include "Platform/System/PlatformTime.h"
#include "Platform/Platform.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 由公历日期算星期（0 = 周日，与 std::tm::tm_wday 同约定）
         *
         * @details 不取 `std::tm::tm_wday`：部分运行库对 1970 年之前的时间给出错误的星期，
         *          而 HTTP 日期头要求对任意时刻都正确。这里用「距 1970-01-01 的天数」推算，
         *          天数用 floor 语义（负数纪元同样成立），1970-01-01 是周四即天数 0 对应 4。
         * @param year 公历年
         * @param month 月 1~12
         * @param day 日 1~31
         * @return int 星期 0~6（0 为周日）
         */
        constexpr int weekdayOfCivilDate(const int year, const int month, const int day) noexcept
        {
            // Howard Hinnant 的 days_from_civil：先归到 400 年周期内的序号，再折算成距纪元天数
            const int      shiftedYear = year - (month <= 2 ? 1 : 0);
            const int      era         = (shiftedYear >= 0 ? shiftedYear : shiftedYear - 399) / 400;
            const unsigned yearOfEra   = static_cast<unsigned>(shiftedYear - era * 400);
            const unsigned dayOfYear   = (153U * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2U) / 5U + static_cast<unsigned>(day) - 1U;
            const unsigned dayOfEra    = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
            const int      daysSinceEpoch = era * 146097 + static_cast<int>(dayOfEra) - 719468;

            // +11 再取模：C++ 的 % 对负数给负余数，直接取模会把 1970 年前的日期算错一天
            return ((daysSinceEpoch % 7) + 11) % 7;
        }
    } // namespace

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

    UtcTimeFields PlatformTime::utcTime(const std::time_t calendarTime) noexcept
    {
        std::tm result{};
#if ASYN_PLATFORM_WIN32
        // MSVC 的入参顺序是 (出参, 入参)，与 POSIX 相反，且以 errno_t 报告失败
        if (::gmtime_s(&result, &calendarTime) != 0)
        {
            // 转换失败时 result 的内容无定义，交回零值结构让调用方一眼看出「没折出来」
            return UtcTimeFields{};
        }
#else
        if (::gmtime_r(&calendarTime, &result) == nullptr)
        {
            return UtcTimeFields{};
        }
#endif
        return UtcTimeFields{
                .year    = result.tm_year + 1900,
                .month   = result.tm_mon + 1,
                .day     = result.tm_mday,
                .hour    = result.tm_hour,
                .minute  = result.tm_min,
                .second  = result.tm_sec,
                .weekday = weekdayOfCivilDate(result.tm_year + 1900, result.tm_mon + 1, result.tm_mday)};
    }
} // namespace AsynGyanis::Platform
