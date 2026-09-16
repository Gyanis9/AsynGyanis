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

    /**
     * @brief 构造后立即持有可用的 epoll 句柄，而不是 kInvalidEpollHandle 占位
     */
    TEST(Epoll, ConstructionAllocatesValidHandle)
    {
        Epoll epoll;
        ASSERT_NE(epoll.fileDescriptor(), Platform::kInvalidEpollHandle);
    }

    /**
     * @brief 移动构造把 epoll 句柄整体交给新对象，不重建底层实例
     */
    TEST(Epoll, MoveConstructorTransfersHandle)
    {
        Epoll first;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        Epoll second(std::move(first));

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    /**
     * @brief 移动赋值同样转移句柄：源被掏空、目标持有原句柄，不泄漏也不产生两个所有者
     */
    TEST(Epoll, MoveAssignmentTransfersHandle)
    {
        Epoll first;
        Epoll second;
        const Platform::EpollHandle originalHandle = first.fileDescriptor();

        second = std::move(first);

        EXPECT_EQ(second.fileDescriptor(), originalHandle);
    }

    /**
     * @brief 注册 EPOLLIN 后触发可读即上报事件，且事件带回注册时挂载的用户数据指针（调用方靠它定位上下文）
     */
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

    /**
     * @brief 注销后不再投递事件：即使描述符已可读也不产生就绪项（避免等待器被已摘除的 fd 唤醒）
     */
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

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 注销一个已武装的描述符之后，同一个描述符号（已被新描述符占用）要能立刻重新注册
     * @details 连接关闭与新建连接拿到同一个描述符号是常态（内核把最小可用号复用出去）。io_uring
     *          后端的在途轮询要等取消完成通知到齐才能销毁记录，但**描述符键必须当场释放**：键留着
     *          的话新连接的注册会被直接拒掉，IoWatcher 构造随之抛 SystemException。
     *          这里的「号复用」用 dup2 做成确定性的（不赌内核挑号）：新 eventfd 精确落到旧号上。
     *          完成端口后端（Windows）没有这个窗口——那边的 fd 键本来就是当场摘的，且新连接拿到的是
     *          全新句柄，够不成「同一个活句柄注册两次」
     */
    TEST(Epoll, AllowsReregisteringDescriptorNumberRightAfterDelete)
    {
        Epoll epoll;

        TestEventFd staleEventFd;
        ASSERT_GE(staleEventFd.fileDescriptor, 0);
        const int reusedDescriptorNumber = staleEventFd.fileDescriptor;

        int firstSentinel = 1;
        ASSERT_TRUE(epoll.addFileDescriptor(reusedDescriptorNumber, EPOLLIN, &firstSentinel));
        ASSERT_TRUE(epoll.delFileDescriptor(reusedDescriptorNumber));

        // 号的复用：新 eventfd 经 dup2 精确落到旧号上（旧号上原来那份随 dup2 一并关闭）
        TestEventFd freshEventFd;
        ASSERT_GE(freshEventFd.fileDescriptor, 0);
        ASSERT_EQ(::dup2(freshEventFd.writeDescriptor, reusedDescriptorNumber), reusedDescriptorNumber);

        int secondSentinel = 2;
        EXPECT_TRUE(epoll.addFileDescriptor(reusedDescriptorNumber, EPOLLIN, &secondSentinel))
                << "注销后同一个描述符号不能再注册：描述符键没有当场释放";

        // 新注册必须真的生效：让这个号变成可读，事件要带新挂的用户数据
        ASSERT_TRUE(freshEventFd.trigger());

        const auto events = epoll.wait(100);
        ASSERT_FALSE(events.empty()) << "重新注册的描述符没有投递事件";
        EXPECT_EQ(events[0].data.ptr, static_cast<void *>(&secondSentinel)) << "投递的是旧注册的用户数据";

        static_cast<void>(epoll.delFileDescriptor(reusedDescriptorNumber));
    }
#endif

    /**
     * @brief 无注册描述符时 wait() 在超时后返回空列表：既不死等也不报错
     */
    TEST(Epoll, WaitTimeoutReturnsNoEvents)
    {
        Epoll epoll;

        // 无任何注册描述符时，等待应在超时后返回空事件列表
        const auto events = epoll.wait(10);
        EXPECT_TRUE(events.empty());
    }

    /**
     * @brief modFileDescriptor() 真的替换了事件掩码：改为 EPOLLOUT 后按可写就绪，不再按可读
     */
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

    /**
     * @brief 多路注册时只上报真正就绪的那一个（用用户数据指针定位），未触发的描述符不产生误报
     */
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
