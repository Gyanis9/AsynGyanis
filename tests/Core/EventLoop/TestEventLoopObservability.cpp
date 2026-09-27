// 事件循环自观测：观测表的登记与摘除、隔着线程读到的相位与计数、被处理器占住一轮时的高水位与告警

#include "Core/EventLoop/EventLoop.h"
#include "Core/Coroutine/Scheduler.h"

#include "Base/Log/LogEvent.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::EventLoopThread;
        using TestSupport::kWaitTimeout;
        using TestSupport::waitForCondition;

        /// 用例里制造的那条「被占住的工作段」时长：阈值 100 ms 的三倍，慢机器上只会更长不会更短
        constexpr std::chrono::milliseconds kBlockedWorkingSegment{300};

        /**
         * @brief 与 Sink 共享的消息表
         * @details 表与锁放在一起：用例侧要轮询「告警报了没有」，而写侧是循环线程，
         *          取副本必须在锁内——直接把 vector 交给用例读就是与 push_back 撞同一个对象
         */
        struct MessageTable
        {
            mutable std::mutex       mutex;    ///< 保护下面那张表
            std::vector<std::string> messages; ///< 已收到的日志原文
        };

        /**
         * @brief 把 root 日志器收到的消息原文收进共享表，用于断言「这条告警到底报了没有」
         */
        class RecordingSink final : public Base::LogSink
        {
        public:
            /**
             * @brief 绑定共享表
             * @param table 用例创建并持有的表，Sink 只借它写字
             */
            explicit RecordingSink(std::shared_ptr<MessageTable> table) : m_table(std::move(table))
            {
            }

            /// @brief 记下一条消息的原文（本用例只关心 message 字段）
            void write(const Base::LogEvent &event) override
            {
                const std::lock_guard lock(m_table->mutex);
                m_table->messages.push_back(event.message);
            }

            /// 不落盘，没有缓冲需要刷新
            void flush() override
            {
            }

        private:
            std::shared_ptr<MessageTable> m_table; ///< 与用例共享的消息表
        };

        /// 锁内取一份消息副本
        [[nodiscard]] std::vector<std::string> messagesOf(const MessageTable &table)
        {
            const std::lock_guard lock(table.mutex);
            return table.messages;
        }

        /**
         * @brief 作用域结束时换掉整棵 root 日志器，摘掉本用例挂上去的 Sink
         * @details Logger 只有 clearSinks() 而没有「摘掉单个 Sink」的口，沿用仓库既有的「整份换掉 root」做法
         */
        class RootSinkScope
        {
        public:
            RootSinkScope()                                 = default;
            RootSinkScope(const RootSinkScope &)            = delete;
            RootSinkScope &operator=(const RootSinkScope &) = delete;

            ~RootSinkScope()
            {
                Base::LoggerRegistry::instance().clear();
            }
        };

        /**
         * @brief 在整表里找那条由给定线程跑的循环
         * @param ownerThread 线程标识
         * @return std::optional<EventLoopSnapshot> 找到则返回其快照；表里没有这条线程则为空
         */
        [[nodiscard]] std::optional<EventLoopSnapshot> snapshotOwnedBy(const std::thread::id ownerThread)
        {
            for (const auto &entry: eventLoopSnapshots())
            {
                if (entry.snapshot.ownerThread == ownerThread)
                {
                    return entry.snapshot;
                }
            }
            return std::nullopt;
        }

        /// 把一条空可调用体投给循环，并等这一轮真的跑完（工作段计数落下）
        void runOneRound(EventLoopThread &driver)
        {
            const auto        segmentsBefore = driver.loop().snapshot().completedWorkingSegments;
            std::atomic<bool> ran{false};
            driver.loop().scheduler().postRemote([&ran] { ran.store(true); });
            // 等到计数落下而不是等到 ran：计数是在工作段收尾时才写的，早于它就只能断言「代码跑过了」
            ASSERT_TRUE(waitForCondition([&driver, segmentsBefore] { return driver.loop().snapshot().completedWorkingSegments > segmentsBefore; }))
                    << "投递的活儿没能让工作段计数加一";
            EXPECT_TRUE(ran.load());
        }
    } // namespace

    /**
     * @brief 钉住：循环活着就占一个观测槽位，销毁即归还；未登记的差额恒为零
     * @note 断的是「登记表与循环的生死同步」。只增不减的话，跑久了的进程会报出一堆早就不存在的循环
     */
    TEST(EventLoopObservability, TableRegistersEachLoopAndDropsItOnDestruction)
    {
        const std::size_t baselineCount = eventLoopSnapshots().size();
        {
            EventLoop loop;
            EXPECT_EQ(eventLoopSnapshots().size(), baselineCount + 1);
        }
        EXPECT_EQ(eventLoopSnapshots().size(), baselineCount);
        EXPECT_EQ(unregisteredEventLoopCount(), 0);
    }

    /**
     * @brief 钉住：没跑起来的循环也认账——门口堆了几件外来投递要能隔着线程读到
     * @details 这条用例刻意不起线程：真停住的循环没法应答，读数因此必须来自原子量而不是「投进去问一句」
     */
    TEST(EventLoopObservability, NotRunningLoopStillCountsWorkThrownAtIt)
    {
        EventLoop loop;
        loop.scheduler().postRemote([] {});
        loop.scheduler().postRemote([] {});

        const EventLoopSnapshot snapshot = loop.snapshot();
        EXPECT_FALSE(snapshot.isRunning);
        EXPECT_EQ(snapshot.phase, LoopPhase::NotStarted);
        EXPECT_EQ(snapshot.remotePendingCount, 2U);
        EXPECT_EQ(snapshot.completedWorkingSegments, 0U);
    }

    /**
     * @brief 钉住：跑起来的循环报出自己那条线程，投给它一件活儿就多一条工作段
     */
    TEST(EventLoopObservability, RunningLoopRecordsItsThreadAndCountsWorkingSegments)
    {
        EventLoopThread driver;
        ASSERT_TRUE(driver.waitUntilRunning());

        const auto snapshotBefore = snapshotOwnedBy(driver.threadId());
        ASSERT_TRUE(snapshotBefore.has_value()) << "整表里没有后台线程跑着的那条循环";
        EXPECT_TRUE(snapshotBefore->isRunning);
        // 线程标识是 run() 进来时写的，未启动过的循环读到的是默认值，因此这一条能分出「有没有落戳」
        EXPECT_EQ(snapshotBefore->ownerThread, driver.threadId());

        const auto segmentsBefore = snapshotBefore->completedWorkingSegments;
        runOneRound(driver);

        const auto snapshotAfter = snapshotOwnedBy(driver.threadId());
        ASSERT_TRUE(snapshotAfter.has_value());
        EXPECT_GT(snapshotAfter->completedWorkingSegments, segmentsBefore);
        EXPECT_NE(snapshotAfter->phase, LoopPhase::NotStarted);
    }

    /**
     * @brief 钉住：等事件的空闲不算工作段，相位要落在 WaitingForEvents
     * @details 「停顿」与「空闲」的分别是这一层最要紧的口径：把等待也算进工作段，空闲服务就会一路告警
     */
    TEST(EventLoopObservability, IdleLoopSitsInTheWaitingPhase)
    {
        EventLoopThread driver;
        ASSERT_TRUE(driver.waitUntilRunning());
        runOneRound(driver);

        // 有界轮询等它进等待相：这条循环此后没活儿，进相之后就一直停在那儿
        ASSERT_TRUE(waitForCondition([&driver] { return driver.loop().snapshot().phase == LoopPhase::WaitingForEvents; })) << "跑完一轮的循环没有回到等待相";
        EXPECT_TRUE(driver.loop().snapshot().isRunning);
    }

    /**
     * @brief 钉住：一条工作段被占住三百毫秒，高水位要记下这段时长，并落一条超阈值的 ERROR
     * @note 两条断言各钉一头：只钉日志会放过「记了日志但高水位恒为 0」，只钉高水位会放过「数算了但从不告状」
     */
    TEST(EventLoopObservability, BlockedWorkingSegmentRaisesHighWaterAndLogsAnAlert)
    {
        RootSinkScope sinkScope;
        // 表由用例持有：Sink 交给日志器之后就再也拿不到它的指针，而收尾换掉 root 之后表仍要在
        const auto messages = std::make_shared<MessageTable>();
        Base::LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<RecordingSink>(messages));

        EventLoopThread driver;
        ASSERT_TRUE(driver.waitUntilRunning());

        const auto segmentsBefore = driver.loop().snapshot().completedWorkingSegments;
        driver.loop().scheduler().postRemote([] { std::this_thread::sleep_for(kBlockedWorkingSegment); });
        ASSERT_TRUE(waitForCondition([&driver, segmentsBefore] { return driver.loop().snapshot().completedWorkingSegments > segmentsBefore; }))
                << "睡了一觉的活儿没能让工作段计数加一";

        const EventLoopSnapshot snapshot = driver.loop().snapshot();
        EXPECT_GE(snapshot.slowestWorkingSegment, kBlockedWorkingSegment) << "最慢工作段的高水位没记下这一段";

        // 告警与高水位写在同一处，但不保证用例这边已经读到：按有界轮询等它出现
        const auto alertArrived = waitForCondition(
                [messages]
                {
                    const std::vector<std::string> snapshotOfMessages = messagesOf(*messages);
                    return std::ranges::any_of(snapshotOfMessages, [](const std::string &message) { return message.find("工作段耗时") != std::string::npos; });
                });
        ASSERT_TRUE(alertArrived) << "超阈值的工作段没有落 ERROR";
    }

} // namespace AsynGyanis::Core
