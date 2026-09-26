// DefaultFormatter 单元测试：字段完整性、等级占位串与 Debug/Release 两种版式

#include "Base/Log/Formatters/DefaultFormatter.h"

#include "BaseTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /// 固定文本：断言版式时按它比对
        constexpr auto kFixedTimestamp = "2026-09-10 12:34:56.789";
        /// 事件携带的固定时刻：由上面那段本地挂钟折出，格式化器渲染回去即得同一文本，故断言与时区无关
        const TimestampMoment kFixedTimestampMoment = TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789);
        constexpr auto        kThreadId             = "tid-778899";
        constexpr auto        kLoggerName           = "formatter_logger";
        constexpr auto        kSourceFile           = "formatter_fixture.cpp";
        constexpr auto        kSourceFunction       = "testFunction";
        constexpr int         kSourceLine           = 4271;

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件，便于逐字段断言版式
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "formatter message")
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
    // 共用工具 SourceLocationText：两个格式化器 Debug 分支共同依赖的「文件:行号」生成
    // ============================================================================

    TEST(SourceLocationText, FitsExactlyWhenBufferMatchesRequiredLength)
    {
        const SourceLocation                            location("short_fixture.cpp", 42, kSourceFunction);
        std::array<char, kSourceLocationTextBufferSize> buffer{};
        const std::string_view                          expected = "short_fixture.cpp:42";

        const std::string_view text = tryFormatSourceLocationText(location, std::span<char>(buffer).first(expected.size()));

        EXPECT_EQ(text, expected);
        // 命中时视图必须直接指向调用方缓冲，中间不产生任何临时串
        EXPECT_EQ(text.data(), buffer.data());
    }

    TEST(SourceLocationText, ReturnsEmptyViewWhenBufferIsOneByteShort)
    {
        const SourceLocation                            location("short_fixture.cpp", 42, kSourceFunction);
        std::array<char, kSourceLocationTextBufferSize> buffer{};
        const std::string_view                          expected = "short_fixture.cpp:42";

        // 缓冲比所需长度少 1 字节：必须整体判为未命中，而不是把半截文本交出去
        const std::string_view text = tryFormatSourceLocationText(location, std::span<char>(buffer).first(expected.size() - 1));

        EXPECT_TRUE(text.empty());
    }

    TEST(SourceLocationText, ReturnsEmptyViewWhenFileNameExceedsBuffer)
    {
        const std::string                               longFileName(2 * kSourceLocationTextBufferSize, 'n');
        std::array<char, kSourceLocationTextBufferSize> buffer{};
        const SourceLocation                            location(longFileName.c_str(), 1234567, kSourceFunction);

        EXPECT_TRUE(tryFormatSourceLocationText(location, buffer).empty());
    }

    TEST(SourceLocationText, EmptyFileNameStillRendersLineNumber)
    {
        std::array<char, kSourceLocationTextBufferSize> buffer{};

        // 空文件名不是未命中：文本仍需给出 ':' 与行号，因此恒不为空视图
        EXPECT_EQ(tryFormatSourceLocationText(SourceLocation("", 7, kSourceFunction), buffer), ":7");
        // 完全默认构造的位置（fileName 为空指针）同样只输出行号
        EXPECT_EQ(tryFormatSourceLocationText(SourceLocation(), buffer), ":0");
    }

    // ============================================================================
    // 通用版式
    // ============================================================================

    TEST(DefaultFormatter, RendersTimestampLevelLoggerNameAndMessage)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info));

        EXPECT_FALSE(output.empty());
        EXPECT_TRUE(contains(output, kFixedTimestamp)) << output;
        EXPECT_TRUE(contains(output, "INFO ")) << output;
        EXPECT_TRUE(contains(output, "[formatter_logger]")) << output;
        EXPECT_TRUE(contains(output, "formatter message")) << output;
    }

    TEST(DefaultFormatter, WarnLevelIsRenderedAsFiveCharacterWarnToken)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Warn, "careful"));

        EXPECT_TRUE(contains(output, "WARN ")) << output;
        EXPECT_TRUE(contains(output, "careful")) << output;
    }

    TEST(DefaultFormatter, EverySeverityRendersItsOwnPaddedToken)
    {
        DefaultFormatter                                    formatter;
        const std::vector<std::pair<LogLevel, std::string>> levelTokens = {
                {LogLevel::Trace, "TRACE"}, {LogLevel::Debug, "DEBUG"}, {LogLevel::Info, "INFO "},
                {LogLevel::Warn, "WARN "},  {LogLevel::Error, "ERROR"}, {LogLevel::Fatal, "FATAL"},
        };

        for (const auto &[level, token]: levelTokens)
        {
            const std::string output = formatter.format(makeEvent(level, "token_" + token));
            EXPECT_TRUE(contains(output, "[" + token + "]")) << "level token " << token << " missing in " << output;
        }
    }

    TEST(DefaultFormatter, OutputCarriesNoAnsiEscapeSequence)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Error, "plain only"));

        EXPECT_EQ(output.find("\033["), std::string::npos) << output;
    }

    TEST(DefaultFormatter, UnknownLevelFallsBackToPlaceholderToken)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(static_cast<LogLevel>(99), "odd level"));

        EXPECT_TRUE(contains(output, "[?????]")) << output;
        EXPECT_TRUE(contains(output, "odd level")) << output;
    }

    // ============================================================================
    // 边界输入
    // ============================================================================

    TEST(DefaultFormatter, EmptyMessageStillProducesFullHeader)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, ""));

        EXPECT_FALSE(output.empty());
        EXPECT_TRUE(contains(output, kFixedTimestamp)) << output;
        EXPECT_TRUE(contains(output, "[formatter_logger]")) << output;
    }

    TEST(DefaultFormatter, LongMessageIsAppendedVerbatim)
    {
        DefaultFormatter  formatter;
        const std::string longMessage(10000, 'A');

        const std::string output = formatter.format(makeEvent(LogLevel::Info, longMessage));

        EXPECT_TRUE(contains(output, longMessage)) << "output size " << output.size();
    }

    TEST(DefaultFormatter, EmptySourceLocationDoesNotBreakFormatting)
    {
        DefaultFormatter formatter;
        const LogEvent   event(LogLevel::Debug, kFixedTimestampMoment, kThreadId, SourceLocation(), kLoggerName, "no location");

        const std::string output = formatter.format(event);

        EXPECT_TRUE(contains(output, "DEBUG")) << output;
        EXPECT_TRUE(contains(output, "no location")) << output;
    }

    // ============================================================================
    // 构建配置差异：Debug 额外带线程号与源码位置，Release 精简
    // ============================================================================

