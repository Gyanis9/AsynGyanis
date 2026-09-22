// Timer 单元测试：循环级定时器队列上的构造、到期唤醒、到期顺序与取消
//
// 用例手工推进事件循环（复刻 EventLoop::run() 的「分发事件 + 清空调度队列」两步），
// 不引入循环线程，因此时序由用例自己掌握；真实定时器仍需等待内核到期，超时判失败而不是把用例挂住。

#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::advanceUntil;
        using TestSupport::kWaitTimeout;

        /// 参与交错取消的定时器条数：堆被撑到好几层，取消点因此散落在顶、中、尾各处
        constexpr std::size_t kScrambledTimerCount = 18;

        /**
         * @brief 第 index 个定时器等待的毫秒数
         * @details 只有 15/45/135/405 这四个短值，相邻之间相差三倍：登记十几条等待只花掉毫秒级，
         *          再慢的机器也颠倒不了它们的先后，到期顺序因此只由堆序决定。
         *          其余一律 30 秒量级，用例期间永不到期，只负责把堆撑大、把取消点摊到各层
         */
        constexpr std::array<long long, kScrambledTimerCount> kScrambledDurationsMs
            {30000, 15, 30001, 45, 30002, 30003, 30004, 135, 30005,
             30006, 30007, 405, 30008, 30009, 30010, 30011, 30012, 30013};

        /// 截止时刻最短的一批里没被取消的下标，按截止时间升序——这就是预期的醒来顺序
        constexpr std::array<std::size_t, 3> kExpectedFiredIndexes{1, 7, 11};

        /// 每隔两个取消一条时被摘掉的条数（下标 0、3、6…）
        constexpr std::size_t kCancelledTimerCount = (kScrambledTimerCount + 2) / 3;

        /// 第 index 个定时器的等待时长
        std::chrono::milliseconds scrambledTimerDuration(const std::size_t index) noexcept
        {
            return std::chrono::milliseconds(kScrambledDurationsMs[index]);
        }

        /**
         * @brief 等一个截止时刻被打乱的定时器，醒来后把它的下标记进顺序表
         * @details 下标走函数形参而不是闭包捕获：Task 是惰性的，协程帧只按地址记住闭包，
         *          「构造后立即调用的临时闭包」在语句结束时就已销毁，恢复时读捕获即释放后使用
         */
        Task<> recordFiringOnExpiry(Timer &timer, std::vector<std::size_t> &firedIndexes, const std::size_t index)
        {
            co_await timer.waitFor(scrambledTimerDuration(index));
            firedIndexes.push_back(index);
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
        [[maybe_unused]] auto awaiter = timer.waitFor(std::chrono::milliseconds(100));
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
     * @brief 同一批里到期的多个定时器仍按截止时间先后唤醒（到期时刻都已经过去的情形）
     * @details 与上一条的区别在于时序：先让两个定时器都已过期，再推进循环——一次分发里
     *          会同时捞出两个等待器。此时唤醒顺序只能由「投递给调度器的顺序」决定，
     *          而调度器的本地队列是 LIFO：正序投递会让更晚截止的先跑（CI 上实测到过）。
     *          用例刻意不等内核按各自截止时间分两次唤醒，因此钉住的正是这条批量路径。
     */
    TEST(Timer, BatchExpiredTimersFireInDeadlineOrder)
    {
        EventLoop loop;
        Timer     timer(loop);

        std::vector<int> firedOrder;

        auto laterBody = [&timer, &firedOrder]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(40));
            firedOrder.push_back(1);
        };
        auto later = laterBody();
        later.handle().resume();

        auto earlierBody = [&timer, &firedOrder]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(10));
            firedOrder.push_back(2);
        };
        auto earlier = earlierBody();
        earlier.handle().resume();

        // 两个截止时刻都过去之后再推进循环：这一次分发同时捞出两个等待器
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        ASSERT_TRUE(advanceUntil(loop, [&firedOrder] { return firedOrder.size() >= 2U; }, kWaitTimeout)) << "两个定时器没有都在时限内到期";
        ASSERT_EQ(firedOrder.size(), 2U);
        EXPECT_EQ(firedOrder[0], 2) << "同批到期时更早截止的定时器没有先跑：投递顺序与调度器的 LIFO 相反";
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

    /**
     * @brief 交错取消一批等待者后，剩下的按截止时间先后到期，被取消的一个都不许醒来
     * @details 钉住的是「取消靠等待器自带的堆下标定位、补位后上下浮」这条路径：登记顺序与
     *          截止顺序彻底错开，取消点因此散落在堆顶、堆中与堆尾各个位置；任何一次漏回写下标
     *          或漏下沉都会表现为到期顺序错乱、漏唤醒或多唤醒。
     *          参与到期断言的只有间隔成倍的几条，其余挂在 30 秒外，因此用例不断言真实墙钟差，
     *          登记的毫秒级耗时也颠倒不了它们的先后
     */
    TEST(Timer, InterleavedCancellationsKeepDeadlineOrder)
    {
        EventLoop loop;
        Timer     timer(loop);

        std::vector<std::size_t> firedIndexes;
        std::vector<std::unique_ptr<Task<> > > waiters;
        waiters.reserve(kScrambledTimerCount);

        for (std::size_t index = 0; index < kScrambledTimerCount; ++index)
        {
            waiters.push_back(std::make_unique<Task<> >(recordFiringOnExpiry(timer, firedIndexes, index)));
            waiters.back()->handle().resume();
        }
        ASSERT_EQ(loop.timerQueue().pendingCount(), kScrambledTimerCount);

        // 每隔两个取消一条：短截止的那条也在其中，堆顶/堆中/堆尾的摘除路径一次全走到
        for (std::size_t index = 0; index < kScrambledTimerCount; index += 3)
        {
            waiters[index].reset();
        }
        EXPECT_EQ(loop.timerQueue().pendingCount(), kScrambledTimerCount - kCancelledTimerCount)
            << "取消后堆里剩的项数不对：补位或下标回写漏了";

        // 等到最后一条短截止到期：被取消的那条 45 毫秒此刻早已越过时限，它若漏摘就会多出一条记录
        ASSERT_TRUE(advanceUntil(loop, [&firedIndexes] { return firedIndexes.size() >= kExpectedFiredIndexes.size(); },
                                 kWaitTimeout))
            << "有未被取消的定时器没有到期：取消后的堆顶或重新武装被弄坏了";

        EXPECT_EQ(firedIndexes, std::vector<std::size_t>(kExpectedFiredIndexes.begin(), kExpectedFiredIndexes.end()))
            << "醒来的不是「按截止时间升序的、未被取消的那几条」：堆序、取消定位或补位坏了";

        // 三条已摘走，剩下的只有远截止那几条：它们一条都不该被派发
        EXPECT_EQ(loop.timerQueue().pendingCount(), kScrambledTimerCount - kCancelledTimerCount - kExpectedFiredIndexes.size())
            << "未到期的定时器被误摘或误派发";

        // 收尾销毁全部等待者：摘除点再次覆盖堆的各层，漏回写下标会在这里留下悬空登记
        waiters.clear();
        EXPECT_EQ(loop.timerQueue().pendingCount(), 0U) << "销毁全部等待者后堆里还有残留";
    }

    /**
     * @brief 截止时间从「真正挂起」那一刻算起，而不是等待器构造那一刻
     * @details 先构造等待器、干一会儿事再 co_await，等的是挂起时刻起的那段时长。
     *          若截止时间在构造时就定下，这里会立刻「早就到期」——用例先睡够再挂起，
     *          因此立刻完成与等满时长是可区分的
     */
    TEST(Timer, WaitsFullDurationWhenAwaitedLate)
    {
        EventLoop loop;
        Timer     timer(loop);

        // 构造等待器后先让时间过去：构造时刻起算的截止时间此刻已经过期
        auto awaiter = timer.waitFor(std::chrono::milliseconds(80));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        bool isExpired    = false;
        auto waitingBody = [&awaiter, &isExpired]() -> Task<>
        {
            co_await awaiter;
            isExpired = true;
        };
        auto waiting = waitingBody();
        waiting.handle().resume();

        // 挂起后 30ms 内不应到期：此刻到期的只可能是「构造时起算」的旧截止时间
        EXPECT_FALSE(advanceUntil(loop, [&isExpired] { return isExpired; }, std::chrono::milliseconds(30)))
                << "等待器在被 co_await 之前就算到期了：截止时间应当在挂起时才算出";
        EXPECT_FALSE(isExpired);

        // 再给足时间：正常路径下 80ms 的等待必须真的完成
        EXPECT_TRUE(advanceUntil(loop, [&isExpired] { return isExpired; }, kWaitTimeout)) << "80ms 的等待没有在时限内到期";
    }

    /**
     * @brief 时长大到无法与当前时刻相加时按「很久」处理，而不是溢出成过去的时刻立刻到期
     */
    TEST(Timer, HugeDurationDoesNotWrapIntoImmediateExpiry)
    {
        EventLoop loop;
        Timer     timer(loop);

        bool isExpired    = false;
        auto waitingBody = [&timer, &isExpired]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds::max());
            isExpired = true;
        };
        auto waiting = waitingBody();
        waiting.handle().resume();

        // 溢出成负值的话截止时间落在过去，这一次推进就会把它捞出来
        EXPECT_FALSE(advanceUntil(loop, [&isExpired] { return isExpired; }, std::chrono::milliseconds(50)))
                << "milliseconds::max() 的等待立刻完成了：截止时间在相加时溢出";
        EXPECT_FALSE(isExpired);
        EXPECT_EQ(loop.timerQueue().pendingCount(), 1U);
    }

    /**
     * @brief 已到期待恢复的等待者若在此之前被销毁，不会被恢复
     * @details 两个等待者在同一批里到期，先被恢复的那个顺手销毁另一个的帧（连接收尾就是这么做的：
     *          一个协程的收尾销毁同一批里挂着的其它帧）。恢复动作若投的是裸句柄，循环随后会
     *          resume 一块已释放的帧——旧实现下本用例在 ASan 构建里必报释放后使用。
     *          用例只断言「销毁确实发生了、进程没崩」，恢复顺序在两种实现下都是先早截止的那个
     */
    TEST(Timer, WaiterDestroyedBeforeItsResumeIsNotResumed)
    {
        EventLoop loop;
        Timer     timer(loop);

        bool isVictimDestroyed = false;

        // 牺牲者：等待 1ms（截止时间晚于下面那个「尽快到期」的销毁者）
        auto victimBody = [&timer]() -> Task<>
        {
            co_await timer.waitFor(std::chrono::milliseconds(1));
        };
        std::unique_ptr<Task<>> victim = std::make_unique<Task<>>(victimBody());
        victim->handle().resume();

        auto destroyerBody = [&timer, &victim, &isVictimDestroyed]() -> Task<>
        {
            // 非正数时长表示「尽快到期」：截止时间早于牺牲者，因此先被恢复
            co_await timer.waitFor(std::chrono::milliseconds::zero());
            // 恢复途中销毁仍在等待的另一个帧：它的到期恢复可能已经排在这一轮里了
            victim.reset();
            isVictimDestroyed = true;
        };
        auto destroyer = destroyerBody();
        destroyer.handle().resume();

        ASSERT_TRUE(advanceUntil(loop, [&isVictimDestroyed] { return isVictimDestroyed; }, kWaitTimeout)) << "销毁者没有在时限内被恢复";
        EXPECT_TRUE(isVictimDestroyed);
        EXPECT_TRUE(destroyer.isReady());
    }
} // namespace AsynGyanis::Core
