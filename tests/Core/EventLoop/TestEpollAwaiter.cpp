/**
 * @file TestEpollAwaiter.cpp
 * @brief EpollAwaiter 单元测试：注册失败必须报错而不是静默挂死，以及恢复后的自动注销
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件直接驱动 EpollAwaiter，不引入事件循环线程：
 *          等待器只需要一个 Epoll 实例，而事件到来后由谁恢复协程是可观测的——
 *          epoll_event.data.ptr 里存的就是协程句柄地址，测试取出来自行 resume，
 *          因此时序完全确定，不依赖线程调度。
 *
 *          钉住的契约：
 *          1、向 epoll 注册失败（同一个 fd 已被另一个等待器注册、或 fd 无效）时，
 *             await_suspend 必须抛出而不是吞掉返回值——否则协程已经挂起、却再没有任何
 *             事件能唤醒它，表现为整个连接静默卡死且没有错误线索；
 *          2、正常恢复后必须自动注销该 fd，否则同一个 fd 的下一次等待会因 EEXIST 失败；
 *          3、协程帧在恢复前被销毁时，析构函数兜底注销（下文的 RAII 用例）。
 */
#include "Core/EventLoop/EpollAwaiter.h"

#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <span>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 等待指定 fd 变为可读（协程体只做一次 co_await）
         * @param epoll epoll 实例引用
         * @param fileDescriptor 待等待的文件描述符
         * @return Task<> 惰性协程，首次 resume 时注册并挂起
         */
        Task<> waitUntilReadable(Epoll &epoll, const int fileDescriptor)
        {
            co_await EpollAwaiter(epoll, fileDescriptor, EPOLLIN);
        }

        /**
         * @brief 向对端写入一个字节，使本端 fd 变为可读
         * @param fileDescriptor 目标文件描述符
         */
        void makeReadable(const int fileDescriptor)
        {
            const char payload = 'x';
            [[maybe_unused]] auto _ = Platform::FileDescriptor::write(fileDescriptor, &payload, 1);
        }
    } // namespace

    /**
     * @brief 注册失败（同一 fd 已注册导致 EEXIST）时 await_suspend 必须抛出：协程以异常结束而不是永久挂起
     * @details 修复前这里吞掉返回值，协程已挂起却再无事件能唤醒它，表现为静默卡死且没有任何错误线索
     */
    TEST(EpollAwaiter, RegistrationFailureThrowsInsteadOfSuspendingForever)
    {
        Epoll epoll;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        // 先手工把同一个 fd 注册上去，制造「该 fd 已被注册」这一失败条件：
        // epoll 对已存在的 fd 执行 ADD 会返回 EEXIST，这正是等待器注册失败的典型来源
        ASSERT_TRUE(epoll.addFileDescriptor(localDescriptor, EPOLLIN, nullptr));

        Task<> waiting = waitUntilReadable(epoll, localDescriptor);
        waiting.handle().resume();

        // 关键断言：协程必须以异常结束，而不是停在挂起状态——修复前这里会被永久挂起
        ASSERT_TRUE(waiting.isReady()) << "注册失败时协程不应停在挂起状态";
        EXPECT_THROW(waiting.handle().promise().result(), Base::SystemException);

        static_cast<void>(epoll.delFileDescriptor(localDescriptor));
        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 事件到达恢复协程后自动注销该 fd：恢复后 MOD 失败即证明注册无残留，同一 fd 的下一次等待不会因 EEXIST 失败
     */
    TEST(EpollAwaiter, ResumeDeregistersFileDescriptor)
    {
        Epoll epoll;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        Task<> waiting = waitUntilReadable(epoll, localDescriptor);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady()) << "没有事件时协程应当处于挂起状态";

        // 让本端可读，再取一次事件并手动恢复（data.ptr 存的就是协程句柄地址）
        makeReadable(peerDescriptor);
        const std::span<epoll_event> events = epoll.wait(1000);
        ASSERT_GT(events.size(), 0U) << "对端已写入数据，本端应当就绪";
        std::coroutine_handle<>::from_address(events.front().data.ptr).resume();
        ASSERT_TRUE(waiting.isReady());

        // await_resume 已注销该 fd：此时 MOD 会因 fd 不在集合里而失败，
        // 这就是「恢复后自动注销」的可观测证据
        EXPECT_FALSE(epoll.modFileDescriptor(localDescriptor, EPOLLIN, nullptr));

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 协程帧在挂起状态下被销毁（异常展开/取消）时由等待器析构兜底注销：不留悬空注册，同一 fd 之后还能重新 ADD
     */
    TEST(EpollAwaiter, DestroyingSuspendedFrameDeregistersFileDescriptor)
    {
        Epoll epoll;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        {
            // 协程在挂起状态下被销毁（异常展开、取消等场景的抽象）：
            // 等待器对象随协程帧一起析构，必须把 fd 从 epoll 里摘掉
            Task<> waiting = waitUntilReadable(epoll, localDescriptor);
            waiting.handle().resume();
            ASSERT_FALSE(waiting.isReady());
        }

        // 已注销：MOD 失败即证明注册没有残留
        EXPECT_FALSE(epoll.modFileDescriptor(localDescriptor, EPOLLIN, nullptr));

        // 更关键的是不留悬空注册：下一次对同一 fd 的 ADD 必须成功
        EXPECT_TRUE(epoll.addFileDescriptor(localDescriptor, EPOLLIN, nullptr));
        static_cast<void>(epoll.delFileDescriptor(localDescriptor));

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

} // namespace AsynGyanis::Core
