// ColorFormatter 单元测试：ANSI 转义包裹、等级到颜色的映射与 Debug/Release 版式

#include "Base/Log/Formatters/ColorFormatter.h"

#include "BaseTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogColor.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /// 固定文本：断言版式时按它比对
        constexpr auto kFixedTimestamp = "2026-09-10 12:34:56.789";
        /// 事件携带的固定时刻：由上面那段本地挂钟折出，渲染回去即得同一文本，故断言与时区无关
        const TimestampMoment kFixedTimestampMoment = TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789);
        constexpr auto        kThreadId             = "tid-665544";
        constexpr auto        kLoggerName           = "color_logger";
        constexpr auto        kSourceFile           = "color_fixture.cpp";
        constexpr auto        kSourceFunction       = "colorTestFunction";
        constexpr int         kSourceLine           = 5312;
        constexpr auto        kAnsiPrefix           = "\033[";

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "color message")
        {
            return {level, kFixedTimestampMoment, kThreadId, SourceLocation(kSourceFile, kSourceLine, kSourceFunction), kLoggerName, std::move(message)};
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
        const std::vector<LogLevel> levels = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal};

        for (const LogLevel level: levels)
        {
            const std::string output = formatter.format(makeEvent(level, "sweep"));
            EXPECT_TRUE(contains(output, LogColor::colorForLevel(level))) << "color missing for level " << static_cast<int>(level) << ": " << output;
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

        const std::string output =
                formatter.format(LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation("a.cpp", 1, kSourceFunction), kLoggerName, "padding short"));

        EXPECT_TRUE(contains(output, paddedLocation)) << output;
    }

    TEST(ColorFormatter, DebugBuildHandlesSourceLocationLongerThanStackBuffer)
    {
        // 与 DefaultFormatter 同形：超长「文件:行号」走 SourceLocationText 的回退分支，
        // 输出必须与短文件名一样完整呈现，且与消息之间仍只有一个分隔空格
        ColorFormatter    formatter;
        const std::string longFileName(80, 'c');

        const std::string output = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation(longFileName.c_str(), 1234567, kSourceFunction), kLoggerName, "padding long name"));

        EXPECT_TRUE(contains(output, longFileName + ":1234567")) << output;
        EXPECT_TRUE(contains(output, longFileName + ":1234567 padding long name")) << output;
    }

    TEST(ColorFormatter, DebugBuildMatchesReferenceLayoutOnStackBufferPath)
    {
        // 命中栈缓冲的路径必须与「直接用 std::format 独立拼出整行」的参考版式逐字节相同
        ColorFormatter    formatter;
        const std::string shortFileName = "hit_fixture.cpp";

        const std::string expected = std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}", kFixedTimestamp, kThreadId, LogColor::colorForLevel(LogLevel::Info),
                                                 logLevelToString(LogLevel::Info), LogColor::kReset, kLoggerName, std::format("{}:{}", shortFileName, 42), "hit path");

        const std::string output =
                formatter.format(LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation(shortFileName.c_str(), 42, kSourceFunction), kLoggerName, "hit path"));

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
        LogEvent event   = makeEvent(LogLevel::Error, "boom");
        event.stackTrace = captureStackTrace();
        if (!TestSupport::hasResolvedStackTraceFrames(formatStackTrace(event.stackTrace)))
        {
            GTEST_SKIP() << "本构建没有调试信息（既无 PDB 也无 -g），栈帧只剩模块加偏移，符号解析断言不适用";
        }

        ColorFormatter    formatter;
        const std::string output = formatter.format(event);

        const std::size_t headingPosition = output.find(kStackTraceHeading);
        ASSERT_NE(headingPosition, std::string::npos) << output;
        EXPECT_FALSE(output.substr(headingPosition + kStackTraceHeading.size()).empty()) << output;
    }

    /**
     * @brief 带颜色的整行逐字段拼接结果与 `std::format` 规范逐字节相同
     * @details 颜色码只夹在等级两侧、不参与补齐，因此「先追加颜色码再补空格」与「补齐后再套颜色」
     *          这两种读法结果不同——这条对拍把它钉成唯一解。输入同样覆盖非 ASCII 的短文件名：
     *          `{:<N}` 量的是显示宽度（宽字符两格），按码元数补齐在这一条上会当场少几格。
     */
    TEST(ColorFormatter, LineAssemblyMatchesFormatSpecByteForByte)
    {
        /**
         * @brief 一条版式输入：只列会改变文本形状的那些取值
         */
        struct ShapeCase
        {
            LogLevel    level;    ///< 事件等级（同时决定颜色码与等级名）
            const char *file;     ///< 源文件名，shortFileName 的输入
            const char *function; ///< 函数名
            std::string logger;   ///< 日志器名，空串表示根日志器
            std::string message;  ///< 消息文本
        };

        const std::vector<ShapeCase> shapes = {
                {LogLevel::Info, "color_fixture.cpp", "colorTestFunction", "color_logger", "color message"},
                {LogLevel::Fatal, "b.cpp", "f", "", "空 logger 名"},
                {LogLevel::Debug, "源.cpp", "f", "库存", "非 ASCII 的短文件名"},
        };

        ColorFormatter formatter;
        for (const auto &shape: shapes)
        {
            const LogEvent event{shape.level, kFixedTimestampMoment, kThreadId, SourceLocation{shape.file, kSourceLine, shape.function}, shape.logger, shape.message};

            std::array<char, kTimestampTextBufferSize> timestampBuffer{};
            const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);

#ifdef ASYN_DEBUG
            // 源码位置只在 Debug 版式里出现，取它的这三行因此也只能在 Debug 分支里跑：
            // 放在 #ifdef 之外，Release 构建会因「赋值了却没读」在 -Werror 下整个编译不过，
            // 于是 Release 侧的 Base 用例全都构建不出来（表现是 ctest 少一整套，而不是某条红）
            std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
            std::string                                     locationOverflow;
            const std::string_view                          location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);
            const std::string expected = std::format("{} {} [{}{:<5}{}] [{}] {:<13} {}", timestampText, event.threadIdView(), LogColor::colorForLevel(shape.level),
                                                     logLevelToString(shape.level), LogColor::kReset, event.loggerNameView(), location, event.message);
#else
            const std::string expected = std::format("{} [{}{:<5}{}] [{}] {}", timestampText, LogColor::colorForLevel(shape.level), logLevelToString(shape.level), LogColor::kReset,
                                                     event.loggerNameView(), event.message);
#endif
            EXPECT_EQ(formatter.format(event), expected) << "形状：文件 " << shape.file;
        }
    }
} // namespace AsynGyanis::Base
