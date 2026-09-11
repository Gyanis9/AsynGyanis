/**
 * @file TestAsyncSink.cpp
 * @brief AsyncSink 单元测试：后台线程转发、flush/stop 语义与三种队列溢出策略
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Sinks/AsyncSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Base/Log/LogEvent.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/SourceLocation.h"

#include "BaseTestSupport.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 桩 Sink 的共享记录
         *
         * @details 用 shared_ptr 承载，使 AsyncSink 连同下游 Sink 一起析构后，
         *          用例仍能读取后台线程落下的记录。
         */
        struct RecordedEvents
        {
            mutable std::mutex       mutex;           ///< 保护消息向量
            std::vector<std::string> messages;        ///< 按到达顺序记录的消息
            std::vector<LogLevel>    levels;          ///< 按到达顺序记录的等级
            std::atomic<std::size_t> enteredCount{0}; ///< 进入 write() 的次数（含被阻塞的调用）
            std::atomic<std::size_t> flushCount{0};   ///< 下游被刷新的次数

            /**
             * @brief 追加一条事件记录
             * @param event 日志事件
             */
            void append(const LogEvent &event)
            {
                std::lock_guard lock(mutex);
                messages.push_back(event.message);
                levels.push_back(event.level);
            }

            /**
             * @brief 获取消息快照
             * @return std::vector<std::string> 已记录的消息
             */
            [[nodiscard]] std::vector<std::string> snapshot() const
            {
                std::lock_guard lock(mutex);
                return messages;
            }

            /**
             * @brief 已记录的消息条数
             * @return std::size_t 条数
             */
            [[nodiscard]] std::size_t size() const
            {
                std::lock_guard lock(mutex);
                return messages.size();
            }

            /**
             * @brief 判断是否记录了给定消息
             * @param message 待查找消息
             * @return true 找到
             */
            [[nodiscard]] bool contains(const std::string &message) const
            {
                std::lock_guard lock(mutex);
                return std::find(messages.begin(), messages.end(), message) != messages.end();
            }

            /**
             * @brief 获取指定位置记录的等级
             * @param index 事件下标
             * @return LogLevel 下标越界时返回 LogLevel::Off
             */
            [[nodiscard]] LogLevel levelAt(const std::size_t index) const
            {
                std::lock_guard lock(mutex);
                return index < levels.size() ? levels[index] : LogLevel::Off;
            }
        };

        /**
         * @brief 立即记录事件的桩 Sink，可选按事件耗时模拟慢速下游
         */
        class RecordingSink final : public LogSink
        {
        public:
            /**
             * @brief 构造记录型桩 Sink
             * @param events 共享记录对象，缺省时自行创建
             * @param perEventCost 每条事件的模拟处理耗时
             */
            explicit RecordingSink(std::shared_ptr<RecordedEvents> events       = std::make_shared<RecordedEvents>(),
                                   const std::chrono::microseconds perEventCost = std::chrono::microseconds::zero()) :
                m_events(std::move(events))
                , m_perEventCost(perEventCost)
            {
            }

            /**
             * @brief 记录到达的事件
             * @details 重写 LogSink::write()：先按需休眠模拟慢速 IO，再持锁追加记录，
             *          用于统计后台线程实际转发的条数。
             * @param event 日志事件
             */
            void write(const LogEvent &event) override
            {
                if (m_perEventCost > std::chrono::microseconds::zero())
                {
                    std::this_thread::sleep_for(m_perEventCost);
                }
                m_events->enteredCount.fetch_add(1, std::memory_order_relaxed);
                m_events->append(event);
            }

            /**
             * @brief 记录一次刷新调用
             * @details 重写 LogSink::flush()：桩无缓冲区，仅累加计数供 flush 语义断言。
             */
            void flush() override
            {
                m_events->flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief 获取共享记录
             * @return const std::shared_ptr<RecordedEvents>& 记录对象
             */
            [[nodiscard]] const std::shared_ptr<RecordedEvents> &events() const noexcept
            {
                return m_events;
            }

        private:
            std::shared_ptr<RecordedEvents> m_events;       ///< 共享记录
            std::chrono::microseconds       m_perEventCost; ///< 单条事件模拟耗时
        };

        /**
         * @brief 首条事件会阻塞在 write() 内的桩 Sink，用于制造「队列被填满」的场景
         */
        class BlockingRecordingSink final : public LogSink
        {
        public:
            /**
             * @brief 使用共享记录构造阻塞桩 Sink
             * @param events 共享记录对象
             */
            explicit BlockingRecordingSink(std::shared_ptr<RecordedEvents> events) :
                m_events(std::move(events))
            {
            }

            /**
             * @brief 阻塞直到 release() 后记录事件
             * @details 重写 LogSink::write()：累加进入计数后等待放行条件，被放行后像普通桩一样记录，
             *          从而在测试期间稳定地占住后台线程、让队列保持满状态。
             * @param event 日志事件
             */
            void write(const LogEvent &event) override
            {
                {
                    std::unique_lock lock(m_gateMutex);
                    m_events->enteredCount.fetch_add(1, std::memory_order_relaxed);
                    m_gateCondition.wait(lock, [this]
                    {
                        return m_released;
                    });
                }
                m_events->append(event);
            }

            /**
             * @brief 记录一次刷新调用
             * @details 重写 LogSink::flush()：累加下游刷新计数。
             */
            void flush() override
            {
                m_events->flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief 放行被阻塞的写入线程
             */
            void release()
            {
                {
                    std::lock_guard lock(m_gateMutex);
                    m_released = true;
                }
                m_gateCondition.notify_all();
            }

        private:
            std::shared_ptr<RecordedEvents> m_events;           ///< 共享记录
            std::mutex                      m_gateMutex;        ///< 放行条件互斥锁
            std::condition_variable         m_gateCondition;    ///< 放行条件变量
            bool                            m_released = false; ///< 是否已放行
        };

        /**
         * @brief 构造字段齐备的日志事件
         * @param level 日志等级
         * @param message 日志消息
         * @return LogEvent 日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "async message")
        {
            return {
                    level, "2026-09-10 12:34:56.789", "tid-556677",
                    SourceLocation("async_fixture.cpp", 3456, "asyncTestFunction"),
                    "async_logger", std::move(message)
            };
        }

        /**
         * @brief 轮询等待条件的超时上限（毫秒），给慢速机器留足余量
         */
        constexpr int kWaitTimeoutMilliseconds = 3000;

        /**
         * @brief 使用「下游被卡住」的桩 Sink 组装 AsyncSink 的夹具
         */
        class AsyncSinkWithBlockedDownstream : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                m_events = std::make_shared<RecordedEvents>();
            }

            /**
             * @brief 用例结束时先放行下游再销毁 AsyncSink，避免 join 卡死
             */
            void TearDown() override
            {
                if (m_downstream != nullptr)
                {
                    m_downstream->release();
                }
                m_async.reset();
            }

            /**
             * @brief 启动带阻塞下游的 AsyncSink
             * @param queueSize 队列容量
             * @param policy 溢出策略
             */
            void startAsyncSink(const std::size_t queueSize, const AsyncSink::OverflowPolicy policy)
            {
                auto downstream = std::make_unique<BlockingRecordingSink>(m_events);
                m_downstream    = downstream.get();
                m_async         = std::make_unique<AsyncSink>(std::move(downstream), queueSize, policy);
                // 此处不需要等待后台线程「就绪」：入队与消费由互斥量加带判据的条件变量同步，
                // 构造后立即写入也不会丢失唤醒；真正的时序等待由用例在写入之后完成。
            }

            std::shared_ptr<RecordedEvents> m_events;               ///< 共享记录
            std::unique_ptr<AsyncSink>      m_async;                ///< 被测异步 Sink
            BlockingRecordingSink *         m_downstream = nullptr; ///< 下游桩裸指针（所有权在 AsyncSink）
        };
    } // namespace

    // ============================================================================
    // 构造与生命周期
    // ============================================================================

    TEST(AsyncSink, ConstructionAndDestructionDoNotThrow)
    {
        EXPECT_NO_THROW(
                {
                auto downstream = std::make_unique<RecordingSink>();
                AsyncSink sink(std::move(downstream));
                });
    }

    TEST(AsyncSink, OverflowPolicyKeepsStableUnderlyingValues)
    {
        EXPECT_EQ(static_cast<int>(AsyncSink::OverflowPolicy::Block), 0);
        EXPECT_EQ(static_cast<int>(AsyncSink::OverflowPolicy::Drop), 1);
        EXPECT_EQ(static_cast<int>(AsyncSink::OverflowPolicy::DropOldest), 2);
    }

    TEST(AsyncSink, DroppedEventCountStartsAtZero)
    {
        auto            events     = std::make_shared<RecordedEvents>();
        auto            downstream = std::make_unique<RecordingSink>(events);
        const AsyncSink sink(std::move(downstream), 16);

        EXPECT_EQ(sink.droppedEventCount(), 0u);
    }

    TEST(AsyncSink, DestructorDrainsPendingEvents)
    {
        auto events = std::make_shared<RecordedEvents>();

        {
            auto      downstream = std::make_unique<RecordingSink>(events);
            AsyncSink sink(std::move(downstream), 1024);
            sink.write(makeEvent(LogLevel::Info, "drain_on_destroy"));
            // 不调用 flush()/stop()，仅靠析构排空
        }

        EXPECT_TRUE(events->contains("drain_on_destroy")) << "已记录条数 " << events->size();
    }

    TEST(AsyncSink, StopIsIdempotent)
    {
        auto      events     = std::make_shared<RecordedEvents>();
        auto      downstream = std::make_unique<RecordingSink>(events);
        AsyncSink sink(std::move(downstream), 64);

        sink.write(makeEvent(LogLevel::Info, "before stop"));
        EXPECT_NO_THROW(sink.stop());
        EXPECT_NO_THROW(sink.stop());
        EXPECT_NO_THROW(sink.stop());
        EXPECT_TRUE(events->contains("before stop"));
    }

    TEST(AsyncSink, WriteAfterStopIsNotForwarded)
    {
        auto      events     = std::make_shared<RecordedEvents>();
        auto      downstream = std::make_unique<RecordingSink>(events);
        AsyncSink sink(std::move(downstream), 64);

        sink.write(makeEvent(LogLevel::Info, "accepted"));
        sink.stop();
        const std::size_t acceptedCount = events->size();

        sink.write(makeEvent(LogLevel::Info, "rejected"));
        sink.flush();

        EXPECT_EQ(acceptedCount, 1u);
        EXPECT_EQ(events->size(), 1u);
        EXPECT_FALSE(events->contains("rejected"));
    }

    // ============================================================================
    // 事件转发
    // ============================================================================

    TEST(AsyncSink, WriteForwardsEventToWrappedSink)
    {
        auto      events     = std::make_shared<RecordedEvents>();
        auto      downstream = std::make_unique<RecordingSink>(events);
        AsyncSink sink(std::move(downstream), 128);

        sink.write(makeEvent(LogLevel::Warn, "async_forwarded"));
        sink.flush();

        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(events->levelAt(0), LogLevel::Warn);
        EXPECT_TRUE(events->contains("async_forwarded"));
    }

    TEST(AsyncSink, FlushBlocksUntilQueueDrainedAndDownstreamFlushed)
    {
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events);
        AsyncSink     sink(std::move(downstream), 8);
        constexpr int keventCount = 50;

        for (int index = 0; index < keventCount; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "queued_" + std::to_string(index)));
        }
        sink.flush();

        // flush() 返回即代表队列已排空且下游 flush 已转发
        EXPECT_EQ(events->size(), static_cast<std::size_t>(keventCount));
        EXPECT_GE(events->flushCount.load(), 1u);
        EXPECT_TRUE(events->contains("queued_49"));
    }

    TEST(AsyncSink, StopFlushesRemainingEventsToDownstream)
    {
        auto      events     = std::make_shared<RecordedEvents>();
        auto      downstream = std::make_unique<RecordingSink>(events);
        AsyncSink sink(std::move(downstream), 1024);

        for (int index = 0; index < 10; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "before_stop_" + std::to_string(index)));
        }
        sink.stop();

        EXPECT_EQ(events->size(), 10u);
        EXPECT_GE(events->flushCount.load(), 1u);
        EXPECT_TRUE(events->contains("before_stop_9"));
    }

    TEST(AsyncSink, ConcurrentWritesAreAllForwarded)
    {
        auto                     events     = std::make_shared<RecordedEvents>();
        auto                     downstream = std::make_unique<RecordingSink>(events);
        AsyncSink                sink(std::move(downstream), 64);
        constexpr int            kthreadCount     = 4;
        constexpr int            kwritesPerThread = 50;
        std::vector<std::thread> threads;

        threads.reserve(kthreadCount);
        for (int index = 0; index < kthreadCount; ++index)
        {
            threads.emplace_back([&sink, index]
            {
                for (int inner = 0; inner < kwritesPerThread; ++inner)
                {
                    sink.write(makeEvent(LogLevel::Info,
                                         "async_worker" + std::to_string(index) + "_" + std::to_string(inner)));
                }
            });
        }
        for (std::thread &thread: threads)
        {
            thread.join();
        }
        sink.flush();

        EXPECT_EQ(events->size(), static_cast<std::size_t>(kthreadCount) * kwritesPerThread);
        EXPECT_EQ(sink.droppedEventCount(), 0u);
        EXPECT_TRUE(events->contains("async_worker3_49"));
    }

    // ============================================================================
    // 溢出策略：Block
    // ============================================================================

    TEST(AsyncSink, BlockPolicyNeverDropsEvents)
    {
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events);
        AsyncSink     sink(std::move(downstream), 8, AsyncSink::OverflowPolicy::Block);
        constexpr int keventCount = 100;

        for (int index = 0; index < keventCount; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "block_" + std::to_string(index)));
        }
        sink.flush();

        EXPECT_EQ(sink.droppedEventCount(), 0u);
        EXPECT_EQ(events->size(), static_cast<std::size_t>(keventCount));
    }

    TEST(AsyncSink, BlockPolicyWaitsForSlowDownstreamInsteadOfDropping)
    {
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events, std::chrono::microseconds(200));
        AsyncSink     sink(std::move(downstream), 2, AsyncSink::OverflowPolicy::Block);
        constexpr int keventCount = 20;

        const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
        for (int index = 0; index < keventCount; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "slow_" + std::to_string(index)));
        }
        sink.flush();
        const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();

        // 队列容量 2 远小于 20 条事件，未丢事件即说明写入线程被阻塞等待空间
        EXPECT_EQ(sink.droppedEventCount(), 0u);
        EXPECT_EQ(events->size(), static_cast<std::size_t>(keventCount));
        EXPECT_GE(elapsedMilliseconds, 3) << "队列容量 2 时 20 条慢事件应至少耗时 4 毫秒";
    }

    // ============================================================================
    // 溢出策略：Drop / DropOldest
    // ============================================================================

    TEST_F(AsyncSinkWithBlockedDownstream, DropPolicyCountsDroppedEvents)
    {
        startAsyncSink(4, AsyncSink::OverflowPolicy::Drop);
        constexpr int keventCount = 200;

        for (int index = 0; index < keventCount; ++index)
        {
            m_async->write(makeEvent(LogLevel::Info, "drop_" + std::to_string(index)));
        }

        const std::uint64_t droppedWhileBlocked = m_async->droppedEventCount();
        EXPECT_GT(droppedWhileBlocked, 0u);

        m_downstream->release();
        m_async->stop();

        EXPECT_LT(m_events->size(), static_cast<std::size_t>(keventCount));
        EXPECT_EQ(m_async->droppedEventCount(), droppedWhileBlocked) << "放行后不应再产生新的丢弃";
        // 每条入队事件要么被转发要么被计数丢弃
        EXPECT_EQ(m_events->size() + droppedWhileBlocked, static_cast<std::size_t>(keventCount));
    }

    TEST_F(AsyncSinkWithBlockedDownstream, DropOldestPolicyKeepsLatestEvents)
    {
        constexpr std::size_t kqueueCapacity = 4;
        constexpr int         keventCount    = 100;
        startAsyncSink(kqueueCapacity, AsyncSink::OverflowPolicy::DropOldest);

        const std::string newestToken   = std::format("oldest_{:03d}", keventCount - 1);
        const std::string midEarlyToken = std::format("oldest_{:03d}", keventCount / 2);

        for (int index = 0; index < keventCount; ++index)
        {
            m_async->write(makeEvent(LogLevel::Info, std::format("oldest_{:03d}", index)));
        }

        // 队列容量 4 加上后台线程手上 1 条，最多只有 5 条事件能存活
        EXPECT_GE(m_async->droppedEventCount(), static_cast<std::uint64_t>(keventCount) - (kqueueCapacity + 1));

        m_downstream->release();
        m_async->stop();

        EXPECT_TRUE(m_events->contains(newestToken)) << "DropOldest 必须保留最新事件";
        EXPECT_FALSE(m_events->contains(midEarlyToken)) << "DropOldest 应丢弃较早的事件";
    }

    TEST_F(AsyncSinkWithBlockedDownstream, FlushWaitsUntilBlockedDownstreamDrains)
    {
        constexpr int keventCount = 10;
        startAsyncSink(32, AsyncSink::OverflowPolicy::Block);

        for (int index = 0; index < keventCount; ++index)
        {
            m_async->write(makeEvent(LogLevel::Info, "flush_gate_" + std::to_string(index)));
        }

        std::atomic<bool> flushReturned{false};
        std::thread       flusher([this, &flushReturned]
        {
            m_async->flush();
            flushReturned.store(true);
        });

        // 下游仍被卡住，flush() 不应返回
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_FALSE(flushReturned.load());

        m_downstream->release();
        const bool completed = TestSupport::waitForCondition(
                [&flushReturned]
                {
                    return flushReturned.load();
                },
                kWaitTimeoutMilliseconds);
        flusher.join();

        EXPECT_TRUE(completed) << "放行后 flush() 未在超时内返回";
        EXPECT_EQ(m_events->size(), static_cast<std::size_t>(keventCount));
    }
} // namespace AsynGyanis::Base
