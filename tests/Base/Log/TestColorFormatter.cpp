/**
 * @file TestColorFormatter.cpp
 * @brief ColorFormatter 单元测试：ANSI 转义包裹、等级到颜色的映射与 Debug/Release 版式
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/ColorFormatter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Base/Log/LogColor.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        constexpr auto kFixedTimestamp = "2026-09-10 12:34:56.789";
        constexpr auto kThreadId       = "tid-665544";
        constexpr auto kLoggerName     = "color_logger";
        constexpr auto kSourceFile     = "color_fixture.cpp";
        constexpr auto kSourceFunction = "colorTestFunction";
        constexpr int  kSourceLine     = 5312;
        constexpr auto kAnsiPrefix     = "\033[";

        /**
         * @brief 构造字段齐备的日志事件
         * @param level 日志等级
         * @param message 日志消息
         * @return LogEvent 内容固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "color message")
        {
            return {
                    level, kFixedTimestamp, kThreadId,
                    SourceLocation(kSourceFile, kSourceLine, kSourceFunction),
                    kLoggerName, std::move(message)
            };
        }

        /**
         * @brief 判断文本是否包含子串
         * @param haystack 待检查文本
         * @param needle 期望出现的子串
         * @return true 出现
         */
        bool contains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    // ============================================================================
    // 着色行为
    // ============================================================================

    TEST(ColorFormatter, LevelTokenIsWrappedWithAnsiEscapeSequence)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Warn, "color warning"));

        EXPECT_FALSE(output.empty());
        EXPECT_TRUE(contains(output, kAnsiPrefix)) << output;
        EXPECT_TRUE(contains(output, "WARN ")) << output;
        EXPECT_TRUE(contains(output, "color warning")) << output;
    }

    TEST(ColorFormatter, ErrorAndInfoUseDifferentColorCodes)
    {
        ColorFormatter formatter;

        const std::string infoOutput  = formatter.format(makeEvent(LogLevel::Info, "info body"));
        const std::string errorOutput = formatter.format(makeEvent(LogLevel::Error, "error body"));

        EXPECT_NE(infoOutput, errorOutput);
        EXPECT_TRUE(contains(infoOutput, LogColor::colorForLevel(LogLevel::Info))) << infoOutput;
        EXPECT_TRUE(contains(errorOutput, LogColor::colorForLevel(LogLevel::Error))) << errorOutput;
        // 各自只带自己的颜色，不串色
        EXPECT_FALSE(contains(infoOutput, LogColor::colorForLevel(LogLevel::Error))) << infoOutput;
        EXPECT_FALSE(contains(errorOutput, LogColor::colorForLevel(LogLevel::Info))) << errorOutput;
    }

    TEST(ColorFormatter, FatalUsesItsOwnBrightColorCode)
    {
        ColorFormatter formatter;

        const std::string fatalOutput = formatter.format(makeEvent(LogLevel::Fatal, "fatal body"));
        const std::string errorOutput = formatter.format(makeEvent(LogLevel::Error, "error body"));

        EXPECT_TRUE(contains(fatalOutput, LogColor::colorForLevel(LogLevel::Fatal))) << fatalOutput;
        EXPECT_FALSE(contains(fatalOutput, LogColor::colorForLevel(LogLevel::Error))) << fatalOutput;
        EXPECT_TRUE(contains(errorOutput, LogColor::colorForLevel(LogLevel::Error))) << errorOutput;
    }

    TEST(ColorFormatter, EverySeverityUsesItsMappedColorAndResetCode)
    {
        ColorFormatter              formatter;
        const std::vector<LogLevel> levels = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                LogLevel::Warn, LogLevel::Error, LogLevel::Fatal
        };

        for (const LogLevel level: levels)
        {
            const std::string output = formatter.format(makeEvent(level, "sweep"));
            EXPECT_TRUE(contains(output, LogColor::colorForLevel(level))) << "color missing for level "
                                                                          << static_cast<int>(level) << ": " << output;
            EXPECT_TRUE(contains(output, LogColor::kReset)) << output;
        }
    }

    TEST(ColorFormatter, MessageIsAppendedAfterResetCode)
    {
        ColorFormatter formatter;

        const std::string output          = formatter.format(makeEvent(LogLevel::Info, "tail message"));
        const std::size_t resetPosition   = output.find(LogColor::kReset);
        const std::size_t messagePosition = output.find("tail message");

        ASSERT_NE(resetPosition, std::string::npos);
        ASSERT_NE(messagePosition, std::string::npos);
        EXPECT_GT(messagePosition, resetPosition);
    }

    TEST(ColorFormatter, LoggerNameIsBracketedLikePlainTextFormatter)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "bracket check"));

        EXPECT_TRUE(contains(output, "[" + std::string(kLoggerName) + "]")) << output;
        EXPECT_TRUE(contains(output, kFixedTimestamp)) << output;
    }

    // ============================================================================
    // 边界输入
    // ============================================================================

    TEST(ColorFormatter, EmptyMessageStillProducesColoredHeader)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, ""));

        EXPECT_TRUE(contains(output, kAnsiPrefix)) << output;
        EXPECT_TRUE(contains(output, "INFO ")) << output;
    }

    TEST(ColorFormatter, UnknownLevelOnlyEmitsResetCode)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(static_cast<LogLevel>(200), "unknown level"));

        EXPECT_TRUE(contains(output, "?????")) << output;
        EXPECT_TRUE(contains(output, LogColor::kReset)) << output;
        EXPECT_TRUE(contains(output, "unknown level")) << output;
    }

    TEST(ColorFormatter, EmbeddedEscapeLikeTextIsPassedThrough)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "body with \033[X fake"));

        EXPECT_TRUE(contains(output, "body with \033[X fake")) << output;
    }

    // ============================================================================
    // 构建配置差异
    // ============================================================================

#ifdef ASYN_DEBUG

    TEST(ColorFormatter, DebugBuildAddsThreadIdAndSourceLocation)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "debug color layout"));

        EXPECT_TRUE(contains(output, kThreadId)) << output;
        EXPECT_TRUE(contains(output, std::string(kSourceFile) + ":" + std::to_string(kSourceLine))) << output;
    }

#else

    TEST(ColorFormatter, ReleaseBuildKeepsColorsInCompactLayout)
    {
        ColorFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Warn, "release color layout"));

        EXPECT_TRUE(contains(output, kAnsiPrefix)) << output;
        EXPECT_TRUE(contains(output, LogColor::colorForLevel(LogLevel::Warn))) << output;
        EXPECT_TRUE(contains(output, kFixedTimestamp)) << output;
        EXPECT_TRUE(contains(output, "[" + std::string(kLoggerName) + "]")) << output;
        EXPECT_TRUE(contains(output, "release color layout")) << output;
        EXPECT_FALSE(contains(output, kThreadId)) << output;
        EXPECT_FALSE(contains(output, kSourceFile)) << output;
    }

#endif
} // namespace AsynGyanis::Base