#ifdef ASYN_DEBUG

    TEST(DefaultFormatter, DebugBuildAddsThreadIdAndSourceLocation)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "debug layout"));

        EXPECT_TRUE(contains(output, kThreadId)) << output;
        EXPECT_TRUE(contains(output, std::string(kSourceFile) + ":" + std::to_string(kSourceLine))) << output;
    }

    TEST(DefaultFormatter, DebugBuildPadsSourceLocationFieldToFixedWidth)
    {
        DefaultFormatter formatter;

        const std::string paddedLocation = []
        {
            std::string location = "a.cpp:1";
            location.resize(13, ' ');
            return location;
        }();

        const std::string shortOutput =
                formatter.format(LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation("a.cpp", 1, kSourceFunction), kLoggerName, "padding short"));
        const std::string longOutput = formatter.format(LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId,
                                                                 SourceLocation("a-much-longer-fixture-file-name.cpp", 987654, kSourceFunction), kLoggerName, "padding long"));

        EXPECT_TRUE(contains(shortOutput, paddedLocation)) << shortOutput;
        EXPECT_TRUE(contains(longOutput, "a-much-longer-fixture-file-name.cpp:987654")) << longOutput;
    }

    TEST(DefaultFormatter, DebugBuildHandlesSourceLocationLongerThanStackBuffer)
    {
        // 「文件:行号」超出实现内部的栈缓冲（64 字节）时走回退分支，
        // 输出必须与短文件名一样完整呈现，且与消息之间仍只有一个分隔空格
        DefaultFormatter  formatter;
        const std::string longFileName(80, 'n');

        const std::string output = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation(longFileName.c_str(), 1234567, kSourceFunction), kLoggerName, "padding long name"));

        EXPECT_TRUE(contains(output, longFileName + ":1234567")) << output;
        EXPECT_TRUE(contains(output, longFileName + ":1234567 padding long name")) << output;
    }

    TEST(DefaultFormatter, DebugBuildHitAndFallbackPathsMatchReferenceLayout)
    {
        // 命中栈缓冲（短文件名）与回退分配（超长文件名）两条路径，都必须与「直接用
        // std::format 独立拼出整行」的参考版式逐字节相同，含 {:<13} 的右侧填充空格
        DefaultFormatter formatter;

        const auto referenceLine = [](const char *fileName, const int line, const std::string &message)
        {
            return std::format("{} {} [{:<5}] [{}] {:<13} {}", kFixedTimestamp, kThreadId, logLevelToString(LogLevel::Info), kLoggerName, std::format("{}:{}", fileName, line),
                               message);
        };

        const std::string hitFileName = "hit_fixture.cpp";
        const std::string fallbackFileName(80, 'n');

        const std::string hitOutput =
                formatter.format(LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation(hitFileName.c_str(), 42, kSourceFunction), kLoggerName, "hit path"));
        const std::string fallbackOutput = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestampMoment, kThreadId, SourceLocation(fallbackFileName.c_str(), 1234567, kSourceFunction), kLoggerName, "fallback path"));

        EXPECT_EQ(hitOutput, referenceLine(hitFileName.c_str(), 42, "hit path")) << hitOutput;
        EXPECT_EQ(fallbackOutput, referenceLine(fallbackFileName.c_str(), 1234567, "fallback path")) << fallbackOutput;
    }

