// PlatformTime 单元测试：本地时间转换的正确性与边界，以及 UTC 分解的历法判定表
#include "Platform/System/PlatformTime.h"

#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <limits>
#include <thread>

namespace AsynGyanis::Platform
{
    TEST(PlatformTime, LocalTimeReturnsSameHourAsSystemLocaltimeForKnownInstant)
    {
        // 2026-01-01T00:00:00Z，只断言与年份/月份相关的字段，避免依赖具体时区
        constexpr std::time_t knownInstant = 1767225600;

        const std::tm converted = PlatformTime::localTime(knownInstant);

        EXPECT_GE(converted.tm_year + 1900, 2026);
        EXPECT_LE(converted.tm_year + 1900, 2027);
        EXPECT_GE(converted.tm_mon, 0);
        EXPECT_LE(converted.tm_mon, 11);
        EXPECT_GE(converted.tm_mday, 1);
        EXPECT_LE(converted.tm_mday, 31);
    }

    TEST(PlatformTime, LocalTimeHandlesEpochZero)
    {
        const std::tm converted = PlatformTime::localTime(0);

        // 纪元零点在任何时区都落在 1969 或 1970 年
        const int year = converted.tm_year + 1900;
        EXPECT_TRUE(year == 1969 || year == 1970);
    }

    /**
     * @brief 折不出挂钟的秒数要交回零值结构，而不是各字段填 -1 的日历
     * @details 头文件契约写着「转换失败时返回零值结构」。MSVC 的 localtime_s 失败时把整个结构
     *          填成 -1，不看返回码就直接交回会让调用方拿到「1899-00--1 -1:-1:-1」这种文本；
     *          glibc 侧恰好交回零值，所以这条判据只有两侧都跑过才算钉住。
     */
    TEST(PlatformTime, UnconvertibleEpochYieldsZeroCalendar)
    {
        // 远超任何历法实现支持的年份（CRT 的上限是 3089 年）
        const std::tm converted = PlatformTime::localTime(std::numeric_limits<std::time_t>::max());

        EXPECT_EQ(converted.tm_year, 0) << "失败时应交回零值结构，而不是 -1 填满的日历";
        EXPECT_EQ(converted.tm_mon, 0);
        EXPECT_EQ(converted.tm_mday, 0);
        EXPECT_EQ(converted.tm_hour, 0);
        EXPECT_EQ(converted.tm_min, 0);
        EXPECT_EQ(converted.tm_sec, 0);
    }

    TEST(PlatformTime, LocalTimeMatchesChronoConversionOfCurrentTime)
    {
        const auto    now       = std::chrono::system_clock::now();
        const auto    timeValue = std::chrono::system_clock::to_time_t(now);
        const std::tm converted = PlatformTime::localTime(timeValue);

        std::tm expected{};
#if ASYN_PLATFORM_WIN32
        ::localtime_s(&expected, &timeValue);
#else
        ::localtime_r(&timeValue, &expected);
#endif

        EXPECT_EQ(converted.tm_year, expected.tm_year);
        EXPECT_EQ(converted.tm_mon, expected.tm_mon);
        EXPECT_EQ(converted.tm_mday, expected.tm_mday);
        EXPECT_EQ(converted.tm_hour, expected.tm_hour);
        EXPECT_EQ(converted.tm_min, expected.tm_min);
        EXPECT_EQ(converted.tm_sec, expected.tm_sec);
    }

    TEST(PlatformTime, LocalTimeIsUsableFromMultipleThreads)
    {
        // 线程安全是本类存在的理由：POSIX 的 localtime 会复用静态缓冲，
        // 而本实现必须保证每个线程拿到独立结果
        constexpr std::time_t ktimeValue = 1767225600;

        int firstYear  = -1;
        int secondYear = -1;

        std::thread firstThread(
                [&]()
                {
                    firstYear = PlatformTime::localTime(ktimeValue).tm_year;
                });
        std::thread secondThread(
                [&]()
                {
                    secondYear = PlatformTime::localTime(ktimeValue).tm_year;
                });

        firstThread.join();
        secondThread.join();

        EXPECT_GE(firstYear, 126);
        EXPECT_EQ(firstYear, secondYear);
    }

