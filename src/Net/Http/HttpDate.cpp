#include "Net/Http/HttpDate.h"

#include "Net/Http/HttpHeaderRules.h"
#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        // RFC 9110 §5.6.7 固定使用英文三字母缩写，与 locale 无关
        constexpr std::array<std::string_view, 7>  kWeekdayNames{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        constexpr std::array<std::string_view, 12> kMonthNames{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

        /// RFC 850 的星期是全称（"Sunday,"），收端要连它一起认，否则老客户端的条件请求头会整条判无效
        constexpr std::array<std::string_view, 7> kFullWeekdayNames{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

        /// RFC 850 去掉星期段之后的定长形状："06-Nov-94 08:49:37 GMT"
        constexpr std::size_t kRfc850BodyLength = 22;

        /// asctime 的整条定长形状："Sun Nov  6 08:49:37 1994"（日占两位，个位数右对齐补空格）
        constexpr std::size_t kAsctimeLength = 24;

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
            const int          adjustedYear = year - (month <= 2 ? 1 : 0);
            const std::int64_t era          = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
            const unsigned     yearOfEra    = static_cast<unsigned>(adjustedYear - era * 400);
            const unsigned     dayOfYear    = (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
            const unsigned     dayOfEra     = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
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

            unsigned parsedValue               = 0;
            const auto [endPointer, errorCode] = std::from_chars(text.data(), text.data() + text.size(), parsedValue);
            if (errorCode != std::errc() || endPointer != text.data() + text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }

        /// 把一个十位数写成两位（0 填充）并推进游标；入参取自 std::tm，取值范围天然落在 0..99
        void putTwoDigits(std::array<char, kHttpDateTextLength> &text, std::size_t &cursor, const int value) noexcept
        {
            text[cursor++] = static_cast<char>('0' + value / 10);
            text[cursor++] = static_cast<char>('0' + value % 10);
        }

        /// 抄入三个字母的星期/月份缩写
        void putThreeLetters(std::array<char, kHttpDateTextLength> &text, std::size_t &cursor, const std::string_view word) noexcept
        {
            text[cursor++] = static_cast<char>(word[0]);
            text[cursor++] = static_cast<char>(word[1]);
            text[cursor++] = static_cast<char>(word[2]);
        }

        /**
         * @brief 把 UTC 字段折成 29 字节的 IMF-fixdate
         * @details 手工拼装而不是 std::format：定长格式没有可选字段，而这条路径每条响应（Date 头）
         *          与每次静态文件请求（Last-Modified）都要走，std::format 处理七个字段要 270 ns 上下，
         *          比整个折算本身的开销高一个量级。结果与原实现逐字节一致，不受 locale 影响。
         * @param fields UTC 字段（utcTime 失败时是零值结构）
         * @return std::array<char, kHttpDateTextLength> 定长文本
         */
        [[nodiscard]] std::array<char, kHttpDateTextLength> buildHttpDateText(const Platform::UtcTimeFields &fields) noexcept
        {
            // 索引先夹到合法区间：utcTime 失败时返回的是零值结构，直接用来查表会越界
            const std::size_t weekdayIndex = (fields.weekday >= 0 && fields.weekday < 7) ? static_cast<std::size_t>(fields.weekday) : 0U;
            const std::size_t monthIndex   = (fields.month >= 1 && fields.month <= 12) ? static_cast<std::size_t>(fields.month - 1) : 0U;

            std::array<char, kHttpDateTextLength> text{};
            std::size_t                           cursor = 0;
            putThreeLetters(text, cursor, kWeekdayNames[weekdayIndex]);
            text[cursor++] = ',';
            text[cursor++] = ' ';
            putTwoDigits(text, cursor, fields.day);
            text[cursor++] = ' ';
            putThreeLetters(text, cursor, kMonthNames[monthIndex]);
            text[cursor++] = ' ';
            // 年份按十进制定宽四位（与原来 "{:04d}" 一致，1900 起够用，不引入负号分支）
            text[cursor++] = static_cast<char>('0' + fields.year / 1000);
            text[cursor++] = static_cast<char>('0' + fields.year / 100 % 10);
            text[cursor++] = static_cast<char>('0' + fields.year / 10 % 10);
            text[cursor++] = static_cast<char>('0' + fields.year % 10);
            text[cursor++] = ' ';
            putTwoDigits(text, cursor, fields.hour);
            text[cursor++] = ':';
            putTwoDigits(text, cursor, fields.minute);
            text[cursor++] = ':';
            putTwoDigits(text, cursor, fields.second);
            text[cursor++] = ' ';
            text[cursor++] = 'G';
            text[cursor++] = 'M';
            text[cursor++] = 'T';
            return text;
        }
    } // namespace

    namespace
    {
        /**
         * @brief 在缩写表里查一个名字的下标
         * @param names 名称表（大小写敏感的逐字比较：HTTP 日期的 ABNF 已固定大小写）
         * @param text 待查文本
         * @return std::optional<std::size_t> 命中的下标；没查到返回空
         */
        template<std::size_t Count>
        [[nodiscard]] std::optional<std::size_t> findNameIndex(const std::array<std::string_view, Count> &names, const std::string_view text) noexcept
        {
            const auto found = std::ranges::find(names, text);
            if (found == names.end())
            {
                return std::nullopt;
            }
            return static_cast<std::size_t>(found - names.begin());
        }

        /**
         * @brief 把已经拆出来的六个字段折成时间点
         * @details 三条格式（IMF、RFC 850、asctime）共用这一份折算与范围检查：闰秒允许 second==60，
         *          日必须落在该月的实际天数内（闰年 2 月 29 合法、4 月 31 不合法）。
         * @return std::optional<std::chrono::system_clock::time_point> 字段越界时返回空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> makeUtcTimePoint(const unsigned year, const unsigned month, const unsigned day, const unsigned hour,
                                                                                            const unsigned minute, const unsigned second)
        {
            if (hour > 23 || minute > 59 || second > 60)
            {
                return std::nullopt;
            }
            if (day > daysInMonth(static_cast<int>(year), month))
            {
                return std::nullopt;
            }

            const std::int64_t days = daysFromCivil(static_cast<int>(year), month, day);
            const std::int64_t seconds =
                    days * kSecondsPerDay + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
            // 这一步构造 time_point 会把「秒」换成时钟的周期（libstdc++ 是 1 纳秒、MSVC 是 100 纳秒），
            // 也就是一次乘法：越界的秒数交给它就是把一条远期日期折回过去。折法与两端钳位都在
            // timePointFromUnixSeconds() 里，与「文件系统 mtime」「证书 notAfter」共用同一份判据
            return timePointFromUnixSeconds(seconds);
        }

        /**
         * @brief 解析 RFC 850 的过时格式：`Sunday, 06-Nov-94 08:49:37 GMT`
         * @details 星期允许全称（RFC 850 的原形）也允许三字母缩写——发过这种头的老客户端两种都在写。
         *          两位年份按 RFC 9110 §5.6.7 指向的 RFC 6265 规则折叠：0..69 记 2000 年代，
         *          70..99 记 1900 年代。**不**按「离现在最近的世纪」解释，那会让同一个头在不同时刻
         *          解析出不同结果，缓存验证器要的是可复现的判据。
         * @param text 已去首尾空白的整条文本
         * @return std::optional<std::chrono::system_clock::time_point> 形状不符时返回空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseRfc850Date(const std::string_view text)
        {
            const std::size_t commaPosition = text.find(',');
            if (commaPosition == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::string_view weekdayText = text.substr(0, commaPosition);
            if (!findNameIndex(kWeekdayNames, weekdayText).has_value() && !findNameIndex(kFullWeekdayNames, weekdayText).has_value())
            {
                return std::nullopt;
            }

            const std::string_view body = text.substr(commaPosition + 1);
            if (body.size() != kRfc850BodyLength + 1 || body.front() != ' ')
            {
                return std::nullopt;
            }
            // "06-Nov-94 08:49:37 GMT"：位置固定，逐段切
            const std::string_view dateText = body.substr(1, 9);
            const std::string_view timeText = body.substr(11, 8);
            if (body[10] != ' ' || body[19] != ' ' || body.substr(20, 3) != "GMT" || dateText[2] != '-' || dateText[6] != '-')
            {
                return std::nullopt;
            }

            unsigned twoDigitYear = 0;
            unsigned day          = 0;
            if (!parseDigits(dateText.substr(0, 2), day) || !parseDigits(dateText.substr(7, 2), twoDigitYear))
            {
                return std::nullopt;
            }
            const std::optional<std::size_t> monthIndex = findNameIndex(kMonthNames, dateText.substr(3, 3));
            if (!monthIndex.has_value())
            {
                return std::nullopt;
            }
            const unsigned year = twoDigitYear <= 69U ? 2000U + twoDigitYear : 1900U + twoDigitYear;

            unsigned hour   = 0;
            unsigned minute = 0;
            unsigned second = 0;
            if (!parseDigits(timeText.substr(0, 2), hour) || timeText[2] != ':' || !parseDigits(timeText.substr(3, 2), minute) || timeText[5] != ':' ||
                !parseDigits(timeText.substr(6, 2), second))
            {
                return std::nullopt;
            }
            return makeUtcTimePoint(year, static_cast<unsigned>(*monthIndex) + 1U, day, hour, minute, second);
        }

        /**
         * @brief 解析 asctime 的过时格式：`Sun Nov  6 08:49:37 1994`
         * @details 日占两位、个位数右对齐补空格，这是它跟其它两种最容易分叉的一处；年份本就是四位，
         *          不做折叠。时区段在该格式里不存在，按 GMT 处理（RFC 9110 §5.6.7 就是这么定的）。
         * @param text 已去首尾空白的整条文本
         * @return std::optional<std::chrono::system_clock::time_point> 形状不符时返回空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseAsctimeDate(const std::string_view text)
        {
            if (text.size() != kAsctimeLength)
            {
                return std::nullopt;
            }
            if (!findNameIndex(kWeekdayNames, text.substr(0, 3)).has_value() || text[3] != ' ' || text[7] != ' ' || text[10] != ' ' || text[19] != ' ')
            {
                return std::nullopt;
            }
            const std::optional<std::size_t> monthIndex = findNameIndex(kMonthNames, text.substr(4, 3));
            if (!monthIndex.has_value())
            {
                return std::nullopt;
            }

            // 日字段右对齐：个位数时首位是空格，把它当成空串再单独解析
            const std::string_view dayText = text.substr(8, 2);
            unsigned               day     = 0;
            if (dayText[0] == ' ')
            {
                if (!parseDigits(dayText.substr(1, 1), day))
                {
                    return std::nullopt;
                }
            } else if (!parseDigits(dayText, day))
            {
                return std::nullopt;
            }

            const std::string_view timeText = text.substr(11, 8);
            unsigned               hour     = 0;
            unsigned               minute   = 0;
            unsigned               second   = 0;
            if (!parseDigits(timeText.substr(0, 2), hour) || timeText[2] != ':' || !parseDigits(timeText.substr(3, 2), minute) || timeText[5] != ':' ||
                !parseDigits(timeText.substr(6, 2), second))
            {
                return std::nullopt;
            }
            unsigned year = 0;
            if (!parseDigits(text.substr(20, 4), year))
            {
                return std::nullopt;
            }
            return makeUtcTimePoint(year, static_cast<unsigned>(*monthIndex) + 1U, day, hour, minute, second);
        }
    } // namespace

    std::string_view formatHttpDate(const std::chrono::system_clock::time_point time, const std::span<char, kHttpDateTextLength> buffer) noexcept
    {
        const std::time_t                           calendarTime = std::chrono::system_clock::to_time_t(time);
        const Platform::UtcTimeFields               fields       = Platform::PlatformTime::utcTime(calendarTime);
        const std::array<char, kHttpDateTextLength> text         = buildHttpDateText(fields);
        for (std::size_t index = 0; index < kHttpDateTextLength; ++index)
        {
            buffer[index] = text[index];
        }
        return std::string_view(buffer.data(), buffer.size());
    }

    std::string formatHttpDate(const std::chrono::system_clock::time_point time)
    {
        // 拼装只有一份：按值交出的那条先落在栈上，再拷成调用方的 string
        std::array<char, kHttpDateTextLength> text{};
        const std::string_view                formatted = formatHttpDate(time, text);
        return std::string(formatted.begin(), formatted.end());
    }

    std::string_view currentHttpDateText()
    {
        thread_local std::int64_t                          cachedSecondOfEpoch = -1;
        thread_local std::array<char, kHttpDateTextLength> cachedText{};

        // Date 头每条响应都要写一份，而文本精度只到秒：同一秒内重复折算是纯浪费。
        // 判据用「不相等」而不是「更晚」，时钟被 NTP 往回调时也会照常重算，不会继续发未来的那一秒。
        // 缓存是 thread_local：各事件循环线程自己刷，不需要锁，也不会跨线程伪共享。
        const std::int64_t currentSecondOfEpoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (cachedSecondOfEpoch != currentSecondOfEpoch)
        {
            cachedText          = buildHttpDateText(Platform::PlatformTime::utcTime(static_cast<std::time_t>(currentSecondOfEpoch)));
            cachedSecondOfEpoch = currentSecondOfEpoch;
        }
        return std::string_view(cachedText.data(), cachedText.size());
    }

    std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text)
    {
        text = trimOptionalWhitespace(text);

        // IMF-fixdate 是定长格式：长度不符就不必逐字段试，直接转去认两种过时格式
        // （RFC 9110 §5.6.7 要求收端对它们保持兼容，判不出来就等于把老客户端的条件请求整条作废）
        if (text.size() != kHttpDateTextLength)
        {
            if (const auto rfc850Text = parseRfc850Date(text); rfc850Text.has_value())
            {
                return rfc850Text;
            }
            return parseAsctimeDate(text);
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

        const std::string_view monthText     = text.substr(8, 3);
        const auto             monthIterator = std::ranges::find(kMonthNames, monthText);
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

        unsigned hour   = 0;
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

        // 逐字段范围检查与折算同另外两种格式共用一份（含闰秒那一条）
        return makeUtcTimePoint(year, month, day, hour, minute, second);
    }

    std::optional<std::chrono::seconds> parseRetryAfter(std::string_view text, const std::chrono::system_clock::time_point now)
    {
        const std::string_view trimmed = trimOptionalWhitespace(text);
        if (trimmed.empty())
        {
            return std::nullopt;
        }

        // 第一种写法：delay-seconds（RFC 9110 §10.1.2 的 HTTP-Seconds，就是若干个十进制数字）。
        // 首字符不是数字就不可能是这一种，直接去认绝对日期
        if (const char firstCharacter = trimmed.front(); firstCharacter >= '0' && firstCharacter <= '9')
        {
            long long  secondsValue = 0;
            const auto parseResult  = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), secondsValue);
            // 必须看 ec 而不是只看停下来的位置：取值大到装不进 long long 时标准规定 ptr 指向末尾而
            // errc 是 result_out_of_range——只看 ptr 会把这种「读不懂」当成 0 交回，而 0 的含义是
            // 「现在就再试一次」，等于对着一台明确说了要限流的机器加速撞上去
            if (parseResult.ec == std::errc{} && parseResult.ptr == trimmed.data() + trimmed.size() && secondsValue >= 0)
            {
                return std::chrono::seconds{secondsValue};
            }
            // 混进非数字、或大到没有合法含义：宁可不认，也不编造一个对方没给过的等待时长
            return std::nullopt;
        }

        // 第二种写法：绝对的 HTTP-date，换算成还要等多久。已经过去的按 0 交回——调用方要的是一个能直接
        // 喂给定时器的正数，而「已经过去」的真实处置就是现在就能重试
        const std::optional<std::chrono::system_clock::time_point> moment = parseHttpDate(trimmed);
        if (!moment.has_value())
        {
            return std::nullopt;
        }
        const auto remainingSeconds = std::chrono::duration_cast<std::chrono::seconds>(moment.value() - now).count();
        return remainingSeconds > 0 ? std::optional{std::chrono::seconds{remainingSeconds}} : std::optional{std::chrono::seconds{0}};
    }

    std::chrono::system_clock::time_point timePointFromUnixSeconds(const std::int64_t unixSeconds) noexcept
    {
        // 「秒 × 时钟周期」这一步在本平台上是一次乘法（libstdc++ 的周期是 1 纳秒、MSVC 是 100 纳秒），
        // 越过 int64 上界就是有符号溢出——UB。它在运行期的落法很具体：**远期折回过去**，而这条路上
        // 走的数往往不是本框架写的：ext4/xfs 存得下 2262 年以后的 mtime，ASN.1 的 GENERALIZEDTIME
        // 允许 9999 年。按本时钟能表达的两端各钳一刀，方向与语义都对得上：
        // 超出可表达的远期 = 永不到期，超出可表达的久远过去 = 早已过期
        constexpr std::int64_t kTicksPerSecond = std::chrono::seconds{1} / std::chrono::system_clock::duration{1};
        constexpr std::int64_t maximumSeconds  = std::numeric_limits<std::int64_t>::max() / kTicksPerSecond;
        constexpr std::int64_t minimumSeconds  = std::numeric_limits<std::int64_t>::min() / kTicksPerSecond;
        if (unixSeconds >= maximumSeconds)
        {
            return std::chrono::system_clock::time_point::max();
        }
        if (unixSeconds <= minimumSeconds)
        {
            return std::chrono::system_clock::time_point::min();
        }
        return std::chrono::system_clock::time_point(std::chrono::seconds(unixSeconds));
    }
} // namespace AsynGyanis::Net
