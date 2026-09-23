// AsyncSink 单元测试：后台线程转发、flush/stop 语义与三种队列溢出策略

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
#include <stdexcept>
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
             */
            void append(const LogEvent &event)
            {
                std::lock_guard lock(mutex);
                messages.push_back(event.message);
                levels.push_back(event.level);
            }

            /**
             * @brief 获取消息快照
             */
            [[nodiscard]] std::vector<std::string> snapshot() const
            {
                std::lock_guard lock(mutex);
                return messages;
            }

            /**
             * @brief 已记录的消息条数
             */
            [[nodiscard]] std::size_t size() const
            {
                std::lock_guard lock(mutex);
                return messages.size();
            }

            /**
             * @brief 判断是否记录了给定消息
             */
            [[nodiscard]] bool contains(const std::string &message) const
            {
                std::lock_guard lock(mutex);
                return std::find(messages.begin(), messages.end(), message) != messages.end();
            }

            /**
             * @brief 获取指定位置记录的等级
             * @return 下标越界时返回 LogLevel::Off
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
         * @brief 每次 write 都抛出的桩 Sink，用于制造「worker 落地失败」的场景
         */
        class ThrowingSink final : public LogSink
        {
        public:
            /**
             * @brief 抛出异常，模拟下游写入失败
             * @details 重写 LogSink::write()：滚动时反复打不开文件走的就是这条抛出路径，异步包装
             *          必须把这一条当作「没能落地」处理，而不是让 worker 线程跟着倒下。
             */
            void write(const LogEvent &) override
            {
                throw std::runtime_error("桩：下游写入必然失败");
            }

            /**
             * @brief 桩无缓冲区，刷新为空操作
             */
            void flush() override
            {
            }
        };

        /**
         * @brief 首条事件会阻塞在 write() 内的桩 Sink，用于制造「队列被填满」的场景
         */
        class BlockingRecordingSink final : public LogSink
        {
        public:
            /**
             * @brief 使用共享记录构造阻塞桩 Sink
             */
            explicit BlockingRecordingSink(std::shared_ptr<RecordedEvents> events) :
                m_events(std::move(events))
            {
            }

            /**
             * @brief 阻塞直到 release() 后记录事件
             * @details 重写 LogSink::write()：累加进入计数后等待放行条件，被放行后像普通桩一样记录，
             *          从而在测试期间稳定地占住后台线程、让队列保持满状态。
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
         * @brief 刷新会占住一段时限的桩 Sink：write 立即返回，只有 flush 慢
         * @details 用来把「下游刷新正在跑」造成为一个可观测、确定会过去的窗口，
         *          从而单测上游在这段窗口里还能不能写进队列
         */
        class SlowFlushSink final : public LogSink
        {
        public:
            /**
             * @brief 使用共享记录构造慢刷新桩 Sink
             * @param events 共享记录
             * @param flushHold 每次 flush 占住的时长
             */
            explicit SlowFlushSink(std::shared_ptr<RecordedEvents> events,
                                   const std::chrono::milliseconds flushHold = std::chrono::milliseconds(500)) :
                m_events(std::move(events))
                , m_flushHold(flushHold)
            {
            }

            /**
             * @brief 立即记录事件
             * @details 重写 LogSink::write()：刻意不占时间，本用例要量的只有刷新那一段。
             */
            void write(const LogEvent &event) override
            {
                m_events->append(event);
            }

            /**
             * @brief 占住 m_flushHold 那么久，期间可被 release() 提前放行
             * @details 重写 LogSink::flush()：先置「已进入」标记再等放行。到点自动结束而不是
             *          死等，这样即使上游真的握着锁、本用例也只是读数变红而不是挂住不放。
             */
            void flush() override
            {
                m_entered.store(true, std::memory_order_release);
                {
                    std::unique_lock lock(m_gateMutex);
                    m_gateCondition.wait_for(lock, m_flushHold, [this]
                    {
                        return m_released;
                    });
                }
                m_events->flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief 放行正在占位的刷新
             */
            void release()
            {
                {
                    std::lock_guard lock(m_gateMutex);
                    m_released = true;
                }
                m_gateCondition.notify_all();
            }

            /**
             * @brief 有界等待刷新进入占位窗口
             * @param timeout 等待上限
             * @return true 已进入
             */
            [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (m_entered.load(std::memory_order_acquire))
                    {
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return m_entered.load(std::memory_order_acquire);
            }

        private:
            std::shared_ptr<RecordedEvents> m_events;         ///< 共享记录
            std::chrono::milliseconds       m_flushHold;      ///< 单次刷新的占位时长
            std::mutex                      m_gateMutex;      ///< 放行条件互斥锁
            std::condition_variable         m_gateCondition;  ///< 放行条件变量（只挂「已放行」一个谓词）
            bool                            m_released = false; ///< 是否已放行
            std::atomic<bool>               m_entered{false};   ///< 是否已进入刷新占位窗口
        };

        /**
         * @brief 构造字段齐备、各字段取值固定的日志事件
         */
        LogEvent makeEvent(const LogLevel level, std::string message = "async message")
        {
            return {
                    level, TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789), "tid-556677",
                    SourceLocation("async_fixture.cpp", 3456, "asyncTestFunction"),
                    "async_logger", std::move(message)
            };
        }

        /**
         * @brief 轮询等待条件的超时上限（毫秒），给慢速机器留足余量
         */
        constexpr int kWaitTimeoutMilliseconds = 10000;

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

    /**
     * @brief 按基类引用交出右值事件时，走的必须是接管那条并且内容不丢
     * @details Logger 手里拿的是 LogSink 基类指针，虚派发是否落到 write(LogEvent &&)
     *          只有从基类调用才看得出来；从基类走通了才算「一条日志不再复制事件」这条改动生效。
     */
    TEST(AsyncSink, RvalueEventViaBaseReferenceKeepsItsContent)
    {
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events);
        AsyncSink     sink(std::move(downstream), 16);
        LogSink      &asBaseSink = sink;

        asBaseSink.write(makeEvent(LogLevel::Error, "handed over by rvalue"));
        sink.flush();

        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(events->snapshot().front(), "handed over by rvalue") << "移进队列时把消息丢了";
        EXPECT_EQ(events->levelAt(0), LogLevel::Error);
    }

    /**
     * @brief 左值事件走复制那条路，内容同样必须完整到达
     * @details 与上一条配对：接管那条省下的只是拷贝，两条出口的落地结果必须一模一样，
     *          否则「省一次分配」就是靠少带数据换来的。
     */
    TEST(AsyncSink, LvalueEventIsCopiedIntoTheQueueWithoutLosingContent)
    {
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events);
        AsyncSink     sink(std::move(downstream), 16);
        const LogEvent event = makeEvent(LogLevel::Warn, "kept as lvalue");

        sink.write(event);
        sink.flush();

        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(events->snapshot().front(), "kept as lvalue");
        EXPECT_EQ(sink.droppedEventCount(), 0u);
    }

    TEST(AsyncSink, MinimumQueueSizeIsOne)
    {
        EXPECT_EQ(AsyncSink::kMinimumQueueSize, 1u);
    }

    TEST(AsyncSink, ZeroQueueSizeIsClampedToMinimumForBlockPolicy)
    {
        // 容量 0 会让 Block 策略的等待谓词「size() < 0」恒不成立，每次写入都要等满超时后按丢弃处理，
        // 事件无法全部落地；钳到 1 之后容量虽小，事件仍必须全部落地
        auto          events     = std::make_shared<RecordedEvents>();
        auto          downstream = std::make_unique<RecordingSink>(events);
        AsyncSink     sink(std::move(downstream), 0, AsyncSink::OverflowPolicy::Block);
        constexpr int keventCount = 5;

        for (int index = 0; index < keventCount; ++index)
        {
            sink.write(makeEvent(LogLevel::Info, "zero_queue_" + std::to_string(index)));
        }
        sink.flush();

        EXPECT_EQ(sink.droppedEventCount(), 0u);
        EXPECT_EQ(events->size(), static_cast<std::size_t>(keventCount));
        EXPECT_TRUE(events->contains("zero_queue_4"));
    }

    TEST(AsyncSink, ZeroQueueSizeIsClampedToMinimumForDiscardingPolicies)
    {
        // Drop / DropOldest 不等待消费：容量 0 会让丢弃路径在空队列上取值、使待落地计数回绕，
        // 故钳到 1；钳后不变量为「每条入队事件要么被转发、要么被计数丢弃」
        const std::vector<AsyncSink::OverflowPolicy> policies{
                AsyncSink::OverflowPolicy::Drop,
                AsyncSink::OverflowPolicy::DropOldest,
        };
        constexpr int keventCount = 20;

        for (const AsyncSink::OverflowPolicy policy: policies)
        {
            auto      events     = std::make_shared<RecordedEvents>();
            auto      downstream = std::make_unique<RecordingSink>(events);
            AsyncSink sink(std::move(downstream), 0, policy);

            for (int index = 0; index < keventCount; ++index)
            {
                sink.write(makeEvent(LogLevel::Info, "zero_discard_" + std::to_string(index)));
            }
            sink.flush();

            EXPECT_EQ(events->size() + sink.droppedEventCount(), static_cast<std::size_t>(keventCount))
                    << "policy " << static_cast<int>(policy);
        }
    }

    TEST(AsyncSink, BlockPolicyCountsEventsDroppedAfterStop)
    {
        auto      events     = std::make_shared<RecordedEvents>();
        auto      downstream = std::make_unique<RecordingSink>(events);
        AsyncSink sink(std::move(downstream), 8, AsyncSink::OverflowPolicy::Block);

        sink.write(makeEvent(LogLevel::Info, "accepted_before_stop"));
        sink.stop();
        EXPECT_EQ(sink.droppedEventCount(), 0u);

        sink.write(makeEvent(LogLevel::Info, "dropped_after_stop_one"));
        sink.write(makeEvent(LogLevel::Info, "dropped_after_stop_two"));

        // 停止后事件既不入队也不落地，必须计入丢弃数，否则监控会少报丢失规模
        EXPECT_EQ(sink.droppedEventCount(), 2u);
        EXPECT_FALSE(events->contains("dropped_after_stop_one"));
        EXPECT_FALSE(events->contains("dropped_after_stop_two"));
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

    /**
     * @brief 被包装 sink 自己的 level 过滤不能被旁路
     * @details LogSink 的契约是「过滤由调用方用 shouldLog() 预筛」，而被包装的 sink 永远不是
     *          Logger 的直接子节点——Logger 只按挂在它下面的 AsyncSink 预筛。少了这一步，
     *          配置里写成 `wrapped: {type: file, level: ERROR}` 的那个 sink 会收到 DEBUG/INFO 全量
     */
    TEST(AsyncSink, RespectsWrappedSinkLevelFilter)
    {
        auto downstream = std::make_unique<RecordingSink>(std::make_shared<RecordedEvents>());
        auto events     = downstream->events();
        downstream->setLevel(LogLevel::Error);

        AsyncSink sink(std::move(downstream), 128);

        sink.write(makeEvent(LogLevel::Info, "below_wrapped_level"));
        sink.write(makeEvent(LogLevel::Error, "at_wrapped_level"));
        sink.flush();

        EXPECT_FALSE(events->contains("below_wrapped_level")) << "被包装 sink 的 level 被旁路了";
        EXPECT_TRUE(events->contains("at_wrapped_level")) << "达到等级的事件被误挡";
        EXPECT_EQ(events->size(), 1u);
        EXPECT_EQ(sink.droppedEventCount(), 1u) << "被下游等级挡下的事件应计入丢弃";
    }

    TEST(AsyncSink, EventsLostToAThrowingDownstreamAreCountedAsDropped)
    {
        AsyncSink sink(std::make_unique<ThrowingSink>(), 8);

        sink.write(makeEvent(LogLevel::Info, "lost-one"));
        sink.write(makeEvent(LogLevel::Info, "lost-two"));
        // 下游每次都抛：worker 必须把它咽下来继续跑（jthread 入口的未捕获异常会直接 terminate 进程），
        // 且待落地账要照清——否则 flush() 会等一条永远不会被核销的账
        sink.flush();

        EXPECT_EQ(sink.droppedEventCount(), 2u)
                << "落地失败没计入丢弃数：异步路径上这条丢失连一行诊断都没有，计数是它唯一的出口";
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

        // 先写一条并等后台线程真正阻塞在下游里，再灌入其余事件。工作线程从启动到阻塞在下游
        // 需要一点时间，若不等就写入，先到的几条会被正常转发，丢弃数达不到「容量 + 1」的预期，
        // 高负载下就成了偶发失败（等待有明确上界，不会让用例挂住）
        m_async->write(makeEvent(LogLevel::Info, std::format("oldest_{:03d}", 0)));
        ASSERT_TRUE(TestSupport::waitForCondition(
                [this]
                {
                    return m_events->enteredCount.load(std::memory_order_acquire) >= 1U;
                },
                kWaitTimeoutMilliseconds));

        for (int index = 1; index < keventCount; ++index)
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

    TEST_F(AsyncSinkWithBlockedDownstream, BlockedProducerCountsItsEventAsDroppedWhenStopped)
    {
        // 场景：容量 1，worker 卡在下游；队列里已有 1 条，生产者写第 3 条时阻塞在等待空间上。
        // 此时停止必须唤醒它并把这条「等不到空间」的事件计入丢弃数
        startAsyncSink(1, AsyncSink::OverflowPolicy::Block);

        m_async->write(makeEvent(LogLevel::Info, "in_flight"));
        ASSERT_TRUE(TestSupport::waitForCondition(
                [this]
                {
                    return m_events->enteredCount.load(std::memory_order_acquire) >= 1U;
                },
                kWaitTimeoutMilliseconds));

        m_async->write(makeEvent(LogLevel::Info, "queued"));

        std::atomic<bool> producerReturned{false};
        std::thread blockedProducer([this, &producerReturned]
        {
            m_async->write(makeEvent(LogLevel::Info, "waits_for_space"));
            producerReturned.store(true, std::memory_order_release);
        });

        // worker 卡在下游时队列不会腾出空间，生产者不可能返回；
        // 这段等待只是让「生产者已进入等待」成为常态，即便未进入，后续断言同样成立
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(producerReturned.load(std::memory_order_acquire));

        // 停止会阻塞到 worker 退出，而 worker 正卡在下游，因此由独立线程发起停止
        std::thread stopper([this]
        {
            m_async->stop();
        });

        EXPECT_TRUE(TestSupport::waitForCondition(
                [this]
                {
                    return m_async->droppedEventCount() >= 1u;
                },
                kWaitTimeoutMilliseconds))
                << "停止时被唤醒的生产者事件未被计入丢弃数";

        m_downstream->release();
        stopper.join();
        blockedProducer.join();

        EXPECT_EQ(m_async->droppedEventCount(), 1u);
        EXPECT_TRUE(m_events->contains("in_flight"));
        EXPECT_TRUE(m_events->contains("queued"));
        EXPECT_FALSE(m_events->contains("waits_for_space")) << "等不到空间的事件不得落地";
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

    // ============================================================================
    // 停止协议：唤醒不得被丢弃（回归）
    // ============================================================================

    /**
     * @brief 反复「构造完立刻析构」必须次次完成：停止请求的唤醒一旦被丢弃就是永久挂死
     * @details 该形状正好把停止请求打进 worker 的首次等待窗口，是丢唤醒的最短路径。
     *          循环放在独立线程里、主线程有界等待——回归时用例是**失败**，而不是把测试进程
     *          挂到作业超时（120 分钟）才被发现。
     */
    TEST(AsyncSink, RepeatedConstructionAndDestructionAlwaysCompletes)
    {
        constexpr int     kIterationCount         = 2000;
        constexpr int     kCompletionMilliseconds = 30000;
        std::atomic<bool> finished{false};

        std::thread cycle([&finished]
        {
            for (int index = 0; index < kIterationCount; ++index)
            {
                const AsyncSink sink(std::make_unique<RecordingSink>());
            }
            finished.store(true, std::memory_order_release);
        });

        const bool completed = TestSupport::waitForCondition([&finished]
        {
            return finished.load(std::memory_order_acquire);
        }, kCompletionMilliseconds);

        if (completed)
        {
            cycle.join();
        } else
        {
            // 已卡在停止路径上（join 不会回来）：留着它等进程退出，让断言报失败而不是挂住进程
            cycle.detach();
        }
        EXPECT_TRUE(completed) << "构造/析构循环未在时限内完成：停止请求的唤醒可能被丢弃（worker 永久睡在条件变量上）";
    }

    /**
     * @brief 下游刷新期间不得握着队列锁，别的线程还要往里写
     * @details 钉的是锁的边界：flush() 转给下游的那一句可能是一次 FlushFileBuffers 或一次
     *          标准输出刷新，而生产者的 write() 取的是同一把队列锁——握着锁刷新就等于让全进程
     *          写日志的线程排在一块慢盘后面。判据取一对不可能同时误命中的时限：下游占位 500 ms，
     *          而这一次写入必须在 100 ms 内返回
     */
    TEST(AsyncSink, ProducerIsNotQueuedBehindTheDownstreamFlush)
    {
        auto events    = std::make_shared<RecordedEvents>();
        auto downstream = std::make_unique<SlowFlushSink>(events);
        auto *gate      = downstream.get();
        AsyncSink sink(std::move(downstream), 16U);

        // 先投一行并等它落地，让 flush 的等待谓词从一开始就成立、直奔下游那一句
        sink.write(makeEvent(LogLevel::Info, "drained_before_flush"));

        std::thread flusher([&sink]
        {
            sink.flush();
        });
        ASSERT_TRUE(gate->waitUntilEntered(std::chrono::seconds(5)))
                << "下游刷新从未开始，本用例没测到刷新那一段";

        const auto startedAt = std::chrono::steady_clock::now();
        sink.write(makeEvent(LogLevel::Info, "written_during_downstream_flush"));
        const auto producerWait = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        gate->release();
        flusher.join();

        EXPECT_LT(producerWait, std::chrono::milliseconds(100))
                << "实测等了 " << producerWait.count() << " ms：flush() 握着队列锁做下游刷新，"
                   "生产者被排在一次慢刷新后面";
    }
    namespace
    {
        /**
         * @brief 每条固定睡 200 微秒的下游：把「队列里排着多少条」折算成看得见的毫秒
         * @details 有意不带 flush() 的转发目标——本用例要证的只有队列侧的账，
         *          下游刷新是另一条用例的事。
         */
        class DelayedDownstream final : public LogSink
        {
        public:
            void write(const LogEvent &event) override
            {
                static_cast<void>(event);
                std::this_thread::sleep_for(std::chrono::microseconds{200});
            }
            void flush() override {}
        };
    } // namespace

    /**
     * @brief flush() 只等「本次进入时已受理的那批」，不等后来者
     * @details 等「待落地数清零」在生产者持续写入时永远不成立（实测同一形状下 1.9 秒仍未返回，
     *          而按容量 64 × 每条 200 微秒算只有约 13 毫秒的账要等）。水位判据把这条上限
     *          固定回本次调用自己该等的量。
     *          生产者每次写入之间留一拍：不留的话读数会混进队列锁的公平性，而不是本用例的说法。
     */
    TEST(AsyncSinkFlush, WaitsOnlyForEventsAcceptedBeforeTheCall)
    {
        constexpr std::size_t kQueueCapacity = 64U;
        AsyncSink sink(std::make_unique<DelayedDownstream>(), kQueueCapacity, AsyncSink::OverflowPolicy::Drop);

        // 生产者每分钟约 1 万条、下游每条约 200 微秒（5 千条/秒）：队列保持饱和，
        // 「待落地数清零」因此永远不成立。写入之间刻意留一拍，免得把结论下在锁的公平性上
        std::atomic<bool> producing{true};
        std::thread producer([&sink, &producing]
        {
            while (producing.load(std::memory_order_acquire))
            {
                sink.write(makeEvent(LogLevel::Info, "busy producer"));
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

        std::atomic<bool>         flushReturned{false};
        std::atomic<long long>    waitedMs{-1};
        std::thread               flusher([&]
        {
            const auto began = std::chrono::steady_clock::now();
            sink.flush();
            waitedMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(),
                           std::memory_order_relaxed);
            flushReturned.store(true, std::memory_order_release);
        });
        // 观察窗口 600 毫秒，远大于「容量 64 × 200 微秒 ≈ 13 毫秒」这个应有上限
        for (int tick = 0; tick < 60 && !flushReturned.load(std::memory_order_acquire); ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        const bool blockedWhileProducerRan = !flushReturned.load(std::memory_order_acquire);
        producing.store(false, std::memory_order_release);
        producer.join();
        flusher.join();

        EXPECT_FALSE(blockedWhileProducerRan)
                << "flush() 在持续生产者面前一直不返回：等的是「清零」，不是「进场时那批已落地」";
        EXPECT_GE(waitedMs.load(), 0);
        EXPECT_LT(waitedMs.load(), 100) << "实测等了 " << waitedMs.load() << " ms，而该等的只有容量 " << kQueueCapacity << " 条";
    }
} // namespace AsynGyanis::Base
