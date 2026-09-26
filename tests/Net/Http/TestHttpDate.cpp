// HttpDate 单元测试：IMF-fixdate 的格式化黄金值、解析往返与非法输入拒绝
#include "Net/Http/HttpDate.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

        /// 一次「不跨秒」的采样结果：整秒与当时拿到的缓存文本
        struct StableDateSample
        {
            std::int64_t secondOfEpoch = 0; ///< 采样落在哪一秒
            std::string  text;              ///< 该秒对应的缓存文本
        };

        /// 等墙钟跨过当前秒的轮数上限：每轮 2 ms，1000 轮即 2 秒，正常一秒必然到
        constexpr int kSecondRollOverWaitRoundLimit = 1000;

        /**
         * @brief 采一次「整段都落在同一秒内」的 currentHttpDateText 结果
         * @details 缓存文本对应的是函数内部那次 now()，测试只能在调用前后各读一次墙钟来界定它。
         *          两次相等才说明这一段没有跨秒；恰好跨秒时不判失败，重来（本用例不赌调度时序）。
         * @param attemptLimit 重试上限
         * @return 取到可信样本时给出整秒与文本，否则为空
         */
        std::optional<StableDateSample> sampleStableCurrentHttpDateText(const int attemptLimit = 200)
        {
            for (int attempt = 0; attempt < attemptLimit; ++attempt)
            {
                const std::int64_t     secondBefore = secondsOf(std::chrono::system_clock::now());
                const std::string_view cachedText   = currentHttpDateText();
                const std::int64_t     secondAfter  = secondsOf(std::chrono::system_clock::now());
                if (secondBefore == secondAfter)
                {
                    return StableDateSample{.secondOfEpoch = secondBefore, .text = std::string(cachedText)};
                }
            }
            return std::nullopt;
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

    /**
     * @brief 定长缓冲那条出口与按值交出的那条逐字节同文，且产物确实落在调用方的缓冲里
     * @details 静态文件的 Last-Modified 走的是缓冲出口（每请求一份，29 字节超出小串内联）。两条必须
     *          同文，否则同一份文件在两次请求里会出现两种验证器；视图指回调用方的缓冲，才说明这条
     *          真的没碰堆。取样覆盖纪元、纪元前一秒与两个正常年份。
     */
    TEST(HttpDate, FixedBufferFormatProducesTheSameTextIntoTheCallersBuffer)
    {
        for (const std::int64_t seconds: {784111777LL, 0LL, -1LL, 1788393600LL})
        {
            const std::chrono::system_clock::time_point instant = instantFromSeconds(seconds);
            std::array<char, kHttpDateTextLength>       buffer{};
            const std::string_view                      formatted = formatHttpDate(instant, buffer);
            EXPECT_EQ(formatted, formatHttpDate(instant)) << "秒数 " << seconds << " 两条出口不同文";
            EXPECT_EQ(formatted.data(), buffer.data()) << "产物没落在调用方的缓冲里，这条还是在碰堆";
            EXPECT_EQ(formatted.size(), kHttpDateTextLength);
        }
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

    /**
     * @brief 钉住：按秒缓存的「此刻日期」与直接格式化同一秒的结果逐字相同
     * @details 缓存换掉的是重复折算，不是文本本身：任何一位数字、缩写或补零的差异，
     *          都会让对端看到与 RFC 9110 §5.6.7 不符的 Date 头。
     */
    TEST(HttpDate, CurrentHttpDateTextMatchesFormattingForTheSameSecond)
    {
        const std::optional<StableDateSample> sample = sampleStableCurrentHttpDateText();
        ASSERT_TRUE(sample.has_value()) << "没能在重试上限内取到不跨秒的样本";
        EXPECT_EQ(sample->text, formatHttpDate(instantFromSeconds(sample->secondOfEpoch)));
        EXPECT_EQ(sample->text.size(), kHttpDateTextLength);

        // 同一秒内两次取值必须完全一样：缓存的意义就是这一秒内不再重复折算
        const std::optional<StableDateSample> nextSample = sampleStableCurrentHttpDateText();
        ASSERT_TRUE(nextSample.has_value()) << "没能在重试上限内取到第二个不跨秒的样本";
        if (nextSample->secondOfEpoch == sample->secondOfEpoch)
        {
            EXPECT_EQ(nextSample->text, sample->text) << "同一秒内文本却变了";
        }
    }

    /**
     * @brief 钉住：秒一走到下一段，缓存文本必须跟着走，不能停在旧那一秒
     * @details 用例自己构造出「跨秒」这个条件：等到墙钟秒真的变了再取值，而不是假设采样时刻恰好安全。
     */
    TEST(HttpDate, CurrentHttpDateTextAdvancesWhenTheSecondRollsOver)
    {
        const std::optional<StableDateSample> firstSample = sampleStableCurrentHttpDateText();
        ASSERT_TRUE(firstSample.has_value()) << "没能在重试上限内取到第一个不跨秒的样本";

        // 等到墙钟真的跨过那一秒（上限 2 秒）：用例自己构造出「秒已改变」这个条件
        for (int waitRoundCount = 0; secondsOf(std::chrono::system_clock::now()) <= firstSample->secondOfEpoch; ++waitRoundCount)
        {
            ASSERT_LT(waitRoundCount, kSecondRollOverWaitRoundLimit) << "墙钟秒没有推进，环境时钟异常";
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        const std::optional<StableDateSample> laterSample = sampleStableCurrentHttpDateText();
        ASSERT_TRUE(laterSample.has_value()) << "没能在重试上限内取到跨秒后的样本";
        EXPECT_GT(laterSample->secondOfEpoch, firstSample->secondOfEpoch) << "秒已经变了，缓存却还停在旧的那一秒";
        EXPECT_EQ(laterSample->text, formatHttpDate(instantFromSeconds(laterSample->secondOfEpoch)));
    }

    /**
     * @brief 钉住：缓存是线程局部的，另一个线程上取值不依赖主线程那份
     * @details 共享一份缓存而不加同步，就是数据竞争（Linux 侧 TSan 会报）；这里从新线程采样一次，
     *          要求它与该线程自己看到的秒一致——线程局部缓存在新线程上首次调用必然重算，因此成立。
     */
    TEST(HttpDate, CurrentHttpDateTextIsSampledPerThread)
    {
        std::optional<StableDateSample> threadSample;
        std::thread                     worker([&threadSample] { threadSample = sampleStableCurrentHttpDateText(); });
        worker.join();

        ASSERT_TRUE(threadSample.has_value()) << "工作线程没能在重试上限内取到不跨秒的样本";
        EXPECT_EQ(threadSample->text, formatHttpDate(instantFromSeconds(threadSample->secondOfEpoch)));
    }
} // namespace AsynGyanis::Net
