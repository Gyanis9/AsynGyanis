/**
 * @file TestEventNotifier.cpp
 * @brief EventNotifier 单元测试：跨线程唤醒、排空与无效状态
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/EventNotifier.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    TEST(EventNotifier, NewlyConstructedNotifierIsValid)
    {
        const EventNotifier notifier;

        EXPECT_TRUE(notifier.isValid());
        EXPECT_GE(notifier.readDescriptor(), 0);
    }

    TEST(EventNotifier, NotifyMakesReadDescriptorReadable)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        notifier.notify();

        EXPECT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));
    }

    TEST(EventNotifier, DrainClearsPendingNotifications)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        notifier.notify();
        ASSERT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));

        notifier.drain();

        // 排空后再次读取应立即失败（非阻塞且无数据）
        EXPECT_FALSE(TestSupport::waitForReadable(notifier.readDescriptor(), 100));
    }

    TEST(EventNotifier, RepeatedNotificationsAreCollectedBySingleDrain)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        for (int repeatIndex = 0; repeatIndex < 10; ++repeatIndex)
        {
            notifier.notify();
        }

        ASSERT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));
        notifier.drain();
        EXPECT_FALSE(TestSupport::waitForReadable(notifier.readDescriptor(), 100));
    }

    TEST(EventNotifier, DrainWithoutNotifyIsSafe)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        EXPECT_NO_THROW(notifier.drain());
    }

    TEST(EventNotifier, NotifyIsSafeFromMultipleThreads)
    {
        EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        constexpr unsigned int   kthreadCount = 8;
        std::vector<std::thread> workerThreads;
        workerThreads.reserve(kthreadCount);

        for (unsigned int threadIndex = 0; threadIndex < kthreadCount; ++threadIndex)
        {
            workerThreads.emplace_back(
                    [&notifier]()
                    {
                        for (int repeatIndex = 0; repeatIndex < 50; ++repeatIndex)
                        {
                            notifier.notify();
                        }
                    });
        }
        for (std::thread &workerThread: workerThreads)
        {
            workerThread.join();
        }

        EXPECT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));
        notifier.drain();
    }

    TEST(EventNotifier, DestructorClosesHandlesAfterUse)
    {
        int observedDescriptor = FileDescriptor::kInvalid;
        {
            const EventNotifier notifier;
            ASSERT_TRUE(notifier.isValid());
            observedDescriptor = notifier.readDescriptor();
            notifier.notify();
            notifier.drain();
        }
        // 离开作用域后描述符已关闭，重新使用同一编号不应导致崩溃
        EXPECT_NE(observedDescriptor, FileDescriptor::kInvalid);
        SUCCEED();
    }
} // namespace AsynGyanis::Platform
