// LogEvent 单元测试：构造与值语义、事件时刻的存取、线程号缓存及压力循环
// 时间戳文本的渲染规则已随「事件只带时刻」上收到格式化器一侧，由 TestTimestampText 直测

#include "Base/Log/LogEvent.h"

#include "BaseTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 固定的事件时刻：本地挂钟 2026-09-10 12:34:56.789（正午，避开夏令时的空档与重合）
        const TimestampMoment kFixedMoment = TestSupport::makeLocalMoment(2026, 9, 10, 12, 34, 56, 789);

        /** @brief 构造一条字段齐全、便于断言的日志事件 */
        LogEvent makeEventWithMessage(const std::string &message)
        {
            return {
                    LogLevel::Warn, kFixedMoment, "12345",
                    SourceLocation("TestLogEvent.cpp", 100, "makeEventWithMessage"), "eventLogger", message
            };
        }
    } // namespace

    TEST(LogEvent, DefaultConstructionZeroInitializesEveryField)
    {
        const LogEvent event;

        EXPECT_EQ(event.level, LogLevel::Trace);
        // 默认构造的时刻是「epoch 起算 0」而非某个真实挂钟：默认成员初始化把时刻归零，
        // 与其余字段的「空/零」口径一致，不留下一个看着像真时间的默认值
        EXPECT_EQ(event.timestamp.time_since_epoch().count(), 0);
        EXPECT_TRUE(event.threadIdView().empty());
        EXPECT_TRUE(event.loggerNameView().empty());
        EXPECT_TRUE(event.message.empty());
        EXPECT_EQ(event.location.fileName, nullptr);
        EXPECT_EQ(event.location.line, 0);
        EXPECT_EQ(event.location.functionName, nullptr);
    }

    TEST(LogEvent, FullConstructorStoresEveryField)
    {
        constexpr SourceLocation location("App.cpp", 77, "runApplication");
        const TimestampMoment    moment = TestSupport::makeLocalMoment(2026, 9, 10, 8, 9, 10, 11);

        const LogEvent event(LogLevel::Error, moment, "9527", location, "appLogger", "disk full");

        EXPECT_EQ(event.level, LogLevel::Error);
        EXPECT_EQ(event.timestamp, moment);
        EXPECT_EQ(event.threadIdView(), "9527");
        EXPECT_EQ(event.loggerNameView(), "appLogger");
        EXPECT_EQ(event.message, "disk full");
        EXPECT_STREQ(event.location.fileName, "App.cpp");
        EXPECT_EQ(event.location.line, 77);
        EXPECT_STREQ(event.location.functionName, "runApplication");
    }

    TEST(LogEvent, FullConstructorAcceptsEmptyStrings)
    {
        const LogEvent event(LogLevel::Off, TimestampMoment{}, "", SourceLocation(), "", "");

        EXPECT_EQ(event.level, LogLevel::Off);
        EXPECT_EQ(event.timestamp.time_since_epoch().count(), 0);
        EXPECT_TRUE(event.threadIdView().empty());
        EXPECT_TRUE(event.loggerNameView().empty());
        EXPECT_TRUE(event.message.empty());
        EXPECT_EQ(event.location.fileName, nullptr);
    }

    TEST(LogEvent, ValueSemanticsTraitsHold)
    {
        static_assert(std::is_default_constructible_v<LogEvent>);
        static_assert(std::is_copy_constructible_v<LogEvent>);
        static_assert(std::is_copy_assignable_v<LogEvent>);
        static_assert(std::is_move_constructible_v<LogEvent>);
        static_assert(std::is_move_assignable_v<LogEvent>);
        static_assert(!std::is_trivially_copyable_v<LogEvent>);

        EXPECT_TRUE(std::is_class_v<LogEvent>);
    }

    TEST(LogEvent, CopyKeepsOriginalEventIntact)
    {
        const LogEvent original = makeEventWithMessage("copied message");

        const LogEvent &copied(original);

        EXPECT_EQ(copied.level, original.level);
        EXPECT_EQ(copied.timestamp, original.timestamp);
        EXPECT_EQ(copied.threadId, original.threadId);
        EXPECT_EQ(copied.loggerNameView(), original.loggerNameView());
        EXPECT_EQ(copied.message, "copied message");
        EXPECT_EQ(copied.location.line, original.location.line);
        EXPECT_STREQ(copied.location.fileName, original.location.fileName);

        EXPECT_EQ(original.message, "copied message");

        LogEvent assigned = makeEventWithMessage("placeholder");
        assigned          = original;
        EXPECT_EQ(assigned.message, "copied message");
        EXPECT_EQ(assigned.level, LogLevel::Warn);
    }

    TEST(LogEvent, MoveTransfersPayloadAndLeavesSourceValid)
    {
        LogEvent original = makeEventWithMessage("moved message");

        const LogEvent moved(std::move(original));

        EXPECT_EQ(moved.level, LogLevel::Warn);
        EXPECT_EQ(moved.message, "moved message");
        EXPECT_EQ(moved.loggerNameView(), "eventLogger");
        EXPECT_EQ(moved.timestamp, kFixedMoment);
        EXPECT_EQ(moved.threadIdView(), "12345");

        EXPECT_NO_THROW(original.message.clear());
        EXPECT_TRUE(original.message.empty());
        EXPECT_EQ(original.level, LogLevel::Warn);

        LogEvent assigned   = makeEventWithMessage("placeholder");
        LogEvent moveSource = makeEventWithMessage("reassigned message");
        assigned            = std::move(moveSource);
        EXPECT_EQ(assigned.message, "reassigned message");
        EXPECT_EQ(assigned.level, LogLevel::Warn);
    }

    TEST(LogEvent, ThreadIdStringIsNonEmptyAndCachedPerThread)
    {
        const std::shared_ptr<const std::string> &firstCall = threadIdString();

        ASSERT_FALSE(firstCall->empty());
        EXPECT_TRUE(firstCall->size() <= std::string::size_type(64));

        const std::shared_ptr<const std::string> &secondCall = threadIdString();
        EXPECT_TRUE(&firstCall == &secondCall) << "同一线程内应当复用同一份快照";
        EXPECT_EQ(*firstCall, *secondCall);
    }

    TEST(LogEvent, ThreadIdStringDiffersBetweenThreads)
    {
        constexpr std::size_t kworkerCount = 4;

        const std::shared_ptr<const std::string> &mainThreadId = threadIdString();
        std::vector<std::string>                  workerIds(kworkerCount);
        std::vector<std::thread> workers;

        workers.reserve(kworkerCount);
        for (std::size_t index = 0; index < kworkerCount; ++index)
        {
            workers.emplace_back([&workerIds, index]()
            {
                workerIds[index] = *threadIdString();
            });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        for (std::size_t index = 0; index < kworkerCount; ++index)
        {
            EXPECT_FALSE(workerIds[index].empty()) << "worker " << index;
            EXPECT_NE(workerIds[index], *mainThreadId) << "worker " << index;
            for (std::size_t otherIndex = 0; otherIndex < index; ++otherIndex)
            {
                EXPECT_NE(workerIds[index], workerIds[otherIndex]);
            }
        }
    }

    TEST(LogEvent, EventAssembledFromHelpersMatchesCallerContext)
    {
        const SourceLocation location = SourceLocation::current();
        const TimestampMoment moment   = std::chrono::system_clock::now();
        const LogEvent       event(LogLevel::Info, moment, threadIdString(), location, std::make_shared<const std::string>("root"), "assembled message");

        EXPECT_EQ(event.timestamp, moment) << "事件带出的时刻必须就是构造时那个，不在中途改取";
        EXPECT_EQ(event.threadId.get(), threadIdString().get()) << "事件应当直接共享本线程的 ID 快照，而不是另分配一份";
        EXPECT_STREQ(event.location.functionName, location.functionName);
        EXPECT_EQ(event.location.line, location.line);
    }

    TEST(LogEvent, ThreadIdStringSurvivesStressLoop)
    {
        constexpr int kiterationCount = 500;

        int failureCount = 0;
        for (int iteration = 0; iteration < kiterationCount; ++iteration)
        {
            if (threadIdString()->empty())
            {
                ++failureCount;
            }
        }

        EXPECT_EQ(failureCount, 0);
        EXPECT_FALSE(threadIdString()->empty());
    }
} // namespace AsynGyanis::Base
