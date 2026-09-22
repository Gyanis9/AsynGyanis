// TestCpuAffinity.cpp —— 线程绑核的覆盖：许可集合的读回、绑到许可内的核、两类拒绝面
//   （编号超出 64 位掩码能表达的范围、核不在本进程被允许的集合里）。
//   断言一律拿操作系统自己的亲和性查询与 std::thread 的机器核数当独立判据，
//   不用本类的返回值互相印证；拒绝面还要额外确认掩码没被改到一半。

#include "Platform/System/CpuAffinity.h"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
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

        /**
         * @brief 从 offset 处起取文本里下一个整数，并把 offset 推进到它之后
         * @return false 表示该位置起没有整数（例如 cgroup v2 的「max」）
         */
        bool nextInteger(const std::string_view text, std::size_t &offset, std::int64_t &value)
        {
            while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t'))
            {
                ++offset;
            }
            const auto result = std::from_chars(text.data() + offset, text.data() + text.size(), value);
            if (result.ec != std::errc{})
            {
                return false;
            }
            offset = static_cast<std::size_t>(result.ptr - text.data());
            return true;
        }

        /**
         * @brief 用例自己读一遍本进程的 cgroup CPU 配额并折算核数，判据必须独立于实现
         * @details 分组路径取自 cgroupPathFromProcRecord()（那是纯函数，由 CgroupPathFromProcRecord...
         *          用例按表钉住），这里的独立性体现在「自己开文件、自己拆两列、自己做向上取整」——
         *          实现漏读配额或折算方向反了都会在这里变红。
         * @return std::size_t 等效核数；读不到、没有 cgroup 或不设限时返回 0
         */
        std::size_t readOwnCgroupQuotaCoreCount()
        {
            const auto readFirstLine = [](const std::string &path) -> std::string
            {
                std::ifstream stream(path);
                std::string line;
                if (stream)
                {
                    std::getline(stream, line);
                }
                return line;
            };

            std::ifstream procStream("/proc/self/cgroup");
            const std::string procContents{std::istreambuf_iterator<char>(procStream), std::istreambuf_iterator<char>()};
            const std::string groupPath = CpuAffinity::cgroupPathFromProcRecord(procContents);

            std::int64_t quota  = 0;
            std::int64_t period = 0;
            std::size_t offset  = 0;
            if (const std::string cpuMax = readFirstLine("/sys/fs/cgroup" + groupPath + "/cpu.max"); !cpuMax.empty())
            {
                // cgroup v2：一行两列「<quota|max> <period>」，首列写成 max 就是不设限
                const std::string_view text(cpuMax);
                if (!nextInteger(text, offset, quota) || !nextInteger(text, offset, period))
                {
                    return 0;
                }
            }
            else
            {
                // cgroup v1：两个文件，未限时配额写 -1
                const std::string quotaText  = readFirstLine("/sys/fs/cgroup/cpu" + groupPath + "/cpu.cfs_quota_us");
                const std::string periodText = readFirstLine("/sys/fs/cgroup/cpu" + groupPath + "/cpu.cfs_period_us");
                std::size_t quotaOffset  = 0;
                std::size_t periodOffset = 0;
                if (!nextInteger(quotaText, quotaOffset, quota) || !nextInteger(periodText, periodOffset, period))
                {
                    return 0;
                }
            }

            if (quota <= 0 || period <= 0)
            {
                return 0;
            }
            return static_cast<std::size_t>((quota + period - 1) / period);
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

    /**
     * @brief cgroup 配额折算核数的判定表：向上取整、不设限与残缺数据三类
     * @details 「不设限」和「读不到」都必须折成 0（意为没有这条约束），不能悄悄算成 1 核——
     *          那会把裸机上的线程池压成单条循环；反过来 0.5 核的配额要给 1 而不是 0
     */
    TEST(CpuAffinity, CoresFromCgroupQuotaRoundsUpAndTreatsUnlimitedAsNoConstraint)
    {
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(200000, 100000), 2U) << "两倍的周期配额即 2 核";
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(150000, 100000), 2U) << "除不尽时向上取整：1.5 核给 2";
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(50000, 100000), 1U) << "半核也只该有一条循环，不能折成 0";
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(-1, 100000), 0U) << "v1 的 -1 是不设限";
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(0, 100000), 0U) << "配额为 0 按不设限处理";
        EXPECT_EQ(CpuAffinity::coresFromCgroupQuota(200000, 0), 0U) << "周期缺失是残缺数据，不得拿去当约束";
    }

    /**
     * @brief 推荐并行度不得越过本进程的任一 CPU 约束，且与用例自己读到的配额一致
     * @details 判据独立于实现：用例自己读 cgroup 的两个数、自己做向上取整，再与许可集合与机器
     *          核数取最小。这样「实现忘了读配额」会在这里变红（配额受限时读数大于约束值），
     *          而不是只在实现内部自证
     */
    TEST(CpuAffinity, RecommendedWorkerCountRespectsThisProcessQuotaAndAllowedSet)
    {
        const std::size_t recommended = CpuAffinity::recommendedWorkerCount();
        EXPECT_GE(recommended, 1U) << "推荐值为 0 会让线程池起不出任何一条循环";

        if (const unsigned hardwareCoreCount = std::thread::hardware_concurrency(); hardwareCoreCount > 0)
        {
            EXPECT_LE(recommended, static_cast<std::size_t>(hardwareCoreCount));
        }
        if (const std::size_t allowedCoreCount = CpuAffinity::availableCoreCount(); allowedCoreCount > 0)
        {
            EXPECT_LE(recommended, allowedCoreCount) << "许可集合之外的核跑不到，不该按它起线程";
        }

        const std::size_t quotaCoreCount = readOwnCgroupQuotaCoreCount();
        if (quotaCoreCount > 0)
        {
            EXPECT_LE(recommended, quotaCoreCount)
                    << "本进程被 cgroup 配额限到 " << quotaCoreCount << " 核，推荐值却更高";
        }
    }

    /**
     * @brief /proc/self/cgroup 的文本按表解析成分组路径
     * @details 限额挂在进程自己所在的分组上，只读根分组的 cpu.max 会把 systemd `CPUQuota=`
     *          与自建分组错读成「不设限」，因此这一步的格式边界要单独钉住
     */
    TEST(CpuAffinity, CgroupPathFromProcRecordHandlesBothVersionsAndMalformedLines)
    {
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::/\n"), "/") << "v2 根分组";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::/system.slice/my.service\n"), "/system.slice/my.service")
                << "v2 子分组：限额挂在自己的路径下";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord(
                      "12:cpu,cpuacct:/user.slice/1000.scope\n11:memory:/user.slice/1000.scope\n"),
                  "/user.slice/1000.scope") << "v1 每个控制器一行，路径取第一行";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::init.scope\n"), "/init.scope")
                << "缺前导斜杠要补上，拼出来才是 /sys/fs/cgroup 下的真实目录";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::/a/b\r\n"), "/a/b") << "行尾的 \\r 不得留在路径里";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::/a/b \n"), "/a/b") << "行尾空白同样剥掉";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("garbage\n"), "/") << "没有冒号的残缺行按根分组处理";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord(""), "/") << "空文本按根分组处理";
        EXPECT_EQ(CpuAffinity::cgroupPathFromProcRecord("0::\n10::/x\n"), "/x") << "路径为空的行跳过，继续找下一行";
    }

} // namespace AsynGyanis::Platform
