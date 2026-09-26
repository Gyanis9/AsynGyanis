// LogThrottle 单元测试：首条即时放行、窗口内只压条数、下一窗口带出被压计数、
// 零间隔退回不压、多线程抢同一拍只放行一条、每个调用点各一份状态

#include "Base/Log/LogThrottle.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        using namespace std::chrono_literals;

        /// 一个「肯定跨不过去」的窗口：用它测压制的稳态，用例本身不必等待
        constexpr auto kHugeInterval = 3600s;

        /// 一个「一次睡眠肯定跨得过去」的窗口：用它测窗口到期，睡眠只保证下界因此不会误判
        constexpr auto kShortInterval = 250ms;

        /// 跨过 kShortInterval 所需的睡眠：只保证下界，故窗口到期一侧的判定是确定的
        constexpr auto kWindowSleep = 400ms;

        /// 抢同一拍的线程数（供「同一瞬到齐」那条用例用）
        constexpr std::size_t kRacingThreadCount = 8U;

        /// 高频压一遍时每个线程按压的次数
        constexpr std::size_t kHammerAttemptsPerThread = 500U;

        /**
         * @brief 连按 n 次 acquire，回报其中放行了几条
         * @param throttle 待探测的闸门
         * @param attemptCount 按压次数
         * @return 放行的条数
         */
        std::size_t pressTimes(LogThrottle &throttle, const std::size_t attemptCount)
        {
            std::size_t passedCount = 0U;
            for (std::size_t attemptIndex = 0; attemptIndex < attemptCount; ++attemptIndex)
            {
                passedCount += throttle.acquire() ? 1U : 0U;
            }
            return passedCount;
        }

        /**
         * @brief 一个只用自身那份闸门、只回报是否放行的调用点
         * @return 本次是否放行
         * @details 宏展开在函数体内，因此那份函数局部 static 只属于本调用点；间隔在这里定死，
         *          为的是让「跨调用点共用状态」这一缺陷只能靠两个调用点各按一次来暴露
         */
        bool pressFirstCallSite()
        {
            return ASYN_LOG_THROTTLED(kHugeInterval).acquire();
        }

        /// 与上一份互不相干的第二个调用点
        bool pressSecondCallSite()
        {
            return ASYN_LOG_THROTTLED(kHugeInterval).acquire();
        }

        /// 第三个调用点：专测「状态跨调用留存」，不与上面两处共享窗口
        bool pressThirdCallSite()
        {
            return ASYN_LOG_THROTTLED(kHugeInterval).acquire();
        }

        /// 一次调用点读到的两样东西：是否放行，以及放行时交出的被压条数
        struct CallSiteOutcome
        {
            bool passed{false};       ///< 本次是否放行
            std::uint64_t droppedCount{0U}; ///< 上一段被压掉的条数
        };

        /// 第四个调用点：带间隔，专测宏这一侧能不能读出计数
        CallSiteOutcome pressFourthCallSite()
        {
            auto &throttle = ASYN_LOG_THROTTLED(kShortInterval);
            const bool passed = throttle.acquire();
            return CallSiteOutcome{passed, passed ? throttle.droppedCount() : 0U};
        }
    } // namespace

    TEST(LogThrottle, PassesTheFirstHitWithoutWaitingForTheWindow)
    {
        LogThrottle throttle(kHugeInterval);

        // 首条必须当场出去：运维要看得见「有人在打这个端口」的第一起，而不是等一整个窗口
        EXPECT_TRUE(throttle.acquire());
        EXPECT_EQ(throttle.droppedCount(), 0U) << "刚建好的闸门还没有压过任何一条";
    }

    TEST(LogThrottle, SuppressesEverythingInsideTheWindow)
    {
        LogThrottle throttle(kHugeInterval);
        ASSERT_TRUE(throttle.acquire());

        // 被压掉的条数只在放行那一刻才交出去，所以窗口内读它是 0
        EXPECT_EQ(pressTimes(throttle, 64U), 0U) << "窗口内还在放行：压制没有生效";
        EXPECT_EQ(throttle.droppedCount(), 0U);
    }

    TEST(LogThrottle, FoldsTheSuppressedCountIntoTheNextWindow)
    {
        LogThrottle throttle(kShortInterval);
        ASSERT_TRUE(throttle.acquire());
        static_cast<void>(pressTimes(throttle, 7U));

        std::this_thread::sleep_for(kWindowSleep);
        EXPECT_TRUE(throttle.acquire()) << "窗口到期后仍不放行：闸门会把这条告警永久吞掉";
        EXPECT_EQ(throttle.droppedCount(), 7U) << "放行那条没有带上被压掉的条数：量级信息丢了";
    }

    TEST(LogThrottle, CountsEachWindowSeparatelyRatherThanAccumulating)
    {
        LogThrottle throttle(kShortInterval);
        ASSERT_TRUE(throttle.acquire());
        static_cast<void>(pressTimes(throttle, 9U));
        std::this_thread::sleep_for(kWindowSleep);
        ASSERT_TRUE(throttle.acquire());
        ASSERT_EQ(throttle.droppedCount(), 9U);

        static_cast<void>(pressTimes(throttle, 2U));
        std::this_thread::sleep_for(kWindowSleep);
        ASSERT_TRUE(throttle.acquire());
        // 报的是「上一段压掉多少」，不是自开闸以来的总数：运维据此判断的是当下这一段的频率
        EXPECT_EQ(throttle.droppedCount(), 2U) << "计数在累加：读出来的频率会越看越大";
    }

    TEST(LogThrottle, KeepsThePreviousCountWhileTheCurrentWindowIsStillSuppressed)
    {
        LogThrottle throttle(kShortInterval);
        ASSERT_TRUE(throttle.acquire());
        static_cast<void>(pressTimes(throttle, 5U));
        std::this_thread::sleep_for(kWindowSleep);
        ASSERT_TRUE(throttle.acquire());
        ASSERT_EQ(throttle.droppedCount(), 5U);

        // 下一次放行之前，被压掉的条数不该改动已经交出去的那个读数
        static_cast<void>(pressTimes(throttle, 4U));
        EXPECT_EQ(throttle.droppedCount(), 5U) << "压制分支改写了已交出的读数：那条正在写的日志会跟着变";
    }

    TEST(LogThrottle, TreatsNonPositiveIntervalAsNoSuppression)
    {
        LogThrottle zeroInterval(std::chrono::milliseconds{0});
        EXPECT_EQ(pressTimes(zeroInterval, 16U), 16U) << "间隔为 0 时仍在压制：这条路径等于被关掉了";
        EXPECT_EQ(zeroInterval.droppedCount(), 0U);

        LogThrottle negativeInterval(std::chrono::milliseconds{-5});
        EXPECT_EQ(pressTimes(negativeInterval, 16U), 16U) << "间隔为负时仍在压制";
    }

    /**
     * @brief 多条线程抢同一拍时只放行一条，且高频并发下窗口照旧推进
     * @details 这条钉的是「窗口会推进」与「至少放行一条」两端——把窗口起点写成常量（永不推进）会放行
     *          4000 条，把判定反过来会一条都不放。它**钉不住** CAS 本身：把
     *          `compare_exchange_strong` 换成「先 load 判断、再 store 立窗口」，本机 10 次 × 20 轮 ×
     *          8 线程一次都没撞上过（load→store 之间只有几纳秒，而线程放开彼此的间隔是它的百倍量级）。
     *          那条改动留在实现里是为了语义精确，不当作可测判据写在这里——写一条测不出来的断言，比不写更容易骗人。
     */
    TEST(LogThrottle, HandsTheWindowToExactlyOneThreadWhenTheyRace)
    {
        // 起跑线用 std::barrier：八条线程在栅栏上转够圈才一起放开，去 acquire 的那一瞬彼此只差几十纳秒。
        // 各线程自己跑、由主线程 notify 放开的写法测不出并发（唤醒本身被内核串行化），实测过
        constexpr std::size_t kRoundCount = 20U;
        for (std::size_t roundIndex = 0; roundIndex < kRoundCount; ++roundIndex)
        {
            LogThrottle                 throttle(kHugeInterval);
            std::atomic<std::size_t>    passedCount{0U};
            std::barrier                goLine{static_cast<int>(kRacingThreadCount)};
            std::vector<std::thread>    threads;
            threads.reserve(kRacingThreadCount);
            for (std::size_t threadIndex = 0; threadIndex < kRacingThreadCount; ++threadIndex)
            {
                threads.emplace_back(
                        [&throttle, &passedCount, &goLine]
                        {
                            goLine.arrive_and_wait();
                            passedCount.fetch_add(throttle.acquire() ? 1U : 0U, std::memory_order_relaxed);
                        });
            }
            for (std::thread &thread: threads)
            {
                thread.join();
            }

            // 一轮里只可能放行一条：首条必过，其余七条撞在同一个窗口里
            EXPECT_EQ(passedCount.load(), 1U) << "第 " << roundIndex << " 轮同一拍放行/吞掉了多条";
        }

        // 再压一遍多线程高频调用：这一段的目的是让窗口推进与计数在并发下不丢、不死锁
        // （也是这份实现在 Linux TSan 作业上会被看着跑的一段）
        LogThrottle                 hammeredThrottle(kHugeInterval);
        std::atomic<std::size_t>    hammeredPassedCount{0U};
        std::vector<std::thread>    hammerThreads;
        hammerThreads.reserve(kRacingThreadCount);
        for (std::size_t threadIndex = 0; threadIndex < kRacingThreadCount; ++threadIndex)
        {
            hammerThreads.emplace_back(
                    [&hammeredThrottle, &hammeredPassedCount]
                    {
                         hammeredPassedCount.fetch_add(pressTimes(hammeredThrottle, kHammerAttemptsPerThread),
                                                       std::memory_order_relaxed);
                    });
        }
        for (std::thread &thread: hammerThreads)
        {
            thread.join();
        }
        EXPECT_EQ(hammeredPassedCount.load(), 1U) << "多线程高频调用下窗口没有推进，或者一条都没放行";
    }

    TEST(LogThrottle, GivesEachCallSiteItsOwnWindow)
    {
        // 两处都在压制的稳态里：第二个调用点若与第一个共用状态，就会被误压掉
        EXPECT_TRUE(pressFirstCallSite());
        EXPECT_TRUE(pressSecondCallSite()) << "两个调用点共用了一份窗口：A 点的洪水把 B 点也压掉了";
        EXPECT_FALSE(pressFirstCallSite());
        EXPECT_FALSE(pressSecondCallSite());
    }

    TEST(LogThrottle, KeepsItsWindowStateAcrossCalls)
    {
        EXPECT_TRUE(pressThirdCallSite());
        EXPECT_FALSE(pressThirdCallSite()) << "宏每次展开都新建闸门：那等于一条都不压";
    }

    TEST(LogThrottle, ReportsTheSuppressedCountThroughTheMacroCallSite)
    {
        ASSERT_TRUE(pressFourthCallSite().passed);
        for (std::size_t attemptIndex = 0; attemptIndex < 3U; ++attemptIndex)
        {
            EXPECT_FALSE(pressFourthCallSite().passed);
        }
        std::this_thread::sleep_for(kWindowSleep);
        const CallSiteOutcome outcome = pressFourthCallSite();
        EXPECT_TRUE(outcome.passed);
        EXPECT_EQ(outcome.droppedCount, 3U) << "宏这一侧读不到被压掉的条数：放行的那条日志带不出量级";
    }

} // namespace AsynGyanis::Base
