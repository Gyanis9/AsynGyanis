/**
 * @file CpuAffinity.h
 * @brief 当前线程的 CPU 亲和性：可用逻辑核数量、把线程绑到指定核、读回生效的核掩码
 * @author Gyanis
 * @date 2026-09-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Platform
{
    /**
     * @brief 线程级 CPU 亲和性工具
     *
     * @details 事件循环线程被调度器搬来搬去时，L1/L2 缓存与 TLB 每搬一次就要重建一遍，
     *          绑核能稳定尾延迟；绑的动作用 Windows 的 SetThreadAffinityMask 与 Linux 的
     *          sched_setaffinity 各自实现，收在这里是因为 Platform 独占 OS 底层。
     * @note 「可用核」按**本进程被允许使用的核**算，不是硬件核数：容器 cpuset 与
     *       taskset 收窄过的环境下 hardware_concurrency() 会给出机器核数而超出许可范围。
     * @warning 可绑的逻辑核编号范围取各平台自己那套 CPU 集合的宽度：Linux 的 cpu_set_t 有 1024 位，
     *          Windows 的 SetThreadAffinityMask 只能在一个处理器组内按位选核（因此只有 64 位，且线程
     *          落在第 1 组起时读不到可用的核，按不可用处理）。超出范围一律报错而不是静默截断。
     * @see currentThreadCoreMask() 交出的掩码只是 0-63 那一段的紧凑诊断视图，不决定可绑范围。
     */
    class CpuAffinity
    {
    public:
        /**
         * @brief 取本线程当前被允许使用的逻辑核数量
         * @details 按平台自己的 CPU 集合整份数出来，不经 64 位掩码中转——线程池与事件循环按这个数
         *          定容，读数被掩码截断就等于把宽机器上的一半并行度静默扔掉。
         * @return std::size_t 许可集合里的核数；查询失败时返回 0（调用方据此放弃绑核即可）
         */
        [[nodiscard]] static std::size_t availableCoreCount() noexcept;

        /**
         * @brief 把 cgroup 的 CPU 配额折算成「等效核数」
         * @details 配额是「每周期可用多少微秒」，除不尽时向上取整：1.5 核的额度起 2 条循环比起
         *          1 条更接近实际并行度，而 0.5 核也只该有一条循环（取整到 0 会让线程池空掉）。
         * @param quotaMicroseconds 一个调度周期内可用的 CPU 时间（微秒）；≤0 表示不设限
         *        （cgroup v2 的 `max`、v1 的 -1 都归到这里）
         * @param periodMicroseconds 调度周期长度（微秒）；≤0 表示读不到有效周期
         * @return std::size_t 等效核数；不设限或周期无效时返回 0，由调用方按「没有这条约束」处理
         */
        [[nodiscard]] static std::size_t coresFromCgroupQuota(std::int64_t quotaMicroseconds, std::int64_t periodMicroseconds) noexcept;

        /**
         * @brief 从 /proc/self/cgroup 的文本里取出本进程所在 cgroup 的相对路径
         * @details 抽成纯函数是为了能把格式边界摆开验（v2 单行 `0::/path`、v1 每个控制器一行、
         *          没有冒号的残缺行）；只读根路径会把 systemd `CPUQuota=` 这类子分组上的限额
         *          错读成「不设限」，所以这一步不能省。
         * @param procContents /proc/self/cgroup 的全文
         * @return std::string 以 '/' 开头的相对路径；解析不出时返回 "/"
         */
        [[nodiscard]] static std::string cgroupPathFromProcRecord(std::string_view procContents);

        /**
         * @brief 本进程实际能跑到多少并行度：硬件核数、许可核集合与 cgroup CPU 配额三者取最小
         * @details 起多少条事件循环该由「进程真能跑到多少并行」决定。`hardware_concurrency()` 只看
         *          机器：容器里 `--cpus` 走 CFS 配额、`--cpuset-cpus` 走许可集合，它两边都不看，
         *          于是在 2 核配额的 Pod 上给出宿主核数——每条循环自带一份 epoll 与定时器描述符，
         *          白占内存与文件描述符，还把上下文切换拉满。
         * @note 读不到任何约束（裸机、Windows、没有 cgroup）时退化成硬件核数。
         * @return std::size_t 至少 1，永不返回 0（调用方可以直接拿来当线程数）
         */
        [[nodiscard]] static std::size_t recommendedWorkerCount() noexcept;

        /**
         * @brief 取本线程当前被允许使用的逻辑核掩码（bit i 对应核 i）
         * @details 给断言与诊断用：绑核之后掩码应当只剩一位，否则说明绑定没生效。它只有 64 位，
         *          因此既不参与定容也不决定可绑范围——那两件事按平台自己的集合判定。
         * @return std::uint64_t 许可掩码；编号不小于 64 的核不体现在掩码里，
         *         查询失败时返回 0
         */
        [[nodiscard]] static std::uint64_t currentThreadCoreMask() noexcept;

        /**
         * @brief 把当前线程绑到指定逻辑核上
         * @details 只影响调用线程本身，不动进程亲和性，因此同一进程里可以按工作线程逐个绑。
         * @param coreIndex 目标逻辑核编号，必须落在本平台的集合宽度内（Linux 0-1023、Windows 0-63）
         *        且在本进程的许可集合内
         * @return 成功返回 void
         * @return 失败返回中文原因与替代做法（编号越界、核不在许可集合内、系统调用被拒）
         */
        [[nodiscard]] static std::expected<void, std::string> pinCurrentThreadToCore(std::size_t coreIndex);
    };

} // namespace AsynGyanis::Platform
