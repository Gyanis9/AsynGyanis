/**
 * @file TestLogEvent.cpp
 * @brief LogEvent 单元测试：构造与值语义、时间戳格式、线程号缓存及压力循环
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/LogEvent.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// currentTimestamp() 的固定输出长度："YYYY-MM-DD HH:MM:SS.mmm"
        constexpr std::size_t kTimestampLength = 23;

        /**
         * @brief 判断时间戳中除分隔符外的字符是否全部为数字
         * @param timestamp 待检查时间戳
         * @return true 全部为数字
         */
        bool onlyDigitsOutsideSeparators(const std::string &timestamp)
        {
            for (std::size_t index = 0; index < timestamp.size(); ++index)
            {
                if (const bool isSeparator = index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19)
                {
                    continue;
                }
                if (!std::isdigit(static_cast<unsigned char>(timestamp[index])))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 构造一条字段齐全、便于断言的日志事件
         * @param message 日志消息内容
         * @return LogEvent 构造好的事件
         */
        LogEvent makeEventWithMessage(const std::string &message)
        {
            return {
                    LogLevel::Warn, "2026-09-10 12:34:56.789", "12345",
                    SourceLocation("TestLogEvent.cpp", 100, "makeEventWithMessage"), "eventLogger", message
            };
        }
    } // namespace

    TEST(LogEvent, DefaultConstructionZeroInitializesEveryField)
    {
        const LogEvent event;

        EXPECT_EQ(event.level, LogLevel::Trace);
        EXPECT_TRUE(event.timestamp.empty());
        EXPECT_TRUE(event.threadId.empty());
        EXPECT_TRUE(event.loggerNameView().empty());
        EXPECT_TRUE(event.message.empty());
        EXPECT_EQ(event.location.fileName, nullptr);
        EXPECT_EQ(event.location.line, 0);
        EXPECT_EQ(event.location.functionName, nullptr);
    }

    TEST(LogEvent, FullConstructorStoresEveryField)
    {
        constexpr SourceLocation location("App.cpp", 77, "runApplication");
        const LogEvent           event(LogLevel::Error, "2026-09-10 08:09:10.011", "9527", location, "appLogger", "disk full");

        EXPECT_EQ(event.level, LogLevel::Error);
        EXPECT_EQ(event.timestamp, "2026-09-10 08:09:10.011");
        EXPECT_EQ(event.threadId, "9527");
        EXPECT_EQ(event.loggerNameView(), "appLogger");
        EXPECT_EQ(event.message, "disk full");
        EXPECT_STREQ(event.location.fileName, "App.cpp");
        EXPECT_EQ(event.location.line, 77);
        EXPECT_STREQ(event.location.functionName, "runApplication");
    }

    TEST(LogEvent, FullConstructorAcceptsEmptyStrings)
    {
        const LogEvent event(LogLevel::Off, "", "", SourceLocation(), "", "");

        EXPECT_EQ(event.level, LogLevel::Off);
        EXPECT_TRUE(event.timestamp.empty());
        EXPECT_TRUE(event.threadId.empty());
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
        EXPECT_EQ(moved.timestamp, "2026-09-10 12:34:56.789");
        EXPECT_EQ(moved.threadId, "12345");

        EXPECT_NO_THROW(original.message.clear());
        EXPECT_TRUE(original.message.empty());
        EXPECT_EQ(original.level, LogLevel::Warn);

        LogEvent assigned   = makeEventWithMessage("placeholder");
        LogEvent moveSource = makeEventWithMessage("reassigned message");
        assigned            = std::move(moveSource);
        EXPECT_EQ(assigned.message, "reassigned message");
        EXPECT_EQ(assigned.level, LogLevel::Warn);
    }

    TEST(LogEvent, CurrentTimestampHasFixedWidthFormat)
    {
        const std::string timestamp = currentTimestamp();

        ASSERT_EQ(timestamp.size(), kTimestampLength);
        EXPECT_EQ(timestamp[4], '-');
        EXPECT_EQ(timestamp[7], '-');
        EXPECT_EQ(timestamp[10], ' ');
        EXPECT_EQ(timestamp[13], ':');
        EXPECT_EQ(timestamp[16], ':');
        EXPECT_EQ(timestamp[19], '.');
        EXPECT_TRUE(onlyDigitsOutsideSeparators(timestamp));
    }

    TEST(LogEvent, CurrentTimestampFieldsStayInCalendarRanges)
    {
        const std::string timestamp = currentTimestamp();

        ASSERT_EQ(timestamp.size(), kTimestampLength);
        const int year        = std::stoi(timestamp.substr(0, 4));
        const int month       = std::stoi(timestamp.substr(5, 2));
        const int dayOfMonth  = std::stoi(timestamp.substr(8, 2));
        const int hour        = std::stoi(timestamp.substr(11, 2));
        const int minute      = std::stoi(timestamp.substr(14, 2));
        const int second      = std::stoi(timestamp.substr(17, 2));
        const int millisecond = std::stoi(timestamp.substr(20, 3));

        EXPECT_GE(year, 1970);
        EXPECT_GE(month, 1);
        EXPECT_LE(month, 12);
        EXPECT_GE(dayOfMonth, 1);
        EXPECT_LE(dayOfMonth, 31);
        EXPECT_GE(hour, 0);
        EXPECT_LE(hour, 23);
        EXPECT_GE(minute, 0);
        EXPECT_LE(minute, 59);
        EXPECT_GE(second, 0);
        EXPECT_LE(second, 60);
        EXPECT_GE(millisecond, 0);
        EXPECT_LE(millisecond, 999);
    }

    TEST(LogEvent, CurrentTimestampNeverGoesBackwardsOnConsecutiveCalls)
    {
        std::string previous = currentTimestamp();

        for (int iteration = 0; iteration < 50; ++iteration)
        {
            const std::string current = currentTimestamp();

            ASSERT_EQ(current.size(), kTimestampLength) << "iteration " << iteration;
            EXPECT_GE(current, previous) << "iteration " << iteration;
            previous = current;
        }
    }

    TEST(LogEvent, ThreadIdStringIsNonEmptyAndCachedPerThread)
    {
        const std::string &firstCall = threadIdString();

        ASSERT_FALSE(firstCall.empty());
        EXPECT_TRUE(firstCall.size() <= std::string::size_type(64));

        const std::string &secondCall = threadIdString();
        EXPECT_TRUE(&firstCall == &secondCall);
        EXPECT_EQ(firstCall, secondCall);
    }

    TEST(LogEvent, ThreadIdStringDiffersBetweenThreads)
    {
        constexpr std::size_t kworkerCount = 4;

        const std::string &      mainThreadId = threadIdString();
        std::vector<std::string> workerIds(kworkerCount);
        std::vector<std::thread> workers;

        workers.reserve(kworkerCount);
        for (std::size_t index = 0; index < kworkerCount; ++index)
        {
            workers.emplace_back([&workerIds, index]()
            {
                workerIds[index] = threadIdString();
            });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        for (std::size_t index = 0; index < kworkerCount; ++index)
        {
            EXPECT_FALSE(workerIds[index].empty()) << "worker " << index;
            EXPECT_NE(workerIds[index], mainThreadId) << "worker " << index;
            for (std::size_t otherIndex = 0; otherIndex < index; ++otherIndex)
            {
                EXPECT_NE(workerIds[index], workerIds[otherIndex]);
            }
        }
    }

    TEST(LogEvent, EventAssembledFromHelpersMatchesCallerContext)
    {
        const SourceLocation location = SourceLocation::current();
        const LogEvent       event(LogLevel::Info, currentTimestamp(), threadIdString(), location, "root", "assembled message");

        EXPECT_EQ(event.timestamp.size(), kTimestampLength);
        EXPECT_EQ(event.threadId, threadIdString());
        EXPECT_STREQ(event.location.functionName, location.functionName);
        EXPECT_EQ(event.location.line, location.line);
    }

    TEST(LogEvent, TimestampAndThreadIdSurviveStressLoop)
    {
        constexpr int kiterationCount = 500;

        int failureCount = 0;
        for (int iteration = 0; iteration < kiterationCount; ++iteration)
        {
            if (currentTimestamp().size() != kTimestampLength)
            {
                ++failureCount;
            }
            if (threadIdString().empty())
            {
                ++failureCount;
            }
        }

        EXPECT_EQ(failureCount, 0);
        EXPECT_FALSE(threadIdString().empty());
    }
} // namespace AsynGyanis::Base
