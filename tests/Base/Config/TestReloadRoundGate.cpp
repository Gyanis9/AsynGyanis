// ReloadRoundGate 单元测试：节流协议的单线程状态迁移、并发收尾时「不丢变更」的对照复现，
// 以及多线程下「同一时刻至多一轮占着执行权」的安全不变式。

#include "Base/Config/Detail/ReloadRoundGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace AsynGyanis::Base::Detail
{
    namespace
    {
        /**
         * @brief 被替换掉的「pending + dirty 双旗」写法，原样搬来当对照
         * @details 用例要证明「那条交错真的会丢变更」，否则「新写法不丢」只是自说自话。
         *          步骤逐行照旧实现的顺序：收尾先清 pending、再 exchange 掉 dirty、最后才抢接力。
         */
        struct LegacyDualFlagProtocol
        {
            std::atomic<bool> pending{false}; ///< 是否已有一轮占住执行权
            std::atomic<bool> dirty{false};   ///< 共享的「之后还要再来一轮」欠账

            /**
             * @brief 变更到达：抢到执行权返回 true，抢不到就记欠账
             */
            bool noteChangedAndClaim()
            {
                bool expected = false;
                if (!pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                {
                    dirty.store(true, std::memory_order_release);
                    return false;
                }
                return true;
            }

            /**
             * @brief 交还执行权（旧实现收尾的第一步）
             */
            void handBackClaim()
            {
                pending.store(false, std::memory_order_release);
            }

            /**
             * @brief 消耗欠账并尝试接力（旧实现收尾的第二、三步）
             * @return true 抢到接力，要再跑一轮
             */
            bool consumeDebtAndRelay()
            {
                if (!dirty.exchange(false, std::memory_order_acq_rel))
                {
                    return false;
                }
                bool expected = false;
                return pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
            }

            /**
             * @brief 完整的一轮收尾
             */
            bool finishRound()
            {
                handBackClaim();
                return consumeDebtAndRelay();
            }
        };
    } // namespace

    TEST(ReloadRoundGate, FirstClaimWinsUntilReleased)
    {
        ReloadRoundGate gate;

        EXPECT_TRUE(gate.claimRound());
        EXPECT_FALSE(gate.claimRound()) << "执行权必须一次只交给一轮";

        // 交还时世代号没变过：没有欠账，不该多起一轮
        EXPECT_FALSE(gate.releaseRound(gate.observedGeneration()));
        EXPECT_TRUE(gate.claimRound()) << "交还之后执行权应可再次占到";
    }

    TEST(ReloadRoundGate, ReleaseAfterChangeRelaysExactlyOneMoreRound)
    {
        ReloadRoundGate gate;

        ASSERT_TRUE(gate.claimRound());
        const std::uint64_t observed = gate.observedGeneration();

        gate.noteChanged();
        gate.noteChanged();

        // 两次变更只需要再补一轮：那一轮会把整份目录重读一遍，把两次改动一起收进去
        EXPECT_TRUE(gate.releaseRound(observed));
        EXPECT_EQ(gate.observedGeneration(), observed + 2);
        EXPECT_FALSE(gate.releaseRound(gate.observedGeneration())) << "接力之后没有新变更就不该再续轮";
    }

    /**
     * @brief 并发收尾时，停在中途的旧轮次不许把别人的变更吃掉
     * @details 复现被替换掉的那条交错（旧写法丢变更、新写法不丢）。七步全是确定性的调用顺序，
     *          不依赖线程调度：R1 交还执行权后停在接力之前，R2 在此期间起完又跑完，
     *          最后一笔变更落在 R2 读盘之后
     */
    TEST(ReloadRoundGate, StalledReleaseDoesNotStrandALaterChange)
    {
        ReloadRoundGate gate;

        ASSERT_TRUE(gate.claimRound()); // 1. R1 占到执行权
        const std::uint64_t seenByFirstRound = gate.observedGeneration();
        gate.noteChanged(); // 2. W1：R1 已经读过盘，这次要留给下一轮
        EXPECT_FALSE(gate.claimRound()) << "W1 的事件应看到 R1 还在跑";
        gate.finishRunning(); // 3. R1 交还执行权，然后停在接力之前

        gate.noteChanged(); // 4. W2 到达并起走 R2
        ASSERT_TRUE(gate.claimRound());
        const std::uint64_t seenBySecondRound = gate.observedGeneration();

        gate.noteChanged(); // 5. Wx 落在 R2 读盘之后 —— 必须还有人重读
        EXPECT_FALSE(gate.claimRound()) << "Wx 的事件应看到 R2 还在跑";

        // 6. R1 此刻才继续：它那份快照确实旧了，但只许「比对」不许「消耗」
        EXPECT_FALSE(gate.claimAnotherRound(seenByFirstRound)) << "R1 抢不到执行权时不该顺手清掉欠账";

        // 7. R2 收尾：必须看到 Wx 并接力第三轮，否则配置就永久停在 Wx 之前的旧值
        EXPECT_TRUE(gate.releaseRound(seenBySecondRound)) << "Wx 这笔变更没有任何一轮去重读";
    }

    /**
     * @brief 对照：同一条交错下，旧的「pending + dirty」写法确实把变更丢了
     * @details 这条不是要保留旧行为，而是上一条的证伪证据——若这里也「不丢」，说明我构造的
     *          交错根本不成立，上一条的断言就没有分量
     */
    TEST(ReloadRoundGate, LegacyDualFlagProtocolLosesThatChange)
    {
        LegacyDualFlagProtocol legacy;

        ASSERT_TRUE(legacy.noteChangedAndClaim());  // 1. R1 起轮
        ASSERT_FALSE(legacy.noteChangedAndClaim()); // 2. W1 抢不到执行权，记下欠账
        legacy.handBackClaim();                     // 3. R1 交还执行权后停在 exchange 之前
        ASSERT_TRUE(legacy.noteChangedAndClaim());  // 4. W2 起走 R2（走抢权分支，不动欠账）
        ASSERT_FALSE(legacy.noteChangedAndClaim()); // 5. Wx 抢不到 R2 的执行权，记下欠账

        // 6. R1 继续：把 Wx 的欠账 exchange 掉，却因 R2 仍在跑而抢不到接力 —— 欠账就此消失
        ASSERT_FALSE(legacy.consumeDebtAndRelay()) << "R1 应当既吃掉欠账又接力失败";

        // 7. R2 收尾：dirty 已被 R1 吃掉，它以为没有欠账 —— Wx 再没人重读
        EXPECT_FALSE(legacy.finishRound()) << "对照实现本该在此处丢掉 Wx，否则这条交错没构造出来";
    }

    TEST(ReloadRoundGate, ConcurrentClaimsNeverOverlapRounds)
    {
        ReloadRoundGate  gate;
        std::atomic<int> roundsInFlight{0};
        // fetch_max 只在无符号整数特化上提供（有符号的 atomic<int> 没有这个成员）
        std::atomic<std::uint32_t> maximumRoundsInFlight{0};
        std::atomic<bool>          keepGoing{true};
        constexpr int              kWorkerCount = 4;

        auto worker = [&gate, &roundsInFlight, &maximumRoundsInFlight, &keepGoing]
        {
            // 接力成功等于执行权仍在自己手里：要直接跑下一轮，不能再 claim（自己抢自己永远抢不到）
            bool ownsRound = false;
            for (; keepGoing.load(std::memory_order_relaxed);)
            {
                gate.noteChanged();
                if (!ownsRound)
                {
                    ownsRound = gate.claimRound();
                    if (!ownsRound)
                    {
                        continue;
                    }
                }
                const std::uint64_t observed = gate.observedGeneration();
                // 测量只覆盖「持着执行权」那一段：交还之后别的线程本来就能进来，
                // 把交还也圈进测量区量到的就不是互斥，而是四个线程排队过同一个门
                const std::uint32_t inFlightNow = static_cast<std::uint32_t>(roundsInFlight.fetch_add(1, std::memory_order_relaxed) + 1);
                // 实测 MSVC 的 std::atomic<unsigned int> 没给 fetch_max（C2039），这里按语义自己写 CAS 循环，
                // 两个编译器都走得通；compare_exchange_weak 失败时会把 previous 刷新成当前值
                for (std::uint32_t previous = maximumRoundsInFlight.load(std::memory_order_relaxed);
                     inFlightNow > previous && !maximumRoundsInFlight.compare_exchange_weak(previous, inFlightNow, std::memory_order_relaxed);)
                {
                }
                --roundsInFlight;
                gate.finishRunning();
                ownsRound = gate.claimAnotherRound(observed);
            }
            if (ownsRound)
            {
                gate.finishRunning();
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(kWorkerCount);
        for (int index = 0; index < kWorkerCount; ++index)
        {
            workers.emplace_back(worker);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        keepGoing.store(false, std::memory_order_relaxed);
        for (auto &workerThread: workers)
        {
            workerThread.join();
        }

        EXPECT_EQ(maximumRoundsInFlight.load(), 1U) << "同时占住执行权的两轮会并发读同一份目录";
        EXPECT_EQ(roundsInFlight.load(), 0) << "有线程带着执行权退出";
    }
} // namespace AsynGyanis::Base::Detail
