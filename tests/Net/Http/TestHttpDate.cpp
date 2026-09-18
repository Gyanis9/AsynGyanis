// HttpDate 单元测试：IMF-fixdate 的格式化黄金值、解析往返与非法输入拒绝
#include "Net/Http/HttpDate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 由 Unix 秒构造时间点
         * @param seconds 自 1970-01-01T00:00:00Z 起的秒数
         * @return 对应的 system_clock 时间点
         */
        std::chrono::system_clock::time_point instantFromSeconds(const std::int64_t seconds)
        {
            return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
        }

        /**
         * @brief 取时间点的整秒值
         * @param time 时间点
         * @return 自 Unix 纪元起的秒数
         */
        std::int64_t secondsOf(const std::chrono::system_clock::time_point time)
        {
            return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
        }
    } // namespace

    TEST(HttpDate, FormatsKnownInstantAsImfFixdate)
    {
        // RFC 9110 §5.6.7 给出的样例时刻（1994-11-06T08:49:37Z）
        EXPECT_EQ(formatHttpDate(instantFromSeconds(784111777)), "Sun, 06 Nov 1994 08:49:37 GMT");
    }

    TEST(HttpDate, PadsSingleDigitDayWithLeadingZero)
    {
        // 个位数日必须补零：这条钉住 "03" 而不是 "3"，也不允许用空格补位
        EXPECT_EQ(formatHttpDate(instantFromSeconds(1788393600)), "Thu, 03 Sep 2026 00:00:00 GMT");
    }

    TEST(HttpDate, FormatsLeapDayAcrossCenturyRule)
    {
        // 2024 是普通闰年，2000 是「能被 400 整除」的世纪闰年：两者都必须落在 02-29
        EXPECT_EQ(formatHttpDate(instantFromSeconds(1709210096)), "Thu, 29 Feb 2024 12:34:56 GMT");
        EXPECT_EQ(formatHttpDate(instantFromSeconds(951868799)), "Tue, 29 Feb 2000 23:59:59 GMT");
    }

    TEST(HttpDate, FormatsUnixEpochAndSecondBeforeIt)
    {
        EXPECT_EQ(formatHttpDate(instantFromSeconds(0)), "Thu, 01 Jan 1970 00:00:00 GMT");
        EXPECT_EQ(formatHttpDate(instantFromSeconds(-1)), "Wed, 31 Dec 1969 23:59:59 GMT");
    }

    TEST(HttpDate, ProducesFixedLengthText)
    {
        EXPECT_EQ(formatHttpDate(instantFromSeconds(0)).size(), kHttpDateTextLength);
        EXPECT_EQ(formatHttpDate(instantFromSeconds(784111777)).size(), kHttpDateTextLength);
    }

    TEST(HttpDate, ParsesKnownTextBackToSameInstant)
    {
        const auto parsed = parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(secondsOf(*parsed), 784111777);
    }

    TEST(HttpDate, RoundTripsEveryFormattedInstant)
    {
        // 格式化与解析必须互逆：任一方向偏移一秒都会让条件请求永远命中不了
        const std::vector<std::int64_t> instants{0, 1, -1, 784111777, 1709210096, 1788393600, 951868799};

        for (const std::int64_t instant: instants)
        {
            const auto parsed = parseHttpDate(formatHttpDate(instantFromSeconds(instant)));
            ASSERT_TRUE(parsed.has_value()) << "秒值：" << instant;
            EXPECT_EQ(secondsOf(*parsed), instant) << "秒值：" << instant;
        }
    }

    TEST(HttpDate, ToleratesSurroundingWhitespace)
    {
        const auto parsed = parseHttpDate("  Sun, 06 Nov 1994 08:49:37 GMT\t");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(secondsOf(*parsed), 784111777);
    }

    TEST(HttpDate, RejectsMalformedTextWithoutThrowing)
    {
        // 每条都对应一类真实的畸形输入：缺 GMT、大小写不符、字段越界、月份下溢、多余尾巴
        const std::vector<std::string> malformedTexts{
                "",
                "Not a date",
                "Sun, 06 Nov 1994 08:49:37",
                "Sun, 06 Nov 1994 08:49:37 UTC",
                "Sun, 06 Nov 1994 08:49:37 gmt",
                "Sun, 06 Nov 1994 24:49:37 GMT",
                "Sun, 06 Nov 1994 08:60:37 GMT",
                "Sun, 06 Nov 1994 08:49:61 GMT",
                "Sun, 31 Feb 2024 00:00:00 GMT",
                "Xyz, 06 Nov 1994 08:49:37 GMT",
                "Sun, 06 Xxx 1994 08:49:37 GMT",
                "Sun, 6 Nov 1994 08:49:37 GMT",
                "Sun, 06 Nov 1994 08-49-37 GMT",
                "Sun, 06 Nov 1994 08:49:37 GMT extra",
                "Mon, 06 Nov 1994 08:49:37 GMT extra",
        };

        for (const std::string &malformedText: malformedTexts)
        {
            // 外部输入不能靠异常否定整个请求：解析失败一律交回空 optional
            EXPECT_FALSE(parseHttpDate(malformedText).has_value()) << "畸形输入：「" << malformedText << "」";
        }
    }
} // namespace AsynGyanis::Net
