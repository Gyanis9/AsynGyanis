// LogLevel 单元测试：枚举数值连续性与顺序、等级字符串双向转换、大小写折叠的边界、非法输入回落

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Base/Log/LogLevel.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 去掉等级标签为固定宽度对齐补的尾部空格
         * @param text 待处理的标签文本
         * @return 去掉尾部空格后的文本
         */
        std::string trimTrailingSpaces(std::string text)
        {
            while (!text.empty() && text.back() == ' ')
            {
                text.pop_back();
            }
            return text;
        }

        /// 全部有效日志等级，按严重程度递增排列
        const std::vector<LogLevel> kAllLevels = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal, LogLevel::Off,
        };
    } // namespace

    TEST(LogLevel, UnderlyingTypeIsUnsignedByte)
    {
        static_assert(std::is_same_v<std::underlying_type_t<LogLevel>, std::uint8_t>);
        static_assert(std::is_enum_v<LogLevel>);
        EXPECT_EQ(sizeof(LogLevel), sizeof(std::uint8_t));
    }

    TEST(LogLevel, EnumValuesAreSequentialFromZero)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Trace), 0);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Debug), 1);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Info), 2);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Warn), 3);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Error), 4);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Fatal), 5);
        EXPECT_EQ(static_cast<std::uint8_t>(LogLevel::Off), 6);

        ASSERT_EQ(kAllLevels.size(), 7U);
        for (std::size_t index = 0; index < kAllLevels.size(); ++index)
        {
            EXPECT_EQ(static_cast<std::uint8_t>(kAllLevels[index]), static_cast<std::uint8_t>(index)) << "index " << index;
        }
    }

    TEST(LogLevel, OrderingFollowsSeverityFromTraceToOff)
    {
        EXPECT_LT(LogLevel::Trace, LogLevel::Debug);
        EXPECT_LT(LogLevel::Debug, LogLevel::Info);
        EXPECT_LT(LogLevel::Info, LogLevel::Warn);
        EXPECT_LT(LogLevel::Warn, LogLevel::Error);
        EXPECT_LT(LogLevel::Error, LogLevel::Fatal);
        EXPECT_LT(LogLevel::Fatal, LogLevel::Off);

        EXPECT_GT(LogLevel::Off, LogLevel::Trace);
    }

    TEST(LogLevel, ToStringPadsEveryLabelToFiveCharacters)
    {
        for (const LogLevel level: kAllLevels)
        {
            const std::string_view label(logLevelToString(level));
            EXPECT_EQ(label.size(), 5U) << "level value " << static_cast<int>(level);
        }
    }

    TEST(LogLevel, ToStringReturnsExpectedLabelPerLevel)
    {
        EXPECT_STREQ(logLevelToString(LogLevel::Trace), "TRACE");
        EXPECT_STREQ(logLevelToString(LogLevel::Debug), "DEBUG");
        EXPECT_STREQ(logLevelToString(LogLevel::Error), "ERROR");
        EXPECT_STREQ(logLevelToString(LogLevel::Fatal), "FATAL");
    }

    TEST(LogLevel, ToStringKeepsShortLevelsRightAlignedWithTrailingSpace)
    {
        const std::string informationLabel(logLevelToString(LogLevel::Info));
        const std::string warningLabel(logLevelToString(LogLevel::Warn));

        EXPECT_EQ(informationLabel, "INFO ");
        EXPECT_EQ(warningLabel, "WARN ");
        EXPECT_EQ(informationLabel.size(), 5U);
        EXPECT_EQ(warningLabel.size(), 5U);
        EXPECT_EQ(informationLabel.back(), ' ');
        EXPECT_EQ(warningLabel.back(), ' ');
    }

    TEST(LogLevel, ToStringFallsBackToPlaceholderForUnknownLevels)
    {
        const std::vector<LogLevel> unknownLevels = {
                static_cast<LogLevel>(7),
                static_cast<LogLevel>(100),
                static_cast<LogLevel>(255),
        };

        for (const LogLevel level: unknownLevels)
        {
            EXPECT_STREQ(logLevelToString(level), "?????") << "level value " << static_cast<int>(level);
        }
    }

    TEST(LogLevel, FromStringParsesEveryKnownLabel)
    {
        EXPECT_EQ(logLevelFromString("TRACE"), LogLevel::Trace);
        EXPECT_EQ(logLevelFromString("DEBUG"), LogLevel::Debug);
        EXPECT_EQ(logLevelFromString("INFO"), LogLevel::Info);
        EXPECT_EQ(logLevelFromString("WARN"), LogLevel::Warn);
        EXPECT_EQ(logLevelFromString("ERROR"), LogLevel::Error);
        EXPECT_EQ(logLevelFromString("FATAL"), LogLevel::Fatal);
        EXPECT_EQ(logLevelFromString("OFF"), LogLevel::Off);
    }

    TEST(LogLevel, FromStringIgnoresAsciiCase)
    {
        // 钉住一次契约变更（旧断言：大小写敏感，"error" 判成不认识）：日志配置里其余取值全是
        // 小写（type: file、policy: size、overflow_policy: drop_oldest），于是 `level: error` 是最
        // 自然的写法；把它判成不认识并按 INFO 生效，等于「只留错误日志」的配置在打全量 INFO。
        // 新语义：任何 ASCII 大小写组合都解析为同一等级
        struct LabelCase
        {
            std::string_view label; ///< 配置里可能出现的写法
            LogLevel         level; ///< 期望解析结果
        };

        const std::vector<LabelCase> labelCases = {
                {"trace", LogLevel::Trace}, {"Trace", LogLevel::Trace}, {"tRaCe", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"Debug", LogLevel::Debug},
                {"info", LogLevel::Info},   {"Info", LogLevel::Info},   {"iNFO", LogLevel::Info},   {"warn", LogLevel::Warn},   {"Warn", LogLevel::Warn},
                {"WArN", LogLevel::Warn},   {"error", LogLevel::Error}, {"Error", LogLevel::Error}, {"fatal", LogLevel::Fatal}, {"Fatal", LogLevel::Fatal},
                {"off", LogLevel::Off},     {"Off", LogLevel::Off},
        };

        for (const LabelCase &testCase: labelCases)
        {
            EXPECT_EQ(logLevelFromString(testCase.label), testCase.level) << "label " << testCase.label;
        }
    }

    TEST(LogLevel, LabelFoldingCoversOnlyAsciiLetters)
    {
        // 折叠按码位区间做而不是查 std::tolower 的 C locale 表：后者在土耳其语环境下把 'I' 折成
        // 非 ASCII 字符，"INFO" 就会在那样的进程里解不出来。这里钉住折叠的边界——非字母字节原样
        // 比较，因此长度相同但内容不同的标签一律配不上
        static_assert(detail::toAsciiLowercase('I') == 'i');
        static_assert(detail::toAsciiLowercase('i') == 'i');
        static_assert(detail::toAsciiLowercase('0') == '0');
        static_assert(detail::toAsciiLowercase('\x00') == '\x00');
        // 高位字节（UTF-8 续字节）不被折叠成 ASCII 字母，也就不会误配成已知标签
        static_assert(detail::toAsciiLowercase('\xC1') == '\xC1');

        static_assert(detail::logLevelLabelEquals("info", "INFO"));
        static_assert(detail::logLevelLabelEquals("INF", "INFO") == false);
        static_assert(detail::logLevelLabelEquals("\xC1NFO", "INFO") == false);
        static_assert(detail::logLevelLabelEquals("", "") == true);
    }

    TEST(LogLevel, NonAsciiAndWrongLengthLabelsStillFallBack)
    {
        // 与 FromStringIgnoresAsciiCase 相对的另一面：收大小写不等于收"看着像"。全角写法与
        // 中文写法必须仍然走「诊断 + 回落」，否则一份坏配置连一行提示都没有
        ::testing::internal::CaptureStderr();
        EXPECT_EQ(logLevelFromString("\xef\xbc\xa1\xef\xbc\xb2\xef\xbc\xb2\xef\xbc\xb5\xef\xbc\xb2"), LogLevel::Info); // 全角 ERROR
        EXPECT_EQ(logLevelFromString("\xe9\x94\x99\xe8\xaf\xaf"), LogLevel::Info);                                     // 「错误」
        const std::string diagnostic = ::testing::internal::GetCapturedStderr();
        EXPECT_NE(diagnostic.find("无法识别"), std::string::npos) << diagnostic;
    }

    TEST(LogLevel, FromStringFallsBackToInfoForUnknownLabels)
    {
        const std::vector<std::string_view> unknownLabels = {
                "", "   ", "NOTICE", "CRITICAL", "INFORMATION", "INF", "INFOO",
        };

        for (const std::string_view label: unknownLabels)
        {
            EXPECT_EQ(logLevelFromString(label), LogLevel::Info) << "label " << std::string(label);
        }
    }

    TEST(LogLevel, FromStringEmitsVisibleDiagnosticForUnknownLabels)
    {
        ::testing::internal::CaptureStderr();
        const LogLevel    fallback   = logLevelFromString("INFOO");
        const std::string diagnostic = ::testing::internal::GetCapturedStderr();

        // 返回值仍是刻意容错后的 Info，但写错的配置必须留下可见痕迹
        EXPECT_EQ(fallback, LogLevel::Info);
        EXPECT_NE(diagnostic.find("INFOO"), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find("无法识别"), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find("TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF"), std::string::npos) << diagnostic;
    }

    TEST(LogLevel, FromStringStaysSilentForRecognizedLabels)
    {
        // 小写写法如今也是「认识」的取值，因此同样不该留下诊断：每条被挡下的写法都要付一行 stderr
        const std::vector<std::string_view> knownLabels = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF", "trace", "debug", "info", "warn", "error", "fatal", "off"};

        ::testing::internal::CaptureStderr();
        for (const std::string_view label: knownLabels)
        {
            static_cast<void>(logLevelFromString(label));
        }
        const std::string diagnostic = ::testing::internal::GetCapturedStderr();

        EXPECT_TRUE(diagnostic.empty()) << diagnostic;
    }

    TEST(LogLevel, FromStringAcceptsStringViewWithoutTrailingTerminator)
    {
        const std::string buffer("WARN\0EXTRA", 10);

        EXPECT_EQ(logLevelFromString(std::string_view(buffer.data(), 4)), LogLevel::Warn);
        EXPECT_EQ(logLevelFromString(std::string_view(buffer.data(), buffer.size())), LogLevel::Info);
        EXPECT_EQ(logLevelFromString(buffer), LogLevel::Info);
    }

    TEST(LogLevel, FromStringHandlesOverlongInput)
    {
        const std::string overlongLabel(10000, 'X');
        const std::string overlongWithKnownPrefix = "WARN" + std::string(10000, 'X');

        EXPECT_EQ(logLevelFromString(overlongLabel), LogLevel::Info);
        EXPECT_EQ(logLevelFromString(overlongWithKnownPrefix), LogLevel::Info);
    }

    TEST(LogLevel, SeverityLevelsRoundTripThroughTrimmedLabel)
    {
        const std::vector<LogLevel> roundTripLevels = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal,
        };

        for (const LogLevel level: roundTripLevels)
        {
            const std::string label = trimTrailingSpaces(logLevelToString(level));
            EXPECT_EQ(logLevelFromString(label), level) << "label " << label;
        }
    }

    TEST(LogLevel, OffLabelIntentionallyDoesNotRoundTrip)
    {
        EXPECT_STREQ(logLevelToString(LogLevel::Off), "?????");
        EXPECT_EQ(logLevelFromString(trimTrailingSpaces(logLevelToString(LogLevel::Off))), LogLevel::Info);
    }

    // 阈值过滤是 Logger 与 Sink 共用的那一条判定，这里按判定表整体验证：每个阈值都取
    // 「低一档 / 恰好 / 高一档」三个边界，再加 Off 的两侧与超范围数值这两类反常输入
    TEST(LogLevel, FilterPassesExactlyTheLevelsAtOrAboveThreshold)
    {
        struct FilterCase
        {
            LogLevel threshold;
            LogLevel level;
            bool     expected;
        };

        const std::vector<FilterCase> cases = {
                // 最低阈值放行除 Off 以外的全部等级
                {LogLevel::Trace, LogLevel::Trace, true},
                {LogLevel::Trace, LogLevel::Fatal, true},
                {LogLevel::Trace, LogLevel::Off, false},
                // 中间阈值的三档边界
                {LogLevel::Info, LogLevel::Debug, false},
                {LogLevel::Info, LogLevel::Info, true},
                {LogLevel::Info, LogLevel::Warn, true},
                {LogLevel::Info, LogLevel::Off, false},
                // 最高有效阈值只放行它自己
                {LogLevel::Fatal, LogLevel::Error, false},
                {LogLevel::Fatal, LogLevel::Fatal, true},
                {LogLevel::Fatal, LogLevel::Off, false},
                // 阈值本身是 Off：连超范围的数值也不放行
                {LogLevel::Off, LogLevel::Fatal, false},
                {LogLevel::Off, LogLevel::Off, false},
                {LogLevel::Off, static_cast<LogLevel>(7), false},
                // 数值超出已知范围的等级仍放行：宁可落一行带 "?????" 标签的记录，
                // 也不把一条注了册外的等级的日志静默吞掉
                {LogLevel::Info, static_cast<LogLevel>(7), true},
                {LogLevel::Info, static_cast<LogLevel>(255), true},
        };

        for (const auto &[threshold, level, expected]: cases)
        {
            EXPECT_EQ(logLevelPassesFilter(threshold, level), expected) << "threshold=" << static_cast<int>(threshold) << " level=" << static_cast<int>(level);
        }
    }

    TEST(LogLevel, FilterIsUsableInConstantContext)
    {
        static_assert(logLevelPassesFilter(LogLevel::Info, LogLevel::Warn));
        static_assert(!logLevelPassesFilter(LogLevel::Info, LogLevel::Debug));
        static_assert(!logLevelPassesFilter(LogLevel::Info, LogLevel::Off));
        static_assert(!logLevelPassesFilter(LogLevel::Off, LogLevel::Fatal));
        SUCCEED();
    }
} // namespace AsynGyanis::Base
