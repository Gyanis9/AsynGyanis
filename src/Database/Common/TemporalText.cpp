/**
 * @file TemporalText.cpp
 * @brief 日期时间文本的解析与渲染实现
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#include "Database/Common/TemporalText.h"

#include "Database/Common/DatabaseValue.h"

#include <charconv>
#include <chrono>
#include <format>

namespace AsynGyanis::Database::Detail
{
    namespace
    {
        using namespace std::chrono;

        /// 一天的微秒数与一小时、一分钟的微秒数：拆历法与当日时间时用，不在多处各写一遍
        constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
        constexpr std::int64_t kSecondsPerMinute      = 60;
        constexpr std::int64_t kMinutesPerHour        = 60;
        constexpr std::int64_t kHoursPerDay           = 24;
        /// 偏移的合法幅度：±23:59（引擎给出的偏移不会超过这个范围，超出的一律当认不出）
        constexpr int kMaximumOffsetMinutes = kHoursPerDay * kMinutesPerHour - 1;

        /**
         * @brief 从文本当前位置读 1~2 位十进制数
         * @param text 原文
         * @param offset 读起位置，成功时前移
         * @param value 输出读数
         * @return bool 读到了一两位数字则为真
         */
        bool takeNumber(const std::string_view text, std::size_t &offset, int &value) noexcept
        {
            const std::size_t start  = offset;
            std::size_t       cursor = offset;
            while (cursor < text.size() && cursor - start < 2U && text[cursor] >= '0' && text[cursor] <= '9')
            {
                ++cursor;
            }
            if (cursor == start)
            {
                return false;
            }

            int                          parsedValue = 0;
            const std::from_chars_result result = std::from_chars(text.data() + static_cast<std::ptrdiff_t>(start), text.data() + static_cast<std::ptrdiff_t>(cursor), parsedValue);
            if (result.ec != std::errc{} || result.ptr != text.data() + static_cast<std::ptrdiff_t>(cursor))
            {
                return false;
            }
            value  = parsedValue;
            offset = cursor;
            return true;
        }

        /**
         * @brief 读历年份：可带负号，4~10 位数字（公元一万年以上 PG 会给五位）
         * @param text 原文
         * @param offset 读起位置，成功时前移
         * @param year 输出年份
         * @return bool 形态合法则真
         */
        bool takeYear(const std::string_view text, std::size_t &offset, std::int64_t &year) noexcept
        {
            std::size_t cursor     = offset;
            bool        isNegative = false;
            if (cursor < text.size() && text[cursor] == '-')
            {
                isNegative = true;
                ++cursor;
            }

            const std::size_t digitsStart = cursor;
            while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
            {
                ++cursor;
            }
            const std::size_t digitCount = cursor - digitsStart;
            if (digitCount < 4U || digitCount > 10U)
            {
                return false;
            }

            std::int64_t                 parsedValue = 0;
            const std::from_chars_result result =
                    std::from_chars(text.data() + static_cast<std::ptrdiff_t>(digitsStart), text.data() + static_cast<std::ptrdiff_t>(cursor), parsedValue);
            if (result.ec != std::errc{} || result.ptr != text.data() + static_cast<std::ptrdiff_t>(cursor))
            {
                return false;
            }
            year   = isNegative ? -parsedValue : parsedValue;
            offset = cursor;
            return true;
        }

        /**
         * @brief 读尾部偏移：`Z`、`+HH:MM`、`-HHMM` 三种形态（PostgreSQL 两种都给，SQLite 给 Z）
         * @param text 原文
         * @param offset 读起位置，成功时前移到偏移之后
         * @param offsetMinutes 输出偏移分钟数
         * @return bool 读到合法偏移则真；文本在这里结束（没有偏移）也返回真，并把 offsetMinutes 置空
         */
        bool takeOffset(const std::string_view text, std::size_t &offset, std::optional<int> &offsetMinutes) noexcept
        {
            if (offset == text.size())
            {
                offsetMinutes = std::nullopt;
                return true;
            }

            const char marker = text[offset];
            if (marker == 'Z' || marker == 'z')
            {
                offsetMinutes = 0;
                ++offset;
                return true;
            }
            if (marker != '+' && marker != '-')
            {
                return false;
            }

            const bool isNegative = marker == '-';
            ++offset;
            int hours   = 0;
            int minutes = 0;
            if (!takeNumber(text, offset, hours))
            {
                return false;
            }
            // 冒号是可选项：PostgreSQL 的 ISO 输出带冒号，某些写法与 MySQL 的 TIME 偏移不带
            if (offset < text.size() && text[offset] == ':')
            {
                ++offset;
            }
            // 只给小时的偏移（PostgreSQL 会给出 +08 这种形态）也要认：读不到分钟就当 0
            if (!takeNumber(text, offset, minutes))
            {
                minutes = 0;
            }

            const int totalMinutes = hours * kMinutesPerHour + minutes;
            if (totalMinutes > kMaximumOffsetMinutes)
            {
                return false;
            }
            offsetMinutes = isNegative ? -totalMinutes : totalMinutes;
            return true;
        }

        /**
         * @brief 把「自纪元起算的微秒」拆成历法与当日时间，再交给格式化
         * @param microsecondsSinceCivilEpoch 自 1970-01-01 起算的微秒（可为负）。两个时钟的纪元是同一个，
         *        区别只在「这一串数是 UTC 的读数还是本地历法的读数」，所以这里不加任何纪元偏移
         * @param separator 日期与时间之间的分隔符
         * @param suffix 结尾要加的东西（`Z`、`+00` 或空）
         * @return std::string 定宽文本
         */
        std::string renderFromCivilMicroseconds(const std::int64_t microsecondsSinceCivilEpoch, const char separator, const std::string_view suffix)
        {
            // floor 向负无穷取整：负年份的那一半也要落在正确的那一天上（截断取整会串到前一日）
            const auto           wholeDays = floor<days>(microseconds{microsecondsSinceCivilEpoch});
            const local_days     dayPoint{wholeDays};
            const year_month_day calendarDay{dayPoint};

            const std::int64_t insideDayMicroseconds = (microseconds{microsecondsSinceCivilEpoch} - wholeDays).count();
            const std::int64_t insideDaySeconds      = insideDayMicroseconds / kMicrosecondsPerSecond;
            const std::int64_t microsecondPart       = insideDayMicroseconds % kMicrosecondsPerSecond;
            const std::int64_t hourPart              = insideDaySeconds / (kSecondsPerMinute * kMinutesPerHour);
            const std::int64_t minutePart            = (insideDaySeconds / kSecondsPerMinute) % kMinutesPerHour;
            const std::int64_t secondPart            = insideDaySeconds % kSecondsPerMinute;

            // 年份按「至少四位数字」补零，负号单独占一位：{:04} 会把 -44 排成 "-044"（三位数字），
            // 而解析那一侧要求四位以上——两边不吻合，同一年份渲染出去就认不回来
            const int         civilYear           = static_cast<int>(calendarDay.year());
            const bool        isNegativeCivilYear = civilYear < 0;
            const std::string yearText            = std::format("{}{:04}", isNegativeCivilYear ? "-" : "", isNegativeCivilYear ? -civilYear : civilYear);
            return std::format("{}-{:02}-{:02}{}{:02}:{:02}:{:02}.{:06}{}", yearText, static_cast<unsigned>(calendarDay.month()), static_cast<unsigned>(calendarDay.day()),
                               separator, hourPart, minutePart, secondPart, microsecondPart, suffix);
        }

        /**
         * @brief 读小数秒：调用方已确认当前位置是 `.` 或 `,`
         * @param text 原文
         * @param offset 读起位置（停在小数分隔符上），成功时前移到小数段之后
         * @param microsecond 输出当日以内的微秒余数
         * @return bool 这段写法合法则真；分隔符后没有数字、或数字多于九位为假
         */
        bool takeFraction(const std::string_view text, std::size_t &offset, std::int32_t &microsecond) noexcept
        {
            ++offset;
            std::int64_t fractionValue  = 0;
            int          fractionDigits = 0;
            while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9' && fractionDigits < 9)
            {
                fractionValue = fractionValue * 10 + (text[offset] - '0');
                ++offset;
                ++fractionDigits;
            }
            if (fractionDigits == 0)
            {
                return false;
            }
            // 多于 6 位按微秒截断：截掉的是引擎给出的超出本库承载精度的位数，不是数据
            while (fractionDigits > 6)
            {
                fractionValue /= 10;
                --fractionDigits;
            }
            while (fractionDigits < 6)
            {
                fractionValue *= 10;
                ++fractionDigits;
            }
            microsecond = static_cast<std::int32_t>(fractionValue);

            // 小数位多于 9 位时不认：那是超出本库表示能力的写法，猜一个不如报出来
            return offset >= text.size() || text[offset] < '0' || text[offset] > '9';
        }
    } // namespace

    std::optional<ParsedDateTime> parseTemporalText(const std::string_view text) noexcept
    {
        ParsedDateTime parsed{};
        std::size_t    offset = 0;

        if (!takeYear(text, offset, parsed.year))
        {
            return std::nullopt;
        }
        if (offset >= text.size() || text[offset] != '-')
        {
            return std::nullopt;
        }
        ++offset;
        if (!takeNumber(text, offset, parsed.month) || offset >= text.size() || text[offset] != '-')
        {
            return std::nullopt;
        }
        ++offset;
        if (!takeNumber(text, offset, parsed.day))
        {
            return std::nullopt;
        }

        // 日期到此为止也算认（MySQL/PostgreSQL 的 DATE 列就长这样），时分秒全按零
        if (offset < text.size())
        {
            const char separator = text[offset];
            if (separator != ' ' && separator != 'T' && separator != 't')
            {
                return std::nullopt;
            }
            ++offset;

            if (!takeNumber(text, offset, parsed.hour) || offset >= text.size() || text[offset] != ':')
            {
                return std::nullopt;
            }
            ++offset;
            if (!takeNumber(text, offset, parsed.minute))
            {
                return std::nullopt;
            }

            // 秒可省：PostgreSQL 对整分钟的值会给 `09:20`（少见但合法），MySQL 恒给到秒
            if (offset < text.size() && text[offset] == ':')
            {
                ++offset;
                if (!takeNumber(text, offset, parsed.second))
                {
                    return std::nullopt;
                }
            }

            if (offset < text.size() && (text[offset] == '.' || text[offset] == ','))
            {
                if (!takeFraction(text, offset, parsed.microsecond))
                {
                    return std::nullopt;
                }
            }

            if (!takeOffset(text, offset, parsed.offsetMinutes))
            {
                return std::nullopt;
            }
        }

        // 还有余文就不认：`Thu 10-18-2026 09:20:30 PDT+0530`（非法国 DateStyle）与 `infinity` 都从这里被挡
        if (offset != text.size())
        {
            return std::nullopt;
        }

        if (parsed.month < 1 || parsed.month > 12 || parsed.day < 1 || parsed.day > 31 || parsed.hour < 0 || parsed.hour >= kHoursPerDay || parsed.minute < 0 ||
            parsed.minute >= kMinutesPerHour || parsed.second < 0 || parsed.second > 60 || parsed.microsecond < 0 || parsed.microsecond >= kMicrosecondsPerSecond)
        {
            return std::nullopt;
        }

        return parsed;
    }

    std::string renderSystemClock(const sys_time<microseconds> instant, const char separator, const bool withUtcMarker)
    {
        return renderFromCivilMicroseconds(instant.time_since_epoch().count(), separator, withUtcMarker ? "Z" : "");
    }

    std::string renderSystemClockWithZeroOffset(const sys_time<microseconds> instant)
    {
        return renderFromCivilMicroseconds(instant.time_since_epoch().count(), ' ', "+00");
    }

    std::string renderWallClock(const local_time<microseconds> wall, const char separator)
    {
        return renderFromCivilMicroseconds(wall.time_since_epoch().count(), separator, "");
    }

    std::optional<std::string> renderTemporalParameterText(const DatabaseValue &value, const char separator, const std::string_view instantSuffix)
    {
        if (const auto *instant = std::get_if<Timestamp>(&value))
        {
            return renderFromCivilMicroseconds(instant->time_since_epoch().count(), separator, instantSuffix);
        }
        if (const auto *wall = std::get_if<LocalDateTime>(&value))
        {
            return renderFromCivilMicroseconds(wall->time_since_epoch().count(), separator, "");
        }
        return std::nullopt;
    }

    std::optional<sys_time<microseconds>> toSystemClock(const ParsedDateTime &parsed) noexcept
    {
        const year_month_day calendarDay{year{static_cast<int>(parsed.year)}, month{static_cast<unsigned>(parsed.month)}, day{static_cast<unsigned>(parsed.day)}};
        if (!calendarDay.ok())
        {
            return std::nullopt;
        }

        const local_days dayPoint{calendarDay};
        const auto       wall =
                local_time<microseconds>{dayPoint.time_since_epoch()} + hours{parsed.hour} + minutes{parsed.minute} + seconds{parsed.second} + microseconds{parsed.microsecond};

        // 文本没带偏移就按 UTC 解释：这是 MySQL 那一路的前提（DATETIME 不做会话时区换算，见文件头）
        const int offsetMinutes = parsed.offsetMinutes.has_value() ? *parsed.offsetMinutes : 0;
        return sys_time<microseconds>{(wall - minutes{offsetMinutes}).time_since_epoch()};
    }

    std::optional<local_time<microseconds>> toWallClock(const ParsedDateTime &parsed) noexcept
    {
        // 带偏移的文本折成墙上时间就是抹平「这一列有时区语义」这件事，所以这里拒绝而不是丢掉偏移
        if (parsed.offsetMinutes.has_value())
        {
            return std::nullopt;
        }

        const year_month_day calendarDay{year{static_cast<int>(parsed.year)}, month{static_cast<unsigned>(parsed.month)}, day{static_cast<unsigned>(parsed.day)}};
        if (!calendarDay.ok())
        {
            return std::nullopt;
        }

        const local_days dayPoint{calendarDay};
        return local_time<microseconds>{dayPoint.time_since_epoch()} + hours{parsed.hour} + minutes{parsed.minute} + seconds{parsed.second} + microseconds{parsed.microsecond};
    }

} // namespace AsynGyanis::Database::Detail
