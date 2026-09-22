#include "Platform/System/CpuAffinity.h"

#include "Platform/Platform.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <utility>

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

#if ASYN_PLATFORM_LINUX
        /**
         * @brief 读一个只放着一两行小文本的 procfs / cgroup 文件
         * @param path 文件路径
         * @return std::string 文件内容；打不开或空文件返回空串（本机没有 cgroup 是正常情形）
         */
        std::string readCgroupTextFile(const char *const path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return {};
            }
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 取一段文本里第一个整数字段
         * @details 用 from_chars 而不是 std::stoll：后者遇到 cgroup v2 写「max」这种非数字首字段
         *          会抛 out_of_range，而「max」恰恰是「不设限」的正常取值，按异常处理就把常态当故障
         * @param text 待解析文本
         * @return std::pair<bool, std::int64_t> 第一项为 false 表示开头就不是数字
         */
        std::pair<bool, std::int64_t> parseLeadingInteger(const std::string_view text)
        {
            const auto *begin = text.begin();
            const auto *end   = text.end();
            while (begin != end && (*begin == ' ' || *begin == '\t'))
            {
                ++begin;
            }
            std::int64_t value = 0;
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc{})
            {
                return {false, 0};
            }
            return {true, value};
        }

        /**
         * @brief 跳过第一个空白取下一个整数字段（cpu.max 的第二列）
         * @param text 整行内容
         * @return std::pair<bool, std::int64_t> 没有第二个字段时第一项为 false
         */
        std::pair<bool, std::int64_t> parseTrailingInteger(const std::string_view text)
        {
            const auto separator = text.find_first_of(" \t");
            if (separator == std::string_view::npos)
            {
                return {false, 0};
            }
            return parseLeadingInteger(text.substr(separator + 1));
        }

        /**
         * @brief 读本进程的 cgroup CPU 配额，折成等效核数
         * @return std::size_t 等效核数；不设限或读不到时返回 0
         */
        std::size_t currentCgroupQuotaCoreCount() noexcept
        {
            const std::string relativePath =
                    CpuAffinity::cgroupPathFromProcRecord(readCgroupTextFile("/proc/self/cgroup"));

            // cgroup v2：一行两列「<quota|max> <period>」，各分组有自己的 cpu.max
            const std::string v2Path = std::format("/sys/fs/cgroup{}/cpu.max", relativePath);
            if (const std::string cpuMax = readCgroupTextFile(v2Path.c_str()); !cpuMax.empty())
            {
                const auto [hasQuota, quotaMicroseconds] = parseLeadingInteger(cpuMax);
                const auto [hasPeriod, periodMicroseconds] = parseTrailingInteger(cpuMax);
                if (hasQuota && hasPeriod)
                {
                    return CpuAffinity::coresFromCgroupQuota(quotaMicroseconds, periodMicroseconds);
                }
                // 首字段是「max」即不设限，无需再看 v1
                return 0;
            }

            // cgroup v1：配额与周期分在两个文件，未限时配额写 -1
            const std::string quotaPath = std::format("/sys/fs/cgroup/cpu{}/cpu.cfs_quota_us", relativePath);
            const std::string periodPath = std::format("/sys/fs/cgroup/cpu{}/cpu.cfs_period_us", relativePath);
            const auto [hasQuota, quotaMicroseconds]   = parseLeadingInteger(readCgroupTextFile(quotaPath.c_str()));
            const auto [hasPeriod, periodMicroseconds] = parseLeadingInteger(readCgroupTextFile(periodPath.c_str()));
            if (!hasQuota || !hasPeriod)
            {
                return 0;
            }
            return CpuAffinity::coresFromCgroupQuota(quotaMicroseconds, periodMicroseconds);
        }
#endif // ASYN_PLATFORM_LINUX
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

    std::size_t CpuAffinity::coresFromCgroupQuota(const std::int64_t quotaMicroseconds,
                                                 const std::int64_t periodMicroseconds) noexcept
    {
        // 配额非正数即「不设限」（v2 的 max 解析失败按不设限处理、v1 写 -1）；周期为 0 是残缺数据，
        // 两者都返回 0 让调用方按「没有这条约束」继续，而不是静默算出 1 核把并行度压死
        if (quotaMicroseconds <= 0 || periodMicroseconds <= 0)
        {
            return 0;
        }
        return static_cast<std::size_t>((quotaMicroseconds + periodMicroseconds - 1) / periodMicroseconds);
    }

    std::string CpuAffinity::cgroupPathFromProcRecord(const std::string_view procContents)
    {
        for (std::size_t lineStart = 0; lineStart < procContents.size();)
        {
            const std::size_t lineEnd = procContents.find('\n', lineStart);
            const std::string_view line = procContents.substr(
                    lineStart, (lineEnd == std::string_view::npos ? procContents.size() : lineEnd) - lineStart);

            // 每行形如 "<层级>:<控制器列表>:<路径>"；v2 恒为 "0::<路径>"，v1 每个控制器一行、
            // 路径同样取最后一个冒号之后
            const std::size_t lastColon = line.rfind(':');
            if (lastColon != std::string_view::npos)
            {
                std::string path(line.substr(lastColon + 1));
                while (!path.empty() && (path.back() == '\r' || path.back() == ' '))
                {
                    path.pop_back();
                }
                if (!path.empty())
                {
                    // 相对路径必须自己补上根：拼出来才是 /sys/fs/cgroup 下的真实目录
                    return path.front() == '/' ? path : '/' + path;
                }
            }

            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
        // 一行都读不出路径时按根分组处理：此时读根上的 cpu.max 正是想要的
        return "/";
    }

    std::size_t CpuAffinity::recommendedWorkerCount() noexcept
    {
        std::size_t workerCount = std::thread::hardware_concurrency();

        // 许可核集合比机器小时用它：容器 cpuset 与 taskset 收窄过的环境上硬件核数没有意义
        const std::size_t allowedCoreCount = availableCoreCount();
        if (allowedCoreCount > 0 && (workerCount == 0 || allowedCoreCount < workerCount))
        {
            workerCount = allowedCoreCount;
        }

#if ASYN_PLATFORM_LINUX
        // CFS 配额是另一条独立约束：--cpus 只改配额、不改许可集合，前一步看不出进程被限住了
        const std::size_t quotaCoreCount = currentCgroupQuotaCoreCount();
        if (quotaCoreCount > 0 && (workerCount == 0 || quotaCoreCount < workerCount))
        {
            workerCount = quotaCoreCount;
        }
#endif

        return workerCount > 0 ? workerCount : 1;
    }

} // namespace AsynGyanis::Platform
