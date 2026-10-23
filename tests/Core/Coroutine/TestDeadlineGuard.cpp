// DeadlineGuard 单元测试：到期关闭被监视对象，以及提前销毁时看门狗必须被撤销

#include "Core/Coroutine/DeadlineGuard.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"

#include <gtest/gtest.h>

#include <chrono>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 只统计 close() 次数的被监视对象：DeadlineGuard 只要求这一个接口
        struct CloseProbe
        {
            int closeCount{0};

            void close() noexcept
            {
                ++closeCount;
            }
        };

        /**
         * @brief 在循环上等一段固定时间后停掉循环，驱动定时器真正有机会到期
         * @param loop 目标循环
         * @param duration 观察时长
         */
        void runFor(EventLoop &loop, const std::chrono::milliseconds duration)
        {
            auto drive = [&loop, duration]() -> Task<void>
            {
                Timer timer(loop);
                co_await timer.waitFor(duration);
                loop.stop();
            };
            Task<void> task = drive();
            loop.scheduler().schedule(task.handle());
            loop.run();
        }
    } // namespace

    TEST(DeadlineGuard, ClosesTheWatchedObjectWhenTheDeadlineExpires)
    {
        EventLoop  loop;
        CloseProbe watched;
        {
            DeadlineGuard<CloseProbe> guard(loop, watched, std::chrono::milliseconds{20}, "DeadlineGuardTest");
            runFor(loop, std::chrono::milliseconds{100});
        }
        EXPECT_EQ(watched.closeCount, 1) << "到点没有关闭被监视对象：挂在其上的等待协程永远不会被放出来";
    }

    TEST(DeadlineGuard, DestroyingTheGuardBeforeTheDeadlineCancelsTheWatchdog)
    {
        EventLoop  loop;
        CloseProbe watched;
        {
            DeadlineGuard<CloseProbe> guard(loop, watched, std::chrono::milliseconds{20}, "DeadlineGuardTest");
            // 立即销毁：看门狗帧应随析构一起撤销，20 ms 后不能再碰任何状态
            static_cast<void>(guard);
        }
        runFor(loop, std::chrono::milliseconds{100});
        EXPECT_EQ(watched.closeCount, 0) << "提前销毁的守卫仍在到点后关闭对象：看门狗没有被撤销";
    }
} // namespace AsynGyanis::Core
