/**
 * @file TestTimer.cpp
 * @brief Timer 单元测试：循环级定时器队列上的构造、到期唤醒、到期顺序与取消
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 用例手工推进事件循环（复刻 EventLoop::run() 的「分发事件 + 清空调度队列」两步），
 *          不引入循环线程，因此时序由用例自己掌握；真实定时器仍需等待内核到期，
 *          故每次推进都留出上限，超时判失败而不是把用例挂住。
 */

#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 一般等待上限：毫秒级定时器在调试构建 + ASan 下也能从容到期
        constexpr std::chrono::milliseconds kWaitTimeout{2000};

        /**
         * @brief 推进事件循环直到条件成立
         * @details 与 EventLoop::run() 的两步同构：先取回一批 epoll 事件交给注册对象，
         *          再清空调度队列。用例不涉及跨线程唤醒，因此不会遇到唤醒哨兵
         * @param loop 事件循环
         * @param predicate 待成立的条件
         * @param timeout 时间上限
         * @return true 条件在时限内成立
         */
        template<typename Predicate>
        bool advanceUntil(EventLoop &loop, Predicate predicate, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                for (const auto &event: loop.epoll().wait(5))
                {
                    if (event.data.ptr != nullptr)
                    {
                        static_cast<IoWatcher *>(event.data.ptr)->handleEvents(event.events);
                    }
                }
                loop.scheduler().runAll();
            }
            return true;
        }
    } // namespace

    /**
     * @brief 只持有事件循环即可构造定时器，且构造/析构都不产生副作用
     */
    TEST(Timer, ConstructionSucceeds)
    {
        EventLoop loop;

        EXPECT_NO_THROW(
        {
            Timer timer(loop);
        });
    }

    /**
     * @brief waitFor() 返回可构造的等待器，且未被 co_await 时不会登记到队列上
     */
    TEST(Timer, WaitForReturnsAwaiter)
    {
        EventLoop loop;
        Timer timer(loop);

        // 未被 co_await 的等待器不会登记任何定时器，仅验证可构造
        auto awaiter = timer.waitFor(std::chrono::milliseconds(100));
        (void)awaiter;
        EXPECT_EQ(loop.timerQueue().pendingCount(), 0U);
    }

    /**
     * @brief 到期后等待的协程会被唤醒
     */
    TEST(Timer, WaitForCompletesAfterDeadline)
    {
        EventLoop loop;
        Timer     timer(loop);

        bool isExpired = false;
        // 惰性 Task 的协程帧记住的是闭包对象的地址：闭包必须先落到具名变量上再调用，
        // 否则「构造后立即调用的临时闭包」在语句结束即销毁，恢复协程时读到的捕获已是死对象
        auto waitingBody = [&timer, &isExpired]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(5));
            isExpired = true;
        };
        auto waiting = waitingBody();
        waiting.handle().resume();

        ASSERT_FALSE(isExpired) << "定时器未到期就完成了";
        ASSERT_TRUE(advanceUntil(loop, [&isExpired] { return isExpired; }, kWaitTimeout)) << "定时器到期后没有唤醒等待方：上界 kWaitTimeout";
        EXPECT_TRUE(waiting.isReady());
        EXPECT_EQ(loop.timerQueue().pendingCount(), 0U);
    }

    /**
     * @brief 非正数时长表示「尽快到期」，而不是永不触发
     */
    TEST(Timer, NonPositiveDurationFiresSoon)
    {
        EventLoop loop;
        Timer     timer(loop);

        bool isExpired   = false;
        auto waitingBody = [&timer, &isExpired]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds::zero());
            isExpired = true;
        };
        auto waiting = waitingBody();
        waiting.handle().resume();

        ASSERT_TRUE(advanceUntil(loop, [&isExpired] { return isExpired; }, kWaitTimeout)) << "零时长定时器没有在时限内到期";
    }

    /**
     * @brief 同一循环上的多个定时器按截止时间先后唤醒，先登记的长定时器不会挡住后登记的短定时器
     */
    TEST(Timer, TimersFireInDeadlineOrder)
    {
        EventLoop loop;
        Timer     timer(loop);

        std::vector<int> firedOrder;

        // 先登记 30ms 的，再登记 5ms 的：后者更早到期，队列必须改武装到更早的时刻
        auto laterBody = [&timer, &firedOrder]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(30));
            firedOrder.push_back(1);
        };
        auto later = laterBody();
        later.handle().resume();

        auto earlierBody = [&timer, &firedOrder]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(5));
            firedOrder.push_back(2);
        };
        auto earlier = earlierBody();
        earlier.handle().resume();

        ASSERT_TRUE(advanceUntil(loop, [&firedOrder] { return firedOrder.size() >= 2U; }, kWaitTimeout)) << "两个定时器没有都在时限内到期";
        ASSERT_EQ(firedOrder.size(), 2U);
        EXPECT_EQ(firedOrder[0], 2) << "更早截止的定时器没有先到期：队列没有为更早的登记时间改武装";
        EXPECT_EQ(firedOrder[1], 1);
    }

    /**
     * @brief 提前销毁等待者会撤销登记，既不影响其他定时器，也不留下悬空登记
     */
    TEST(Timer, CancelledWaitIsUnregisteredAndDoesNotBlockOthers)
    {
        EventLoop loop;
        Timer     timer(loop);

        {
            auto cancelledBody = [&timer]() -> Task<>
            {
                co_await timer.waitFor(std::chrono::milliseconds(50));
            };
            auto cancelled = cancelledBody();
            cancelled.handle().resume();
            ASSERT_EQ(loop.timerQueue().pendingCount(), 1U);
        }

        // 帧销毁即取消：队列里不再有这一项（它是最近的截止时间，取消后武装要跟着回退）
        EXPECT_EQ(loop.timerQueue().pendingCount(), 0U);

        bool isExpired   = false;
        auto waitingBody = [&timer, &isExpired]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(5));
            isExpired = true;
        };
        auto waiting = waitingBody();
        waiting.handle().resume();

        ASSERT_TRUE(advanceUntil(loop, [&isExpired] { return isExpired; }, kWaitTimeout)) << "取消一个等待后，其他定时器没有正常到期";
    }
} // namespace AsynGyanis::Core
