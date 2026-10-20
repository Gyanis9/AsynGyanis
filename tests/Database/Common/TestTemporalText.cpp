// 日期时间文本的解析与渲染单元测试：认什么、不认什么、渲染成什么、来回是否同解。
// 覆盖场景：
// RendersUnixEpochAsCivilAnchor / RendersOneMicrosecondBeforeEpochOnPreviousDay
// RoundTripsParsedTextThroughRenderer / WallClockRoundTripsWithoutAnyOffset
// AcceptsOffsetsInEveryShapeTheEnginesGive / SystemClockFoldsOffsetIntoUtc
// RejectsOffsetMinutesAtOrAboveSixty
// AcceptsDateOnlyTextAndSecondsWithoutFraction / SixtySecondsRollsIntoNextMinuteLikeTheEngines
// RejectsZeroDatesAndNonIsoStyles / RejectsInfinityAndUndottedAmbiguities
// TruncatesFractionsBeyondMicroseconds / RejectsFractionsLongerThanNineDigits
// RejectsImpossibleCalendarDays / WallClockRefusesTextCarryingAnOffset
// NegativeYearsRoundTrip / MySqlAndPostgresRenderingsDifferByOffsetMarker
// 这一格是本件的全部纯逻辑：三家引擎给的都是文本，「认与不认」错了就是静默改数，
// 所以两边都要钉 —— 认的要逐字符对回原意，不认的要有名有姓地拒掉。全部用例不碰驱动与服务端。

