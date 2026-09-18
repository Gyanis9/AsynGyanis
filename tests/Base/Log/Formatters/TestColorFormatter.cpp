// ColorFormatter 单元测试：ANSI 转义包裹、等级到颜色的映射与 Debug/Release 版式

#include "Base/Log/Formatters/ColorFormatter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "Base/Log/LogColor.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Base/Log/Formatters/StackTraceText.h"

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
         * @brief 构造字段齐备、各字段取值固定的日志事件
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

    TEST(ColorFormatter, DebugBuildPadsSourceLocationFieldToFixedWidth)
    {
        ColorFormatter formatter;

        std::string paddedLocation = "a.cpp:1";
        paddedLocation.resize(13, ' ');

        const std::string output = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation("a.cpp", 1, kSourceFunction), kLoggerName, "padding short"));

        EXPECT_TRUE(contains(output, paddedLocation)) << output;
    }

    TEST(ColorFormatter, DebugBuildHandlesSourceLocationLongerThanStackBuffer)
    {
        // 与 DefaultFormatter 同形：超长「文件:行号」走 SourceLocationText 的回退分支，
        // 输出必须与短文件名一样完整呈现，且与消息之间仍只有一个分隔空格
        ColorFormatter    formatter;
        const std::string longFileName(80, 'c');

        const std::string output = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation(longFileName.c_str(), 1234567, kSourceFunction), kLoggerName,
                         "padding long name"));

        EXPECT_TRUE(contains(output, longFileName + ":1234567")) << output;
        EXPECT_TRUE(contains(output, longFileName + ":1234567 padding long name")) << output;
    }

    TEST(ColorFormatter, DebugBuildMatchesReferenceLayoutOnStackBufferPath)
    {
        // 命中栈缓冲的路径必须与「直接用 std::format 独立拼出整行」的参考版式逐字节相同
        ColorFormatter    formatter;
        const std::string shortFileName = "hit_fixture.cpp";

        const std::string expected = std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}",
                                                 kFixedTimestamp, kThreadId,
                                                 LogColor::colorForLevel(LogLevel::Info),
                                                 logLevelToString(LogLevel::Info), LogColor::kReset,
                                                 kLoggerName,
                                                 std::format("{}:{}", shortFileName, 42),
                                                 "hit path");

        const std::string output = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation(shortFileName.c_str(), 42, kSourceFunction), kLoggerName, "hit path"));

        EXPECT_EQ(output, expected) << output;
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

    /**
     * @brief 带栈的事件在彩色版式末尾同样附上解析后的调用栈
     */
    TEST(ColorFormatter, AppendsResolvedStackTraceWhenEventCarriesOne)
    {
        LogEvent event = makeEvent(LogLevel::Error, "boom");
        event.stackTrace = captureStackTrace();
        if (formatStackTrace(event.stackTrace).empty())
        {
            GTEST_SKIP() << "调试信息不可用（无 PDB/符号表），栈只以原始帧存在";
        }

        ColorFormatter    formatter;
        const std::string output = formatter.format(event);

        const std::size_t headingPosition = output.find(kStackTraceHeading);
        ASSERT_NE(headingPosition, std::string::npos) << output;
        EXPECT_FALSE(output.substr(headingPosition + kStackTraceHeading.size()).empty()) << output;
    }
} // namespace AsynGyanis::Base