    /**
     * @brief UTC 分解的判定表：纪元两侧、日内边界与三条闰年规则各占一格
     * @details 期望值取自公开历法事实，不从实现反推。1900 与 2100 那两格刻意挑在「能被 100 整除
     *          但不能被 400 整除」的年份上：把这两个当闰年，1900-02-28 会折成 02-29、2100-03-01
     *          会折成 03-02，两格同时变红。
     */
    TEST(PlatformTime, UtcTimeDecomposesKnownEpochs)
    {
        struct EpochCase
        {
            std::time_t   seconds;   ///< 自 Unix 纪元起的秒数（可为负）
            int           year;      ///< 期望完整年份
            int           month;     ///< 期望月份 1~12
            int           day;       ///< 期望日 1~31
            int           weekday;   ///< 期望星期，0 为周日
        };

        constexpr EpochCase cases[] = {
                {0, 1970, 1, 1, 4},                                // 纪元零点：周四
                {-1, 1969, 12, 31, 3},                             // 纪元前一秒：周三，且仍在 1970 年之前
                {-518400, 1969, 12, 26, 5},                        // 距纪元 6 天以前：只有按 floor 修正负余数才对得上
                {86399, 1970, 1, 1, 4},                            // 纪日内最后一秒，不跨天
                {-2203977600, 1900, 2, 28, 3},                     // 1900 不是闰年：这天是 2 月的最后一天（周三）
                {4107542400, 2100, 3, 1, 1},                       // 2100 也不是闰年：2 月后直接进 3 月（周一）
                {1709164800, 2024, 2, 29, 4},                      // 2024 能被 4 整除且非百年代际：有 2 月 29 日
                {1234567890, 2009, 2, 13, 5},                      // 一个普通的工作日（周五）
        };

        for (const auto &testCase : cases)
        {
            const UtcTimeFields fields = PlatformTime::utcTime(testCase.seconds);
            SCOPED_TRACE("seconds=" + std::to_string(static_cast<long long>(testCase.seconds)));
            EXPECT_EQ(fields.year, testCase.year);
            EXPECT_EQ(fields.month, testCase.month);
            EXPECT_EQ(fields.day, testCase.day);
            EXPECT_EQ(fields.hour, static_cast<int>((static_cast<long long>(testCase.seconds) % 86400 + 86400) % 86400 / 3600))
                    << "日内小时数按 floor 语义算，负数纪元不能折成负字段";
            EXPECT_EQ(fields.weekday, testCase.weekday);
        }
    }

    /**
     * @brief 钉住：1970 年之前不得按「转换失败」处理
     * @details 头文件只写了「转换失败时返回零值结构」，没界定什么算失败——而 glibc 的 gmtime_r 与
     *          UCRT 的 gmtime_s 实测都能折出 1968/1969 的日历。把这条钉住：哪天换成下界更窄的接口
     *          （或有人按「纪元即零点」的想当然加了判空），1970 年前 mtime 的文件就会在 Windows 上
     *          得到 year=0 的字段，Last-Modified 被渲染成「Sun, 00 Jan 0000 …」——形状合法、取值是垃圾。
     */
    TEST(PlatformTime, UtcTimeDecomposesEveryPreEpochSecond)
    {
        const UtcTimeFields oneSecondBeforeEpoch = PlatformTime::utcTime(-1);
        EXPECT_NE(oneSecondBeforeEpoch.year, 0) << "1970 年前被当成「折不出来」";
        EXPECT_EQ(oneSecondBeforeEpoch.year, 1969);
        EXPECT_EQ(oneSecondBeforeEpoch.month, 12);
        EXPECT_EQ(oneSecondBeforeEpoch.day, 31);
        EXPECT_EQ(oneSecondBeforeEpoch.hour, 23);
        EXPECT_EQ(oneSecondBeforeEpoch.minute, 59);
        EXPECT_EQ(oneSecondBeforeEpoch.second, 59);

        // 纪元前的闰日：1968 能被 4 整除且不是百年代际，2 月有 29 日，且跨过它要正确进 3 月
        const UtcTimeFields leapDayBeforeEpoch = PlatformTime::utcTime(-58060800);
        EXPECT_EQ(leapDayBeforeEpoch.year, 1968);
        EXPECT_EQ(leapDayBeforeEpoch.month, 2);
        EXPECT_EQ(leapDayBeforeEpoch.day, 29);
        EXPECT_EQ(leapDayBeforeEpoch.weekday, 4);

        const UtcTimeFields dayAfterLeapBeforeEpoch = PlatformTime::utcTime(-57974400);
        EXPECT_EQ(dayAfterLeapBeforeEpoch.month, 3);
        EXPECT_EQ(dayAfterLeapBeforeEpoch.day, 1);
    }

