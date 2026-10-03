// HttpDate 单元测试：IMF-fixdate 的格式化黄金值、解析往返与非法输入拒绝
#include "Net/Http/HttpDate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
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
    /**
     * @brief 钉住：RFC 850 的过时格式解析到与等价 IMF 文本同一时刻
     * @details 判据不写死秒数，而是拿本层自己的 IMF 通路当参照：两条必须落在同一刻，
     *          写死数字的话参照物本身错了也测不出来。
     */
    TEST(HttpDate, ObsoleteRfc850FormatMatchesItsImfEquivalent)
    {
        const std::optional<std::chrono::system_clock::time_point> obsolete  = parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT");
        const std::optional<std::chrono::system_clock::time_point> reference = parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");

        ASSERT_TRUE(obsolete.has_value()) << "RFC 850 的日期被判成不可解析：老客户端的条件请求会整条作废";
        ASSERT_TRUE(reference.has_value());
        EXPECT_EQ(secondsOf(*obsolete), secondsOf(*reference));
        EXPECT_EQ(secondsOf(*obsolete), 784111777LL) << "RFC 9110 的样例时刻本身就是黄金值";
    }

    /**
     * @brief RFC 850 的星期写成三字母缩写也要收（老客户端两种都在发）
     */
    TEST(HttpDate, ObsoleteRfc850AcceptsAbbreviatedWeekday)
    {
        const std::optional<std::chrono::system_clock::time_point> abbreviated = parseHttpDate("Sun, 06-Nov-94 08:49:37 GMT");

        ASSERT_TRUE(abbreviated.has_value());
        EXPECT_EQ(secondsOf(*abbreviated), 784111777LL);
    }

    /**
     * @brief 两位年份按固定规则折叠：0..69 记 2000 年代，70..99 记 1900 年代
     * @details 折叠必须是确定的算术，不能「挑离现在最近的世纪」——那样同一条头会在某个时刻之后
     *          解析出另一个世纪，缓存验证器就不可复现了。
     */
    TEST(HttpDate, ObsoleteRfc850TwoDigitYearFoldsAtFixedBoundary)
    {
        const std::optional<std::chrono::system_clock::time_point> lowerCentury = parseHttpDate("Sunday, 06-Nov-69 08:49:37 GMT");
        const std::optional<std::chrono::system_clock::time_point> upperCentury = parseHttpDate("Sunday, 06-Nov-70 08:49:37 GMT");
        const std::optional<std::chrono::system_clock::time_point> year2069     = parseHttpDate("Sun, 06 Nov 2069 08:49:37 GMT");
        const std::optional<std::chrono::system_clock::time_point> year1970     = parseHttpDate("Thu, 06 Nov 1970 08:49:37 GMT");

        ASSERT_TRUE(lowerCentury.has_value() && upperCentury.has_value() && year2069.has_value() && year1970.has_value());
        EXPECT_EQ(secondsOf(*lowerCentury), secondsOf(*year2069)) << "69 应当落在 2000 年代";
        EXPECT_EQ(secondsOf(*upperCentury), secondsOf(*year1970)) << "70 应当落在 1900 年代";
    }

    /**
     * @brief 钉住：asctime 的两种日宽（个位补空格与两位）都解析到同一 IMF 时刻
     */
    TEST(HttpDate, ObsoleteAsctimeFormatMatchesItsImfEquivalentForBothDayWidths)
    {
        const std::optional<std::chrono::system_clock::time_point> paddedDay       = parseHttpDate("Sun Nov  6 08:49:37 1994");
        const std::optional<std::chrono::system_clock::time_point> wideDay         = parseHttpDate("Wed Nov 16 08:49:37 1994");
        const std::optional<std::chrono::system_clock::time_point> paddedReference = parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
        const std::optional<std::chrono::system_clock::time_point> wideReference   = parseHttpDate("Wed, 16 Nov 1994 08:49:37 GMT");

        ASSERT_TRUE(paddedDay.has_value()) << "日右对齐补空格是 asctime 的原形，判不可解析等于该格式没被实现";
        ASSERT_TRUE(wideDay.has_value());
        ASSERT_TRUE(paddedReference.has_value() && wideReference.has_value());
        EXPECT_EQ(secondsOf(*paddedDay), secondsOf(*paddedReference));
        EXPECT_EQ(secondsOf(*wideDay), secondsOf(*wideReference));
    }

    /**
     * @brief 过时格式的畸形写法照样拒绝：分隔符、名称、日宽、越界日期与时分秒
     * @details 放宽到「两种过时格式」不等于放宽成「什么像日期都收」：收错了会把一个不存在的
     *          验证器当成有效，比判不出来更糟。三条 `00` 的日子钉的是同一件事——日的下界必须
     *          在三种格式上一致：`day` 是无符号的，共享判据只写上界时，`00` 会一路穿到日期换算里，
     *          悄悄给出上个月最后一天（旧实现里 `Sun Nov 00 …` 读出来是 1994-10-31，而 IMF 同样
     *          写法的 `00` 一直是被拒的那一个）。
     */
    TEST(HttpDate, MalformedObsoleteFormatsAreRejected)
    {
        const std::vector<std::string_view> rejectedTexts{
                "Sunday, 06/Nov/94 08:49:37 GMT", // 日期段分隔符错
                "Sundan, 06-Nov-94 08:49:37 GMT", // 星期名不是七个之一
                "Sunday, 06-Nov-94 08:49:37 UTC", // 该格式的时区段固定写作 GMT
                "Sun Nov 6 08:49:37 1994",        // asctime 的日必须占两位
                "Sun Nov  6 08:49:37 199",        // 年份不足四位，整条长度也不符
                "Sunday, 31-Apr-94 08:49:37 GMT", // 四月没有三十一日
                "Sun Nov  6 24:49:37 1994",       // 小时越界
                "Sunday, 00-Nov-94 08:49:37 GMT", // 日历里没有 0 日：RFC 850 那侧曾把它读成上月最后一天
                "Sun Nov 00 08:49:37 1994",       // asctime 同一条下界
                "Thu, 00 Nov 1994 08:49:37 GMT",  // IMF 一直判着下界，这一格钉住它没被放宽
        };

        for (const std::string_view text: rejectedTexts)
        {
            EXPECT_FALSE(parseHttpDate(text).has_value()) << "这条本该判不可解析：" << text;
        }
    }

    /**
     * @brief 钉住：对端写得出的最远/最近年份折进本时钟的可表达范围，不绕成另一个方向
     * @details 年份段是四位数字，所以对端能写出的极限就是 9999 与 0001。折成 time_point 要把「秒」
     *          乘上时钟周期，而周期是实现定义的：libstdc++ 是 1 纳秒，只表达到 2262 年；MSVC 是 100
     *          纳秒，9999 年还在范围内。前者不钳这一刀就踩上有符号溢出（UBSan 报的正是那次乘法），
     *          落法是「远期翻到过去」——一句 `Expires=..., 9999` 的 Cookie 当场被当成已过期摘掉。
     *          期望值从时钟自己的两个端点现读，不写死任何一家的周期，因此两平台各自判各自的那一档。
     */
    TEST(HttpDate, ExtremeYearsClampIntoClockRangeInsteadOfWrapping)
    {
        // 9999-12-31T23:59:59Z 与 0001-01-01T00:00:00Z：四位年份能写出的两头
        constexpr std::int64_t kFarFutureSecond = 253'402'300'799LL;
        constexpr std::int64_t kFarPastSecond   = -62'135'596'800LL;

        const std::int64_t largestExpressibleSecond  = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point::max().time_since_epoch()).count();
        const std::int64_t smallestExpressibleSecond = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point::min().time_since_epoch()).count();

        const std::optional<std::chrono::system_clock::time_point> farFuture = parseHttpDate("Fri, 31 Dec 9999 23:59:59 GMT");
        const std::optional<std::chrono::system_clock::time_point> farPast   = parseHttpDate("Mon, 01 Jan 0001 00:00:00 GMT");

        ASSERT_TRUE(farFuture.has_value()) << "远期日期被判不可解析：这类写法在真实服务器上一直在发";
        ASSERT_TRUE(farPast.has_value());
        EXPECT_EQ(secondsOf(*farFuture), std::min(kFarFutureSecond, largestExpressibleSecond)) << "远期没有钳在时钟上界，而是绕了回去";
        EXPECT_EQ(secondsOf(*farPast), std::max(kFarPastSecond, smallestExpressibleSecond)) << "远期过去没有钳在时钟下界，而是绕到了将来";

        // 方向单独再钉一刀：绕回去时这一条先红，报错信息比对着上一步的秒数好读
        EXPECT_GT(*farFuture, std::chrono::system_clock::now() + std::chrono::years{100});
        EXPECT_LT(*farPast, std::chrono::system_clock::now() - std::chrono::years{100});
    }

    /**
     * @brief 外来的「Unix 秒」折成 time_point 时按可表达范围钳两端，而不是让那次乘法溢出
     * @details `time_point(seconds(n))` 是一次「秒 × 时钟周期」的乘法，n 越过 int64 上界就是 UB，
     *          运行期的落法是远期折回过去。这条路上的秒数都不是本框架写的：文件系统的 mtime
     *          （ext4/xfs 存得下 2262 年以后）、证书里的 notAfter（ASN.1 允许 9999 年）。
     *          上面那条钉的是「文本解析」那一侧的钳位，这一条钉「手里已经有一个秒数」这一侧，
     *          并且两侧各配一格可表达的值——只钳不还原会把正常值一起吃掉。
     */
    TEST(HttpDate, TimePointFromUnixSecondsClampsBothEnds)
    {
        const std::int64_t largestExpressibleSecond  = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point::max().time_since_epoch()).count();
        const std::int64_t smallestExpressibleSecond = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point::min().time_since_epoch()).count();

        EXPECT_EQ(timePointFromUnixSeconds(std::numeric_limits<std::int64_t>::max()), std::chrono::system_clock::time_point::max()) << "远到无法表达的秒数绕回了过去";
        EXPECT_EQ(timePointFromUnixSeconds(std::numeric_limits<std::int64_t>::min()), std::chrono::system_clock::time_point::min()) << "久远到无法表达的秒数绕到了将来";

        // 能表达的那两格要原样折过去：只看到「钳」而把正常值也压到端点上，这条先红
        EXPECT_EQ(secondsOf(timePointFromUnixSeconds(largestExpressibleSecond)), largestExpressibleSecond);
        EXPECT_EQ(secondsOf(timePointFromUnixSeconds(smallestExpressibleSecond)), smallestExpressibleSecond);
        EXPECT_EQ(secondsOf(timePointFromUnixSeconds(1'700'000'000LL)), 1'700'000'000LL);

        // 与文本解析那一侧同一条口径：同一年月日不论从日期串还是从秒数进来，落点必须一致
        const std::optional<std::chrono::system_clock::time_point> farFuture = parseHttpDate("Fri, 31 Dec 9999 23:59:59 GMT");
        ASSERT_TRUE(farFuture.has_value());
        EXPECT_EQ(timePointFromUnixSeconds(secondsOf(*farFuture)), *farFuture);
    }

    /**
     * @brief `Retry-After` 的两种写法都要认，且读不懂时交回空而不是 0
     * @details RFC 9110 §10.2.3 允许相对秒数与绝对的 HTTP-date 两种形状，本框架自己在 429/503 上发的
     *          是前者，而机构可能回后者。把「读不懂」折成 0 秒是最坏的一种省事：0 的含义是「现在就再
     *          试一次」，那等于在限流现场把对端的警告丢掉并加速撞上去，所以判据是空 optional。
     *          绝对的过去折成 0 是对的——那正是「等待已经结束」的意思。
     */
    TEST(HttpDate, ParsesRetryAfterInBothForms)
    {
        const std::chrono::system_clock::time_point now = instantFromSeconds(2'000'000'000);

        EXPECT_EQ(parseRetryAfter("3600", now), std::optional{std::chrono::seconds{3600}});
        EXPECT_EQ(parseRetryAfter("0", now), std::optional{std::chrono::seconds{0}}) << "0 是合法写法：机构说的是现在就能再试";
        EXPECT_EQ(parseRetryAfter("  42  ", now), std::optional{std::chrono::seconds{42}}) << "首尾空白是头部取值的一部分";

        // 绝对时刻：未来按差值交回，过去一律 0
        std::array<char, kHttpDateTextLength> futureBuffer{};
        const std::string_view                futureText = formatHttpDate(instantFromSeconds(2'000'000'000 + 120), futureBuffer);
        EXPECT_EQ(parseRetryAfter(futureText, now), std::optional{std::chrono::seconds{120}});

        std::array<char, kHttpDateTextLength> pastBuffer{};
        const std::string_view                pastText = formatHttpDate(instantFromSeconds(2'000'000'000 - 999), pastBuffer);
        EXPECT_EQ(parseRetryAfter(pastText, now), std::optional{std::chrono::seconds{0}}) << "已经过去的时刻应当读成「现在就能再试」，而不是负数或空";

        // 拒绝面：形状不对就不给数，宁缺毋造
        for (const std::string_view rejectedText: {std::string_view{""}, std::string_view{"abc"}, std::string_view{"-5"}, std::string_view{"1.5"}, std::string_view{"1e3"},
                                                   std::string_view{"12x"}, std::string_view{"99999999999999999999999"}, std::string_view{"Wed, 33 Xyz 2026 00:00:00 GMT"}})
        {
            EXPECT_FALSE(parseRetryAfter(rejectedText, now).has_value()) << "这条本该判读不懂：" << rejectedText;
        }
    }
} // namespace AsynGyanis::Net
