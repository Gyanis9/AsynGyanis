/**
 * @file CrashScene.h
 * @brief 崩溃现场：信号与 terminate 的进程级处理器，把「怎么死的、死在哪」写出来再按默认语义死
 * @author Gyanis
 * @date 2026-10-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

namespace AsynGyanis::Core
{
    /**
     * @brief 崩溃现场记录器（进程级、可重复安装的服务器前提件）
     *
     * @details 没有处理器时，一次段错误只留下「进程没了」：服务侧看的是 PID 消失与补位计数，
     *          堆栈在哪里断的一无所知。装上之后，致命信号与 std::terminate 都会往 stderr
     *          写一份现场（致因、进程号、线程指纹、捕获到的调用栈），随后**恢复默认处置并重发
     *          信号**——进程仍以「信号致死」退出，WIFSIGNALED 与 coredump 的语义都保留，
     *          上层看护（WorkerSupervisor 的补位计数）照常工作。
     *
     * @warning 现场是 **best-effort**：栈捕获（std::stacktrace）与符号解析都会分配内存，
     *          在信号上下文里不保证重入安全——崩在分配器里时这份现场可能残缺，但「写不写得全」
     *          不影响「死得正确」：任何一步失败都退回最小现场或直接放行到默认处置。
     * @note 现场写 stderr 而不是日志器：崩溃时刻日志器可能正持锁或有未刷出的缓冲，
     *       而服务的 stderr 按部署口径本来就该落到文件（run-soak 与 CI 都是这么接的）。
     */
    class ASYN_CORE_API CrashScene
    {
    public:
        /**
         * @brief 安装崩溃现场处理器（进程级）
         * @details 覆盖 SIGSEGV / SIGABRT / SIGFPE / SIGILL（Windows 与 Linux 同名的那一组）
         *          与 Linux 多出的 SIGBUS，外加 std::set_terminate；调试器挂着的场合请自行决定
         *          要不要装（GDB 断在信号上时本处理器不会抢在它前面）。重复调用是空操作
         * @return true 本次调用完成了安装；false 此前已装过
         */
        static bool install() noexcept;

        /**
         * @brief 本进程是否已安装崩溃现场处理器
         * @return true 已安装
         */
        [[nodiscard]] static bool isInstalled() noexcept;
    };
} // namespace AsynGyanis::Core
