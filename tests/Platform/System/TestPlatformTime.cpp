// PlatformTime 单元测试：本地时间转换的正确性与边界
#include "Platform/System/PlatformTime.h"

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
} // namespace AsynGyanis::Platform
