/**
 * @file TestConsoleSink.cpp
 * @brief ConsoleSink 单元测试：控制台写入安全性、等级到流的分流与彩色开关切换
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Sinks/ConsoleSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 成对捕获/归还标准输出与标准错误的 RAII 守卫
         *
         * @details 控制台 Sink 会真的写 std::cout/std::cerr，捕获后可在不污染测试日志的前提下
         *          检查行数与内容；析构时归还流，避免用例之间互相影响。
         */
        class ScopedConsoleCapture
        {
        public:
            /**
             * @brief 开始捕获标准输出与标准错误
             */
            ScopedConsoleCapture()
            {
                ::testing::internal::CaptureStdout();
                ::testing::internal::CaptureStderr();
            }

            /**
             * @brief 析构时确保捕获已归还
             */
            ~ScopedConsoleCapture()
            {
                finish();
            }

            ScopedConsoleCapture(const ScopedConsoleCapture &) = delete;

            ScopedConsoleCapture &operator=(const ScopedConsoleCapture &) = delete;

            /**
             * @brief 获取已捕获的标准输出内容
             * @details 首次调用会先结束捕获，保证后续断言文本能正常输出到真实标准输出。
             * @return const std::string& 文本内容
             */
            [[nodiscard]] const std::string &stdOut() const
            {
                finish();
                return m_stdOut;
            }

            /**
             * @brief 获取已捕获的标准错误内容
             * @details 首次调用会先结束捕获。
             * @return const std::string& 文本内容
             */
            [[nodiscard]] const std::string &stdErr() const
            {
                finish();
                return m_stdErr;
            }

            /**
             * @brief 标准输出中的换行数量
             * @return std::size_t 行数
             */
            [[nodiscard]] std::size_t stdOutLineCount() const
            {
                finish();
                return static_cast<std::size_t>(std::count(m_stdOut.begin(), m_stdOut.end(), '\n'));
            }

            /**
             * @brief 标准错误中的换行数量
             * @return std::size_t 行数
             */
            [[nodiscard]] std::size_t stdErrLineCount() const
            {
                finish();
                return static_cast<std::size_t>(std::ranges::count(m_stdErr, '\n'));
            }

        private:
            /**
             * @brief 结束捕获并取回两个流的内容，重复调用只生效一次
             */
            void finish() const
            {
                if (m_finished)
                {
                    return;
                }
                m_finished = true;
                m_stdOut   = ::testing::internal::GetCapturedStdout();
                m_stdErr   = ::testing::internal::GetCapturedStderr();
            }

            mutable bool        m_finished{false}; ///< 捕获是否已归还
            mutable std::string m_stdOut;          ///< 捕获到的标准输出
            mutable std::string m_stdErr;          ///< 捕获到的标准错误
        };

        /**
         * @brief 构造字段齐备的日志事件
         * @param level 日志等级
         * @param message 日志消息
         * @return LogEvent 日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "console message")
        {
            return {
                    level, "2026-09-10 12:34:56.789", "tid-334455",
                    SourceLocation("console_fixture.cpp", 6421, "consoleTestFunction"),
                    "console_logger", std::move(message)
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
    // 构造与基本写入
    // ============================================================================

    TEST(ConsoleSink, ConstructionInColorAndPlainTextModeDoesNotThrow)
    {
        ScopedConsoleCapture capture;

        EXPECT_NO_THROW({ ConsoleSink colorSink(true); });
        EXPECT_NO_THROW({ ConsoleSink plainSink(false); });
    }

    TEST(ConsoleSink, WriteAtEverySeverityDoesNotThrow)
    {
        ScopedConsoleCapture capture;
        ConsoleSink          sink(false);

        for (const LogLevel level: {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal})
        {
            EXPECT_NO_THROW(sink.write(makeEvent(level, "severity_" + std::to_string(static_cast<int>(level)))));
        }
    }

    TEST(ConsoleSink, FlushDoesNotThrow)
    {
        ScopedConsoleCapture capture;
        ConsoleSink          sink(true);

        sink.write(makeEvent(LogLevel::Info, "to flush"));
        EXPECT_NO_THROW(sink.flush());
    }

    TEST(ConsoleSink, WriteEmitsExactlyOneLinePerEvent)
    {
        ConsoleSink sink(false);
        {
            const ScopedConsoleCapture capture;
            sink.write(makeEvent(LogLevel::Info, "single line one"));
            sink.write(makeEvent(LogLevel::Debug, "single line two"));
            sink.flush();

            EXPECT_EQ(capture.stdOutLineCount(), 2u);
            EXPECT_TRUE(contains(capture.stdOut(), "single line one")) << capture.stdOut();
            EXPECT_TRUE(contains(capture.stdOut(), "single line two")) << capture.stdOut();
        }
    }

    TEST(ConsoleSink, EmptyMessageStillWritesTerminatedLine)
    {
        ConsoleSink sink(false);

        const ScopedConsoleCapture capture;
        sink.write(makeEvent(LogLevel::Info, ""));
        sink.flush();

        EXPECT_EQ(capture.stdOutLineCount(), 1u);
        EXPECT_TRUE(contains(capture.stdOut(), "console_logger")) << capture.stdOut();
    }

    // ============================================================================
    // 等级到输出流的分流
    // ============================================================================

    TEST(ConsoleSink, LevelsBelowWarnGoToStandardOutput)
    {
        ConsoleSink sink(false);

        const ScopedConsoleCapture capture;
        sink.write(makeEvent(LogLevel::Trace, "route_trace"));
        sink.write(makeEvent(LogLevel::Debug, "route_debug"));
        sink.write(makeEvent(LogLevel::Info, "route_info"));
        sink.flush();

        EXPECT_TRUE(contains(capture.stdOut(), "route_trace")) << capture.stdOut();
        EXPECT_TRUE(contains(capture.stdOut(), "route_debug")) << capture.stdOut();
        EXPECT_TRUE(contains(capture.stdOut(), "route_info")) << capture.stdOut();
        EXPECT_TRUE(capture.stdErr().empty()) << capture.stdErr();
    }

    TEST(ConsoleSink, LevelsFromWarnUpGoToStandardError)
    {
        ConsoleSink sink(false);

        const ScopedConsoleCapture capture;
        sink.write(makeEvent(LogLevel::Warn, "route_warn"));
        sink.write(makeEvent(LogLevel::Error, "route_error"));
        sink.write(makeEvent(LogLevel::Fatal, "route_fatal"));
        sink.flush();

        EXPECT_TRUE(contains(capture.stdErr(), "route_warn")) << capture.stdErr();
        EXPECT_TRUE(contains(capture.stdErr(), "route_error")) << capture.stdErr();
        EXPECT_TRUE(contains(capture.stdErr(), "route_fatal")) << capture.stdErr();
        EXPECT_TRUE(capture.stdOut().empty()) << capture.stdOut();
        EXPECT_EQ(capture.stdErrLineCount(), 3u);
    }

    // ============================================================================
    // 彩色开关
    // ============================================================================

    TEST(ConsoleSink, ColorToggleKeepsWritingEveryEvent)
    {
        ConsoleSink sink(false);

        ScopedConsoleCapture capture;
        EXPECT_NO_THROW(sink.setColorEnabled(true));
        sink.write(makeEvent(LogLevel::Info, "after_enable"));
        EXPECT_NO_THROW(sink.setColorEnabled(false));
        sink.write(makeEvent(LogLevel::Info, "after_disable"));
        sink.flush();

        EXPECT_TRUE(contains(capture.stdOut(), "after_enable")) << capture.stdOut();
        EXPECT_TRUE(contains(capture.stdOut(), "after_disable")) << capture.stdOut();
        EXPECT_EQ(capture.stdOutLineCount(), 2u);
    }

    TEST(ConsoleSink, RepeatedColorToggleDoesNotThrow)
    {
        ConsoleSink sink(true);

        const ScopedConsoleCapture capture;
        for (int index = 0; index < 8; ++index)
        {
            EXPECT_NO_THROW(sink.setColorEnabled(index % 2 == 0));
        }
        sink.write(makeEvent(LogLevel::Info, "toggled"));
        sink.flush();

        EXPECT_TRUE(contains(capture.stdOut(), "toggled")) << capture.stdOut();
    }

    TEST(ConsoleSink, LevelThresholdIsStillExposedAfterColorToggle)
    {
        ConsoleSink sink(false);

        sink.setColorEnabled(true);
        sink.setLevel(LogLevel::Warn);
        sink.setColorEnabled(false);

        EXPECT_EQ(sink.getLevel(), LogLevel::Warn);
        EXPECT_FALSE(sink.shouldLog(LogLevel::Info));
        EXPECT_TRUE(sink.shouldLog(LogLevel::Error));
    }

    // ============================================================================
    // 并发安全
    // ============================================================================

    TEST(ConsoleSink, ConcurrentWritesProduceOneCompleteLinePerEvent)
    {
        ConsoleSink              sink(false);
        constexpr int            kthreadCount     = 4;
        constexpr int            kwritesPerThread = 25;
        std::vector<std::thread> threads;

        {
            const ScopedConsoleCapture capture;
            for (int index = 0; index < kthreadCount; ++index)
            {
                threads.emplace_back([&sink, index]
                {
                    for (int inner = 0; inner < kwritesPerThread; ++inner)
                    {
                        sink.write(makeEvent(LogLevel::Info,
                                             "thread" + std::to_string(index) + "_msg" + std::to_string(inner)));
                    }
                });
            }
            for (std::thread &thread: threads)
            {
                thread.join();
            }
            sink.flush();

            EXPECT_EQ(capture.stdOutLineCount(), static_cast<std::size_t>(kthreadCount) * kwritesPerThread);
            EXPECT_TRUE(capture.stdErr().empty()) << capture.stdErr();
            for (int index = 0; index < kthreadCount; ++index)
            {
                for (int inner = 0; inner < kwritesPerThread; ++inner)
                {
                    const std::string token = "thread" + std::to_string(index) + "_msg" + std::to_string(inner);
                    EXPECT_TRUE(contains(capture.stdOut(), token)) << token;
                }
            }
        }
    }

    TEST(ConsoleSink, ConcurrentColorToggleAndWriteStaySafe)
    {
        ConsoleSink              sink(true);
        std::atomic<bool>        stopToggling{false};
        std::vector<std::thread> togglers;
        std::vector<std::thread> writers;
        constexpr int            kwriterCount     = 3;
        constexpr int            kwritesPerWriter = 50;

        const ScopedConsoleCapture capture;
        togglers.reserve(2);
        for (int index = 0; index < 2; ++index)
        {
            togglers.emplace_back([&sink, &stopToggling]
            {
                while (!stopToggling.load(std::memory_order_relaxed))
                {
                    sink.setColorEnabled(true);
                    sink.setColorEnabled(false);
                }
            });
        }
        writers.reserve(kwriterCount);
        for (int index = 0; index < kwriterCount; ++index)
        {
            writers.emplace_back([&sink]
            {
                for (int inner = 0; inner < kwritesPerWriter; ++inner)
                {
                    sink.write(makeEvent(LogLevel::Info, "racy_" + std::to_string(inner)));
                }
            });
        }

        for (std::thread &writer: writers)
        {
            writer.join();
        }
        stopToggling.store(true);
        for (std::thread &toggler: togglers)
        {
            toggler.join();
        }
        sink.flush();

        EXPECT_EQ(capture.stdOutLineCount(), static_cast<std::size_t>(kwriterCount) * kwritesPerWriter);
        EXPECT_TRUE(capture.stdErr().empty()) << capture.stdErr();
    }

    TEST(ConsoleSink, DestructionAfterWritesIsSafe)
    {
        ScopedConsoleCapture capture;

        EXPECT_NO_THROW(
                {
                ConsoleSink sink(true);
                sink.write(makeEvent(LogLevel::Error, "before destroy"));
                });
    }
} // namespace AsynGyanis::Base
