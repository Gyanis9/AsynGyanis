// LogSink 抽象基类单元测试：等级过滤、格式化器注入与原子替换的线程安全

#include "Base/Log/Sinks/LogSink.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/LogEvent.h"

#include "BaseTestSupport.h"
#include "Base/Log/Formatters/LogFormatter.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 可测的 LogSink 桩实现
         *
         * @details write() 记录基类 formatEvent() 的渲染结果，并把受保护的 formatEvent()
         *          通过 render() 暴露给用例，用于验证等级过滤与格式化器替换行为。
         */
        class TestableSink final : public LogSink
        {
        public:
            /**
             * @brief 记录格式化后的日志行
             * @details 重写 LogSink::write()：持锁保存 formatEvent() 结果，
             *          自身不做等级过滤（预筛由调用方 Logger 负责）。
             */
            void write(const LogEvent &event) override
            {
                std::string     formatted = formatEvent(event);
                std::lock_guard lock(m_mutex);
                m_lines.push_back(std::move(formatted));
            }

            /**
             * @brief 统计刷新调用次数
             * @details 重写 LogSink::flush()：桩无缓冲区，仅累加计数供用例断言。
             */
            void flush() override
            {
                m_flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief 以当前格式化器渲染事件，暴露受保护的 formatEvent()
             */
            [[nodiscard]] std::string render(const LogEvent &event) const
            {
                return formatEvent(event);
            }

            /**
             * @brief 获取已记录的日志行快照
             */
            [[nodiscard]] std::vector<std::string> lines() const
            {
                std::lock_guard lock(m_mutex);
                return m_lines;
            }

            /**
             * @brief 获取最后一条日志行
             */
            [[nodiscard]] std::string lastLine() const
            {
                std::lock_guard lock(m_mutex);
                return m_lines.empty() ? std::string{} : m_lines.back();
            }

            /**
             * @brief 获取写入次数
             */
            [[nodiscard]] int writeCount() const
            {
                std::lock_guard lock(m_mutex);
                return static_cast<int>(m_lines.size());
            }

            /**
             * @brief 获取刷新次数
             */
            [[nodiscard]] int flushCount() const noexcept
            {
                return m_flushCount.load(std::memory_order_relaxed);
            }

        private:
            mutable std::mutex       m_mutex;         ///< 保护记录向量的互斥锁
            std::vector<std::string> m_lines;         ///< 已记录的日志行
            std::atomic<int>         m_flushCount{0}; ///< 刷新调用计数
        };

        /**
         * @brief 带前缀标记的格式化器桩，用于区分当前生效的是哪一个实现
         */
        class MarkerFormatter final : public LogFormatter
        {
        public:
            /**
             * @brief 使用给定前缀构造格式化器
             */
            explicit MarkerFormatter(std::string prefix) :
                m_prefix(std::move(prefix))
            {
            }

            /**
             * @brief 输出「前缀:消息」
             * @details 重写 LogFormatter::format()：只保留可辨识的极简版式，
             *          便于断言当前生效的格式化器身份。
             */
            std::string format(const LogEvent &event) override
            {
                return m_prefix + ":" + event.message;
            }

        private:
            std::string m_prefix; ///< 输出前缀标记
        };

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "sink message")
        {
            return {
                    level, TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789), "tid-112233",
                    SourceLocation("sink_fixture.cpp", 8123, "sinkTestFunction"),
                    "sink_logger", std::move(message)
            };
        }

        /**
         * @brief 全部日志等级（含 Off）
         */
        const std::vector<LogLevel> &allLevels()
        {
            static const std::vector<LogLevel> klevels = {
                    LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                    LogLevel::Error, LogLevel::Fatal, LogLevel::Off
            };
            return klevels;
        }
    } // namespace

    // ============================================================================
    // 等级过滤
    // ============================================================================

    TEST(LogSink, DefaultLevelAcceptsEverySeverity)
    {
        const TestableSink sink;

        EXPECT_EQ(sink.getLevel(), LogLevel::Trace);
        for (const LogLevel level: allLevels())
        {
            EXPECT_TRUE(sink.shouldLog(level)) << "level " << static_cast<int>(level);
        }
    }

    TEST(LogSink, SetLevelRoundTripsForEverySeverity)
    {
        TestableSink sink;

        for (const LogLevel level: allLevels())
        {
            sink.setLevel(level);
            EXPECT_EQ(sink.getLevel(), level) << "level " << static_cast<int>(level);
        }
    }

    TEST(LogSink, EventAtThresholdLevelIsAccepted)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Info);

        EXPECT_TRUE(sink.shouldLog(LogLevel::Info));
    }

    TEST(LogSink, EventBelowThresholdLevelIsRejected)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Info);

        EXPECT_FALSE(sink.shouldLog(LogLevel::Trace));
        EXPECT_FALSE(sink.shouldLog(LogLevel::Debug));
    }

    TEST(LogSink, EventAboveThresholdLevelIsAccepted)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Info);

        EXPECT_TRUE(sink.shouldLog(LogLevel::Warn));
        EXPECT_TRUE(sink.shouldLog(LogLevel::Error));
        EXPECT_TRUE(sink.shouldLog(LogLevel::Fatal));
    }

    TEST(LogSink, FatalThresholdAcceptsOnlyFatal)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Fatal);

        EXPECT_TRUE(sink.shouldLog(LogLevel::Fatal));
        EXPECT_FALSE(sink.shouldLog(LogLevel::Error));
        EXPECT_FALSE(sink.shouldLog(LogLevel::Info));
    }

    TEST(LogSink, OffThresholdRejectsEverySeverity)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Off);

        for (const LogLevel level: allLevels())
        {
            EXPECT_FALSE(sink.shouldLog(level)) << "level " << static_cast<int>(level);
        }
    }

    TEST(LogSink, LevelSetByAnotherThreadIsObserved)
    {
        TestableSink            sink;
        std::promise<void>      levelStored;
        const std::future<void> levelStoredFuture = levelStored.get_future();
        LogLevel                observed          = LogLevel::Trace;

        std::thread reader([&sink, &levelStoredFuture, &observed]
        {
            levelStoredFuture.wait();
            observed = sink.getLevel();
        });

        sink.setLevel(LogLevel::Error);
        levelStored.set_value();
        reader.join();

        EXPECT_EQ(observed, LogLevel::Error);
    }

    // ============================================================================
    // 格式化器注入与回落
    // ============================================================================

    TEST(LogSink, InjectedFormatterShapesWrittenLine)
    {
        TestableSink sink;
        sink.setFormatter(std::make_unique<MarkerFormatter>("MARK-A"));

        sink.write(makeEvent(LogLevel::Info, "injected"));

        ASSERT_EQ(sink.writeCount(), 1);
        EXPECT_EQ(sink.lastLine(), "MARK-A:injected");
    }

    TEST(LogSink, ReplacingFormatterTakesEffectOnNextWrite)
    {
        TestableSink sink;
        sink.setFormatter(std::make_unique<MarkerFormatter>("MARK-A"));
        sink.setFormatter(std::make_unique<MarkerFormatter>("MARK-B"));

        sink.write(makeEvent(LogLevel::Info, "second"));

        EXPECT_EQ(sink.lastLine(), "MARK-B:second");
    }

    TEST(LogSink, MissingFormatterFallsBackToDefaultFormatter)
    {
        const TestableSink sink;
        DefaultFormatter   expectedFormatter;
        const LogEvent     event = makeEvent(LogLevel::Warn, "fallback");

        EXPECT_EQ(sink.render(event), expectedFormatter.format(event));
    }

    TEST(LogSink, FallbackRenderingKeepsLevelToken)
    {
        const TestableSink sink;

        const std::string rendered = sink.render(makeEvent(LogLevel::Warn, "token check"));

        EXPECT_NE(rendered.find("WARN "), std::string::npos) << rendered;
        EXPECT_EQ(rendered.find("\033["), std::string::npos) << rendered;
    }

    TEST(LogSink, InjectingColorFormatterAddsEscapeSequences)
    {
        TestableSink sink;
        sink.setFormatter(std::make_unique<ColorFormatter>());

        const std::string rendered = sink.render(makeEvent(LogLevel::Error, "colored"));

        EXPECT_NE(rendered.find("\033["), std::string::npos) << rendered;
        EXPECT_NE(rendered.find("ERROR"), std::string::npos) << rendered;
    }

    TEST(LogSink, ReplacedFormatterLeavesAlreadyRenderedLinesIntact)
    {
        TestableSink sink;
        sink.setFormatter(std::make_unique<MarkerFormatter>("FIRST"));

        const std::string firstRender = sink.render(makeEvent(LogLevel::Info, "kept"));

        sink.setFormatter(std::make_unique<MarkerFormatter>("SECOND"));
        const std::string secondRender = sink.render(makeEvent(LogLevel::Info, "kept"));

        EXPECT_EQ(firstRender, "FIRST:kept");
        EXPECT_EQ(secondRender, "SECOND:kept");
    }

    TEST(LogSink, WriteForwardsEveryEventRegardlessOfLevel)
    {
        TestableSink sink;
        sink.setLevel(LogLevel::Fatal);

        sink.write(makeEvent(LogLevel::Trace, "below threshold"));

        // write() 不做等级过滤，预筛由 Logger 负责；此处固化该契约避免误改
        EXPECT_EQ(sink.writeCount(), 1);
    }

    TEST(LogSink, FlushIsDispatchedToDerivedImplementation)
    {
        TestableSink sink;

        sink.flush();
        sink.flush();

        EXPECT_EQ(sink.flushCount(), 2);
    }

    TEST(LogSink, SinkStaysUsableThroughBaseReference)
    {
        const auto sink = std::make_unique<TestableSink>();
        LogSink &  base = *sink;
        base.setLevel(LogLevel::Debug);
        base.setFormatter(std::make_unique<MarkerFormatter>("BASE"));

        base.write(makeEvent(LogLevel::Info, "via base"));

        EXPECT_EQ(base.getLevel(), LogLevel::Debug);
        EXPECT_TRUE(base.shouldLog(LogLevel::Info));
        EXPECT_EQ(sink->lastLine(), "BASE:via base");
    }

    // ============================================================================
    // 并发安全
    // ============================================================================

    TEST(LogSink, ConcurrentFormatterReplacementAndWriteStaySafe)
    {
        TestableSink             sink;
        constexpr int            kwriterCount     = 4;
        constexpr int            kwritesPerThread = 200;
        constexpr int            kswapperCount    = 2;
        std::atomic<bool>        stopSwapping{false};
        std::atomic<int>         swapsPerformed{0};
        std::vector<std::thread> writers;
        std::vector<std::thread> swappers;

        sink.setFormatter(std::make_unique<MarkerFormatter>("A"));

        swappers.reserve(kswapperCount);
        for (int index = 0; index < kswapperCount; ++index)
        {
            swappers.emplace_back([&sink, &stopSwapping, &swapsPerformed]
            {
                while (!stopSwapping.load(std::memory_order_relaxed))
                {
                    sink.setFormatter(std::make_unique<MarkerFormatter>("A"));
                    sink.setFormatter(std::make_unique<ColorFormatter>());
                    sink.setFormatter(std::make_unique<MarkerFormatter>("B"));
                    swapsPerformed.fetch_add(3, std::memory_order_relaxed);
                }
            });
        }

        writers.reserve(kwriterCount);
        for (int index = 0; index < kwriterCount; ++index)
        {
            writers.emplace_back([&sink]
            {
                for (int inner = 0; inner < kwritesPerThread; ++inner)
                {
                    sink.write(makeEvent(LogLevel::Info, "concurrent_" + std::to_string(inner)));
                }
            });
        }

        for (std::thread &writer: writers)
        {
            writer.join();
        }
        stopSwapping.store(true);
        for (std::thread &swapper: swappers)
        {
            swapper.join();
        }

        const std::vector<std::string> recorded = sink.lines();
        EXPECT_EQ(static_cast<int>(recorded.size()), kwriterCount * kwritesPerThread);
        EXPECT_GT(swapsPerformed.load(), 0);
        for (const std::string &line: recorded)
        {
            // 每条输出都必须由某个完整的格式化器生成，不允许出现半截内容
            const bool fromMarker = line.starts_with("A:concurrent_") || line.starts_with("B:concurrent_");
            const bool fromColor  = line.find("concurrent_") != std::string::npos && line.find("\033[") != std::string::npos;
            EXPECT_TRUE(fromMarker || fromColor) << line;
        }
    }

    TEST(LogSink, ConcurrentWriteAndLevelUpdatesStaySafe)
    {
        TestableSink             sink;
        constexpr int            kthreadCount = 4;
        constexpr int            kiterations  = 150;
        std::vector<std::thread> threads;

        threads.reserve(kthreadCount);
        for (int index = 0; index < kthreadCount; ++index)
        {
            threads.emplace_back([&sink, index]
            {
                for (int inner = 0; inner < kiterations; ++inner)
                {
                    sink.setLevel(static_cast<LogLevel>(inner % 6));
                    (void) sink.shouldLog(LogLevel::Info);
                    sink.write(makeEvent(LogLevel::Info, "mixed_" + std::to_string(index)));
                }
            });
        }
        for (std::thread &thread: threads)
        {
            thread.join();
        }

        EXPECT_EQ(sink.writeCount(), kthreadCount * kiterations);
    }

    TEST(LogSink, DestructorReleasesInjectedFormatterWithoutThrowing)
    {
        EXPECT_NO_THROW(
                {
                const auto sink = std::make_unique<TestableSink>();
                sink->setFormatter(std::make_unique<ColorFormatter>());
                sink->write(makeEvent(LogLevel::Info, "last"));
                });
    }
} // namespace AsynGyanis::Base