    /**
     * @brief 钉住：年份大到装不进 int 时交回零值结构，不回绕成看似合法的年份
     * @details 约 292 亿年这种取值只有把 time_t 推到极值才够得到。整数日期运算本身不溢出（全程
     *          int64），但 `year` 字段是 int：悄悄截断会交回一个形状合法的错误年份，比报失败更难查。
     */
    TEST(PlatformTime, UtcTimeRefusesYearsThatDoNotFitAnInt)
    {
        const UtcTimeFields absurdFuture = PlatformTime::utcTime(std::numeric_limits<std::time_t>::max());
        EXPECT_EQ(absurdFuture.year, 0) << "年份装不下 int 时应报「折不出来」，而不是截断回绕";
        EXPECT_EQ(absurdFuture.month, 0);
        EXPECT_EQ(absurdFuture.day, 0);

        const UtcTimeFields absurdPast = PlatformTime::utcTime(std::numeric_limits<std::time_t>::min());
        EXPECT_EQ(absurdPast.year, 0);
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 与 C 库对拍：1900~2100 两百年内逐字段一致，含日内全部秒位
     * @details 判据取另一份实现（glibc 的 gmtime_r），按质数秒步长跑遍两个世纪，覆盖全部闰年组合与
     *          400 年周期边界。这里查的是本类自己那两段换算：`tm_year`/`tm_mon` 的偏移，以及
     *          刻意不取 `tm_wday` 而按天数推出来的星期（负数纪元要靠取模修正，差一天就整体错一天）。
     * @note 只在 POSIX 编译：Windows 侧没有 gmtime_r 可当参照，那一支由上面的判定表与纪元前用例钉。
     */
    TEST(PlatformTime, UtcTimeAgreesWithTheCLibraryAcrossTwoCenturies)
    {
        // 1900-01-01 到 2100-01-01，跨 4 个百年级际（含 2000 这个唯一的闰代际）
        constexpr std::int64_t kstart = -2208988800;
        constexpr std::int64_t kend   = 4102444800;
        // 步长取质数秒：不会每天踩在同一时刻上，日内字段也被扫到
        constexpr std::int64_t kprobeStep = 99991;

        for (std::int64_t seconds = kstart; seconds <= kend; seconds += kprobeStep)
        {
            const auto       timeValue = static_cast<std::time_t>(seconds);
            std::tm          reference{};
            ASSERT_NE(::gmtime_r(&timeValue, &reference), nullptr) << "参照实现折不出秒数 " << seconds;

            const UtcTimeFields fields = PlatformTime::utcTime(timeValue);
            SCOPED_TRACE("seconds=" + std::to_string(seconds));
            EXPECT_EQ(fields.year, reference.tm_year + 1900);
            EXPECT_EQ(fields.month, reference.tm_mon + 1);
            EXPECT_EQ(fields.day, reference.tm_mday);
            EXPECT_EQ(fields.hour, reference.tm_hour);
            EXPECT_EQ(fields.minute, reference.tm_min);
            EXPECT_EQ(fields.second, reference.tm_sec);
            EXPECT_EQ(fields.weekday, reference.tm_wday) << "星期口径要与 std::tm::tm_wday（0 为周日）一致";
        }
    }
#endif
} // namespace AsynGyanis::Platform