#else

    TEST(DefaultFormatter, ReleaseBuildOmitsThreadIdAndSourceLocation)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "release layout"));

        EXPECT_FALSE(contains(output, kThreadId)) << output;
        EXPECT_FALSE(contains(output, kSourceFile)) << output;
        EXPECT_FALSE(contains(output, std::to_string(kSourceLine))) << output;
        EXPECT_TRUE(contains(output, kFixedTimestamp)) << output;
        EXPECT_TRUE(contains(output, "[formatter_logger]")) << output;
        EXPECT_TRUE(contains(output, "release layout")) << output;
    }

    TEST(DefaultFormatter, ReleaseBuildKeepsCompactFourFieldLayout)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Warn, "compact"));

        // Release 版式固定为「时间 [等级] [日志器] 消息」，源码位置为空也不会引入多余字段
        EXPECT_EQ(output, std::string(kFixedTimestamp) + " [WARN ] [" + kLoggerName + "] compact");
    }

#endif

    // ============================================================================
    // 调用栈：带栈事件在 Sink 侧渲染（符号解析发生在这一步）
    // ============================================================================

    TEST(DefaultFormatter, LeavesPlainEventsUntouchedByStackTraceRendering)
    {
        DefaultFormatter formatter;

        const std::string output = formatter.format(makeEvent(LogLevel::Info, "no stack"));

        EXPECT_EQ(output.find(kStackTraceHeading), std::string::npos) << "不带栈的事件不应出现调用栈引导行";
    }

    TEST(DefaultFormatter, AppendsResolvedStackTraceWhenEventCarriesOne)
    {
        LogEvent event   = makeEvent(LogLevel::Error, "boom");
        event.stackTrace = captureStackTrace();

        if (!TestSupport::hasResolvedStackTraceFrames(formatStackTrace(event.stackTrace)))
        {
            GTEST_SKIP() << "本构建没有调试信息（既无 PDB 也无 -g），栈帧只剩模块加偏移，符号解析断言不适用";
        }

        DefaultFormatter  formatter;
        const std::string output = formatter.format(event);

        const std::size_t headingPosition = output.find(kStackTraceHeading);
        ASSERT_NE(headingPosition, std::string::npos) << output;
        EXPECT_FALSE(output.substr(headingPosition + kStackTraceHeading.size()).empty()) << output;
    }

    /**
     * @brief 逐字段拼出的整行与 `std::format` 的版式规范逐字节相同
     * @details 版式从 `std::format_to` 改成逐字段追加之后，「字段顺序、分隔符、补齐口径」三件事全靠这条
     *          对拍钉住——期望值仍按原格式串现算，不跟着实现改。输入覆盖各等级、空 logger 名、超长与
     *          非 ASCII 的源码位置：`{:<N}` 量的是**显示宽度**（宽字符算两格），实现若按码元数就少补
     *          几格，非 ASCII 那一条当场红（本轮实测正是这样抓到的）。
     */
    TEST(DefaultFormatter, LineAssemblyMatchesFormatSpecByteForByte)
    {
        /**
         * @brief 一条版式输入：只列会改变文本形状的那些取值
         */
        struct ShapeCase
        {
            LogLevel    level;    ///< 事件等级（决定等级名与是否要补齐）
            const char *file;     ///< 源文件名，shortFileName 的输入
            const char *function; ///< 函数名，Debug 版式不参与文本但参与定位串生成
            std::string logger;   ///< 日志器名，空串表示根日志器
            std::string message;  ///< 消息文本
        };

        const std::vector<ShapeCase> shapes = {
                {LogLevel::Info, "formatter_fixture.cpp", "testFunction", "formatter_logger", "formatter message"},
                {LogLevel::Trace, "a.cpp", "f", "", "空 logger 名"},
                {LogLevel::Warn, "源.cpp", "f", "订单", "非 ASCII 的短文件名"},
                {LogLevel::Error, "一个很长很长的夹具文件名.cpp", "veryLongFunctionName", "net.http2.session", "超长定位串"},
        };

        DefaultFormatter formatter;
        for (const auto &shape: shapes)
        {
            const LogEvent event{shape.level, kFixedTimestampMoment, kThreadId, SourceLocation{shape.file, kSourceLine, shape.function}, shape.logger, shape.message};

            std::array<char, kTimestampTextBufferSize> timestampBuffer{};
            const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);

#ifdef ASYN_DEBUG
            // 同 TestColorFormatter：这三行只在 Debug 版式里被读到，留在 #ifdef 外会让 Release
            // 构建因「赋值了却没读」在 -Werror 下失败，整套 Release 侧的 Base 用例随之构建不出来
            std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
            std::string                                     locationOverflow;
            const std::string_view                          location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);
            const std::string expected = std::format("{} {} [{:<5}] [{}] {:<13} {}", timestampText, event.threadIdView(), logLevelToString(shape.level), event.loggerNameView(),
                                                     location, event.message);
#else
            const std::string expected = std::format("{} [{:<5}] [{}] {}", timestampText, logLevelToString(shape.level), event.loggerNameView(), event.message);
#endif
            EXPECT_EQ(formatter.format(event), expected) << "形状：文件 " << shape.file << "、logger 是否为空 " << shape.logger.empty();
        }
    }
} // namespace AsynGyanis::Base
