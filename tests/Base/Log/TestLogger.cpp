// Logger 单元测试：名称与等级、Sink 分发与异常隔离、格式化降级与并发写入

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include "Platform/Platform.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/Logger.h"
#include "Base/Log/SourceLocation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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
            /** @brief 记录一条日志事件 */
            void record(const LogEvent &event)
            {
                const std::lock_guard lock(m_mutex);
                m_events.push_back(event);
            }

            /**
             * @brief 收下并搬走事件本体
             * @details 与上面那条的区别就是这里真的把消息搬进账本：调用方的事件因此被搬空，
             *          用来还原 AsyncSink 那一类「接管本体的 Sink」的实际行为
             */
            void record(LogEvent &&event)
            {
                const std::lock_guard lock(m_mutex);
                m_events.push_back(std::move(event));
            }

            /** @brief 累加一次 flush 调用计数 */
            void countFlush()
            {
                m_flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /** @brief 已记录的事件数量 */
            [[nodiscard]] size_t eventCount() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events.size();
            }

            /** @brief 已记录事件的快照 */
            [[nodiscard]] std::vector<LogEvent> events() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events;
            }

            /**
             * @brief 最近一条事件的文本内容
             * @return 消息内容，无事件时返回空串
             */
            [[nodiscard]] std::string lastMessage() const
            {
                const std::lock_guard lock(m_mutex);
                return m_events.empty() ? std::string{} : m_events.back().message;
            }

            /** @brief flush 被调用的次数 */
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
            /** @brief 构造记录型 Sink，事件写入给定账本 */
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
         * @brief 记录事件、并在被调用时放行另一个 Sink 阈值的桩
         * @details 用来把「两个 Sink 的过滤答案在派发中途变化」造成确定事件：本 Sink 的 write()
         *          正好落在派发循环对后一个 Sink 求值之前，无需赌另一个线程何时 setLevel
         */
        class PeerEnablingSink final : public LogSink
        {
        public:
            /**
             * @brief 构造会放行同伴的 Sink
             * @param ledger 本 Sink 的事件账本
             * @param peer 每次收到事件后要把阈值放开的那个 Sink（不持有所有权）
             */
            PeerEnablingSink(std::shared_ptr<SinkLedger> ledger, LogSink *peer) :
                m_ledger(std::move(ledger))
                , m_peer(peer)
            {
            }

            /**
             * @brief 记录左值事件并放行同伴
             * @details 重写 LogSink::write(const LogEvent &)：收到的是副本，同伴随后仍能拿到完整事件。
             * @param event 日志事件
             */
            void write(const LogEvent &event) override
            {
                m_ledger->record(event);
                m_peer->setLevel(LogLevel::Trace);
            }

            /**
             * @brief 收下被交接过来的事件本体并放行同伴
             * @details 重写 LogSink::write(LogEvent &&)：走到这一条就说明拿到的是事件本体，
             *          这里像 AsyncSink 那样真把它搬进账本——调用方那份就此变空，
             *          后一个 Sink 只能记出一条只剩时间与级别的空行。这正是本桩要暴露的那一步。
             * @param event 日志事件
             */
            void write(LogEvent &&event) override
            {
                m_ledger->record(std::move(event));
                m_peer->setLevel(LogLevel::Trace);
            }

            /**
             * @brief 刷新空实现
             */
            void flush() override
            {
            }

        private:
            std::shared_ptr<SinkLedger> m_ledger; ///< 事件账本
            LogSink *m_peer;                      ///< 收到事件后要放行阈值的同伴 Sink（不持有）
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

        /** @brief 判断文本是否包含子串 */
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

        /** @brief 创建一个写入本夹具账本的记录型 Sink */
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
                // Off 不是可记录的消息等级：放行会落出一行等级标签为 "?????" 的记录
                {.threshold = LogLevel::Info, .level = LogLevel::Off, .expected = false},
                // 数值在已知范围之外仍放行：宁可留下带 "?????" 标签的一行，也不静默吞掉
                {.threshold = LogLevel::Info, .level = static_cast<LogLevel>(7), .expected = true},
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

    TEST_F(LoggerTest, OffLevelMessageIsNotDeliveredToSink)
    {
        Logger logger("off-as-message-level");
        logger.setLevel(LogLevel::Info);
        logger.addSink(recordingSink());

        // Off 只是「关掉全部输出」的阈值取值，拿它当消息等级是用错了枚举：放行后落盘的是一行
        // 等级标签为 "?????" 的记录，采集端按等级解析就读到一个不存在的等级。两个公开入口同判
        logger.log(LogLevel::Off, "不该落盘的 Off 级消息");
        logger.logFormat(LogLevel::Off, SourceLocation::current(), "不该落盘的 {}", "Off 级格式化消息");

        // 对照：同阈值下的真实等级照常落地，说明这条断言不是被阈值挡出来的假绿
        logger.log(LogLevel::Fatal, "该落盘的 Fatal 消息");

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        EXPECT_EQ(m_ledger->events().front().level, LogLevel::Fatal);
    }

    TEST_F(LoggerTest, LogFillsTimestampAndThreadId)
    {
        Logger logger("metadata");
        logger.addSink(recordingSink());

        const TimestampMoment momentBefore = std::chrono::system_clock::now();
        logger.log(LogLevel::Warn, "metadata check");
        const TimestampMoment momentAfter = std::chrono::system_clock::now();

        ASSERT_EQ(m_ledger->eventCount(), 1u);
        const LogEvent event = m_ledger->events().front();
        // 事件带的必须就是这次调用采到的那个时刻：既不是默认零值，也不是 Sink 落地时刻
        EXPECT_GE(event.timestamp, momentBefore);
        EXPECT_LE(event.timestamp, momentAfter);
        EXPECT_FALSE(event.threadIdView().empty());
    }

    TEST_F(LoggerTest, EventsShareTheLoggerNameInstance)
    {
        Logger logger("shared_name_logger");
        logger.addSink(recordingSink());

        logger.log(LogLevel::Info, "direct log");
        logger.logFormat(LogLevel::Info, SourceLocation::current(), "formatted {}", 1);

        ASSERT_EQ(m_ledger->eventCount(), 2u);
        // 名字与 Logger 共享同一份常量字符串，避免每条日志各自分配、拷贝一个名字
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

    /**
     * @brief 过滤答案在派发中途变化时，后一个 Sink 不许拿到被搬空的事件
     * @details 钉的是「交出事件本体」这条省拷贝判据的取值方式：它一度数的是「有几个 Sink 愿意收」，
     *          而那要把 shouldLog 问两遍。两遍之间阈值变了，就会按「只有一个人收」把本体移给前一个，
     *          后一个则记出一条只剩时间与级别的空行。用例用前一个 Sink 的 write() 去翻后一个的阈值，
     *          把这件事造成确定发生而不是赌调度
     */
    TEST_F(LoggerTest, LaterSinkNeverReceivesAnEventWhoseBodyWentToAnEarlierSink)
    {
        auto firstLedger  = std::make_shared<SinkLedger>();
        auto secondLedger = std::make_shared<SinkLedger>();

        // 消息必须长过小串内联，否则被搬空的那个 std::string 仍留着原文，用例看不出差别
        constexpr std::string_view kMessage = "the dispatch decision must be sampled once, so this message stays long";

        Logger logger("dispatch");
        auto secondSink      = std::make_unique<RecordingSink>(secondLedger);
        auto *secondSinkView = secondSink.get();
        secondSinkView->setLevel(LogLevel::Off);
        logger.addSink(std::make_unique<PeerEnablingSink>(firstLedger, secondSinkView));
        logger.addSink(std::move(secondSink));

        logger.log(LogLevel::Info, kMessage);

        EXPECT_EQ(firstLedger->lastMessage(), kMessage) << "前一个 Sink 就该收到完整事件";
        ASSERT_EQ(secondLedger->eventCount(), 1U) << "阈值被中途放行的后一个 Sink 也会收到这一行";
        EXPECT_EQ(secondLedger->lastMessage(), kMessage)
                << "后一个 Sink 记下了空行：事件本体已被移交给前一个 Sink";
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

    /**
     * @brief Sink 抛异常不能静默：日志系统自身出故障时，至少要往标准错误留一行
     */
    TEST_F(LoggerTest, ThrowingSinkLeavesDiagnosticOnStandardError)
    {
        Logger logger("noisy_sink");
        logger.addSink(std::make_unique<ThrowingSink>());

        std::ostringstream capturedError;
        std::streambuf    *const originalErrorBuffer = std::cerr.rdbuf(capturedError.rdbuf());
        EXPECT_NO_THROW(logger.log(LogLevel::Info, "diagnostic message"));
        std::cerr.rdbuf(originalErrorBuffer);

        const std::string diagnostic = capturedError.str();
        EXPECT_TRUE(contains(diagnostic, "noisy_sink")) << diagnostic;
        EXPECT_TRUE(contains(diagnostic, "intentional sink failure")) << diagnostic;
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

    /**
     * @brief Fatal 分发后立刻刷新每一个收下它的 Sink，其余等级不刷
     * @details 打 Fatal 的调用方往往接着就 abort()/退出，不会替日志系统补那一次 flush；于是「最想知道的
     *          最后一条」留在文件流的用户态缓冲里一起没了。刷新因此只挂在 Fatal 上——常规等级不为它付
     *          这笔钱（FileSink 的 flush 是一次 FlushFileBuffers/fsync）。
     */
    TEST(Logger, FatalLineFlushesEverySinkThatTookIt)
    {
        auto firstLedger  = std::make_shared<SinkLedger>();
        auto secondLedger = std::make_shared<SinkLedger>();
        Logger logger("fatal_flush");
        logger.setLevel(LogLevel::Trace);
        logger.addSink(std::make_unique<RecordingSink>(firstLedger));
        logger.addSink(std::make_unique<RecordingSink>(secondLedger));

        logger.log(LogLevel::Info, "buffered info line");
        EXPECT_EQ(firstLedger->eventCount(), 1U);
        EXPECT_EQ(firstLedger->flushCount(), 0) << "常规等级不该为刷新付钱";

        logger.log(LogLevel::Fatal, "the last line before abort");
        EXPECT_EQ(firstLedger->flushCount(), 1) << "Fatal 之后仍要靠调用方记得 flush，那条日志就可能随进程一起丢";
        EXPECT_EQ(secondLedger->flushCount(), 1) << "每个收下这条的 Sink 都要刷：只刷第一个等于漏";
        EXPECT_EQ(secondLedger->eventCount(), 2U);

        // 没收下这条的 Sink 不该被顺带刷新（它的缓冲里没有这条内容，刷了也只是白付一次系统调用）
        auto skippedLedger = std::make_shared<SinkLedger>();
        Logger warnOnly("fatal_flush_warn_only");
        warnOnly.setLevel(LogLevel::Warn);
        warnOnly.addSink(std::make_unique<RecordingSink>(skippedLedger));
        warnOnly.log(LogLevel::Info, "filtered out");
        EXPECT_EQ(skippedLedger->eventCount(), 0U);
        EXPECT_EQ(skippedLedger->flushCount(), 0);
    }
} // namespace AsynGyanis::Base
