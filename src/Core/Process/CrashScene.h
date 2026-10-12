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
     * @warning 现场是 **best-effort**，且「死得正确」不再依赖它写不写得全：信号处理器内先只走无分配的
     *          一条（静态文本 + 手工转写的最小现场）；会分配/加锁的完整现场（std::format 与
     *          std::stacktrace 的符号解析）在 Linux 侧移进 fork 出来的子进程写——子进程被继承的锁
     *          锁死也只锁它自己（alarm 兜底收走），进程仍以「信号致死」退出，WIFSIGNALED 与 coredump
     *          不受影响。崩溃发生在分配器持锁处时，完整现场可能残缺，最小现场必有。
     * @note Linux 侧处理器经 SA_ONSTACK 落在**每条线程各自**的备用栈上（128 KiB，随该线程第一次调用
     *       install() 时挂上）：爆栈引发的 SIGSEGV 因此也进得来——其余线程要各自调一次 install()
     *       才有这份落脚处（信号处置是进程级的，重复调用只补备用栈）。
     * @note Windows 侧没有 fork：完整现场在进程内尽力写（最小现场先落，stderr 走 CRT 的 _write）。
     *       该侧 CRT 对硬件异常的信号路由与 raise 路径没有本仓用例覆盖，属未验证面
     *       （Linux 侧由 TestCrashScene 的用例覆盖）。
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
