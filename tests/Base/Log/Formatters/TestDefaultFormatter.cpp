// DefaultFormatter 单元测试：字段完整性、等级占位串与 Debug/Release 两种版式

#include "Base/Log/Formatters/DefaultFormatter.h"

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
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        constexpr auto kFixedTimestamp = "2026-09-10 12:34:56.789";
        constexpr auto kThreadId       = "tid-778899";
        constexpr auto kLoggerName     = "formatter_logger";
        constexpr auto kSourceFile     = "formatter_fixture.cpp";
        constexpr auto kSourceFunction = "testFunction";
        constexpr int  kSourceLine     = 4271;

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件，便于逐字段断言版式
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "formatter message")
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
        DefaultFormatter                                     formatter;
        const std::vector<std::pair<LogLevel, std::string> > levelTokens = {
                {LogLevel::Trace, "TRACE"},
                {LogLevel::Debug, "DEBUG"},
                {LogLevel::Info, "INFO "},
                {LogLevel::Warn, "WARN "},
                {LogLevel::Error, "ERROR"},
                {LogLevel::Fatal, "FATAL"},
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
        const LogEvent   event(LogLevel::Debug, kFixedTimestamp, kThreadId, SourceLocation(), kLoggerName, "no location");

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

        const std::string shortOutput = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation("a.cpp", 1, kSourceFunction), kLoggerName, "padding short"));
        const std::string longOutput = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation("a-much-longer-fixture-file-name.cpp", 987654, kSourceFunction),
                         kLoggerName, "padding long"));

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
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation(longFileName.c_str(), 1234567, kSourceFunction), kLoggerName, "padding long name"));

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
            return std::format("{} {} [{:<5}] [{}] {:<13} {}",
                               kFixedTimestamp, kThreadId, logLevelToString(LogLevel::Info),
                               kLoggerName, std::format("{}:{}", fileName, line), message);
        };

        const std::string hitFileName = "hit_fixture.cpp";
        const std::string fallbackFileName(80, 'n');

        const std::string hitOutput = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation(hitFileName.c_str(), 42, kSourceFunction), kLoggerName, "hit path"));
        const std::string fallbackOutput = formatter.format(
                LogEvent(LogLevel::Info, kFixedTimestamp, kThreadId,
                         SourceLocation(fallbackFileName.c_str(), 1234567, kSourceFunction), kLoggerName, "fallback path"));

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
        LogEvent event = makeEvent(LogLevel::Error, "boom");
        event.stackTrace = captureStackTrace();

        if (formatStackTrace(event.stackTrace).empty())
        {
            GTEST_SKIP() << "调试信息不可用（无 PDB/符号表），栈只以原始帧存在";
        }

        DefaultFormatter  formatter;
        const std::string output = formatter.format(event);

        const std::size_t headingPosition = output.find(kStackTraceHeading);
        ASSERT_NE(headingPosition, std::string::npos) << output;
        EXPECT_FALSE(output.substr(headingPosition + kStackTraceHeading.size()).empty()) << output;
    }
} // namespace AsynGyanis::Base
