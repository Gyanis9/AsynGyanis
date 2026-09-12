/**
 * @file TestIoWatcher.cpp
 * @brief IoWatcher 单元测试：常驻注册、就绪缓存、关闭唤醒与等待者互斥
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件直接驱动 IoWatcher，不引入事件循环线程：
 *          事件分发就是 EventLoop::run() 里那一步（取回事件后交给注册对象的 handleEvents），
 *          测试自己做同样的一步，因此时序完全确定，不依赖线程调度。
 *
 *          钉住的契约：
 *          1、描述符在构造时注册一次，之后每次等待都不再产生 epoll_ctl（这一点由
 *             「等待期间不再调用任何 epoll 控制接口」间接体现：用例只推进事件分发即可完成等待）；
 *          2、边沿到达时若没有协程在等，就绪会被**记下来**，下一次等待立即完成——边沿触发下
 *             不缓存就必然丢事件，等待者会一直睡下去；
 *          3、销毁注册对象会唤醒仍挂着的等待者并以「未就绪」结束它的等待，等待方因此能收尾
 *             而不是永久挂起（关闭描述符本身不会唤醒 epoll 的等待者）；
 *          4、同一方向的第二个等待者当场抛错，而不是静默让其中一个永远等不到。
 */
#include "Core/EventLoop/IoWatcher.h"

#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <memory>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 等待结果：true 表示事件就绪，false 表示注册已失效
        using WaitOutcome = bool;

        /**
         * @brief 等待指定注册对象的可读事件
         * @param watcher 目标注册对象
         * @return Task<WaitOutcome> 惰性协程，co_await 后得到等待结果
         */
        Task<WaitOutcome> waitReadableOnce(IoWatcher &watcher)
        {
            co_return co_await watcher.waitReadable();
        }

        /**
         * @brief 推进一次事件分发：取回一批 epoll 事件并交给各自的注册对象
         * @details 与 EventLoop::run() 内分发那一步完全同构；事件里挂载的就是注册对象地址
         * @param loop 事件循环
         * @param timeoutMilliseconds 等待事件的超时
         * @return size_t 本次取回并分发的事件数
         */
        std::size_t dispatchOnce(EventLoop &loop, const int timeoutMilliseconds = 1000)
        {
            std::size_t dispatchedCount = 0;
            for (const auto &event: loop.epoll().wait(timeoutMilliseconds))
            {
                if (event.data.ptr != nullptr)
                {
                    static_cast<IoWatcher *>(event.data.ptr)->handleEvents(event.events);
                    ++dispatchedCount;
                }
            }
            return dispatchedCount;
        }

        /**
         * @brief 向文件描述符写一个字节，使其对端变为可读
         * @param fileDescriptor 目标描述符
         */
        void makeReadable(const int fileDescriptor)
        {
            const char payload = 'x';
            [[maybe_unused]] auto _ = Platform::FileDescriptor::write(fileDescriptor, &payload, 1);
        }
    } // namespace

    TEST(IoWatcher, InvalidDescriptorYieldsUnusableWatcher)
    {
        EventLoop loop;

        // 负数描述符（占位、已关闭）不注册，也不抛异常：持有空描述符的对象因此可以统一处理
        const IoWatcher watcher(loop, -1, EPOLLIN);
        EXPECT_FALSE(watcher.isValid());

        // 在这种对象上等待会立刻以「未就绪」结束，而不是挂起或抛异常
        Task<WaitOutcome> waiting = waitReadableOnce(const_cast<IoWatcher &>(watcher));
        waiting.handle().resume();
        ASSERT_TRUE(waiting.isReady());
        EXPECT_FALSE(waiting.handle().promise().result());
    }

    TEST(IoWatcher, WaitCompletesWhenEventIsDispatched)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor, EPOLLIN);
        ASSERT_TRUE(watcher.isValid());

        Task<WaitOutcome> waiting = waitReadableOnce(watcher);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady()) << "尚无数据时应当挂起等待";

        // 每次等待都不再有 epoll_ctl：只需要让事件到达并分发，等待就完成了
        makeReadable(peerDescriptor);
        ASSERT_GT(dispatchOnce(loop), 0U);

        ASSERT_TRUE(waiting.isReady());
        EXPECT_TRUE(waiting.handle().promise().result());

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, ReadinessIsCachedWhenNobodyIsWaiting)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor, EPOLLIN);
        ASSERT_TRUE(watcher.isValid());

        // 先让事件到达并分发——此刻没有协程在等，就绪必须被记下来。
        // 边沿触发只报「从不可用变为可用」的那一刻，不缓存这个边沿就永远丢了
        makeReadable(peerDescriptor);
        ASSERT_GT(dispatchOnce(loop), 0U);

        // 之后才发起的等待应当立即完成（不再需要任何新事件）
        Task<WaitOutcome> waiting = waitReadableOnce(watcher);
        waiting.handle().resume();
        ASSERT_TRUE(waiting.isReady()) << "缓存的就绪没有被下一次等待取走";
        EXPECT_TRUE(waiting.handle().promise().result());

        // 取走之后标记不再残留：再等一次会重新挂起（否则就绪标记会让等待空转）
        Task<WaitOutcome> secondWaiting = waitReadableOnce(watcher);
        secondWaiting.handle().resume();
        EXPECT_FALSE(secondWaiting.isReady()) << "就绪标记被取走后不应当再次立即完成";

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, DestroyingWatcherWakesPendingWaiter)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        auto watcher = std::make_unique<IoWatcher>(loop, localDescriptor, EPOLLIN);
        ASSERT_TRUE(watcher->isValid());

        Task<WaitOutcome> waiting = waitReadableOnce(*watcher);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady());

        // 销毁注册对象（等价于关闭描述符那条路径）：仍挂着的等待者必须被唤醒并得知注册已失效，
        // 否则它会永远等一个不可能再到来的事件
        watcher.reset();

        // 唤醒是投递到调度队列的（本对象正在析构，不能就地恢复），因此推进一次调度
        ASSERT_TRUE(loop.scheduler().runOne());
        ASSERT_TRUE(waiting.isReady());
        EXPECT_FALSE(waiting.handle().promise().result());

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, SecondWaiterOnSameDirectionThrows)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor, EPOLLIN);
        ASSERT_TRUE(watcher.isValid());

        Task<WaitOutcome> first = waitReadableOnce(watcher);
        first.handle().resume();
        ASSERT_FALSE(first.isReady());

        // 同一方向的第二个等待者：当场抛错而不是让其中一个永远等不到
        Task<WaitOutcome> second = waitReadableOnce(watcher);
        second.handle().resume();
        ASSERT_TRUE(second.isReady()) << "第二个等待者应当以异常结束";
        EXPECT_THROW(second.handle().promise().result(), Base::LogicException);

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

} // namespace AsynGyanis::Core
