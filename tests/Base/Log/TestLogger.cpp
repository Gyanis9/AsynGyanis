/**
 * @file TestLogger.cpp
 * @brief Logger 单元测试：名称与等级、Sink 分发与异常隔离、格式化降级与并发写入
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Logger.h"
#include "Base/Log/SourceLocation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 记录型 Sink 的共享账本
         *
         * @details Sink 由 Logger 以 unique_ptr 持有并可能在 clearSinks() 时销毁，
         *          因此把「已记录的事件」放在账本里由测试自己共享持有，避免断言时悬垂。
         */
        class SinkLedger
        {
        public:
            /**
             * @brief 记录一条日志事件
             * @param event 待记录的日志事件
             */
            void record(const LogEvent &event)
            {
                const std::lock_guard lock(m_mutex);
                m_events.push_back(event);
            }

            /**
             * @brief 累加一次 flush 调用计数
             */
            void countFlush()
            {
                m_flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief 已记录的事件数量
             * @return size_t 事件条数
             */
            [[nodiscard]] size_t eventCount() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events.size();
            }

            /**
             * @brief 已记录事件的快照
             * @return std::vector<LogEvent> 事件副本列表
             */
            [[nodiscard]] std::vector<LogEvent> events() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events;
            }

            /**
             * @brief 最近一条事件的文本内容
             * @return std::string 消息内容，无事件时返回空串
             */
            [[nodiscard]] std::string lastMessage() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events.empty() ? std::string{} : m_events.back().message;
            }

            /**
             * @brief flush 被调用的次数
             * @return int 次数
             */
            [[nodiscard]] int flushCount() const noexcept
            {
                return m_flushCount.load(std::memory_order_relaxed);
            }

        private:
            mutable std::mutex    m_mutex{};       ///< 保护事件列表的互斥锁
            std::vector<LogEvent> m_events{};      ///< 已记录的事件列表
            std::atomic<int>      m_flushCount{0}; ///< flush 调用计数
        };

        /**
         * @brief 把事件写入共享账本的记录型 Sink 桩
         */
        class RecordingSink : public LogSink
        {
        public:
            /**
             * @brief 构造记录型 Sink
             * @param ledger 共享的事件账本
             */
            explicit RecordingSink(std::shared_ptr<SinkLedger> ledger) :
                m_ledger(std::move(ledger))
            {
            }

            /**
             * @brief 将事件记入账本
             * @param event 日志事件
             */
            void write(const LogEvent &event) override
            {
                m_ledger->record(event);
            }

            /**
             * @brief 记账一次 flush 调用
             */
            void flush() override
            {
                m_ledger->countFlush();
            }

        private:
            std::shared_ptr<SinkLedger> m_ledger; ///< 事件账本
        };

        /**
         * @brief write() 必抛异常的 Sink 桩，用于验证 Sink 之间的故障隔离
         */
        class ThrowingSink : public LogSink
        {
        public:
            /**
             * @brief 写入时始终抛出异常
             * @param event 日志事件（忽略）
             */
            void write(const LogEvent &event) override
            {
                (void) event;
                throw std::runtime_error("intentional sink failure");
            }

            /**
             * @brief 刷新空实现
             */
            void flush() override
            {
            }
        };

        /**
         * @brief 等级阈值判定用例的一行数据
         */
        struct ShouldLogCase
        {
            LogLevel threshold; ///< 日志器等级阈值
            LogLevel level;     ///< 待判定等级
            bool     expected;  ///< 期望的判定结果
        };

        /**
         * @brief 判断文本是否包含子串
         * @param haystack 待检查文本
         * @param needle 子串
         * @return true 包含
         */
        bool contains(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    /**
     * @brief Logger 测试夹具：提供事件账本与配套的记录型 Sink 构造入口
     */
    class LoggerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_ledger = std::make_shared<SinkLedger>();
        }

        /**
         * @brief 创建一个写入本夹具账本的记录型 Sink
         * @return std::unique_ptr<LogSink> Sink 所有权
         */
        [[nodiscard]] std::unique_ptr<LogSink> recordingSink() const
        {
            return std::make_unique<RecordingSink>(m_ledger);
        }

        std::shared_ptr<SinkLedger> m_ledger; ///< 事件账本
    };

    // ============================================================================
    // 名称与初始等级
    // ============================================================================

    TEST_F(LoggerTest, ConstructorKeepsGivenName)
    {
        const Logger logger("network");

        EXPECT_EQ(logger.name(), "network");
    }

    TEST_F(LoggerTest, ConstructorAcceptsEmptyName)
    {
        const Logger logger("");

        EXPECT_TRUE(logger.name().empty());
    }

    TEST_F(LoggerTest, ConstructorAcceptsVeryLongName)
    {
        const std::string longName(500, 'L');

        const Logger logger(longName);

        EXPECT_EQ(logger.name(), longName);
    }

    TEST_F(LoggerTest, NewLoggerStartsAtTraceLevel)
    {
        const Logger logger("fresh");

        EXPECT_EQ(logger.getLevel(), LogLevel::Trace);
    }

    TEST_F(LoggerTest, NewLoggerHasNoSinkSoLoggingProducesNothing)
    {
        Logger logger("sinkless");

        EXPECT_NO_THROW(logger.log(LogLevel::Info, "dropped by missing sink"));
        EXPECT_EQ(m_ledger->eventCount(), 0u);
    }

    // ============================================================================
    // setLevel / getLevel / shouldLog
    // ============================================================================

    TEST_F(LoggerTest, SetLevelRoundTripsThroughGetLevel)
    {
        Logger logger("level");

        for (const LogLevel level: {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal, LogLevel::Off})
        {
            logger.setLevel(level);
            EXPECT_EQ(logger.getLevel(), level) << "threshold=" << logLevelToString(level);
        }
    }

    TEST_F(LoggerTest, ShouldLogAcceptsLevelAtOrAboveThreshold)
    {
        Logger logger("threshold");
        logger.setLevel(LogLevel::Info);

        const std::vector<ShouldLogCase> cases{
                {.threshold = LogLevel::Info, .level = LogLevel::Trace, .expected = false},
                {.threshold = LogLevel::Info, .level = LogLevel::Debug, .expected = false},
                {.threshold = LogLevel::Info, .level = LogLevel::Info, .expected = true},
                {.threshold = LogLevel::Info, .level = LogLevel::Warn, .expected = true},
                {.threshold = LogLevel::Info, .level = LogLevel::Error, .expected = true},
                {.threshold = LogLevel::Info, .level = LogLevel::Fatal, .expected = true},
                {.threshold = LogLevel::Info, .level = LogLevel::Off, .expected = true},
        };

        for (const auto &[threshold, level, expected]: cases)
        {
            logger.setLevel(threshold);
            EXPECT_EQ(logger.shouldLog(level), expected)
                    << "threshold=" << logLevelToString(threshold)
                    << " level=" << logLevelToString(level);
        }
    }

    TEST_F(LoggerTest, TraceThresholdLetsEveryLevelThrough)
    {
        Logger logger("trace");
        logger.setLevel(LogLevel::Trace);

        for (const LogLevel level: {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal})
        {
            EXPECT_TRUE(logger.shouldLog(level)) << "level=" << logLevelToString(level);
        }
    }

    TEST_F(LoggerTest, OffThresholdSuppressesEveryRealLevel)
    {
        Logger logger("off");
        logger.setLevel(LogLevel::Off);

        for (const LogLevel level: {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal})
        {
            EXPECT_FALSE(logger.shouldLog(level)) << "level=" << logLevelToString(level);
        }

        // Off 表示关闭全部日志输出，因此连 Off 级自身也不放行
        EXPECT_FALSE(logger.shouldLog(LogLevel::Off));
    }

    // ============================================================================
    // Sink 管理与分发
    // ============================================================================

    TEST_F(LoggerTest, LogDeliversEventToAttachedSink)
    {
        Logger logger("dispatch");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "hello sink");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_EQ(event.message, "hello sink");
        EXPECT_EQ(event.level, LogLevel::Info);
        EXPECT_EQ(event.loggerNameView(), "dispatch");
    }

    TEST_F(LoggerTest, LogFillsTimestampAndThreadId)
    {
        Logger logger("metadata");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Warn, "metadata check");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_FALSE(event.timestamp.empty());
        EXPECT_FALSE(event.threadId.empty());
    }

    TEST_F(LoggerTest, EventsShareTheLoggerNameInstance)
    {
        Logger logger("shared_name_logger");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "direct log");
        logger.logFormat(LogLevel::Info, SourceLocation::current(), "formatted {}", 1);

        ASSERT_EQ(m_ledger->eventCount(), 2u);
        // 名字与 Logger 共享同一份常量字符串：每条日志不再各自分配/拷贝一个名字
        for (const LogEvent &event: m_ledger->events())
        {
            ASSERT_NE(event.loggerName, nullptr);
            EXPECT_EQ(event.loggerName.get(), &logger.name());
            EXPECT_EQ(event.loggerNameView(), "shared_name_logger");
        }
    }

    TEST_F(LoggerTest, LogFormatForwardsFormattedMessageWithoutExtraCopy)
    {
        Logger logger("fmt_forward");
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Info, SourceLocation::current(), "user={} age={} ratio={:.2f}", "alice", 30, 0.5);

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        // 格式化结果被移动进事件（而非再拷贝一次），内容与等级必须完整保留
        EXPECT_EQ(m_ledger->events().front().message, "user=alice age=30 ratio=0.50");
        EXPECT_EQ(m_ledger->events().front().level, LogLevel::Info);
    }

    TEST_F(LoggerTest, LogCarriesExplicitSourceLocation)
    {
        Logger logger("location");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Debug, "with location", SourceLocation("SomeFile.cpp", 42, "someFunction"));

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_EQ(std::string_view(event.location.shortFileName()), "SomeFile.cpp");
        EXPECT_EQ(event.location.line, 42);
    }

    TEST_F(LoggerTest, LogBelowLoggerThresholdReachesNoSink)
    {
        Logger logger("filtered");
        logger.setLevel(LogLevel::Error);
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "suppressed_by_logger");
        logger.log(LogLevel::Error, "kept_by_logger");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "kept_by_logger");
    }

    TEST_F(LoggerTest, SinkThresholdFiltersEventsOfSameLogger)
    {
        Logger logger("sink_filter");
        auto   warnOnlySink = std::make_unique<RecordingSink>(m_ledger);
        warnOnlySink->setLevel(LogLevel::Warn);
        logger.addSink(std::move(warnOnlySink));

        logger.log(LogLevel::Info, "below_sink_level");
        logger.log(LogLevel::Warn, "at_sink_level");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "at_sink_level");
    }

    TEST_F(LoggerTest, EveryAttachedSinkReceivesTheSameEvent)
    {
        Logger logger("broadcast");
        logger.addSink(recordingSink());
        logger.addSink(recordingSink());
        logger.addSink(recordingSink());

        logger.log(LogLevel::Error, "broadcast message");

        // 三个 Sink 共享同一账本，因此共收到 3 条记录
        EXPECT_EQ(m_ledger->eventCount(), 3u);
    }

    TEST_F(LoggerTest, ThrowingSinkDoesNotStopOtherSinks)
    {
        Logger logger("isolated");
        logger.addSink(std::make_unique<ThrowingSink>());
        logger.addSink(recordingSink());

        EXPECT_NO_THROW(logger.log(LogLevel::Info, "survives failing sink"));

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "survives failing sink");
    }

    TEST_F(LoggerTest, ThrowingSinkDoesNotBreakSubsequentFlush)
    {
        Logger logger("flush_isolation");
        logger.addSink(std::make_unique<ThrowingSink>());
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "before flush");
        logger.flush();
        logger.log(LogLevel::Info, "after flush");
        logger.flush();

        EXPECT_EQ(m_ledger->eventCount(), 2u);
        EXPECT_GE(m_ledger->flushCount(), 2);
    }

    TEST_F(LoggerTest, ClearSinksStopsFurtherOutput)
    {
        Logger logger("clear");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "before_clear");
        logger.clearSinks();
        logger.log(LogLevel::Info, "after_clear");

        EXPECT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "before_clear");
    }

    TEST_F(LoggerTest, FlushIsForwardedToEverySink)
    {
        Logger logger("flush");
        logger.addSink(recordingSink());
        logger.addSink(recordingSink());

        logger.flush();

        EXPECT_EQ(m_ledger->flushCount(), 2);
    }

    TEST_F(LoggerTest, FlushAfterClearSinksReachesNoSink)
    {
        Logger logger("flush_after_clear");
        logger.addSink(recordingSink());
        logger.clearSinks();

        logger.flush();

        EXPECT_EQ(m_ledger->flushCount(), 0);
    }

    TEST_F(LoggerTest, NullSinkIsSkippedByDispatchAndFlush)
    {
        Logger logger("null_sink");
        logger.addSink(nullptr);
        logger.addSink(recordingSink());

        EXPECT_NO_THROW(logger.log(LogLevel::Info, "past the null sink"));
        EXPECT_NO_THROW(logger.flush());

        EXPECT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->flushCount(), 1);
    }

    TEST_F(LoggerTest, LoggerDestructorReleasesSinksWithoutLosingEvents)
    {
        auto logger = std::make_unique<Logger>("raii");
        logger->addSink(recordingSink());
        logger->log(LogLevel::Info, "owned by unique_ptr");

        logger.reset();

        EXPECT_EQ(m_ledger->eventCount(), 1u);
    }

    // ============================================================================
    // logFormat
    // ============================================================================

    TEST_F(LoggerTest, LogFormatSubstitutesIntegerArgument)
    {
        Logger logger("fmt");
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Info, SourceLocation::current(), "value={}", 42);

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "value=42");
    }

    TEST_F(LoggerTest, LogFormatSubstitutesMultipleArguments)
    {
        Logger logger("fmt");
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Warn, SourceLocation::current(), "{} + {} = {}", 2, 3, 5);

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "2 + 3 = 5");
    }

    TEST_F(LoggerTest, LogFormatSubstitutesStringArgument)
    {
        Logger logger("fmt");
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Debug, SourceLocation::current(), "Hello, {}!", "World");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->lastMessage(), "Hello, World!");
    }

    TEST_F(LoggerTest, LogFormatKeepsLevelAndLoggerName)
    {
        Logger logger("fmt_meta");
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Fatal, SourceLocation::current(), "code={}", 7);

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_EQ(event.level, LogLevel::Fatal);
        EXPECT_EQ(event.loggerNameView(), "fmt_meta");
    }

    TEST_F(LoggerTest, InvalidFormatStringDegradesToSingleErrorEvent)
    {
        Logger logger("fmt_broken");
        logger.addSink(recordingSink());

        EXPECT_NO_THROW(logger.logFormat(LogLevel::Info, SourceLocation::current(), "malformed {", "argument"));

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_EQ(event.level, LogLevel::Error);
        EXPECT_TRUE(contains(event.message, "日志格式化错误")) << event.message;
        EXPECT_TRUE(contains(event.message, "malformed {")) << event.message;
    }

    TEST_F(LoggerTest, FormatStringWithMissingArgumentDegradesToError)
    {
        Logger logger("fmt_missing_argument");
        logger.addSink(recordingSink());

        EXPECT_NO_THROW(logger.logFormat(LogLevel::Info, SourceLocation::current(), "no argument for this placeholder: {}"));

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        EXPECT_EQ(event.level, LogLevel::Error);
        EXPECT_TRUE(contains(event.message, "no argument for this placeholder")) << event.message;
    }

    TEST_F(LoggerTest, LogFormatSkipsEventConstructionBelowThreshold)
    {
        Logger logger("fmt_threshold");
        logger.setLevel(LogLevel::Error);
        logger.addSink(recordingSink());

        logger.logFormat(LogLevel::Info, SourceLocation::current(), "expensive {}", std::string(2048, 'x'));

        EXPECT_EQ(m_ledger->eventCount(), 0u);
    }

    TEST_F(LoggerTest, LogSkipsEventConstructionBelowThreshold)
    {
        Logger logger("log_threshold");
        logger.setLevel(LogLevel::Fatal);
        logger.addSink(recordingSink());

        logger.log(LogLevel::Error, "not fatal enough");

        EXPECT_EQ(m_ledger->eventCount(), 0u);
    }

    // ============================================================================
    // 并发写入
    // ============================================================================

    TEST_F(LoggerTest, ConcurrentLoggingDeliversEveryEventExactlyOnce)
    {
        Logger logger("concurrent");
        logger.addSink(recordingSink());

        constexpr int            kthreadCount       = 4;
        constexpr int            kmessagesPerThread = 250;
        std::vector<std::thread> workers;
        workers.reserve(kthreadCount);

        for (int workerIndex = 0; workerIndex < kthreadCount; ++workerIndex)
        {
            workers.emplace_back([&logger, workerIndex, kmessagesPerThread]
            {
                for (int messageIndex = 0; messageIndex < kmessagesPerThread; ++messageIndex)
                {
                    logger.log(LogLevel::Info,
                               "worker" + std::to_string(workerIndex) + "_message" + std::to_string(messageIndex));
                }
            });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        ASSERT_EQ(m_ledger->eventCount(), static_cast<size_t>(kthreadCount) * kmessagesPerThread);

        std::unordered_set<std::string> distinctMessages;
        for (const auto &event: m_ledger->events())
        {
            distinctMessages.insert(event.message);
        }
        EXPECT_EQ(distinctMessages.size(), static_cast<size_t>(kthreadCount) * kmessagesPerThread);
    }

    TEST_F(LoggerTest, ConcurrentLoggingAndFlushDoNotDeadlock)
    {
        Logger logger("concurrent_flush");
        logger.addSink(recordingSink());
        logger.addSink(std::make_unique<ThrowingSink>());

        constexpr int kmessageCount = 500;

        std::atomic<bool> keepFlushing{true};
        std::thread       flusher([&logger, &keepFlushing]
        {
            while (keepFlushing.load(std::memory_order_acquire))
            {
                logger.flush();
            }
        });

        for (int messageIndex = 0; messageIndex < kmessageCount; ++messageIndex)
        {
            logger.log(LogLevel::Info, "flush race message");
        }

        keepFlushing.store(false, std::memory_order_release);
        flusher.join();

        logger.flush();

        EXPECT_EQ(m_ledger->eventCount(), static_cast<size_t>(kmessageCount));
        EXPECT_GE(m_ledger->flushCount(), 1);
    }
} // namespace AsynGyanis::Base
