// TestCpuAffinity.cpp —— 线程绑核的覆盖：许可集合的读回、绑到许可内的核、两类拒绝面
//   （编号超出 64 位掩码能表达的范围、核不在本进程被允许的集合里）。
//   断言一律拿操作系统自己的亲和性查询与 std::thread 的机器核数当独立判据，
//   不用本类的返回值互相印证；拒绝面还要额外确认掩码没被改到一半。

#include "Platform/System/CpuAffinity.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 数出掩码里有几枚核（用例自己算一遍，不复用实现的私有写法）
         * @param coreMask 逻辑核掩码
         * @return std::size_t 置位个数
         */
        std::size_t countCoresInMask(const std::uint64_t coreMask) noexcept
        {
            std::size_t coreCount = 0;
            for (std::uint64_t remainingMask = coreMask; remainingMask != 0; remainingMask &= remainingMask - 1)
            {
                ++coreCount;
            }
            return coreCount;
        }

        /// 取掩码里最低置位的核编号；掩码为空时返回 nullopt
        std::optional<std::size_t> lowestSetCoreIndex(const std::uint64_t coreMask) noexcept
        {
            if (coreMask == 0)
            {
                return std::nullopt;
            }
            std::size_t coreIndex = 0;
            while ((coreMask & (std::uint64_t{1} << coreIndex)) == 0)
            {
                ++coreIndex;
            }
            return coreIndex;
        }

        /// 取 0-63 里第一个没被放行的核编号；全部放行时返回 nullopt
        std::optional<std::size_t> firstDisallowedCoreIndex(const std::uint64_t coreMask) noexcept
        {
            for (std::size_t coreIndex = 0; coreIndex < 64; ++coreIndex)
            {
                if ((coreMask & (std::uint64_t{1} << coreIndex)) == 0)
                {
                    return coreIndex;
                }
            }
            return std::nullopt;
        }

        /// 判断掩码是否恰好指向一枚核（绑核生效的形状判据）
        bool pointsAtExactlyOneCore(const std::uint64_t coreMask) noexcept
        {
            return coreMask != 0 && (coreMask & (coreMask - 1)) == 0;
        }

        /// 有界等待工作线程把结果写完，避免固定 sleep
        bool waitUntilSettled(const std::atomic<bool> &isFinished)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!isFinished.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 在一条独立线程里执行一次绑核，并把该线程读回的掩码交出来
         * @details 绑核是线程级属性：在本用例线程里绑会把整个用例进程后续的用例都绑窄，
         *          因此每次绑定都开一条用完即 join 的线程。
         * @param coreIndex 目标核编号
         * @param[out] maskAfterPinning 输出：绑定调用之后该线程自己的掩码
         * @param[out] isSuccessText 输出：绑定调用是否成功
         * @return true 工作线程在时限内跑完
         */
        bool pinOnDedicatedThread(const std::size_t coreIndex, std::uint64_t &maskAfterPinning, bool &isSuccess)
        {
            std::atomic<bool> isFinished{false};
            std::thread worker([&]
            {
                const auto pinResult = CpuAffinity::pinCurrentThreadToCore(coreIndex);
                // 先写全部载荷，再用 release 发布完成标记：等待方拿到标记时读数一定已落地
                maskAfterPinning = CpuAffinity::currentThreadCoreMask();
                isSuccess = pinResult.has_value();
                isFinished.store(true, std::memory_order_release);
            });
            const bool isSettled = waitUntilSettled(isFinished);
            worker.join();
            return isSettled;
        }
    } // namespace

    TEST(CpuAffinity, AllowedCoreSetIsNonEmptyAndNeverWiderThanTheMachine)
    {
        const std::uint64_t allowedMask = CpuAffinity::currentThreadCoreMask();
        EXPECT_NE(allowedMask, 0U) << "读不到本线程可用的 CPU 集合，绑核与断言都无从谈起";

        const std::size_t allowedCoreCount = CpuAffinity::availableCoreCount();
        EXPECT_GE(allowedCoreCount, 1U);
        EXPECT_EQ(allowedCoreCount, countCoresInMask(allowedMask)) << "数量与掩码两处读数不一致";

        // 许可集合是本进程可用核的子集：容器 cpuset 收窄后只会更少，不会多于机器核数
        const unsigned hardwareCoreCount = std::thread::hardware_concurrency();
        if (hardwareCoreCount > 0)
        {
            EXPECT_LE(allowedCoreCount, static_cast<std::size_t>(hardwareCoreCount));
        }
    }

    TEST(CpuAffinity, PinningAllowedCoreNarrowsThatThreadToOneBit)
    {
        const std::optional<std::size_t> targetCore = lowestSetCoreIndex(CpuAffinity::currentThreadCoreMask());
        ASSERT_TRUE(targetCore.has_value()) << "本机没有放行任何核，无法验证绑定";

        std::uint64_t maskAfterPinning = 0;
        bool isSuccess = false;
        ASSERT_TRUE(pinOnDedicatedThread(*targetCore, maskAfterPinning, isSuccess));

        EXPECT_TRUE(isSuccess) << "绑到许可集合内的核 " << *targetCore << " 应当成功";
        EXPECT_EQ(maskAfterPinning, std::uint64_t{1} << *targetCore);
        EXPECT_TRUE(pointsAtExactlyOneCore(maskAfterPinning));
    }

    TEST(CpuAffinity, PinningDoesNotWidenOrNarrowTheThreadThatOnlyAsks)
    {
        // 本线程全程不绑核：工作线程的绑定不得串到这里来（亲和性按线程而非按进程生效）
        const std::uint64_t ownMaskBefore = CpuAffinity::currentThreadCoreMask();

        std::uint64_t maskAfterPinning = 0;
        bool isSuccess = false;
        const std::optional<std::size_t> targetCore = lowestSetCoreIndex(ownMaskBefore);
        ASSERT_TRUE(targetCore.has_value());
        ASSERT_TRUE(pinOnDedicatedThread(*targetCore, maskAfterPinning, isSuccess));

        EXPECT_EQ(CpuAffinity::currentThreadCoreMask(), ownMaskBefore) << "别的线程绑核改到了本线程的掩码";
    }

    TEST(CpuAffinity, PinningRefusesCoreNumbersBeyondTheSixtyFourBitMask)
    {
        const std::uint64_t maskBefore = CpuAffinity::currentThreadCoreMask();

        for (const std::size_t tooLargeIndex: {64U, 65U, 1000U})
        {
            const auto pinResult = CpuAffinity::pinCurrentThreadToCore(tooLargeIndex);
            ASSERT_FALSE(pinResult.has_value()) << "编号 " << tooLargeIndex << " 超出 64 位掩码，必须拒绝";
            EXPECT_NE(pinResult.error().find("64"), std::string::npos)
                    << "拒绝文案要写清支持范围，实际：" << pinResult.error();
        }

        // 拒绝路径不留半成品：掩码一位都不该被改过
        EXPECT_EQ(CpuAffinity::currentThreadCoreMask(), maskBefore);
    }

    TEST(CpuAffinity, PinningRefusesCoresOutsideTheAllowedSet)
    {
        const std::uint64_t maskBefore = CpuAffinity::currentThreadCoreMask();
        const std::optional<std::size_t> disallowedCore = firstDisallowedCoreIndex(maskBefore);
        if (!disallowedCore.has_value())
        {
            GTEST_SKIP() << "本机 0-63 号核全在许可集合内，没有可验的「核不在集合内」拒绝面";
        }

        const auto pinResult = CpuAffinity::pinCurrentThreadToCore(*disallowedCore);
        ASSERT_FALSE(pinResult.has_value()) << "核 " << *disallowedCore << " 不在许可集合内，必须拒绝";
        EXPECT_NE(pinResult.error().find("不在本进程被允许的 CPU 集合"), std::string::npos)
                << "拒绝文案要说清原因与下一步，实际：" << pinResult.error();
        EXPECT_EQ(CpuAffinity::currentThreadCoreMask(), maskBefore) << "被拒的绑定不得留下半个效果";
    }

} // namespace AsynGyanis::Platform
