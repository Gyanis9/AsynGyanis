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
     * @warning 只支持逻辑核编号小于 64 的绑定（核掩码是 64 位）。编号更大的机器上本函数
     *          直接报错而不是静默截断，那种规模请交给 taskset/numactl 或改用分组亲和性 API。
     *          Windows 上处理器分两组以上时（第 1 组起）本工具读不到可用的核，同样按不可用处理。
     */
    class CpuAffinity
    {
    public:
        /**
         * @brief 取本线程当前被允许使用的逻辑核数量
         * @return std::size_t 许可集合里的核数；查询失败时返回 0（调用方据此放弃绑核即可）
         */
        [[nodiscard]] static std::size_t availableCoreCount() noexcept;

        /**
         * @brief 取本线程当前被允许使用的逻辑核掩码（bit i 对应核 i）
         * @details 给断言与诊断用：绑核之后掩码应当只剩一位，否则说明绑定没生效。
         * @return std::uint64_t 许可掩码；编号不小于 64 的核不体现在掩码里，
         *         查询失败时返回 0
         */
        [[nodiscard]] static std::uint64_t currentThreadCoreMask() noexcept;

        /**
         * @brief 把当前线程绑到指定逻辑核上
         * @details 只影响调用线程本身，不动进程亲和性，因此同一进程里可以按工作线程逐个绑。
         * @param coreIndex 目标逻辑核编号，必须小于 64 且在本进程的许可集合内
         * @return 成功返回 void
         * @return 失败返回中文原因与替代做法（编号越界、核不在许可集合内、系统调用被拒）
         */
        [[nodiscard]] static std::expected<void, std::string> pinCurrentThreadToCore(std::size_t coreIndex);
    };

} // namespace AsynGyanis::Platform
