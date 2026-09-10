/**
 * @file TestLogMacros.cpp
 * @brief LogMacros 单元测试：源码位置采集宏与等级/格式化宏经由根日志器的输出行为
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/LogMacros.h"
#include "Base/Log/LogSink.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 记录型 Sink：在内存中缓存收到的日志事件，供宏路由断言
         *
         * @details 仅用于测试，不加异步环节，因此宏调用返回后事件一定已可读。
         */
        class RecordingLogSink final : public LogSink
        {
        public:
            /**
             * @brief 缓存事件副本，模拟 Sink 接收输出
             * @param event 待记录的日志事件
             */
            void write(const LogEvent &event) override
            {
                const std::lock_guard lock(m_mutex);
                m_events.push_back(event);
            }

            /**
             * @brief 内存缓冲无需落地，仅累加刷新次数以验证 flush 被调用
             */
            void flush() override
            {
                const std::lock_guard lock(m_mutex);
                ++m_flushCount;
            }

            /**
             * @brief 获取已记录事件的快照
             * @return std::vector<LogEvent> 事件副本列表
             */
            [[nodiscard]] std::vector<LogEvent> events() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events;
            }

            /**
             * @brief 获取最近一条事件
             * @return LogEvent 最近写入的事件副本，无事件时为默认构造
             */
            [[nodiscard]] LogEvent lastEvent() const
            {
                const std::lock_guard lock(m_mutex);
                if (m_events.empty())
                {
                    return LogEvent{};
                }
                return m_events.back();
            }

            /**
             * @brief 获取写入次数
             * @return std::size_t 已接收事件数量
             */
            [[nodiscard]] std::size_t writeCount() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events.size();
            }

            /**
             * @brief 获取刷新次数
             * @return int flush() 被调用次数
             */
            [[nodiscard]] int flushCount() const
            {
                const std::lock_guard lock(m_mutex);
                return m_flushCount;
            }

        private:
            mutable std::mutex    m_mutex;         ///< 保护下列成员
            std::vector<LogEvent> m_events{};      ///< 缓存的事件列表
            int                   m_flushCount{0}; ///< 刷新调用次数
        };

        /**
         * @brief 总是抛出异常的 Sink，用于验证单个 Sink 故障不会波及调用方
         */
        class ThrowingLogSink final : public LogSink
        {
        public:
            /**
             * @brief 无条件抛出异常，模拟落地失败的目标
             * @param event 日志事件（未使用）
             */
            void write(const LogEvent &event) override
            {
                (void) event;
                throw std::runtime_error("sink failure");
            }

            /**
             * @brief 空实现，故障仅在 write 阶段注入
             */
            void flush() override
            {
            }
        };
    } // namespace

    /**
     * @brief LogMacros 测试夹具
     *
     * @details 每个用例前重建根日志器并挂上一个记录型 Sink，用例后注销根日志器，
     *          保证单例注册表不会在用例之间残留状态。
     */
    class LogMacros : public ::testing::Test
    {
    protected:
        /**
         * @brief 用例前置：重置根日志器级别并挂上记录型 Sink
         */
        void SetUp() override
        {
            Logger &rootLogger = LoggerRegistry::instance().getRootLogger();
            rootLogger.clearSinks();
            rootLogger.setLevel(LogLevel::Trace);

            auto ownedSink  = std::make_unique<RecordingLogSink>();
            m_recordingSink = ownedSink.get();
            rootLogger.addSink(std::move(ownedSink));
        }

        /**
         * @brief 用例后置：注销根日志器，避免 Sink 与级别设置残留到其他用例
         */
        void TearDown() override
        {
            LoggerRegistry::instance().unregisterLogger("root");
            m_recordingSink = nullptr;
        }

        RecordingLogSink *m_recordingSink{nullptr}; ///< 指向夹具安装的记录型 Sink（所有权在根日志器）
    };

    TEST_F(LogMacros, SourceLocationMatchesBuildConfiguration)
    {
        const SourceLocation location = LOG_SOURCE_LOCATION();

#ifdef ASYN_DEBUG
        ASSERT_NE(location.fileName, nullptr);
        EXPECT_STREQ(location.shortFileName(), "TestLogMacros.cpp");
        EXPECT_GT(location.line, 0);
        EXPECT_NE(location.functionName, nullptr);
#else
        EXPECT_EQ(location.fileName, nullptr);
        EXPECT_EQ(location.line, 0);
        EXPECT_EQ(location.functionName, nullptr);
        EXPECT_STREQ(location.shortFileName(), "");
#endif
    }

    TEST_F(LogMacros, InfoMacroWritesSingleEventToRootLogger)
    {
        LOG_INFO("macro info message");

        ASSERT_EQ(m_recordingSink->writeCount(), 1U);

        const LogEvent event = m_recordingSink->lastEvent();
        EXPECT_EQ(event.level, LogLevel::Info);
        EXPECT_EQ(event.message, "macro info message");
        EXPECT_EQ(event.loggerName, "root");
        EXPECT_EQ(event.timestamp.size(), 23U);
        EXPECT_EQ(event.threadId, threadIdString());

#ifdef ASYN_DEBUG
        EXPECT_STREQ(event.location.shortFileName(), "TestLogMacros.cpp");
        EXPECT_GT(event.location.line, 0);
#else
        EXPECT_EQ(event.location.fileName, nullptr);
#endif
    }

    TEST_F(LogMacros, EveryLevelMacroReachesRootLoggerInOrder)
    {
        LOG_TRACE("trace message");
        LOG_DEBUG("debug message");
        LOG_INFO("info message");
        LOG_WARN("warn message");
        LOG_ERROR("error message");
        LOG_FATAL("fatal message");

        const std::vector<LogEvent> events = m_recordingSink->events();
        ASSERT_EQ(events.size(), 6U);

        const std::vector<LogLevel> expectedLevels = {
                LogLevel::Trace,
                LogLevel::Debug,
                LogLevel::Info,
                LogLevel::Warn,
                LogLevel::Error,
                LogLevel::Fatal,
        };
        const std::vector<std::string> expectedMessages = {
                "trace message",
                "debug message",
                "info message",
                "warn message",
                "error message",
                "fatal message",
        };

        for (std::size_t index = 0; index < expectedLevels.size(); ++index)
        {
            EXPECT_EQ(events[index].level, expectedLevels[index]) << "index " << index;
            EXPECT_EQ(events[index].message, expectedMessages[index]) << "index " << index;
        }
    }

    TEST_F(LogMacros, PlainMessageBracesAreNotInterpretedAsFormat)
    {
        LOG_INFO("payload={not a placeholder} count={0}");

        ASSERT_EQ(m_recordingSink->writeCount(), 1U);
        EXPECT_EQ(m_recordingSink->lastEvent().message, "payload={not a placeholder} count={0}");
        EXPECT_EQ(m_recordingSink->lastEvent().level, LogLevel::Info);
    }

    TEST_F(LogMacros, FormatMacroSubstitutesArguments)
    {
        const std::string userName = "alice";
        LOG_WARN_FMT("user={} age={} ratio={:.2f}", userName, 30, 0.5);

        ASSERT_EQ(m_recordingSink->writeCount(), 1U);

        const LogEvent event = m_recordingSink->lastEvent();
        EXPECT_EQ(event.level, LogLevel::Warn);
        EXPECT_EQ(event.message, "user=alice age=30 ratio=0.50");
        EXPECT_EQ(event.loggerName, "root");
    }

    TEST_F(LogMacros, ErrorFormatMacroKeepsOriginalLevel)
    {
        LOG_ERROR_FMT("descriptor={} failed", 42);

        ASSERT_EQ(m_recordingSink->writeCount(), 1U);
        EXPECT_EQ(m_recordingSink->lastEvent().level, LogLevel::Error);
        EXPECT_EQ(m_recordingSink->lastEvent().message, "descriptor=42 failed");
    }

    TEST_F(LogMacros, FormatMacroDegradesToErrorWithoutThrowing)
    {
        EXPECT_NO_THROW(LOG_INFO_FMT("argument out of range: {9}", 42));

        const std::vector<LogEvent> events = m_recordingSink->events();
        ASSERT_EQ(events.size(), 1U);

        EXPECT_EQ(events[0].level, LogLevel::Error);
        EXPECT_NE(events[0].message.find("Log format error"), std::string::npos) << events[0].message;
        EXPECT_NE(events[0].message.find("argument out of range: {9}"), std::string::npos) << events[0].message;
    }

    TEST_F(LogMacros, MacrosRespectRootLoggerLevelThreshold)
    {
        LoggerRegistry::instance().getRootLogger().setLevel(LogLevel::Warn);

        LOG_TRACE("suppressed trace");
        LOG_DEBUG("suppressed debug");
        LOG_INFO("suppressed info");
        LOG_INFO_FMT("suppressed {}", "formatted");
        EXPECT_EQ(m_recordingSink->writeCount(), 0U);

        LOG_WARN("visible warn");
        LOG_ERROR_FMT("visible {} {}", "error", 7);
        LOG_FATAL("visible fatal");

        const std::vector<LogEvent> events = m_recordingSink->events();
        ASSERT_EQ(events.size(), 3U);
        EXPECT_EQ(events[0].level, LogLevel::Warn);
        EXPECT_EQ(events[1].level, LogLevel::Error);
        EXPECT_EQ(events[2].level, LogLevel::Fatal);
        EXPECT_EQ(events[1].message, "visible error 7");
    }

    TEST_F(LogMacros, LoggerMacroRoutesToSpecifiedLogger)
    {
        constexpr const char *kTargetLoggerName = "MacroTargetLogger";

        Logger &targetLogger = LoggerRegistry::instance().getLogger(kTargetLoggerName);
        targetLogger.clearSinks();
        targetLogger.setLevel(LogLevel::Trace);

        auto  ownedSink  = std::make_unique<RecordingLogSink>();
        auto *targetSink = ownedSink.get();
        targetLogger.addSink(std::move(ownedSink));

        LOG_LOGGER(targetLogger, LogLevel::Debug, "explicit logger message");
        LOG_LOGGER_INFO_FMT(targetLogger, "explicit {} {}", 1, "two");

        EXPECT_EQ(targetSink->writeCount(), 2U);
        const std::vector<LogEvent> targetEvents = targetSink->events();
        ASSERT_EQ(targetEvents.size(), 2U);
        EXPECT_EQ(targetEvents[0].level, LogLevel::Debug);
        EXPECT_EQ(targetEvents[0].message, "explicit logger message");
        EXPECT_EQ(targetEvents[0].loggerName, kTargetLoggerName);
        EXPECT_EQ(targetEvents[1].level, LogLevel::Info);
        EXPECT_EQ(targetEvents[1].message, "explicit 1 two");

        EXPECT_EQ(m_recordingSink->writeCount(), 0U);

        LoggerRegistry::instance().unregisterLogger(kTargetLoggerName);
    }

    TEST_F(LogMacros, ThrowingSinkDoesNotPropagateToCaller)
    {
        LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<ThrowingLogSink>());

        EXPECT_NO_THROW(LOG_INFO("survives failing sink"));

        EXPECT_EQ(m_recordingSink->writeCount(), 1U);
        EXPECT_EQ(m_recordingSink->lastEvent().message, "survives failing sink");
    }

    TEST_F(LogMacros, RootLoggerFlushReachesInstalledSink)
    {
        LoggerRegistry::instance().getRootLogger().flush();

        EXPECT_EQ(m_recordingSink->flushCount(), 1);
    }
} // namespace AsynGyanis::Base
