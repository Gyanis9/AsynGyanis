/**
 * @file WorkerSupervisorSupport.h
 * @brief WorkerSupervisor 拆分后的共享内部常量
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <chrono>

namespace AsynGyanis::Core
{
    /// 等待 worker 退出时的轮询间隔：比 pollInterval 细，让收尾尽量贴着 shutdownTimeout 收干净
    inline constexpr std::chrono::milliseconds kWorkerReapPollInterval{20};

    /// 强杀之后等子进程被回收的上限：SIGKILL 的投递与调度有个短窗口，
    /// 立刻释放句柄会把 pid 丢掉、从此没人收尸（POSIX 上就是僵尸进程）
    inline constexpr std::chrono::milliseconds kWorkerForcedReapWait{2000};
} // namespace AsynGyanis::Core
