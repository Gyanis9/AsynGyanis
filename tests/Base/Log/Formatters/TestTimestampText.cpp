// 时间戳渲染工具单元测试：挂钟字段、毫秒定长、按秒缓存的复用与刷新、预 1970 的整秒拆分

#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogEvent.h"

#include "BaseTestSupport.h"

#include "Platform/System/PlatformTime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cctype>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 渲染一个时刻并交出 owning 文本，便于按字符串比对
         */
        std::string renderToString(const TimestampMoment moment)
        {
            std::array<char, kTimestampTextBufferSize> buffer{};
            return std::string{formatTimestampText(buffer, moment)};
        }

        /**
         * @brief 参考文本：把 PlatformTime::localTime() 折出的日历按同一版式拼出来
         * @details 被测实现与这里共用 PlatformTime 这一个换算入口，因此本参考只钉「字段进位与
         *          版式」，不重复实现历法换算（那等于把被测代码抄一遍）
         */
        std::string referenceLocalText(const std::int64_t epochSeconds, const std::int64_t millisecondValue)
        {
            const std::tm localTime = Platform::PlatformTime::localTime(static_cast<std::time_t>(epochSeconds));
            return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                               localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                               localTime.tm_hour, localTime.tm_min, localTime.tm_sec, millisecondValue);
        }

        /**
         * @brief 判文本是否除固定分隔位外全是数字，且长度恰为常规 23 字符
         */
        bool hasExpectedShape(const std::string &text)
        {
            if (text.size() != kTimestampTextLength)
            {
                return false;
            }
            if (text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':' || text[16] != ':' || text[19] != '.')
            {
                return false;
            }
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                // 分隔符位置直接跳段：不落成命名变量，免得「只在条件里用一次的局部量」被 GCC 判成未使用
                if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19)
                {
                    continue;
                }
                if (!std::isdigit(static_cast<unsigned char>(text[index])))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    TEST(TimestampText, RendersTheLocalWallClockForAFixedMoment)
    {
        const TimestampMoment moment = TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789);

        // 期望文本与被折出的时刻取自同一组本地字段，因此这条断言在任意时区都成立
        EXPECT_EQ(renderToString(moment), "2026-09-10 12:34:56.789");
    }

    TEST(TimestampText, MillisecondFieldIsAlwaysThreeDigits)
    {
        // 毫秒位数 0/1/2/3 各取一次：补齐零的成本落在渲染侧，不留给各版式的格式化器
        for (const int millisecondValue: {0, 5, 42, 999})
        {
            const TimestampMoment moment = TestSupport::makeLocalMoment(2026, 1, 2, 3, 4, 5, millisecondValue);
            const std::string     text   = renderToString(moment);

            ASSERT_EQ(text.size(), kTimestampTextLength) << "毫秒 " << millisecondValue;
            EXPECT_EQ(text[19], '.');
            EXPECT_EQ(text.substr(20), std::format("{:03d}", millisecondValue)) << "毫秒 " << millisecondValue;
        }
    }

    TEST(TimestampText, SubSecondInstantBeforeEpochKeepsNonNegativeMilliseconds)
    {
        // epoch 往前 1.5 秒落在 1969-12-31 23:59:58.500（本地时区偏移不参与这个判定，
        // 断言比的是「整秒向下取整 + 非负残差」这条拆分规则）
        const TimestampMoment  moment = TimestampMoment{} - std::chrono::milliseconds(1500);
        const std::string      text   = renderToString(moment);

        ASSERT_EQ(text.size(), kTimestampTextLength) << text;
        EXPECT_EQ(text[19], '.') << text;
        EXPECT_EQ(text.substr(20), "500") << text;
        // 整秒必须取 -2 而不是 -1：朝零截断会给出晚整整一秒的挂钟，且残差成了负数
        EXPECT_EQ(text.substr(0, 19), referenceLocalText(-2, 0).substr(0, 19)) << text;
    }

    TEST(TimestampText, PerSecondCacheRefreshesThePrefixWhenTheSecondAdvances)
    {
        std::array<char, kTimestampTextBufferSize> buffer{};

        // 同一秒内换毫秒：前缀走缓存，尾部四位必须被逐次改写，不留上一条的残余
        const TimestampMoment firstMoment  = TestSupport::makeLocalMoment(2026, 6, 1, 10, 0, 0, 111);
        const TimestampMoment sameSecond   = firstMoment + std::chrono::milliseconds(222);
        EXPECT_EQ(std::string_view{formatTimestampText(buffer, firstMoment)}, "2026-06-01 10:00:00.111");
        EXPECT_EQ(std::string_view{formatTimestampText(buffer, sameSecond)}, "2026-06-01 10:00:00.333")
                << "同秒的第二次渲染没换毫秒";

        // 跨过一秒：缓存必须按新的整秒重折日历，而不是只改毫秒
        const TimestampMoment nextSecond = TestSupport::makeLocalMoment(2026, 6, 1, 10, 0, 1, 5);
        EXPECT_EQ(std::string_view{formatTimestampText(buffer, nextSecond)}, "2026-06-01 10:00:01.005")
                << "按秒缓存没有刷新前缀";

        // 时钟回拨同样要跟着回退：缓存键是整秒值本身，不是「比上次大就更新」
        EXPECT_EQ(std::string_view{formatTimestampText(buffer, firstMoment)}, "2026-06-01 10:00:00.111")
                << "回拨后的时刻被上一条的缓存顶掉了";
    }

    TEST(TimestampText, ConsecutiveMomentsNeverRenderBackwards)
    {
        std::string previous = renderToString(std::chrono::system_clock::now());

        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const std::string current = renderToString(std::chrono::system_clock::now());

            ASSERT_EQ(current.size(), kTimestampTextLength) << "iteration " << iteration;
            // 定长且高位在前的版式里，字典序就是时间序
            EXPECT_GE(current, previous) << "iteration " << iteration;
            previous = current;
        }
    }

    TEST(TimestampText, ShapeHoldsAcrossAStressLoop)
    {
        int failureCount = 0;
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            if (!hasExpectedShape(renderToString(std::chrono::system_clock::now())))
            {
                ++failureCount;
            }
        }

        EXPECT_EQ(failureCount, 0);
    }

    TEST(TimestampText, ExtremeMomentNeverWritesPastTheCallerBuffer)
    {
        // 系统时钟的两端都拿真实缓冲试一次：这些时刻折不成合法挂钟（换算失败交出零值日历），
        // 但写入长度必须始终落在缓冲之内——越出一个 std::array 会被 ASan 当场抓住
        for (const TimestampMoment moment: {(std::chrono::system_clock::time_point::max)(),
                                            (std::chrono::system_clock::time_point::min)()})
        {
            std::array<char, kTimestampTextBufferSize> buffer{};
            const std::string_view                     text = formatTimestampText(buffer, moment);

            EXPECT_LE(text.size(), buffer.size()) << "渲染写出的长度越出了调用方给的缓冲";
        }
    }
} // namespace AsynGyanis::Base
