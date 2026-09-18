#include "Net/Http/HttpDate.h"

#include "Net/Http/HttpHeaderRules.h"
#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        // RFC 9110 §5.6.7 固定使用英文三字母缩写，与 locale 无关
        constexpr std::array<std::string_view, 7> kWeekdayNames{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        constexpr std::array<std::string_view, 12> kMonthNames{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

        /// 一天的秒数，用于把「距纪元的天数」折成秒
        constexpr std::int64_t kSecondsPerDay = 86400;

        /**
         * @brief 判断给定年份是否闰年
         * @param year 完整年份
         * @return true 是闰年
         */
        constexpr bool isLeapYear(const int year)
        {
            return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        }

        /**
         * @brief 取某年某月的天数
         * @param year 完整年份
         * @param month 月份 1~12
         * @return 该月天数；月份非法时返回 0，由调用方的范围检查拦下
         */
        constexpr unsigned daysInMonth(const int year, const unsigned month)
        {
            switch (month)
            {
                case 1:
                case 3:
                case 5:
                case 7:
                case 8:
                case 10:
                case 12:
                    return 31;
                case 4:
                case 6:
                case 9:
                case 11:
                    return 30;
                case 2:
                    return isLeapYear(year) ? 29u : 28u;
                default:
                    return 0;
            }
        }

        /**
         * @brief 把公历年月日折成「距 1970-01-01 的天数」
         * @details Howard Hinnant 的 civil 算法，全整数运算、不依赖本地时区；以三月为年首，
         *          把闰日放到年末，闰年规则因此可用整除表达。
         * @param year 完整年份
         * @param month 月份 1~12
         * @param day 日 1~31
         * @return 距纪元的天数，可为负
         */
        constexpr std::int64_t daysFromCivil(const int year, const unsigned month, const unsigned day)
        {
            const int adjustedYear = year - (month <= 2 ? 1 : 0);
            const std::int64_t era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
            const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
            const unsigned dayOfYear = (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
            const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
            return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
        }

        /**
         * @brief 解析一段全数字文本为无符号整数
         * @details 必须整段消费，多余字符（含前导符号、空格）一律判失败，不做部分解析。
         * @param text 待解析文本
         * @param value 输出：解析结果（失败时不被写入）
         * @return true 整段都是十进制数字且解析成功
         */
        bool parseDigits(const std::string_view text, unsigned &value)
        {
            if (text.empty())
            {
                return false;
            }

            unsigned parsedValue = 0;
            const auto [endPointer, errorCode] = std::from_chars(text.data(), text.data() + text.size(), parsedValue);
            if (errorCode != std::errc() || endPointer != text.data() + text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }
    } // namespace

    std::string formatHttpDate(const std::chrono::system_clock::time_point time)
    {
        const std::time_t calendarTime = std::chrono::system_clock::to_time_t(time);
        const Platform::UtcTimeFields fields = Platform::PlatformTime::utcTime(calendarTime);

        // 索引先夹到合法区间：utcTime 失败时返回的是零值结构，直接用来查表会越界
        const std::size_t weekdayIndex = (fields.weekday >= 0 && fields.weekday < 7) ? static_cast<std::size_t>(fields.weekday) : 0U;
        const std::size_t monthIndex = (fields.month >= 1 && fields.month <= 12) ? static_cast<std::size_t>(fields.month - 1) : 0U;

        return std::format("{}, {:02d} {} {:04d} {:02d}:{:02d}:{:02d} GMT",
                           kWeekdayNames[weekdayIndex],
                           fields.day,
                           kMonthNames[monthIndex],
                           fields.year,
                           fields.hour,
                           fields.minute,
                           fields.second);
    }

    std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text)
    {
        text = trimOptionalWhitespace(text);

        // IMF-fixdate 是定长格式，长度不对不必再逐字段试
        if (text.size() != kHttpDateTextLength)
        {
            return std::nullopt;
        }

        const std::string_view weekdayText = text.substr(0, 3);
        if (std::ranges::find(kWeekdayNames, weekdayText) == kWeekdayNames.end())
        {
            return std::nullopt;
        }
        if (text.substr(3, 2) != ", ")
        {
            return std::nullopt;
        }

        unsigned day = 0;
        if (!parseDigits(text.substr(5, 2), day) || day < 1 || day > 31)
        {
            return std::nullopt;
        }
        if (text[7] != ' ')
        {
            return std::nullopt;
        }

        const std::string_view monthText = text.substr(8, 3);
        const auto monthIterator = std::ranges::find(kMonthNames, monthText);
        if (monthIterator == kMonthNames.end())
        {
            return std::nullopt;
        }
        const unsigned month = static_cast<unsigned>(monthIterator - kMonthNames.begin()) + 1U;
        if (text[11] != ' ')
        {
            return std::nullopt;
        }

        unsigned year = 0;
        if (!parseDigits(text.substr(12, 4), year) || text[16] != ' ')
        {
            return std::nullopt;
        }

        unsigned hour = 0;
        unsigned minute = 0;
        unsigned second = 0;
        if (!parseDigits(text.substr(17, 2), hour) || text[19] != ':')
        {
            return std::nullopt;
        }
        if (!parseDigits(text.substr(20, 2), minute) || text[22] != ':')
        {
            return std::nullopt;
        }
        if (!parseDigits(text.substr(23, 2), second))
        {
            return std::nullopt;
        }
        if (text.substr(25, 4) != " GMT")
        {
            return std::nullopt;
        }

        // 逐字段范围检查：秒允许 60 以容纳闰秒
        if (hour > 23 || minute > 59 || second > 60)
        {
            return std::nullopt;
        }
        if (day > daysInMonth(static_cast<int>(year), month))
        {
            return std::nullopt;
        }

        const std::int64_t days = daysFromCivil(static_cast<int>(year), month, day);
        const std::int64_t seconds = days * kSecondsPerDay + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
                                     static_cast<std::int64_t>(second);
        return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    }
} // namespace AsynGyanis::Net
