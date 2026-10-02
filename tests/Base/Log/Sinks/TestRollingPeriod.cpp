// 滚动周期换算直测：边界的严格性与「后缀换代」这条判据——按时间滚动是否真的发生全押在它们上

#include "Base/Log/Sinks/Detail/RollingPeriod.h"
#include "Base/Log/Sinks/RollingFileSink.h"

#include "BaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 由本地日历字段取 epoch 秒
         * @details 用例的输入全部按本地字段给（与 RollingFileSink 的换算同一口径），因此换时区
         *          只是换一批等价的时刻，判据本身不依赖运行环境的时区
         * @param year 公元年
         * @param month 月（1-12）
         * @param dayOfMonth 日
         * @param hour 时
         * @param minute 分
         * @param second 秒
         * @return std::time_t 对应的 epoch 秒
         */
        std::time_t localSeconds(const int year, const int month, const int dayOfMonth, const int hour, const int minute, const int second)
        {
            return std::chrono::system_clock::to_time_t(TestSupport::makeLocalMoment(year, month, dayOfMonth, hour, minute, second, 0));
        }
    } // namespace

    /**
     * @brief 边界恒严格晚于当前时刻，且不超过一个完整周期
     * @details 钉住两条让写入路径不空转的下界与上界：边界要是能等于当前时刻，`checkAndRoll` 就会
     *          每行都重做一遍本地时间换算与后缀格式化；要是能跑到一个周期之外，滚动就迟于周期本身
     */
    TEST(RollingPeriod, BoundaryIsStrictlyAheadAndNeverFurtherThanOnePeriod)
    {
        const std::vector<std::time_t> moments = {
                localSeconds(2026, 9, 10, 12, 34, 56),
                localSeconds(2026, 9, 10, 23, 59, 59),
                localSeconds(2026, 12, 31, 23, 59, 59),
                localSeconds(2026, 3, 15, 1, 0, 30),
        };

        for (const std::time_t moment: moments)
        {
            const std::time_t dailyBoundary  = Detail::nextRollingPeriodBoundary(moment, RollingPolicy::Daily);
            const std::time_t hourlyBoundary = Detail::nextRollingPeriodBoundary(moment, RollingPolicy::Hourly);

            EXPECT_GT(dailyBoundary, moment);
            EXPECT_LE(dailyBoundary - moment, Detail::kSecondsPerDay);
            EXPECT_GT(hourlyBoundary, moment);
            EXPECT_LE(hourlyBoundary - moment, Detail::kSecondsPerHour);
        }
    }

    /**
     * @brief 正好落在周期起点时，边界是一个完整周期之后而不是原地
     * @details 周期内已过秒数为 0 是边界算式最容易写错的一格（写成 `period - elapsed` 之外的形式
     *          就会得到 0 秒后，即每行都触发一次换算）
     */
    TEST(RollingPeriod, BoundaryFromPeriodStartIsOneWholePeriodAhead)
    {
        const std::time_t localMidnight  = localSeconds(2026, 9, 10, 0, 0, 0);
        const std::time_t localTopOfHour = localSeconds(2026, 9, 10, 9, 0, 0);

        EXPECT_EQ(Detail::nextRollingPeriodBoundary(localMidnight, RollingPolicy::Daily) - localMidnight, Detail::kSecondsPerDay);
        EXPECT_EQ(Detail::nextRollingPeriodBoundary(localTopOfHour, RollingPolicy::Hourly) - localTopOfHour, Detail::kSecondsPerHour);
    }

    /**
     * @brief 后缀在同一周期内稳定、跨周期必换代，且换代正好发生在边界那一刻
     * @details 这是按时间滚动的承重判据：`checkAndRoll` 只在「新后缀 != 当前后缀」时才改名换文件，
     *          后缀要是在跨周期时不变（例如按小时忘了拼小时），日志就永远写在一个文件里，
     *          而这从命名用例上看不出来。最后两条把「边界」与「后缀换代」这两件事钉在一起：
     *          边界到达时后缀必须已经换了代，否则边界的判定永远等不到动作
     */
    TEST(RollingPeriod, SuffixAdvancesExactlyWhenThePeriodAdvances)
    {
        const std::time_t noon     = localSeconds(2026, 9, 10, 12, 0, 0);
        const std::time_t sameHour = localSeconds(2026, 9, 10, 12, 59, 59);
        const std::time_t nextHour = localSeconds(2026, 9, 10, 13, 0, 0);
        const std::time_t nextDay  = localSeconds(2026, 9, 11, 12, 0, 0);

        EXPECT_EQ(Detail::rollingPeriodSuffix(noon, RollingPolicy::Hourly), Detail::rollingPeriodSuffix(sameHour, RollingPolicy::Hourly));
        EXPECT_EQ(Detail::rollingPeriodSuffix(noon, RollingPolicy::Daily), Detail::rollingPeriodSuffix(nextHour, RollingPolicy::Daily));

        EXPECT_NE(Detail::rollingPeriodSuffix(noon, RollingPolicy::Hourly), Detail::rollingPeriodSuffix(nextHour, RollingPolicy::Hourly));
        EXPECT_NE(Detail::rollingPeriodSuffix(noon, RollingPolicy::Daily), Detail::rollingPeriodSuffix(nextDay, RollingPolicy::Daily));

        for (const RollingPolicy policy: {RollingPolicy::Daily, RollingPolicy::Hourly})
        {
            const std::time_t boundary = Detail::nextRollingPeriodBoundary(noon, policy);
            EXPECT_NE(Detail::rollingPeriodSuffix(boundary, policy), Detail::rollingPeriodSuffix(noon, policy)) << "边界到达却仍是同一代后缀，滚动条件永远不成立";
            EXPECT_NE(Detail::rollingPeriodSuffix(boundary, policy), Detail::rollingPeriodSuffix(boundary - 1, policy)) << "边界前一秒与后一秒分属两代，这条判定不能空转";
        }
    }

    /**
     * @brief 单位数的月、日、时都补齐两位
     * @details 宽度是承重的：清理与滚动都按「主名.后缀.扩展名」在目录里认自己的产物，
     *          一位数的写法（`_3`）会让同一个 Sink 在不同月份拼出两种形态的名字
     */
    TEST(RollingPeriod, SuffixFieldsAreZeroPaddedToFixedWidth)
    {
        const std::time_t moment = localSeconds(2027, 1, 5, 3, 4, 5);

        EXPECT_EQ(Detail::rollingPeriodSuffix(moment, RollingPolicy::Daily), "2027-01-05");
        EXPECT_EQ(Detail::rollingPeriodSuffix(moment, RollingPolicy::Hourly), "2027-01-05_03");
    }

    /**
     * @brief 重算边界的判据：前进跨界要重算、周期内不重算、**时钟往回跳也要重算**
     * @details 第三条是这轮补的那格：墙上时钟被往回改（NTP 阶跃、手工调表、虚拟机恢复）之后，
     *          上一次算好的边界会跑到未来最多一整个周期，期间新日期的记录一直续写在旧日期的文件里，
     *          文件名与每行自己的时间戳从此对不上。第二条钉的是性能契约——每行只做一次 time_t 比较，
     *          周期内绝不能返航去做本地时间转换与后缀格式化。
     */
    TEST(RollingPeriod, RechecksBoundaryOnForwardCrossingAndBackwardClockStep)
    {
        const std::time_t now           = localSeconds(2027, 1, 5, 12, 0, 0);
        const std::time_t boundary      = localSeconds(2027, 1, 6, 0, 0, 0);
        const std::time_t onePeriodBack = localSeconds(2027, 1, 4, 23, 0, 0);

        // 跨过边界：重算
        EXPECT_TRUE(Detail::shouldRecheckRollingPeriod(boundary, now, boundary)) << "到了边界却不重算，按日期滚动就永远不发生";

        // 周期内且时钟在前进：不重算（这是每行一次比较那条性能契约的全部内容）
        EXPECT_FALSE(Detail::shouldRecheckRollingPeriod(now + std::time_t{60}, now, boundary)) << "周期内每行都去换算本地时间，热路径白付一次转换";

        // 时钟往回跳了一格（新读数比上一次读到的小）：必须重算，否则旧边界还要挡最多一整个周期
        EXPECT_TRUE(Detail::shouldRecheckRollingPeriod(now - std::time_t{1}, now, boundary)) << "时钟回拨被当成「还没到边界」，新日期的记录会继续写进旧日期的文件";

        // 同一时刻又回拨又跨界：两个条件任一成立都要重算
        EXPECT_TRUE(Detail::shouldRecheckRollingPeriod(onePeriodBack, now, boundary)) << "回拨与跨界同时成立时判据漏掉了回拨";

        // 首行（还没有过一次读数，lastObserved 是默认构造的 0）：只由边界决定，现实时刻不会误触发回拨分支
        EXPECT_FALSE(Detail::shouldRecheckRollingPeriod(now, 0, boundary)) << "首次写入被误判成时钟回拨";
    }
} // namespace AsynGyanis::Base
