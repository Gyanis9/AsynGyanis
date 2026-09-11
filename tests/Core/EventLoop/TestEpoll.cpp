/**
 * @file TestEpoll.cpp
 * @brief Epoll 单元测试：实例句柄、移动语义、事件注册/修改/移除与超时等待
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/EventLoop/Epoll.h"
#include "Platform/Platform.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试辅助：创建一个可触发的文件描述符
         *
         * Linux 直接使用 eventfd（读写为同一描述符）；
         * Windows 没有 eventfd，改用互相连通的 socket 描述符对（wepoll 可监听 socket）。
         */
        struct TestEventFd
        {
            int fileDescriptor{Platform::FileDescriptor::kInvalid};  ///< 注册到 epoll 的描述符（Windows 上为读端）
            int writeDescriptor{Platform::FileDescriptor::kInvalid}; ///< 触发事件时写入的描述符（Linux 上与 fileDescriptor 相同）

            TestEventFd()
            {
#if ASYN_PLATFORM_WIN32
                // Windows 用 loopback 描述符对承载 eventfd 的唤醒语义
                if (!Platform::FileDescriptor::createPair(fileDescriptor, writeDescriptor))
                {
                    return;
                }
                // createPair 内部已设置非阻塞，这里保留兜底设置以防实现回退
                Platform::FileDescriptor::setNonBlocking(fileDescriptor);
#else
                fileDescriptor      = static_cast<int>(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
                writeDescriptor = fileDescriptor;
#endif
            }

            ~TestEventFd()
            {
                // 读端始终需要关闭；Linux 上读写同描述符，仅关闭一次
                if (Platform::FileDescriptor::isValid(fileDescriptor))
                {
                    Platform::FileDescriptor::close(fileDescriptor);
                }
#if ASYN_PLATFORM_WIN32
                if (Platform::FileDescriptor::isValid(writeDescriptor) && writeDescriptor != fileDescriptor)
                {
                    Platform::FileDescriptor::close(writeDescriptor);
                }
#endif
            }

            TestEventFd(const TestEventFd &)            = delete;
            TestEventFd &operator=(const TestEventFd &) = delete;

            /**
             * @brief 向写端写入一次计数，触发读端可读事件
             * @return true 写入成功
             */
            bool trigger()
            {
                const std::uint64_t value = 1;
#if ASYN_PLATFORM_WIN32
                // Windows 上描述符实为 socket，写入走 Winsock send
                return ::send(writeDescriptor, reinterpret_cast<const char *>(&value), sizeof(value), 0)
                       == static_cast<int>(sizeof(value));
#else
                return ::write(writeDescriptor, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
#endif
            }
        };
    }

    TEST(Epoll, ConstructionAllocatesValidHandle)
    {
        Epoll epoll;
        ASSERT_NE(epoll.fileDescriptor(), Platform::kInvalidEpollHandle);
    }

    TEST(Epoll, MoveConstructorTransfersHandle)
    {
        Epoll first;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        Epoll second(std::move(first));

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    TEST(Epoll, MoveAssignmentTransfersHandle)
    {
        Epoll first;
        Epoll second;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        second = std::move(first);

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    TEST(Epoll, AddFileDescriptorDeliversReadableEvent)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 0;
        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));

        // 写入计数使描述符变为可读
        ASSERT_TRUE(eventFd.trigger());

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&sentinel));
    }

    TEST(Epoll, DelFileDescriptorStopsEventDelivery)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, nullptr));
        ASSERT_TRUE(epoll.delFileDescriptor(eventFd.fileDescriptor));

        // 移除后再触发不应产生任何就绪事件
        eventFd.trigger();

        const auto events = epoll.wait(10);
        EXPECT_TRUE(events.empty());
    }

    TEST(Epoll, WaitTimeoutReturnsNoEvents)
    {
        Epoll epoll;

        // 无任何注册描述符时，等待应在超时后返回空事件列表
        const auto events = epoll.wait(10);
        EXPECT_TRUE(events.empty());
    }

    TEST(Epoll, ModFileDescriptorChangesEventMask)
    {
        Epoll epoll;
        TestEventFd eventFd;
        ASSERT_GE(eventFd.fileDescriptor, 0);

        int sentinel = 42;
        ASSERT_TRUE(epoll.addFileDescriptor(eventFd.fileDescriptor, EPOLLIN, &sentinel));
        ASSERT_TRUE(epoll.modFileDescriptor(eventFd.fileDescriptor, EPOLLOUT, &sentinel));

        // 描述符始终可写，事件掩码修改为 EPOLLOUT 后应立即就绪
        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());
        EXPECT_TRUE(events[0].events & EPOLLOUT);
    }

    TEST(Epoll, MultipleFileDescriptorsReportTriggeredOne)
    {
        Epoll epoll;
        TestEventFd firstEventFd;
        TestEventFd secondEventFd;
        ASSERT_GE(firstEventFd.fileDescriptor, 0);
        ASSERT_GE(secondEventFd.fileDescriptor, 0);

        int firstSentinel  = 1;
        int secondSentinel = 2;
        ASSERT_TRUE(epoll.addFileDescriptor(firstEventFd.fileDescriptor, EPOLLIN, &firstSentinel));
        ASSERT_TRUE(epoll.addFileDescriptor(secondEventFd.fileDescriptor, EPOLLIN, &secondSentinel));

        // 仅触发第二个描述符
        secondEventFd.trigger();

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty());

        // 就绪列表中应能找到第二个描述符挂载的用户数据
        bool foundSecondSentinel = false;
        for (const auto &event: events)
        {
            if (event.data.ptr == &secondSentinel)
            {
                foundSecondSentinel = true;
            }
        }
        EXPECT_TRUE(foundSecondSentinel);
    }
} // namespace AsynGyanis::Core
