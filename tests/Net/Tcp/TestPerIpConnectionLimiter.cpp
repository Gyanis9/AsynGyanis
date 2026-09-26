// PerIpConnectionLimiter 单元测试：限额开关、按来源独立计数、凭据的移动语义与多线程下的不变式
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 两个来源标识：用例只关心「是不是同一个来源」，不关心地址格式
        constexpr const char *kFirstSource = "10.0.0.1:40000";
        constexpr const char *kSecondSource = "10.0.0.2:40000";
    } // namespace

    /**
     * @brief 限额为 0 表示关闭该项保护，而不是「一条都不许」
     */
    TEST(PerIpConnectionLimiter, ZeroLimitDisablesTracking)
    {
        PerIpConnectionLimiter limiter(0);

        std::vector<PerIpConnectionLimiter::Lease> leases;
        for (int index = 0; index < 8; ++index)
        {
            std::optional<PerIpConnectionLimiter::Lease> lease = limiter.tryAcquire(kFirstSource);
            ASSERT_TRUE(lease.has_value()) << "限额关闭时不应拒绝任何连接";
            // 空壳凭据：析构不归还，调用方因此不必区分「有保护」与「没保护」两条路径
            EXPECT_FALSE(lease->isTracking());
            leases.push_back(std::move(*lease));
        }
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 0u) << "限额关闭时不应记账";

        leases.clear();
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 0u);
    }

    /**
     * @brief 达到上限即拒，且拒绝不会往计数表里留条目
     */
    TEST(PerIpConnectionLimiter, RejectsBeyondLimitWithoutPollutingTable)
    {
        PerIpConnectionLimiter limiter(2);

        const std::optional<PerIpConnectionLimiter::Lease> first = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(first.has_value());
        EXPECT_TRUE(first->isTracking());

        const std::optional<PerIpConnectionLimiter::Lease> second = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 2u);

        EXPECT_FALSE(limiter.tryAcquire(kFirstSource).has_value()) << "第三条应被拒";
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 2u) << "被拒的尝试不得改变计数";

        // 换一个来源不受影响：限额是按来源各算一份，不是全局共享。
        // 凭据必须存进变量——它就是「占着名额」这件事本身，写成临时对象会在语句结束时立即归还，
        // 后面的计数断言就会看到 0（这不是缺陷，正是 RAII 该有的样子）
        const std::optional<PerIpConnectionLimiter::Lease> otherSourceLease = limiter.tryAcquire(kSecondSource);
        ASSERT_TRUE(otherSourceLease.has_value());
        EXPECT_EQ(limiter.activeCountFor(kSecondSource), 1u);
    }

    /**
     * @brief 凭据析构即归还，名额可被重新占用
     */
    TEST(PerIpConnectionLimiter, DestroyingLeaseReleasesSlot)
    {
        PerIpConnectionLimiter limiter(1);

        {
            const std::optional<PerIpConnectionLimiter::Lease> lease = limiter.tryAcquire(kFirstSource);
            ASSERT_TRUE(lease.has_value());
            EXPECT_FALSE(limiter.tryAcquire(kFirstSource).has_value());
        }

        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 0u) << "计数归零后应删掉条目";
        const std::optional<PerIpConnectionLimiter::Lease> reacquired = limiter.tryAcquire(kFirstSource);
        EXPECT_TRUE(reacquired.has_value()) << "归还之后应能重新占用";
    }

    /**
     * @brief 移动语义：名额随凭据转移，被移走的一方不再归还
     */
    TEST(PerIpConnectionLimiter, MovedLeaseKeepsHoldingSlot)
    {
        PerIpConnectionLimiter limiter(1);

        std::optional<PerIpConnectionLimiter::Lease> source = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(source.has_value());

        PerIpConnectionLimiter::Lease moved = std::move(*source);
        source.reset();
        // 被移走的一方已置空：它析构不该归还名额，否则上限会被悄悄放大
        EXPECT_FALSE(limiter.tryAcquire(kFirstSource).has_value()) << "名额仍被 moved 持有";
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 1u);

        // 移动赋值同样只转移一次：先把 moved 转给另一个凭据，再让 moved 析构
        PerIpConnectionLimiter::Lease assigned;
        assigned = std::move(moved);
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 1u);

        assigned = PerIpConnectionLimiter::Lease{};
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 0u) << "归还一次即归零，不能多还";
    }

    /**
     * @brief 凭据可以活得比限额对象久：收尾期的连接协程晚于服务器析构
     */
    TEST(PerIpConnectionLimiter, LeaseOutlivesLimiter)
    {
        std::optional<PerIpConnectionLimiter::Lease> lease;
        {
            PerIpConnectionLimiter limiter(4);
            lease = limiter.tryAcquire(kFirstSource);
            ASSERT_TRUE(lease.has_value());
        }

        // 限额对象已析构，此处归还若访问了已释放的内存，ASan 会直接报出来
        lease.reset();
        SUCCEED();
    }

    /**
     * @brief 多线程并发抢名额：同一时刻的在用数不得越过上限，且每轮的争夺都真实重叠
     *
     * @details 重叠必须由**构造**保证，不能靠调度运气：早先的写法是「取到就立刻还」，在 4 核 runner
     *          （还叠加 ASan 与 ctest 并行）上各线程可能完全串行跑，于是「必然有人被拒」这条断言
     *          变成掷骰子，Linux CI 上红过一次。现在每轮所有线程都在栅栏处等到齐了才归还，
     *          8 个线程抢 4 个名额因此必然同时在场：每轮恰好 4 个被拒、峰值恰好触到上限。
     */
    TEST(PerIpConnectionLimiter, ConcurrentAcquireNeverExceedsLimit)
    {
        constexpr std::size_t kLimit = 4;
        constexpr int kThreadCount = 8;
        constexpr int kRoundsPerThread = 50;

        PerIpConnectionLimiter limiter(kLimit);

        std::atomic<std::size_t> currentlyHeld{0};
        std::atomic<std::size_t> peakHeld{0};
        std::atomic<int> rejectedCount{0};

        // 每轮两道栅栏：第一道保证「抢到的人一直握着、所有人都已尝试过」，
        // 第二道保证「上一轮全部归还完，下一轮才开抢」——两道合起来，每轮都是干净的 8 抢 4
        std::barrier roundGate(kThreadCount);

        const auto worker = [&limiter, &currentlyHeld, &peakHeld, &rejectedCount, &roundGate]
        {
            for (int round = 0; round < kRoundsPerThread; ++round)
            {
                std::optional<PerIpConnectionLimiter::Lease> lease = limiter.tryAcquire(kFirstSource);
                if (lease.has_value())
                {
                    // 占用数在「取到之后、归还之前」这段窗口里递增：峰值就是上限不变式的观测点
                    const std::size_t heldNow = currentlyHeld.fetch_add(1, std::memory_order_acq_rel) + 1;
                    std::size_t observedPeak = peakHeld.load(std::memory_order_relaxed);
                    while (observedPeak < heldNow && !peakHeld.compare_exchange_weak(observedPeak, heldNow, std::memory_order_relaxed))
                    {
                        // compare_exchange 失败时 observedPeak 已被刷新，循环继续比较即可
                    }
                } else
                {
                    rejectedCount.fetch_add(1, std::memory_order_relaxed);
                }

                // 第一道栅栏：本轮所有人尝试完之前不归还，重叠由构造保证
                roundGate.arrive_and_wait();

                if (lease.has_value())
                {
                    currentlyHeld.fetch_sub(1, std::memory_order_acq_rel);
                    lease.reset();
                }

                // 第二道栅栏：等所有人归还干净，免得下一轮有人抢到刚归还的名额、把「每轮恰好 4 个被拒」打散
                roundGate.arrive_and_wait();
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(kThreadCount);
        for (int index = 0; index < kThreadCount; ++index)
        {
            workers.emplace_back(worker);
        }
        for (std::thread &thread : workers)
        {
            thread.join();
        }

        EXPECT_LE(peakHeld.load(), kLimit) << "同时占用的名额数越过了上限：计数表在并发下被破坏";
        EXPECT_EQ(peakHeld.load(), kLimit) << "8 个线程同时在场抢 4 个名额，峰值必然触到上限（没触到说明重叠没构造出来）";
        EXPECT_EQ(limiter.activeCountFor(kFirstSource), 0u) << "所有凭据析构后计数必须归零";
        EXPECT_EQ(rejectedCount.load(), (kThreadCount - static_cast<int>(kLimit)) * kRoundsPerThread)
                << "每轮 8 抢 4，必然恰好 4 个线程被拒";
    }

    /**
     * @brief IPv4 映射写法与点分写法算同一个来源，两种写法共用一格
     * @details 双栈监听器（框架显式关掉 IPV6_V6ONLY）上 IPv4 客户端的对端文本是
     *          `::ffff:a.b.c.d`，纯 IPv4 监听器给的是 `a.b.c.d`；两类监听器共用一份限额时不折前缀
     *          就会各占一格，「单个来源」的上限实际翻倍
     */
    TEST(PerIpConnectionLimiter, MappedIpv4FormSharesSlotWithPlainForm)
    {
        PerIpConnectionLimiter limiter(1);

        const std::optional<PerIpConnectionLimiter::Lease> mappedLease = limiter.tryAcquire("::ffff:10.0.0.1");
        ASSERT_TRUE(mappedLease.has_value());

        EXPECT_FALSE(limiter.tryAcquire("10.0.0.1").has_value()) << "同一来源换一种写法就被当成另一个来源，上限翻倍";
        // 大写前缀也来自同一套语义（IPv6 文本按规范不区分大小写），不能因为写法差异分键
        EXPECT_FALSE(limiter.tryAcquire("::FFFF:10.0.0.1").has_value()) << "大写前缀没被折成同一个键";

        // 查询侧必须与记账侧同一套规则，否则观测读数会显示「这个来源一条都没占」
        EXPECT_EQ(limiter.activeCountFor("10.0.0.1"), 1u);
        EXPECT_EQ(limiter.activeCountFor("::ffff:10.0.0.1"), 1u);
    }

    /**
     * @brief 归还名额后同一来源（换写法）可重新进入，且折键只针对真正的点分四段
     * @details 后半段是折键的边界：`::ffff:` 后面不是合法 IPv4 点分文本时保持原样，
     *          否则就是把「不认识的写法」强行并格，掩盖掉真实的来源差异
     */
    TEST(PerIpConnectionLimiter, ReleasedSlotIsReusableAndFoldingStaysStrict)
    {
        PerIpConnectionLimiter limiter(1);

        std::optional<PerIpConnectionLimiter::Lease> mappedLease = limiter.tryAcquire("::ffff:10.0.0.2");
        ASSERT_TRUE(mappedLease.has_value());
        mappedLease.reset();
        EXPECT_EQ(limiter.activeCountFor("10.0.0.2"), 0u) << "凭据里保管的应是规范化后的键，否则归还找不回那一格";

        const std::optional<PerIpConnectionLimiter::Lease> plainLease = limiter.tryAcquire("10.0.0.2");
        ASSERT_TRUE(plainLease.has_value()) << "名额归还后同一来源换写法应能重新进入";

        // 非点分的尾段不折：`::ffff:1:2` 与 `1:2` 因此是两个键，前者也不会挤掉后者的名额
        const std::optional<PerIpConnectionLimiter::Lease> oddLease = limiter.tryAcquire("::ffff:1:2");
        ASSERT_TRUE(oddLease.has_value());
        EXPECT_EQ(limiter.activeCountFor("::ffff:1:2"), 1u) << "非法点分尾段被折掉了";
        EXPECT_EQ(limiter.activeCountFor("1:2"), 0u);

        // 超出十进制取值范围的也不算 IPv4：`999.0.0.1` 与点分本体是两种不同写法，不该并格
        const std::optional<PerIpConnectionLimiter::Lease> outOfRangeLease = limiter.tryAcquire("::ffff:999.0.0.1");
        ASSERT_TRUE(outOfRangeLease.has_value());
        EXPECT_EQ(limiter.activeCountFor("::ffff:999.0.0.1"), 1u);
        EXPECT_EQ(limiter.activeCountFor("999.0.0.1"), 0u);
    }

    /**
     * @brief 钉住：只有「因达到上限而拒」被计数，且归还不把数冲掉
     * @details 这道闸门挡掉的连接不会成为连接，因此在任何其它计数里都不留痕——没有这个数，
     *          「限额器把谁都挡」与「这段时间没有流量」在服务端侧是同一幅景象。两条反方向也要钉：
     *          放行的那次不自增（否则计数会被流量推着走，失去「挡了多少」的含义），归还名额不把
     *          数往回扣（它是累计量，抓取侧靠两次采样的差值算速率）。
     *          限额关闭（0）那条单独钉：这条路径上既不该记账也不该计数，否则「关掉保护」会被读成
     *          「闸门在挡人」。
     */
    TEST(PerIpConnectionLimiter, CountsOnlyRefusalsAndKeepsThemAcrossReleases)
    {
        PerIpConnectionLimiter limiter(2);

        std::optional<PerIpConnectionLimiter::Lease> first = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(first.has_value());
        std::optional<PerIpConnectionLimiter::Lease> second = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(limiter.rejectedConnectionCount(), 0u) << "放行的连接不该被算成挡下的";

        // 第 3、4 次都超限：两条都要留数
        EXPECT_FALSE(limiter.tryAcquire(kFirstSource).has_value());
        EXPECT_FALSE(limiter.tryAcquire(kFirstSource).has_value());
        EXPECT_EQ(limiter.rejectedConnectionCount(), 2u);

        // 另一个来源照样放行，不牵连计数
        const std::optional<PerIpConnectionLimiter::Lease> otherSource = limiter.tryAcquire(kSecondSource);
        ASSERT_TRUE(otherSource.has_value());
        EXPECT_EQ(limiter.rejectedConnectionCount(), 2u);

        // 归还一个名额之后又能进，但累计的拒绝数不往回扣
        first.reset();
        EXPECT_EQ(limiter.rejectedConnectionCount(), 2u) << "归还名额把拒绝计数冲掉了：抓取侧就没法算差值";
        const std::optional<PerIpConnectionLimiter::Lease> afterRelease = limiter.tryAcquire(kFirstSource);
        ASSERT_TRUE(afterRelease.has_value()) << "归还之后应当能再占上";
        EXPECT_EQ(limiter.rejectedConnectionCount(), 2u);

        PerIpConnectionLimiter disabled(0);
        for (int index = 0; index < 8; ++index)
        {
            EXPECT_TRUE(disabled.tryAcquire(kFirstSource).has_value());
        }
        EXPECT_EQ(disabled.rejectedConnectionCount(), 0u) << "关掉这项保护时不该报出任何拒绝";
    }

} // namespace AsynGyanis::Net
