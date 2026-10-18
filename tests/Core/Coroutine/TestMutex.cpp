// Mutex 单元测试：独占权的串行性（含跨过挂起点）、异常展开路径上的放手、
// 排队帧被销毁后不留死锁、以及本对象先死时交回空守卫。
//
// 用例手工推进事件循环，并用一个「测试自己放行」的挂起门把临界区撑开在两个挂起点之间：
// 只有让持锁协程真的停在半途，才能区分「互斥」与「碰巧没人同时进来」。

#include "Core/Coroutine/Mutex.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include "Base/Exception/LogicException.h"

#include "CoreTestSupport.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 测试用的挂起门：把协程钉在半途，直到用例自己喊放行
         * @details 真实等待点（定时器、套接字）的放行时刻不由用例定，撑不开「持锁且挂起」这一形状
         */
        class SuspendGate
        {
        public:
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> handle) noexcept
            {
                m_handle = handle;
            }

            void await_resume() const noexcept
            {
            }

            /// 放行：恢复挂在门上的协程（门未被等待时为空操作）
            void open() const noexcept
            {
                if (m_handle)
                {
                    m_handle.resume();
                }
            }

        private:
            std::coroutine_handle<> m_handle{}; ///< 挂在门上的协程
        };

        /// 测试用的抛出物：只为了在临界区里制造一次异常展开
        class CriticalSectionFailure : public std::runtime_error
        {
        public:
            CriticalSectionFailure() : std::runtime_error("临界区内抛出")
            {
            }
        };

        /**
         * @brief 一组等待者共同的观察账
         */
        struct Observation
        {
            std::vector<std::string> trace; ///< 按发生先后记下的事件（"名字:动作"）
        };

        /**
         * @brief 协程：拿到锁 → 停在门上（临界区）→ 放手
         */
        Task<void> lockHoldReleaseTask(Mutex &mutex, Observation &observation, const std::string name, SuspendGate &gate)
        {
            std::optional<Mutex::Guard> guard = co_await mutex.lock();
            if (!guard.has_value())
            {
                observation.trace.push_back(name + ":空守卫");
                co_return;
            }
            observation.trace.push_back(name + ":拿到");
            co_await gate;
            observation.trace.push_back(name + ":放手");
        }

        /**
         * @brief 协程：拿到锁 → 停在门上 → 在临界区里抛出
         */
        Task<void> lockThrowTask(Mutex &mutex, Observation &observation, const std::string name, SuspendGate &gate)
        {
            std::optional<Mutex::Guard> guard = co_await mutex.lock();
            if (!guard.has_value())
            {
                observation.trace.push_back(name + ":空守卫");
                co_return;
            }
            observation.trace.push_back(name + ":拿到");
            co_await gate;
            throw CriticalSectionFailure(); // 展开路上 guard 随帧析构：这一格就是「异常也要放手」
        }

        /**
         * @brief 协程：拿到锁就记一笔（临界区长度为空），拿不到就记空守卫
         */
        Task<void> lockQuickTask(Mutex &mutex, Observation &observation, const std::string name)
        {
            const std::optional<Mutex::Guard> guard = co_await mutex.lock();
            observation.trace.push_back(guard.has_value() ? name + ":拿到" : name + ":空守卫");
        }

        /**
         * @brief 把协程投进本地就绪队列
         */
        void submit(EventLoop &loop, Task<void> &task)
        {
            loop.scheduler().schedule(task.handle());
        }
    } // namespace

    TEST(MutexTest, SerialisesCriticalSectionsAcrossSuspensionPoints)
    {
        // 判据：持锁协程停在挂起门上时，第二个协程进不来；第一个放手之后第二个才醒。
        // 记录顺序是独立期望（不是由实现算出来的），实现只要漏一次互斥就会错位
        EventLoop   loop;
        Mutex       mutex(loop);
        Observation observation;
        SuspendGate firstGate;
        SuspendGate secondGate;

        Task<void> first = lockHoldReleaseTask(mutex, observation, "甲", firstGate);
        submit(loop, first);
        loop.scheduler().runOne(); // 甲 拿到锁并停在临界区中间
        const std::vector<std::string> expectedAfterFirstEnter{"甲:拿到"};
        EXPECT_EQ(observation.trace, expectedAfterFirstEnter);

        Task<void> second = lockHoldReleaseTask(mutex, observation, "乙", secondGate);
        submit(loop, second);
        loop.scheduler().runOne(); // 乙 只能排队：锁在甲 手上
        EXPECT_EQ(observation.trace, expectedAfterFirstEnter);

        firstGate.open(); // 甲 走出临界区并放手；放行发生在此刻，醒来排在下一拍
        const std::vector<std::string> expectedAfterFirstLeave{"甲:拿到", "甲:放手"};
        EXPECT_EQ(observation.trace, expectedAfterFirstLeave);

        loop.scheduler().runAll(); // 乙 这才拿到锁，并停在它自己的门上
        const std::vector<std::string> expectedAfterSecondEnter{"甲:拿到", "甲:放手", "乙:拿到"};
        EXPECT_EQ(observation.trace, expectedAfterSecondEnter);

        secondGate.open();
        const std::vector<std::string> expectedAll{"甲:拿到", "甲:放手", "乙:拿到", "乙:放手"};
        EXPECT_EQ(observation.trace, expectedAll);
    }

    TEST(MutexTest, DoesNotSuspendWhenTheLockIsFree)
    {
        // 判据：没人持锁时一趟就跑完，不挂起、也不排进队——否则每条新协程都要多挨一拍
        EventLoop   loop;
        Mutex       mutex(loop);
        Observation observation;

        Task<void> task = lockQuickTask(mutex, observation, "甲");
        submit(loop, task);
        loop.scheduler().runOne();

        const std::vector<std::string> expected{"甲:拿到"};
        EXPECT_EQ(observation.trace, expected);
    }

    TEST(MutexTest, ReleasesTheLockWhenTheCoroutineThrowsInsideTheCriticalSection)
    {
        // 判据：临界区里抛出，展开路径上的守卫照样放手——漏这一格就是把整条通路永久锁死
        EventLoop   loop;
        Mutex       mutex(loop);
        Observation observation;
        SuspendGate throwingGate;

        Task<void> throwing = lockThrowTask(mutex, observation, "甲", throwingGate);
        submit(loop, throwing);
        loop.scheduler().runOne(); // 甲 拿到锁并停在临界区里
        const std::vector<std::string> expectedAfterEnter{"甲:拿到"};
        EXPECT_EQ(observation.trace, expectedAfterEnter);

        Task<void> next = lockQuickTask(mutex, observation, "乙");
        submit(loop, next);
        loop.scheduler().runOne(); // 乙 只能排队
        EXPECT_EQ(observation.trace, expectedAfterEnter);

        throwingGate.open(); // 甲 在这里抛出：帧展开，守卫随之析构
        loop.scheduler().runAll();

        const std::vector<std::string> expectedAll{"甲:拿到", "乙:拿到"};
        EXPECT_EQ(observation.trace, expectedAll);
    }

    TEST(MutexTest, GivesTheLockToTheNextWaiterAfterTheGuardGoesOutOfScopeWithoutAwaiting)
    {
        // 判据：守卫由用例线程直接放手（不是协程自己放手）时，排队的协程醒来拿到的必须是有效守卫，
        // 而不是空守卫——空守卫只该出现在锁对象先死的情形
        EventLoop   loop;
        Mutex       mutex(loop);
        Observation observation;

        std::optional<Mutex::Guard> held = mutex.tryLock();
        ASSERT_HAS_VALUE(held);

        Task<void> waiter = lockQuickTask(mutex, observation, "甲");
        submit(loop, waiter);
        loop.scheduler().runOne();
        EXPECT_TRUE(observation.trace.empty());

        held.reset();
        loop.scheduler().runAll();
        const std::vector<std::string> expected{"甲:拿到"};
        EXPECT_EQ(observation.trace, expected);
    }

    TEST(MutexTest, DoesNotLeaveTheLockStuckWhenAQueuedWaiterDies)
    {
        // 判据：排队中的帧被销毁（取消、超时收尾都是这个形状）之后，锁仍然给得出去
        EventLoop   loop;
        Mutex       mutex(loop);
        Observation observation;

        std::optional<Mutex::Guard> held = mutex.tryLock();
        ASSERT_HAS_VALUE(held);

        {
            Task<void> waiter = lockQuickTask(mutex, observation, "甲");
            submit(loop, waiter);
            loop.scheduler().runOne();
            ASSERT_TRUE(observation.trace.empty());
            // 出作用域：甲 的帧在挂起点上被销毁
        }

        held.reset();
        Observation followUp;
        Task<void>  next = lockQuickTask(mutex, followUp, "乙");
        submit(loop, next);
        loop.scheduler().runAll();

        const std::vector<std::string> expected{"乙:拿到"};
        EXPECT_EQ(followUp.trace, expected);
    }

    TEST(MutexTest, YieldsEmptyGuardWhenTheMutexDiesBetweenGrantAndWakeUp)
    {
        // 判据：本对象在「已授额、未恢复」的窗口里死掉，等待者醒来拿到的是空守卫，
        // 且不会去摸已释放的锁对象——调用方据此收尾，而不是以为自己进了临界区
        EventLoop              loop;
        Observation            observation;
        SuspendGate            holderGate;
        std::unique_ptr<Mutex> mutex = std::make_unique<Mutex>(loop);

        std::unique_ptr<Task<void>> holder = std::make_unique<Task<void>>(lockHoldReleaseTask(*mutex, observation, "甲", holderGate));
        loop.scheduler().schedule(holder->handle());
        loop.scheduler().runOne();
        ASSERT_EQ(observation.trace.size(), 1u); // 甲 持锁停在临界区里

        std::unique_ptr<Task<void>> waiter = std::make_unique<Task<void>>(lockQuickTask(*mutex, observation, "乙"));
        loop.scheduler().schedule(waiter->handle());
        loop.scheduler().runOne();
        ASSERT_EQ(observation.trace.size(), 1u); // 乙 在队上

        holder.reset(); // 甲 的帧先没了：守卫随之析构，授额给乙 并把恢复排到下一拍（甲 停在门上，没走到记「放手」那一步）

        mutex.reset(); // 锁在乙 醒来之前死掉
        loop.scheduler().runAll();

        const std::vector<std::string> expected{"甲:拿到", "乙:空守卫"};
        EXPECT_EQ(observation.trace, expected);
    }

    TEST(MutexTest, RejectsForeignThreadUseOfTryLockOnARunningLoop)
    {
        // 判据：循环正在别处跑时，外来线程不能抢锁、也不能投队（内部计数与链表都没锁）
        TestSupport::EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        Mutex mutex(runner.loop());
        EXPECT_THROW(static_cast<void>(mutex.tryLock()), Base::LogicException);
        EXPECT_THROW(static_cast<void>(mutex.lock()), Base::LogicException);

        runner.join();
        std::optional<Mutex::Guard> held = mutex.tryLock(); // 循环停下：判据放开
        EXPECT_HAS_VALUE(held);
    }
} // namespace AsynGyanis::Core
