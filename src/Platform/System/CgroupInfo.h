/**
 * @file CgroupInfo.h
 * @brief cgroup 对本进程所在控制组的内存与进程数限额、当前用量读数
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace AsynGyanis::Platform
{
    /**
     * @brief 一次 cgroup 读数：内存与进程数的上限、当前用量
     *
     * @details 上限字段的 0 表示内核明确报「无限制」（v2 的 max、v1 的超大哨兵），nullopt 表示
     *          文件缺失或解析失败；两者不能混为一谈，否则「没设上限」会被当成「读不出」。
     * @note 用量是控制组级口径：memory.current 含页缓存，pids.current 含同组其它进程的线程，
     *       不是本进程自己的份额，也不能直接当成本进程的可用余量。
     */
    struct ASYN_PLATFORM_API CgroupLimits
    {
        std::optional<std::uint64_t> memoryLimitBytes;   ///< 内存上限字节数；0 = 无限制，nullopt = 读不出
        std::optional<std::uint64_t> memoryCurrentBytes; ///< 内存当前用量字节数；nullopt = 读不出
        std::optional<std::uint64_t> pidsLimit;          ///< 进程/线程数上限；0 = 无限制，nullopt = 读不出
        std::optional<std::uint64_t> pidsCurrent;        ///< 进程/线程数当前值；nullopt = 读不出
    };

    /**
     * @brief 本进程所在 cgroup 的只读限额与用量
     *
     * @details 容器 OOM 与线程耗尽发生之前，上层只有这两个分母能给出「离上限还有多远」；
     *          本类只读文件，不改任何调度、限额或默认配置。
     * @note 路径与数值格式按 cgroup v2 统一层级与 v1 控制器层级各实现一份；非 Linux 没有这两条通路。
     */
    class ASYN_PLATFORM_API CgroupInfo
    {
    public:
        /**
         * @brief 读本进程所在 cgroup 的限额与用量
         * @return CgroupLimits 读到的字段；非 Linux、没有 cgroup 或文件不可读时各字段为 nullopt
         * @note 每次调用都现读文件，不缓存：限额可能在运行期被运维改动
         */
        [[nodiscard]] static CgroupLimits read() noexcept;

        /**
         * @brief 从指定的 cgroup 挂载根与 /proc/self/cgroup 文本读取读数
         * @details 挂载根与层级记录由调用方给出，夹具因此能覆盖 v1/v2、max 与缺文件各条分支；
         *          生产路径走 read()。
         * @param cgroupRoot cgroup 文件系统的挂载根（通常为 /sys/fs/cgroup）
         * @param selfCgroupContents /proc/self/cgroup 的全文
         * @return CgroupLimits 从 root 下对应控制组目录读到的字段
         * @note 控制器层级逐行判定：v1 行里出现的 memory/pids 优先，否则落到 v2 的 0:: 行
         */
        [[nodiscard]] static CgroupLimits readFrom(const std::filesystem::path &cgroupRoot, std::string_view selfCgroupContents) noexcept;
    };
} // namespace AsynGyanis::Platform
