/**
 * @file TestDefaultFormatter.cpp
 * @brief DefaultFormatter 单元测试：字段完整性、等级占位串与 Debug/Release 两种版式
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Formatters/DefaultFormatter.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

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
         * @brief 构造字段齐备的日志事件，便于逐字段断言版式
         * @param level 日志等级
         * @param message 日志消息
         * @return LogEvent 内容固定的日志事件
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
} // namespace AsynGyanis::Base
