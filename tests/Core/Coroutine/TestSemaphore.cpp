// Semaphore 单元测试：额度记账、严格先进先出、许可归还的三条出路（当场放手/帧析构/授额后未醒来）
// 与容量越界、0 个数、外来线程三项拒绝。
//
// 用例一律手工推进事件循环（投进本地就绪队列后 runOne()/runAll()），不引入循环线程：
// 「谁先醒、醒了几次、额度还剩几格」必须由用例钉住时刻，否则断言就在赌调度运气。
// 最后一条例外是外来线程那条——它要一条真在跑的循环才造得出「正在别处运行」这一前提。

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Semaphore.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::EventLoopThread;

        /**
         * @brief 测试用的挂起门：把协程钉在半途，直到用例自己喊放行
         * @details 真实等待点的放行时刻不由用例定，撑不开「攥着许可再取第二格」这一形状
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

        /**
         * @brief 一组等待者共同的观察账
         */
        struct Observation
        {
            std::vector<std::string> woke;  ///< 醒来看见「拿到许可」的，记 "名字:个数"，按醒来先后
            std::vector<std::string> empty; ///< 醒来看见「没拿到许可」的，记名字
        };

        /**
         * @brief 协程：取 permitCount 个许可，记一笔，然后走到协程结尾
         * @details 许可在协程体结束时随局部对象析构归还（`co_return` 之后、终结挂起点之前），
         *          用例因此可以把「归还」钉在明确的某一次 runOne 之内；帧停在挂起点上被销毁时，
         *          归还走的是等待器析构那条路（见 GivesThePermitBackWhenAGrantedWaiterDiesBeforeResuming）
         */
        Task<void> acquireAndRecordTask(Semaphore &semaphore, Observation &observation, const std::string name, const std::size_t permitCount)
        {
            std::optional<Semaphore::Permit> permit = co_await semaphore.acquire(permitCount);
            if (!permit.has_value())
            {
                observation.empty.push_back(name);
                co_return;
            }
            observation.woke.push_back(name + ":" + std::to_string(permit->permitCount()));
        }

        /**
         * @brief 协程：先取一格许可并攥在手里（停在门上），醒来后再取一格，两格都到手才记一笔
         * @details 同一条协程在持有一格的情况下再次排队——这一格形状专门用来抓「已经消费过的等待器
         *          还留在队上」这类残账：实现若忘了在取走许可时离队，放行的一方会把额度补给那个
         *          早已走完的等待器，第二次取许可因此永远醒不来
         */
        Task<void> takeTwiceTask(Semaphore &semaphore, Observation &observation, const std::string name, SuspendGate &gate)
        {
            const std::optional<Semaphore::Permit> first = co_await semaphore.acquire(1);
            if (!first.has_value())
            {
                observation.empty.push_back(name);
                co_return;
            }
            co_await gate;
            const std::optional<Semaphore::Permit> second = co_await semaphore.acquire(1);
            if (!second.has_value())
            {
                observation.empty.push_back(name + ":第二格");
                co_return;
            }
            observation.woke.push_back(name + ":两格");
        }

        /**
         * @brief 协程：一整条语句里连着取两次许可（第一次的等待器要到这条语句结束才析构）
         * @details 这是「取走许可」与「等待器析构」之间真的存在时间窗的唯一写法：等待器活在整条
         *          表达式里，实现若不在取走许可的那一刻离队，第二次取许可就会排在自家那条旧等待器后面，
         *          而旧等待器要等整条语句结束才放手——两次取许可于是互相等死
         */
        Task<void> takeTwiceInOneExpressionTask(Semaphore &semaphore, Observation &observation, const std::string name)
        {
            const bool bothTaken = (co_await semaphore.acquire(1)).has_value() && (co_await semaphore.acquire(1)).has_value();
            observation.woke.push_back(name + (bothTaken ? ":两格" : ":缺一格"));
        }

        /**
         * @brief 把协程投进本地就绪队列（投完不跑，跑哪一拍由用例决定）
         */
        void submit(EventLoop &loop, Task<void> &task)
        {
            loop.scheduler().schedule(task.handle());
        }
    } // namespace

    TEST(SemaphoreTest, GrantsImmediatelyWhenNobodyIsQueuedAndPermitsRemain)
    {
        // 判据：队空且额度够时走快速通道——一拍就把协程跑到结束，一次挂起、一次排队都没有
        EventLoop   loop;
        Semaphore   semaphore(loop, 3);
        Observation observation;

        Task<void> task = acquireAndRecordTask(semaphore, observation, "甲", 2);
        submit(loop, task);
        loop.scheduler().runOne();

        const std::vector<std::string> expectedWoke{"甲:2"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_TRUE(observation.empty.empty());
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 3u); // 协程体结束时许可已归还：这一拍之内完成借与还

        // 额度见底时同一条口不再放行：快速通道只认「队空且有额度」两个条件
        std::optional<Semaphore::Permit> exhausted = semaphore.tryAcquire(3);
        ASSERT_TRUE(exhausted.has_value());
        Observation second;
        Task<void>  blocked = acquireAndRecordTask(semaphore, second, "乙", 1);
        submit(loop, blocked);
        loop.scheduler().runOne();
        EXPECT_TRUE(second.woke.empty());
        EXPECT_EQ(semaphore.waitingCount(), 1u);
    }

    TEST(SemaphoreTest, KeepsTheNextAcquirerQueuedUntilAPermitComesBack)
    {
        // 判据：额度见底时后来者挂起，挂上队时不拿额度；归还之后才被叫醒
        EventLoop   loop;
        Semaphore   semaphore(loop, 2);
        Observation observation;

        std::optional<Semaphore::Permit> holderFirst  = semaphore.tryAcquire(1);
        std::optional<Semaphore::Permit> holderSecond = semaphore.tryAcquire(1);
        ASSERT_TRUE(holderFirst.has_value());
        ASSERT_TRUE(holderSecond.has_value());
        ASSERT_EQ(semaphore.availablePermitCount(), 0u);

        Task<void> waiting = acquireAndRecordTask(semaphore, observation, "丙", 1);
        submit(loop, waiting);
        loop.scheduler().runAll();

        EXPECT_TRUE(observation.woke.empty());
        EXPECT_EQ(semaphore.waitingCount(), 1u);

        holderFirst.reset(); // 归还一格：放行发生在这一刻，恢复排在下一拍
        loop.scheduler().runAll();

        const std::vector<std::string> expectedWoke{"丙:1"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 1u); // 丙 当场放手，那一格又回到池子里
    }

    TEST(SemaphoreTest, WakesWaitersInRequestOrderOnePerRelease)
    {
        // 判据：三个等待者按申请先后醒，且「放一格只叫醒一个」。用 runOne() 逐拍钉死，
        // 不靠 runAll() 一趟清空（那样顺序只是碰巧）；排队顺序逐个投逐个跑，
        // 因为本地就绪队列是后进先出的栈，批量投再批量跑会把申请顺序倒过来
        EventLoop   loop;
        Semaphore   semaphore(loop, 1);
        Observation observation;

        std::optional<Semaphore::Permit> holder = semaphore.tryAcquire(1);
        ASSERT_TRUE(holder.has_value());

        Task<void> first  = acquireAndRecordTask(semaphore, observation, "甲", 1);
        Task<void> second = acquireAndRecordTask(semaphore, observation, "乙", 1);
        Task<void> third  = acquireAndRecordTask(semaphore, observation, "丙", 1);
        for (Task<void> *task: {&first, &second, &third})
        {
            submit(loop, *task);
            loop.scheduler().runOne(); // 各自跑到挂起点
        }
        ASSERT_EQ(semaphore.waitingCount(), 3u);

        holder.reset();

        loop.scheduler().runOne(); // 只叫醒队头
        const std::vector<std::string> expectedAfterFirst{"甲:1"};
        EXPECT_EQ(observation.woke, expectedAfterFirst);
        EXPECT_EQ(semaphore.waitingCount(), 2u);

        loop.scheduler().runOne(); // 甲 放手之后才轮到第二个
        const std::vector<std::string> expectedAfterSecond{"甲:1", "乙:1"};
        EXPECT_EQ(observation.woke, expectedAfterSecond);
        EXPECT_EQ(semaphore.waitingCount(), 1u);

        loop.scheduler().runOne();
        const std::vector<std::string> expectedAll{"甲:1", "乙:1", "丙:1"};
        EXPECT_EQ(observation.woke, expectedAll);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 1u);
    }

    TEST(SemaphoreTest, DoesNotLetALaterArrivalBargePastABlockedQueueHead)
    {
        // 判据：队头要 5 个而手上只有 1 个时，后面只要 1 个的等待者也必须排队。
        // 顺序是这条判据的一部分：先让队头排上、再放出一格（此刻池子里有额度而队头满足不了），
        // 然后才让小队尾来——它一来就有额度可拿，抢与不抢正好在这里分开
        EventLoop   loop;
        Semaphore   semaphore(loop, 5);
        Observation observation;

        std::optional<Semaphore::Permit> heavyHold   = semaphore.tryAcquire(3);
        std::optional<Semaphore::Permit> firstSlack  = semaphore.tryAcquire(1);
        std::optional<Semaphore::Permit> secondSlack = semaphore.tryAcquire(1);
        ASSERT_TRUE(heavyHold.has_value());
        ASSERT_TRUE(firstSlack.has_value());
        ASSERT_TRUE(secondSlack.has_value());
        ASSERT_EQ(semaphore.availablePermitCount(), 0u);

        Task<void> heavyWaiter = acquireAndRecordTask(semaphore, observation, "大队头", 5);
        submit(loop, heavyWaiter);
        loop.scheduler().runOne();
        ASSERT_EQ(semaphore.waitingCount(), 1u);

        secondSlack.reset(); // 放出 1 个：队头要 5 个，满足不了，这格只能搁在池子里
        loop.scheduler().runAll();
        ASSERT_EQ(semaphore.availablePermitCount(), 1u);
        ASSERT_TRUE(observation.woke.empty());

        Task<void> lightWaiter = acquireAndRecordTask(semaphore, observation, "小队尾", 1);
        submit(loop, lightWaiter);
        loop.scheduler().runOne();
        // 明明有 1 格可用，小队尾也不许越过队头当场拿走
        EXPECT_TRUE(observation.woke.empty());
        EXPECT_EQ(semaphore.waitingCount(), 2u);
        EXPECT_EQ(semaphore.availablePermitCount(), 1u);

        firstSlack.reset(); // 再放 1 个：手上 2 格，仍然凑不齐队头，还是不许给小队尾
        loop.scheduler().runAll();
        EXPECT_TRUE(observation.woke.empty());
        EXPECT_EQ(semaphore.waitingCount(), 2u);
        EXPECT_EQ(semaphore.availablePermitCount(), 2u);

        heavyHold.reset(); // 放满 5 个：队头凑够了，先它醒，然后才轮到小队尾
        loop.scheduler().runAll();
        const std::vector<std::string> expectedWoke{"大队头:5", "小队尾:1"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 5u);
    }

    TEST(SemaphoreTest, GivesThePermitBackWhenAGrantedWaiterDiesBeforeResuming)
    {
        // 判据：许可已记到等待者账上、恢复还没执行，此时销毁协程帧——许可必须回到池子里。
        // 不还就是许可泄漏，现场只表现为「没人再进得去」
        EventLoop   loop;
        Semaphore   semaphore(loop, 1);
        Observation observation;

        std::optional<Semaphore::Permit> holder = semaphore.tryAcquire(1);
        ASSERT_TRUE(holder.has_value());

        {
            Task<void> waiter = acquireAndRecordTask(semaphore, observation, "甲", 1);
            submit(loop, waiter);
            loop.scheduler().runOne();
            ASSERT_EQ(semaphore.waitingCount(), 1u);

            holder.reset(); // 授额给甲，并把恢复排到下一拍——这一拍还不跑
            ASSERT_EQ(semaphore.availablePermitCount(), 0u);
            ASSERT_EQ(semaphore.waitingCount(), 1u);
            // 出作用域：帧在挂起点上被销毁（取消、异常展开都是这个形状）。等待器析构要撤销凭证、
            // 摘掉队列，并把已记账没用上的许可还回去
        }

        loop.scheduler().runAll(); // 凭证已撤销：这一拍不该恢复任何人
        EXPECT_TRUE(observation.woke.empty());
        EXPECT_TRUE(observation.empty.empty());
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 1u);

        // 闸门没被堵死：后来者照常拿得到
        Observation followUp;
        Task<void>  next = acquireAndRecordTask(semaphore, followUp, "乙", 1);
        submit(loop, next);
        loop.scheduler().runAll();
        const std::vector<std::string> expectedWoke{"乙:1"};
        EXPECT_EQ(followUp.woke, expectedWoke);
    }

    TEST(SemaphoreTest, AbandonsWaitersQuietlyWhenTheSemaphoreDiesFirst)
    {
        // 判据：谁都拿不到额度时（容量 0），等待者只能由各自的持有者收走。销毁本对象之后
        // 既不叫醒、也不许被摸——等待器析构看到「已脱离队列」就什么都不做（释放后使用交给 ASan 判）
        EventLoop               loop;
        Observation             observation;
        std::vector<Task<void>> frames;
        auto                    semaphore = std::make_unique<Semaphore>(loop, 0);

        frames.push_back(acquireAndRecordTask(*semaphore, observation, "甲", 1));
        submit(loop, frames.back());
        loop.scheduler().runOne();
        ASSERT_EQ(semaphore->waitingCount(), 1u);

        semaphore.reset(); // 持有者先死：队上那位从此没人管，也不该被叫醒
        EXPECT_TRUE(observation.woke.empty());
        EXPECT_TRUE(observation.empty.empty());

        frames.clear(); // 帧随后销毁：这一步若去摸已释放的信号量，ASan 当场报
        EXPECT_TRUE(observation.woke.empty());
        EXPECT_TRUE(observation.empty.empty());
    }

    TEST(SemaphoreTest, GrantsNothingToAWaiterWokenAfterTheSemaphoreDied)
    {
        // 判据：授额之后、恢复之前本对象被销毁，等待者醒来必须拿到空结果——
        // 给一个指向已死对象的 Permit，等于让调用方以为自己进了临界区
        EventLoop               loop;
        Observation             observation;
        std::vector<Task<void>> frames;
        auto                    semaphore = std::make_unique<Semaphore>(loop, 1);

        std::optional<Semaphore::Permit> holder = semaphore->tryAcquire(1);
        ASSERT_TRUE(holder.has_value());

        frames.push_back(acquireAndRecordTask(*semaphore, observation, "甲", 1));
        submit(loop, frames.back());
        loop.scheduler().runOne();
        ASSERT_EQ(semaphore->waitingCount(), 1u);

        holder.reset(); // 授额给甲，恢复排在下一拍
        semaphore.reset();
        loop.scheduler().runAll(); // 凭证没被撤销，甲照醒——但手上是空的

        const std::vector<std::string> expectedEmpty{"甲"};
        EXPECT_EQ(observation.empty, expectedEmpty);
        EXPECT_TRUE(observation.woke.empty());
    }

    TEST(SemaphoreTest, LetsTheSameCoroutineTakeASecondPermitWhileHoldingTheFirst)
    {
        // 判据：攥着一格的协程再次排队时，第二次取许可照样能成——额度不是按「谁已经拿过」给的，
        // 而是按队列与余额给的。这条形状是给「同一协程分两段占用额度」的用法钉的门
        EventLoop   loop;
        Semaphore   semaphore(loop, 2);
        Observation observation;
        SuspendGate gate;

        Task<void> doubleTake = takeTwiceTask(semaphore, observation, "甲", gate);
        submit(loop, doubleTake);
        loop.scheduler().runOne(); // 第一格走快速通道到手，甲 停在门上
        ASSERT_TRUE(observation.woke.empty());

        std::optional<Semaphore::Permit> other = semaphore.tryAcquire(1); // 把最后一格占住，逼甲 排队
        ASSERT_TRUE(other.has_value());

        gate.open(); // 甲 醒来取第二格：队空但余额为 0，只能挂起
        ASSERT_TRUE(observation.woke.empty());
        ASSERT_EQ(semaphore.waitingCount(), 1u);

        other.reset();
        loop.scheduler().runAll();

        const std::vector<std::string> expectedWoke{"甲:两格"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_TRUE(observation.empty.empty());
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 2u);
    }

    TEST(SemaphoreTest, TakesTwoPermitsInsideOneExpressionWithoutQueueingBehindItsOwnCompletedWaiter)
    {
        // 判据：一整条表达式里连着取两格许可，且第一次是真的挂起过再被叫醒的——第一个等待器
        // 要到整条语句结束才析构，实现若不在取走许可的那一刻离队，第二次取许可就会排在自家那条
        // 旧等待器后面，而旧等待器还攥着第一格谁也唤不醒，两次取许可于是互相等死
        EventLoop   loop;
        Semaphore   semaphore(loop, 2);
        Observation observation;

        std::optional<Semaphore::Permit> all = semaphore.tryAcquire(2); // 先把池子清空，逼第一次挂起
        ASSERT_TRUE(all.has_value());

        Task<void> task = takeTwiceInOneExpressionTask(semaphore, observation, "甲");
        submit(loop, task);
        loop.scheduler().runOne();
        ASSERT_TRUE(observation.woke.empty());
        ASSERT_EQ(semaphore.waitingCount(), 1u);

        all.reset(); // 两格一起放回：队头记走一格，另一格留在池子里等着第二次取用
        loop.scheduler().runAll();

        const std::vector<std::string> expectedWoke{"甲:两格"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 2u); // 两格都随整条语句结束还回来了
    }

    TEST(SemaphoreTest, DoesNotChargeTheSameWaiterTwiceWhenTwoReleasesPrecedeItsWakeUp)
    {
        // 判据：许可在「已记账、还没恢复」的窗口里仍然记在同一位等待者账上——这窗口里再来一次放行，
        // 不许把同一个等待者再扣一遍。扣两遍的现场是额度凭空少一格，谁也说不清是谁借走的
        EventLoop   loop;
        Semaphore   semaphore(loop, 5);
        Observation observation;

        std::optional<Semaphore::Permit> all = semaphore.tryAcquire(5);
        ASSERT_TRUE(all.has_value());

        Task<void> waiter = acquireAndRecordTask(semaphore, observation, "甲", 3);
        submit(loop, waiter);
        loop.scheduler().runOne();
        ASSERT_EQ(semaphore.waitingCount(), 1u);

        all.reset(); // 一次归还 5 格：甲 记走 3 格，余额 2 格，恢复排在下一拍
        ASSERT_EQ(semaphore.availablePermitCount(), 2u);
        ASSERT_TRUE(observation.woke.empty());

        semaphore.release(2);                            // 甲 还没醒，此时手上又多了 2 格：既不能再记给甲，也不该有人被扣
        EXPECT_EQ(semaphore.availablePermitCount(), 4u); // 上限是容量，多放的那两格仍然在池子里

        loop.scheduler().runAll();
        const std::vector<std::string> expectedWoke{"甲:3"};
        EXPECT_EQ(observation.woke, expectedWoke);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
        EXPECT_EQ(semaphore.availablePermitCount(), 5u); // 一格都没凭空少
    }

    TEST(SemaphoreTest, RejectsZeroPermitCountOnEveryEntry)
    {
        // 判据：0 个许可既不是「立刻成功」也不是「永远等不到」，而是用法错——当场抛，且不改账
        EventLoop loop;
        Semaphore semaphore(loop, 2);

        EXPECT_THROW(static_cast<void>(semaphore.acquire(0)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(semaphore.tryAcquire(0)), Base::InvalidArgumentException);
        EXPECT_THROW(semaphore.release(0), Base::InvalidArgumentException);
        EXPECT_EQ(semaphore.availablePermitCount(), 2u);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
    }

    TEST(SemaphoreTest, RejectsReleaseThatWouldExceedCapacityWithoutTouchingTheLedger)
    {
        // 判据：多放一次是把门多开一格，必须抛且抛之前不改状态——静默放行的话超发的许可谁也数不清
        EventLoop loop;
        Semaphore semaphore(loop, 2);

        EXPECT_THROW(semaphore.release(1), Base::LogicException); // 一格都没出去，放回来就是超容量
        EXPECT_EQ(semaphore.availablePermitCount(), 2u);

        std::optional<Semaphore::Permit> holder = semaphore.tryAcquire(1);
        ASSERT_TRUE(holder.has_value());
        EXPECT_THROW(semaphore.release(2), Base::LogicException); // 外面只有 1 格，放 2 个多了一格
        EXPECT_EQ(semaphore.availablePermitCount(), 1u);

        holder.reset(); // 正常归还：账回到容量
        EXPECT_EQ(semaphore.availablePermitCount(), 2u);
        EXPECT_EQ(semaphore.waitingCount(), 0u);
    }

    TEST(SemaphoreTest, RefusesForeignThreadUseWhileTheLoopIsRunning)
    {
        // 判据：循环正在别处跑时，外来线程既不能排队、不能放行，也不能抢走额度（计数与链表都没锁）。
        // 这条要一条真在跑的循环才造得出前提；循环停下后同一线程照常用（构造期与停机后的顺序交接是既有形态）
        EventLoopThread runner;
        ASSERT_TRUE(runner.waitUntilRunning());

        Semaphore               semaphore(runner.loop(), 2);
        Observation             observation;
        std::vector<Task<void>> frames;

        EXPECT_THROW(static_cast<void>(semaphore.acquire(1)), Base::LogicException);
        EXPECT_THROW(static_cast<void>(semaphore.tryAcquire(1)), Base::LogicException);
        EXPECT_THROW(semaphore.release(1), Base::LogicException);
        EXPECT_EQ(semaphore.availablePermitCount(), 2u); // 三次拒绝都没改账

        runner.join(); // 循环停下：判据放开
        std::optional<Semaphore::Permit> permit = semaphore.tryAcquire(1);
        EXPECT_TRUE(permit.has_value());
        EXPECT_NO_THROW(frames.push_back(acquireAndRecordTask(semaphore, observation, "甲", 1)));
        EXPECT_EQ(observation.woke.size(), 0u); // 只是构造了帧，没投给循环，也就没跑
    }
} // namespace AsynGyanis::Core
