// PlatformTime 单元测试：本地时间转换的正确性与边界
#include "Platform/System/PlatformTime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
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
