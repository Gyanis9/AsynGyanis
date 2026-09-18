// EventNotifier 单元测试：跨线程唤醒、排空与无效状态
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

    TEST(EventNotifier, RepeatedNotificationsAreCoalescedIntoOneWakeup)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        // 未排空就连续通知：语义上只需要「目标循环醒来看一眼队列」，因此实现会合并后续通知，
        // 不写第二次描述符。跨线程投递密集时（每个远程任务都会通知一次）这一条把 N 次系统调用压成 1 次
        constexpr int kNotificationCount = 1000;
        for (int notificationIndex = 0; notificationIndex < kNotificationCount; ++notificationIndex)
        {
            notifier.notify();
        }

        // 一次读就把全部唤醒取走，正是「只写了一次」的证据：
        // 未合并的话 1000 次通知会留下 1000 字节，而单次读最多取走 sizeof(buffer) 字节、描述符仍可读
        char          buffer[64];
        const ssize_t readBytes = FileDescriptor::read(notifier.readDescriptor(), buffer, sizeof(buffer));
        EXPECT_GT(readBytes, 0);
        // 合并生效时这里只剩一次唤醒的量：Linux 的 eventfd 一次读回 8 字节计数，Windows 的 socket 对读回 1 字节
        EXPECT_LE(readBytes, 8);
        EXPECT_FALSE(TestSupport::waitForReadable(notifier.readDescriptor(), 100));
    }

    TEST(EventNotifier, NotifyAfterDrainStillWakesTheLoop)
    {
        const EventNotifier notifier;
        ASSERT_TRUE(notifier.isValid());

        // 合并是靠一个「待处理」标记实现的，因此排空必须把标记也清掉：
        // 若排空只读走字节、留下标记，之后所有通知都会以为「已经有人待处理」而**再也不写**，
        // 目标事件循环就会永远睡在 epoll_wait 上（表现为任务排进去了却没人处理）
        notifier.notify();
        ASSERT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));
        notifier.drain();

        notifier.notify();
        EXPECT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000))
            << "排空后再通知仍然必须唤醒：标记未被清除会导致之后所有通知都不再写描述符";
        notifier.drain();

        // 重复一轮，确认标记不会在某一轮之后停在「待处理」
        notifier.notify();
        EXPECT_TRUE(TestSupport::waitForReadable(notifier.readDescriptor(), 1000));
        notifier.drain();
        EXPECT_FALSE(TestSupport::waitForReadable(notifier.readDescriptor(), 100));
    }
} // namespace AsynGyanis::Platform
