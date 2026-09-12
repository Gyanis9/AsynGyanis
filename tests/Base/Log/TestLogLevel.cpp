/**
 * @file TestLogLevel.cpp
 * @brief LogLevel 单元测试：枚举数值连续性与顺序、等级字符串双向转换、非法输入回落
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"
#include "Base/Log/LogLevel.h"

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
         * @brief 去掉尾部用于固定宽度对齐的空格
         * @param text 待处理文本（按值接收，返回处理结果）
         * @return std::string 去掉尾部空格后的文本
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
                LogLevel::Trace,
                LogLevel::Debug,
                LogLevel::Info,
                LogLevel::Warn,
                LogLevel::Error,
                LogLevel::Fatal,
                LogLevel::Off,
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

    TEST(LogLevel, FromStringIsCaseSensitive)
    {
        const std::vector<std::string_view> mismatchedCaseLabels = {
                "trace", "Trace", "tRaCe",
                "debug", "Debug",
                "info", "Info",
                "warn", "Warn",
                "error", "Error",
                "fatal", "Fatal",
                "off", "Off",
        };

        for (const std::string_view label: mismatchedCaseLabels)
        {
            EXPECT_EQ(logLevelFromString(label), LogLevel::Info) << "label " << std::string(label);
        }
    }

    TEST(LogLevel, FromStringFallsBackToInfoForUnknownLabels)
    {
        const std::vector<std::string_view> unknownLabels = {
                "",
                "   ",
                "NOTICE",
                "CRITICAL",
                "INFORMATION",
                "INF",
                "INFOO",
        };

        for (const std::string_view label: unknownLabels)
        {
            EXPECT_EQ(logLevelFromString(label), LogLevel::Info) << "label " << std::string(label);
        }
    }

    TEST(LogLevel, FromStringEmitsVisibleDiagnosticForUnknownLabels)
    {
        ::testing::internal::CaptureStderr();
        const LogLevel fallback = logLevelFromString("INFOO");
        const std::string diagnostic = ::testing::internal::GetCapturedStderr();

        // 返回值仍是刻意容错后的 Info，但写错的配置必须留下可见痕迹
        EXPECT_EQ(fallback, LogLevel::Info);
        EXPECT_NE(diagnostic.find("INFOO"), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find("无法识别"), std::string::npos) << diagnostic;
        EXPECT_NE(diagnostic.find("TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF"), std::string::npos) << diagnostic;
    }

    TEST(LogLevel, FromStringStaysSilentForRecognizedLabels)
    {
        const std::vector<std::string_view> knownLabels = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};

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
                LogLevel::Trace,
                LogLevel::Debug,
                LogLevel::Info,
                LogLevel::Warn,
                LogLevel::Error,
                LogLevel::Fatal,
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
} // namespace AsynGyanis::Base
