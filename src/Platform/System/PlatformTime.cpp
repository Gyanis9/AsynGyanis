#include "Platform/System/PlatformTime.h"
#include "Platform/Platform.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 一天的秒数；UTC 分解要把 time_t 拆成「整日 + 日内秒」两段
        constexpr std::int64_t kSecondsPerDay = 86400;

        /// 一个 400 年格里高利周期的天数：闰年规则在这个长度上正好闭合，日期换算按它分块
        constexpr std::int64_t kDaysPerGregorianEra = 146097;

        /// 1970-01-01（序号 0）是周四，而口径要求 0 表示周日，故偏移 4 天
        constexpr std::int64_t kWeekdayOffsetOfEpoch = 4;

        /**
         * @brief 向下取整的除法（向负无穷取整，而不是 C++ 的向零截断）
         * @details 负数纪元必须走这条取整方向，否则 1969-12-31 23:59:59 会被算到 1969-12-31
         *          之前的那一天去，日内秒还会变成负数。
         * @param dividend 被除数，可为负
         * @param divisor 除数，恒为正
         * @return std::int64_t 向下取整的商
         */
        constexpr std::int64_t floorDivide(const std::int64_t dividend, const std::int64_t divisor) noexcept
        {
            const std::int64_t quotient = dividend / divisor;
            // 只有「异号且真有余数」时截断结果才偏大了一档
            return (dividend % divisor != 0 && (dividend < 0) != (divisor < 0)) ? quotient - 1 : quotient;
        }

        /**
         * @brief 取一个整数模 7 的非负余数
         * @param value 任意整数
         * @return std::int64_t 落在 [0, 6] 的余数
         */
        constexpr std::int64_t weekdayIndexModuloSeven(const std::int64_t value) noexcept
        {
            const std::int64_t remainder = value % 7;
            return remainder < 0 ? remainder + 7 : remainder;
        }

        /**
         * @brief 由「距 1970-01-01 的天数」反算公历年月日
         * @details days_from_civil 的逆运算（Howard Hinnant 的 civil_from_days）：先归到 400 年
         *          周期内，再在周期内解出年序、年内日序与月序。全程整数运算，负数纪元同样成立。
         * @param days 距纪元的天数，可为负
         * @return std::optional<std::tuple<int, int, int>> 年、月 1~12、日 1~31；
         *         年份超出 int 可表达范围时返回空（宁可报「折不出来」，也不回绕成一个看着合法的年份）
         */
        constexpr std::optional<std::tuple<int, int, int>> civilFromDays(const std::int64_t days) noexcept
        {
            // 把纪元起点挪到 0000-03-01：这样闰日落在周期末尾，周期内的算法才是规则的
            const std::int64_t shiftedDays = days + 719468;
            const std::int64_t era         = floorDivide(shiftedDays, kDaysPerGregorianEra);
            const std::int64_t dayOfEra    = shiftedDays - era * kDaysPerGregorianEra;   // [0, 146096]
            const std::int64_t yearOfEra   = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;   // [0, 399]
            const std::int64_t year        = yearOfEra + era * 400;
            const std::int64_t dayOfYear   = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);   // [0, 365]
            const std::int64_t monthProbe  = (5 * dayOfYear + 2) / 153;   // [0, 11]，3 月起算
            const std::int64_t day         = dayOfYear - (153 * monthProbe + 2) / 5 + 1;   // [1, 31]
            const std::int64_t month       = monthProbe + (monthProbe < 10 ? 3 : -9);   // 换回 1 月起的编号
            const std::int64_t calendarYear = year + (month <= 2 ? 1 : 0);   // 1、2 月属于上一个公历年

            if (calendarYear < std::numeric_limits<int>::min() || calendarYear > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }
            return std::tuple{static_cast<int>(calendarYear), static_cast<int>(month), static_cast<int>(day)};
        }
    } // namespace

    std::tm PlatformTime::localTime(const std::time_t calendarTime) noexcept
    {
        // 一次本地换算要把整套时区与夏令时规则走一遍（实测约 23-36 纳秒，而一行日志的格式化总成本
        // 约 300 纳秒），同一秒内的多条日志又必然得到同一个答案。这里留一格「同输入同输出」的缓存：
        // 线程局域因此不需要任何同步，也不会把别的线程的结果搬到本线程的栈上
        thread_local struct
        {
            std::time_t second{};      ///< 上次换算的 UTC 秒
            std::tm     fields{};      ///< 那次换算出的本地日历（失败时是零值结构，同样是确定答案）
            bool        hasCachedValue{false}; ///< 缓存格里是否已有结果：不能拿纪元零点当哨兵，它是合法输入
        } cache;

        if (cache.hasCachedValue && cache.second == calendarTime)
        {
            return cache.fields;
        }

        std::tm converted{};
#if ASYN_PLATFORM_WIN32
        // MSVC 转换失败时把整个结构填成 -1（不是「保持原值」），只看得出返回码：
        // 直接交回会让日志时间戳渲染成「1899-00--1 -1:-1:-1」这种带负号与空字段的文本
        if (::localtime_s(&converted, &calendarTime) != 0)
        {
            converted = std::tm{};
        }
#else
        if (::localtime_r(&calendarTime, &converted) == nullptr)
        {
            converted = std::tm{};
        }
#endif
        cache.second        = calendarTime;
        cache.fields        = converted;
        cache.hasCachedValue = true;
        return converted;
    }

    UtcTimeFields PlatformTime::utcTime(const std::time_t calendarTime) noexcept
    {
        // UTC 分解不需要时区，也不需要闰秒与夏令时规则，因此这里直接算而不是交给 C 库：
        // 交给 gmtime_s 会让 MSVC 把 1970 年之前整个拒掉（POSIX 却能正常折），同一份文件在两个
        // 平台上得到不同的 Last-Modified； Date 头这类输出要求对任意可表示的 time_t 都给出正确日历
        const std::int64_t seconds      = static_cast<std::int64_t>(calendarTime);
        std::int64_t days         = seconds / kSecondsPerDay;
        std::int64_t secondsOfDay = seconds % kSecondsPerDay;
        if (secondsOfDay < 0)
        {
            // 一次除法配一对余数，商与余数同时校正到 floor 口径：换成「整除后乘回去再减」会在 time_t
            // 取到最小值那一档让 days * 86400 越出 int64（容器 UBSan 实测报出），而纯除与纯取模对最负
            // 的输入都有定义——除数是正数，永远碰不到 INT64_MIN / -1 那种溢出
            secondsOfDay += kSecondsPerDay;   // 校正后日内秒落在 [0, 86400)
            --days;                           // 向零截断的商跟着降到向下取整
        }

        const auto civilDate = civilFromDays(days);
        if (!civilDate.has_value())
        {
            // 折不进 int 年份的只有把 time_t 推到 29 亿年这种荒谬取值：交回零值结构，
            // 由 HttpDate 那侧按「没折出来」的既有口径处理，而不是回绕成一个看着合法的年份
            return UtcTimeFields{};
        }

        UtcTimeFields fields{};
        fields.year    = std::get<0>(*civilDate);
        fields.month   = std::get<1>(*civilDate);
        fields.day     = std::get<2>(*civilDate);
        fields.hour    = static_cast<int>(secondsOfDay / 3600);
        fields.minute  = static_cast<int>(secondsOfDay % 3600 / 60);
        fields.second  = static_cast<int>(secondsOfDay % 60);
        // 星期由天数本身算，不必再从年月日推一遍：1970-01-01 是周四，负数纪元靠取模修正
        fields.weekday = static_cast<int>(weekdayIndexModuloSeven(days + kWeekdayOffsetOfEpoch));
        return fields;
    }
} // namespace AsynGyanis::Platform
