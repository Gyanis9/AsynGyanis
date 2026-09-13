/**
 * @file TestConsoleSink.cpp
 * @brief ConsoleSink 单元测试：控制台写入安全性、等级到流的分流、写出即刷新与彩色开关切换
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Sinks/ConsoleSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"
#include "Platform/IO/Console.h"

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
         * @brief 只统计刷新次数的流缓冲
         *
         * @details `std::flush` 走 `rdbuf()->pubsync()`，因此把标准流临时挂到本缓冲上就能观测
         *          「write() 返回前是否真的刷过」；写出的字节一概丢弃，用例只关心刷新时机。
         */
        class SyncCountingBuffer : public std::streambuf
        {
        public:
            /**
             * @brief 已发生的刷新次数
             * @return std::size_t 次数
             */
            [[nodiscard]] std::size_t syncCount() const noexcept
            {
                return m_syncCount;
            }

        protected:
            /**
             * @brief 重写 std::streambuf::sync()：只记账
             * @return int 0 表示成功
             */
            int sync() override
            {
                ++m_syncCount;
                return 0;
            }

            /**
             * @brief 重写 std::streambuf::xsputn()：吞掉字节并报告已写出
             * @param characters 待写缓冲（不使用）
             * @param count 字节数
             * @return std::streamsize 已写出的字节数
             */
            std::streamsize xsputn(const char *characters, const std::streamsize count) override
            {
                static_cast<void>(characters);
                return count;
            }

            /**
             * @brief 重写 std::streambuf::overflow()：吞掉单个字节
             * @param character 待写字符
             * @return int 原样返回表示成功
             */
            int overflow(const int character) override
            {
                return character;
            }

        private:
            std::size_t m_syncCount{0}; ///< 累计刷新次数
        };

        /**
         * @brief 把某个标准流临时改挂到给定缓冲，析构时还原原缓冲
         */
        class ScopedStreamRedirect
        {
        public:
            /**
             * @brief 改挂流缓冲
             * @param stream 目标标准流（std::cout 或 std::cerr）
             * @param buffer 临时缓冲，须活过本对象
             */
            ScopedStreamRedirect(std::ostream &stream, std::streambuf *buffer) :
                m_stream(stream), m_original(stream.rdbuf(buffer))
            {
            }

            /**
             * @brief 析构时还原原缓冲，避免影响其它用例
             */
            ~ScopedStreamRedirect()
            {
                m_stream.rdbuf(m_original);
            }

            ScopedStreamRedirect(const ScopedStreamRedirect &) = delete;

            ScopedStreamRedirect &operator=(const ScopedStreamRedirect &) = delete;

        private:
            std::ostream   &m_stream;   ///< 被改挂的标准流
            std::streambuf *m_original; ///< 原缓冲，析构时还原
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
    // 写出即刷新
    // ============================================================================

    TEST(ConsoleSink, WriteFlushesBelowWarnLevelsBeforeReturning)
    {
        // 钉住：低于 Warn 的等级写 std::cout，且 write() 返回时这一行已经刷出。重定向到文件或
        // 管道时 std::cout 是全缓冲，不刷就 tail 不到实时内容，进程异常退出还会把尾部丢掉
        SyncCountingBuffer   buffer;
        ScopedStreamRedirect redirect(std::cout, &buffer);
        ConsoleSink          sink(false);

        sink.write(makeEvent(LogLevel::Info, "flush_on_write"));

        EXPECT_EQ(buffer.syncCount(), 1u) << "Info 级别的记录没有在 write() 内刷新";
    }

    TEST(ConsoleSink, WriteFlushesWarnLevelsBeforeReturning)
    {
        // Warn 及以上走 std::cerr，它恒为 unitbuf、整行写出即落地；与上一条合起来构成
        // 「write() 返回时该行已落地」这条契约的两条实现路径
        SyncCountingBuffer   buffer;
        ScopedStreamRedirect redirect(std::cerr, &buffer);
        ConsoleSink          sink(false);

        sink.write(makeEvent(LogLevel::Error, "flush_on_write"));

        EXPECT_EQ(buffer.syncCount(), 1u) << "Error 级别的记录没有在 write() 内刷新";
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

    TEST(ConsoleSink, ColorDisabledOutputNeverContainsAnsiEscape)
    {
        ConsoleSink sink(false);

        const ScopedConsoleCapture capture;
        sink.write(makeEvent(LogLevel::Info, "plain_information"));
        sink.write(makeEvent(LogLevel::Error, "plain_error"));
        sink.flush();

        EXPECT_EQ(capture.stdOut().find("\033["), std::string::npos) << capture.stdOut();
        EXPECT_EQ(capture.stdErr().find("\033["), std::string::npos) << capture.stdErr();
    }

    TEST(ConsoleSink, ColorToggleSelectsFormatterUsedByWrite)
    {
        // 颜色开关（构造参数与运行期 setter）都经同一条 formatter 选择路径，
        // 因此开启后是否出现 ANSI 序列只取决于终端能力；关闭后必须恒为纯文本
        const bool ansiSupported = AsynGyanis::Platform::Console::supportsAnsiEscapeCodes();

        ConsoleSink sink(false);
        {
            const ScopedConsoleCapture capture;
            sink.setColorEnabled(true);
            sink.write(makeEvent(LogLevel::Info, "toggled_on"));
            sink.flush();

            const std::string output = capture.stdOut();
            EXPECT_TRUE(contains(output, "toggled_on")) << output;
            EXPECT_EQ(output.find("\033[") != std::string::npos, ansiSupported) << output;
        }
        {
            const ScopedConsoleCapture capture;
            sink.setColorEnabled(false);
            sink.write(makeEvent(LogLevel::Info, "toggled_off"));
            sink.flush();

            EXPECT_TRUE(contains(capture.stdOut(), "toggled_off")) << capture.stdOut();
            EXPECT_EQ(capture.stdOut().find("\033["), std::string::npos) << capture.stdOut();
        }
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
