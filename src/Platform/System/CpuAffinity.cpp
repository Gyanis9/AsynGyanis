#include "Platform/System/CpuAffinity.h"

#include "Platform/Platform.h"

#include <format>

#if ASYN_PLATFORM_LINUX
    #include <sched.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 核掩码是 64 位，因此可绑的逻辑核编号上限就是 64；超限一律显式拒绝而不是回绕成低位
        constexpr std::size_t kMaximumAddressableCoreIndex = 64;

        /**
         * @brief 数出掩码里有几个置位的核
         * @param coreMask 逻辑核掩码
         * @return std::size_t 置位个数
         */
        std::size_t countSetCores(const std::uint64_t coreMask) noexcept
        {
            std::size_t coreCount = 0;
            for (std::uint64_t remainingMask = coreMask; remainingMask != 0; remainingMask &= remainingMask - 1)
            {
                // 每次最低位的 1 被抹掉，循环次数正好等于置位个数
                ++coreCount;
            }
            return coreCount;
        }
    } // namespace

    std::uint64_t CpuAffinity::currentThreadCoreMask() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // 读回用 GetThreadGroupAffinity 而不是名字更直白的 GetThreadAffinityMask：后者在本 SDK 里
        // 只于桌面分区声明（实测未声明，C2065），而组内掩码这一份两条分区都有，够用
        GROUP_AFFINITY groupAffinity{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &groupAffinity))
        {
            return 0;
        }
        if (groupAffinity.Group != 0)
        {
            // 掩码是「组内编号」，第 1 组起的核与这里的 0-63 编号不是一套；超出表达范围就如实报空
            return 0;
        }
        return static_cast<std::uint64_t>(groupAffinity.Mask);
#else
        cpu_set_t allowedSet;
        CPU_ZERO(&allowedSet);
        // 第 0 个参数取 0 表示调用线程本身，不必先取 tid
        if (sched_getaffinity(0, sizeof(allowedSet), &allowedSet) != 0)
        {
            return 0;
        }

        std::uint64_t coreMask = 0;
        for (std::size_t coreIndex = 0; coreIndex < kMaximumAddressableCoreIndex; ++coreIndex)
        {
            // 编号不小于 64 的核放不进掩码：本工具的绑定范围本来就不含它们
            if (CPU_ISSET(coreIndex, &allowedSet))
            {
                coreMask |= std::uint64_t{1} << coreIndex;
            }
        }
        return coreMask;
#endif
    }

    std::size_t CpuAffinity::availableCoreCount() noexcept
    {
        return countSetCores(currentThreadCoreMask());
    }

    std::expected<void, std::string> CpuAffinity::pinCurrentThreadToCore(const std::size_t coreIndex)
    {
        if (coreIndex >= kMaximumAddressableCoreIndex)
        {
            return std::unexpected(std::format("绑核失败：逻辑核编号 {} 不在本工具支持的 0-{} 范围内"
                                               "（核掩码只有 64 位）。请改用操作系统的绑核工具（如 taskset、numactl），"
                                               "或只把线程绑到编号更小的核上",
                                               coreIndex, kMaximumAddressableCoreIndex - 1));
        }

        const std::uint64_t targetBit = std::uint64_t{1} << coreIndex;
        const std::uint64_t allowedMask = currentThreadCoreMask();
        if (allowedMask == 0)
        {
            return std::unexpected("绑核失败：读取本线程可用的 CPU 集合就失败了，无法判定目标核是否可用。"
                                   "请确认运行环境允许查询亲和性（部分沙箱会拦下这类调用），或放弃绑核");
        }
        if ((allowedMask & targetBit) == 0)
        {
            return std::unexpected(std::format("绑核失败：逻辑核 {} 不在本进程被允许的 CPU 集合里（掩码 0x{:016x}）。"
                                               "容器 cpuset 或 taskset 往往只放行一部分核，请把线程绑到集合内的编号上",
                                               coreIndex, allowedMask));
        }

#if ASYN_PLATFORM_WIN32
        if (SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(targetBit)) == 0)
        {
            return std::unexpected(std::format("绑核失败：SetThreadAffinityMask 返回错误码 {}（目标核 {}）。"
                                               "请检查是否有安全策略限制线程亲和性，或放弃绑核",
                                               GetLastError(), coreIndex));
        }
#else
        cpu_set_t targetSet;
        CPU_ZERO(&targetSet);
        CPU_SET(coreIndex, &targetSet);
        if (sched_setaffinity(0, sizeof(targetSet), &targetSet) != 0)
        {
            return std::unexpected(std::format("绑核失败：sched_setaffinity 返回 errno {}（目标核 {}）。"
                                               "请检查 cgroup 的 cpuset 配置，或放弃绑核",
                                               errno, coreIndex));
        }
#endif
        return {};
    }

} // namespace AsynGyanis::Platform