#include "Database/Common/DatabaseValue.h"
#include "Database/Common/TemporalText.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Database::Detail
{
    namespace
    {
        using namespace std::chrono;

        /// 取一段文本的解析结果；解析不出来就直接让用例失败，省下满屏的 has_value 断言
        ParsedDateTime parsed(const std::string_view text)
        {
            const auto result = parseTemporalText(text);
            EXPECT_HAS_VALUE(result) << "应当被认出的文本：" << text;
            return result.value_or(ParsedDateTime{});
        }

        /// UTC 瞬时 ↔ 文本 的往返（渲染用定宽 ISO 带 Z 那一档，与 SQLite/Redis 交出的形态一致）
        std::string renderUtc(const sys_time<microseconds> instant)
        {
            return renderSystemClock(instant, 'T', true);
        }
    } // namespace

    /**
     * @brief 钉住新增备选是「往后追加」而不是插队：既有备选的下标是已发布的契约
     * @details 按固定下标取值的调用点一旦遇到中间插入就会静默取错类型（把十进制当文本、
     *          把瞬时当墙上时间）。这条把已知的先后顺序钉成一条严格递增链，加新档时只需在末尾续一格。
     */
    TEST(TemporalText, NewAlternativesAppendInsteadOfReordering)
    {
        const AsynGyanis::Database::DatabaseValue integer{std::int64_t{1}};
        const AsynGyanis::Database::DatabaseValue text{std::string("a")};
        const AsynGyanis::Database::DatabaseValue bytes{std::vector<std::uint8_t>{1}};
        const DatabaseValue                       decimal{Decimal::tryParse("1.5").value()};
        const DatabaseValue                       instant{AsynGyanis::Database::Timestamp{microseconds{1}}};
        const DatabaseValue                       wall{AsynGyanis::Database::LocalDateTime{microseconds{1}}};

        EXPECT_LT(integer.index(), text.index());
        EXPECT_LT(text.index(), bytes.index());
        EXPECT_LT(bytes.index(), decimal.index());
        EXPECT_LT(decimal.index(), instant.index());
        EXPECT_LT(instant.index(), wall.index());
    }

    TEST(TemporalText, RendersUnixEpochAsTheCommonAnchor)
    {
        // 两个时钟共用同一个纪元（1970-01-01），差别只在「这串数是 UTC 的读数还是本地历法的读数」：
        // 这条钉的是渲染没有偷偷加任何纪元偏移（加了就会差 1969 年）
        EXPECT_EQ(renderUtc(sys_time<microseconds>{microseconds{0}}), "1970-01-01T00:00:00.000000Z");
    }

    TEST(TemporalText, RendersOneMicrosecondBeforeEpochOnPreviousDay)
    {
        // 负方向必须落到前一天而不是同一天：取整用截断（朝零）就会把 1969-12-31 写成 1970-01-01
        EXPECT_EQ(renderUtc(sys_time<microseconds>{microseconds{-1}}), "1969-12-31T23:59:59.999999Z");
    }

    TEST(TemporalText, RoundTripsParsedTextThroughRenderer)
    {
        // 认下来再渲染一次，要与原文逐字符相同：这条同时钉住「解析不猜」与「渲染不缩写」两头
        for (const std::string_view text: {"2026-10-18T03:50:30.123456Z", "2026-10-18T00:00:00.000000Z", "2024-02-29T23:59:59.000001Z", "2026-12-31T23:59:59.999999Z"})
        {
            const auto instant = toSystemClock(parsed(text));
            ASSERT_HAS_VALUE(instant) << text;
            EXPECT_EQ(renderUtc(*instant), text) << "往返一次就不是原来那个瞬间了：" << text;
        }
    }

    TEST(TemporalText, WallClockRoundTripsWithoutAnyOffset)
    {
        for (const std::string_view text: {"2026-03-01T02:30:00.500000", "2026-10-18T09:20:30.000000"})
        {
            const auto wall = toWallClock(parsed(text));
            ASSERT_HAS_VALUE(wall) << text;
            EXPECT_EQ(renderWallClock(*wall, 'T'), text) << text;
        }
    }

    TEST(TemporalText, AcceptsOffsetsInEveryShapeTheEnginesGive)
    {
        // 同一个瞬间的四种写法：Z / +HH:MM / +HHMM / 零偏移。PostgreSQL 两种都给，MySQL 不带偏移
        const auto withColon   = toSystemClock(parsed("2026-10-18 09:20:30.123456+05:30"));
        const auto withCompact = toSystemClock(parsed("2026-10-18 09:20:30.123456+0530"));
        const auto plainUtc    = toSystemClock(parsed("2026-10-18T03:50:30.123456Z"));
        ASSERT_TRUE(withColon.has_value() && withCompact.has_value() && plainUtc.has_value());
        EXPECT_EQ(*withColon, *withCompact) << "带冒号与不带冒号的偏移折出了不同瞬间";
        EXPECT_EQ(renderUtc(*withColon), "2026-10-18T03:50:30.123456Z") << "偏移没有把文本折回 UTC";
        EXPECT_EQ(*withColon, *plainUtc);
    }

    TEST(TemporalText, RejectsOffsetMinutesAtOrAboveSixty)
    {
        // 四位数形态没有冒号分隔，后两位必须当分钟判：75 分钟不是合法偏移，不能被折算进小时
        EXPECT_FALSE(parseTemporalText("2026-10-18T09:20:30+08:75").has_value());
        EXPECT_FALSE(parseTemporalText("2026-10-18T09:20:30+0899").has_value());
        EXPECT_TRUE(parseTemporalText("2026-10-18T09:20:30+0859").has_value());
    }

    TEST(TemporalText, SystemClockFoldsOffsetIntoUtc)
    {
        // 负偏移方向不能反：-08 的 09:20 是 UTC 的 17:20（同一天）
        const auto instant = toSystemClock(parsed("2026-10-18 09:20:00-08"));
        ASSERT_HAS_VALUE(instant);
        EXPECT_EQ(renderUtc(*instant), "2026-10-18T17:20:00.000000Z");
    }

    TEST(TemporalText, AcceptsDateOnlyTextAndSecondsWithoutFraction)
    {
        // 只有日期的文本（DATE 列）与没给秒的文本（PostgreSQL 对整分钟偶尔这样给）都要认
        EXPECT_EQ(renderUtc(*toSystemClock(parsed("2026-10-18"))), "2026-10-18T00:00:00.000000Z");
        EXPECT_EQ(renderUtc(*toSystemClock(parsed("2026-10-18 09:20"))), "2026-10-18T09:20:00.000000Z");
    }

    TEST(TemporalText, SixtySecondsRollsIntoNextMinuteLikeTheEngines)
    {
        // 闰秒：MySQL 会给出 :60，本库不拒也不假装它是别的数，按加法进下一分钟
        EXPECT_EQ(renderUtc(*toSystemClock(parsed("2026-10-18 09:20:60"))), "2026-10-18T09:21:00.000000Z");
    }

    TEST(TemporalText, RejectsZeroDatesAndNonIsoStyles)
    {
        // MySQL 的零值日期：月份 0 不是合法历法，认了就等于造一个瞬间
        EXPECT_FALSE(parseTemporalText("0000-00-00 00:00:00").has_value());
        // PostgreSQL 在非 ISO 的 DateStyle 下给的是带星期与 月-日-年 顺序的文本，不猜
        EXPECT_FALSE(parseTemporalText("Thu 10-18-2026 09:20:30 PDT+0530").has_value());
        EXPECT_FALSE(parseTemporalText("10-18-2026").has_value());
    }

    TEST(TemporalText, RejectsInfinityAndUndottedAmbiguities)
    {
        // PG 的日期列允许 infinity/-infinity：那是没有历法对应的哨兵，不是时刻
        EXPECT_FALSE(parseTemporalText("infinity").has_value());
        EXPECT_FALSE(parseTemporalText("-infinity").has_value());
        // 空串、只有日期没月日、分隔符不对（斜杠）、以及带尾部余文的，一律不认
        EXPECT_FALSE(parseTemporalText("").has_value());
        EXPECT_FALSE(parseTemporalText("2026").has_value());
        EXPECT_FALSE(parseTemporalText("2026/10/18 09:20:30").has_value());
        EXPECT_FALSE(parseTemporalText("2026-10-18 09:20:30 extra").has_value());
        // 偏移越界（一天之内不可能有 24:00 的偏移）
        EXPECT_FALSE(parseTemporalText("2026-10-18 09:20:30+24:00").has_value());
    }

    TEST(TemporalText, TruncatesFractionsBeyondMicroseconds)
    {
        // 多于 6 位但不超过 9 位：截到微秒。截掉的是引擎给的超出本库承载精度的位数，不是数据
        EXPECT_EQ(renderUtc(*toSystemClock(parsed("2026-10-18 09:20:30.123456789Z"))), "2026-10-18T09:20:30.123456Z");
        // 少于 6 位要补零到定宽，否则文本的字典序就不再等于时间序
        EXPECT_EQ(renderUtc(*toSystemClock(parsed("2026-10-18 09:20:30.5Z"))), "2026-10-18T09:20:30.500000Z");
    }

    TEST(TemporalText, RejectsFractionsLongerThanNineDigits)
    {
        // 10 位小数：超出本库能表达的精度档，且不同人截法不同，宁可报出来
        EXPECT_FALSE(parseTemporalText("2026-10-18 09:20:30.1234567890Z").has_value());
    }

    TEST(TemporalText, RejectsImpossibleCalendarDays)
    {
        // 文法对但历法不存在：2 月 30 日、非闰年的 2 月 29 日、13 月
        EXPECT_TRUE(parseTemporalText("2025-02-30").has_value());
        EXPECT_FALSE(toSystemClock(*parseTemporalText("2025-02-30")).has_value());
        EXPECT_FALSE(toSystemClock(*parseTemporalText("2025-02-29")).has_value());
        EXPECT_TRUE(toSystemClock(*parseTemporalText("2024-02-29")).has_value());
        EXPECT_FALSE(parseTemporalText("2026-13-01").has_value());
        // 4 月 31 日的文法与范围都对（1..31），不成立的是历法：文法层认、折算层拒
        EXPECT_TRUE(parseTemporalText("2026-04-31").has_value());
        EXPECT_FALSE(toSystemClock(*parseTemporalText("2026-04-31")).has_value());
    }

    TEST(TemporalText, WallClockRefusesTextCarryingAnOffset)
    {
        // 带偏移说明这一列有实时区语义，成员却声明成墙上时间：丢掉偏移就是抹平，所以拒
        EXPECT_FALSE(toWallClock(*parseTemporalText("2026-10-18 09:20:30+05:30")).has_value());
        EXPECT_FALSE(toWallClock(*parseTemporalText("2026-10-18T09:20:30Z")).has_value());
        EXPECT_TRUE(toWallClock(*parseTemporalText("2026-10-18 09:20:30")).has_value());
    }

    TEST(TemporalText, NegativeYearsRoundTrip)
    {
        // 公元前后是历法的一部分：负年份要能认、能渲染回来（宽度按四位补零，符号占一位）
        const auto beforeEra = toWallClock(parsed("-0044-03-15T12:00:00"));
        ASSERT_HAS_VALUE(beforeEra);
        EXPECT_EQ(renderWallClock(*beforeEra, 'T'), "-0044-03-15T12:00:00.000000");
    }

    TEST(TemporalText, MySqlAndPostgresRenderingsDifferByOffsetMarker)
    {
        const sys_time<microseconds> instant = *toSystemClock(parsed("2026-10-18 09:20:30.5+00"));

        // MySQL 的 DATETIME 不做会话换算，文本不带偏移；PostgreSQL 的 timestamptz 必须带偏移，
        // 否则服务端会按会话 TimeZone 解释成另一个瞬间（见 PostgresValueConversion.h 那一段）
        EXPECT_EQ(renderSystemClock(instant, ' ', false), "2026-10-18 09:20:30.500000");
        EXPECT_EQ(renderSystemClockWithZeroOffset(instant), "2026-10-18 09:20:30.500000+00");
        EXPECT_EQ(renderSystemClock(instant, 'T', true), "2026-10-18T09:20:30.500000Z");
    }

} // namespace AsynGyanis::Database::Detail
