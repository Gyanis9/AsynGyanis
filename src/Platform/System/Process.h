/**
 * @file Process.h
 * @brief 子进程的启动、观察与终止：多进程 worker 模型的平台底座
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/Platform.h"

#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Platform
{
    /**
     * @brief 子进程的启动、观察与终止
     *
     * @details 只提供进程编排放得下的那几件事：起一个进程、问它是否还在、取它的退出码、
     *          请求它退出或强杀。请求退出在 POSIX 上走 SIGTERM；Windows 没有信号，走控制台事件
     *          （CTRL_BREAK），因此要求派生时给出 `LaunchOptions::ownProcessGroup`，否则本方法返回
     *          false，调用方据此改用强杀或自行约定退出机制。
     *
     * @warning spawn() 在 POSIX 上是 fork + exec：fork 之后、exec 之前子进程只能调用
     *          async-signal-safe 的函数，且**父进程此刻不能有其它线程**（多线程下 fork 出的子进程
     *          只带着调用线程，锁状态与运行库状态都可能不自洽）。因此它只该在启动服务器之前、
     *          进程还是单线程时调用——多进程 worker 模型正是在那一刻起的。
     * @note 失败不抛异常：与 Platform 层「错误码 + 返回值」的惯例一致（失败时句柄无效，
     *       原因见 PlatformError::lastErrorCode()），异常由上层折成 Base::SystemException。
     */
    class ASYN_PLATFORM_API Process
    {
    public:
        /**
         * @brief 子进程句柄
         *
         * @details 持有平台侧句柄，析构时释放它（Windows 关句柄，POSIX 顺手回收已退出的子进程
         *          以免留下僵尸），但**不会**终止仍在运行的进程——终止是显式动作
         *          （requestTermination()/forceTermination()），不做成析构的副作用。
         */
        class ASYN_PLATFORM_API Handle
        {
        public:
            Handle() = default;

            ~Handle();

            Handle(Handle &&other) noexcept;

            Handle &operator=(Handle &&other) noexcept;

            Handle(const Handle &) = delete;

            Handle &operator=(const Handle &) = delete;

            /**
             * @brief 句柄是否有效（spawn() 成功过、且未被移动走或关闭）
             * @return true 可用于观察或终止该子进程
             */
            [[nodiscard]] bool isValid() const noexcept;

            /**
             * @brief 取子进程的进程号
             * @return long 进程号，仅用于日志与诊断；句柄无效时返回 0
             */
            [[nodiscard]] long processId() const noexcept;

            /**
             * @brief 释放平台侧句柄
             * @details 幂等。已退出但尚未被取退出码的子进程在这里回收，不留下僵尸；
             *          仍在运行的进程不受影响（句柄一释放就再也观察不到它了）
             * @warning 带 `LaunchOptions::killWithParent` 的句柄是例外：关闭作业句柄就是「随父终止」的
             *          实施方式，仍在运行的子进程会被系统终止。要在本进程退出不杀子进程的形态，
             *          派生时别给这个开关
             * @warning 句柄一释放，那个 pid 就再也没人回收：**刚强杀过子进程的调用方必须先等它结束
             *          再释放句柄**（`isRunning()` / `pollExitCode()` 观察时顺手回收），否则 POSIX 上
             *          会留下僵尸进程。`WorkerSupervisor::shutdown()` 的强杀路径就是这么做的
             */
            void close() noexcept;

            /**
             * @brief 「随父终止」的保护这轮到底生效了没有
             * @details 派生时给了 `LaunchOptions::killWithParent` 不代表系统接受了：主机已在一个禁止嵌套
             *          的作业里时（某些容器与 CI 环境），挂作业会在 `AssignProcessToJobObject` 那步失败，
             *          而进程照常起来了。此时本方法为 false，调用方要点名「保护缺席」而不是当作没事。
             * @return true POSIX 恒为 true（那边一律装 `PR_SET_PDEATHSIG`）；Windows 上表示作业已挂上
             * @return false Windows 上没挂上作业（含压根没要这个保护的情况）
             */
            [[nodiscard]] bool killWithParentGuardActive() const noexcept;

        private:
            friend class Process;

            /// 直接构造：只允许 Process 产生有效句柄。ownConsoleGroup 与 jobHandle 只在 Windows 有意义，
            /// 见 LaunchOptions 里那两个开关的说明
#if ASYN_PLATFORM_WIN32
            explicit Handle(void *processHandle, unsigned long processId, bool ownConsoleGroup = false, void *jobHandle = nullptr) noexcept;
#else
            explicit Handle(int processId) noexcept;
#endif

#if ASYN_PLATFORM_WIN32
            void         *m_processHandle{nullptr}; ///< 进程句柄；空表示无效
            unsigned long m_processId{0};           ///< 进程号，仅用于日志
            /// 本次派生有没有给子进程独立的可控台进程组。只有它成立才允许发 CTRL_BREAK：
            /// 否则「目标进程组」就是我们自己所在的组，那一下会打断宿主自己的键盘输入与服务循环
            bool m_ownsConsoleGroup{false};
            /// 随父终止用的作业句柄；空表示没要这个保护，或系统不让挂（见 killWithParentGuardActive）
            /// 它的生命周期就是保护的寿命：句柄一关（含本进程退出时由系统收回），作业里的子进程即被终止
            void *m_jobHandle{nullptr};
#else
            /// 进程号；负数表示无效。
            /// **mutable**：观察类接口（pollExitCode）收的是 const 引用，而「子进程已被回收」
            /// 这件事只能在那次观察里发现——发现时要把句柄一并作废（否则后面拿这个 pid 去 kill
            /// 可能打到复用它的无关进程），所以它必须能在 const 路径上写
            mutable int m_processId{-1};
#endif
            /// 已回收时的退出码：pollExitCode()/isRunning() 都可能触发回收，回收过一次就记住，
            /// 否则第二次问会拿到「查不到这个子进程」。mutable 是因为它只是缓存——观察一个进程
            /// 不该要求调用方持有非 const 句柄
            mutable std::optional<int> m_exitCode;
        };

        /**
         * @brief 启动参数
         */
        struct LaunchOptions
        {
            std::string              executablePath; ///< 可执行文件路径：不含目录时按 PATH 查找
            std::vector<std::string> arguments;      ///< 参数表（不含 argv[0]；本层按平台补上）
            /// Windows 专用：给子进程独立的可控台进程组，这是 `requestTermination()` 能发
            /// CTRL_BREAK 的前提（不发就只能在「等一等」与「直接强杀」之间二选一）。
            /// POSIX 上无效果——那边的体面退出走 SIGTERM。默认 false：不给就仍只能强杀
            bool ownProcessGroup{false};
            /**
             * @brief 让子进程随本进程消失（Windows 侧的对等物，POSIX 上无效果）
             * @details POSIX 的 spawn 一律装 `PR_SET_PDEATHSIG`，子进程在父侧退出时就会收到 SIGTERM，
             *          不需要这个开关。Windows 没有这条约定，靠作业对象实现：派生时把子进程放进一个
             *          带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的作业，作业句柄随 `Handle` 活着——
             *          本进程被硬杀（句柄由系统收回）时子进程一并被终止，不留一个占着端口却无人编排的孤儿。
             *          刻意默认 false：换代交棒那种「父退出而子必须活下来」的形状要的就是相反的行为，
             *          默认打开会把那条路做成静默自杀。
             * @warning 主机已在一个不允许嵌套的作业里时（某些容器/CI 环境），`AssignProcessToJobObject`
             *          会失败。本层**不因此让派生失败**（起不来服务更糟），但保护就此缺席，
             *          调用方要用 `Handle::killWithParentGuardActive()` 核一遍并出声。
             */
            bool killWithParent{false};
        };

        /**
         * @brief 启动一个子进程
         * @param options 启动参数
         * @return Handle 子进程句柄；启动失败时 isValid() 为 false，原因见 PlatformError::lastErrorCode()
         * @note Windows 上子进程只继承「当下真实存在」的标准句柄：套接字这类可继承句柄不会传下去，
         *       宿主没有控制台（服务、GUI 子系统、被 DETACHED_PROCESS 派出来）时也照样能派生
         * @note POSIX 上子进程 exec 失败时以 127 退出（这是本层的约定，调用方据此区分「exec 没起来」
         *       与业务自己的退出码；POSIX 的 shell 惯例与之相同）
         * @note Linux 上子进程带着「本进程一退出就收 SIGTERM」的约定（exec 之后仍然有效）：编排者被
         *       强杀时不留孤儿进程占着端口。子进程因此不要把自己改成忽略 SIGTERM 的样子——那等于放弃
         *       这条兜底，只能靠编排者主动收口
         * @note Windows 没有那条约定，等价保护要显式要：`LaunchOptions::killWithParent` 把子进程挂进一个
         *       关闭即终止的作业。它可能挂不上（本进程已在一个禁止嵌套的作业里），此时派生照常成功但保护
         *       缺席，务必用 `Handle::killWithParentGuardActive()` 核一遍
         * @warning 该信号的实际触发点是**调用 fork 的那个线程**退出（Linux 语义），不是整个进程：
         *          多线程程序要在还单线程时启动编排，否则线程池收工会提前把子进程带走
         * @warning 「在还单线程时派生」这条还有一层原因：`executablePath` 不含目录时走的是
         *          `execvp`，它要在 PATH 里搜可执行文件，那条路径上可能碰分配器——多线程下 fork 之后
         *          子进程只带着调用线程，父进程此刻若有人握着分配器锁，子进程就会在 exec 之前死等。
         *          给全路径可以绕开这条，但那是调用方的选择，本层不替他改行为
         */
        [[nodiscard]] static Handle spawn(const LaunchOptions &options) noexcept;

        /**
         * @brief 子进程是否仍在运行
         * @details 已退出时顺手回收并把退出码记在句柄里（随后 pollExitCode() 直接给出），
         *          因此本方法与 pollExitCode() 可以任意顺序、任意次数调用。
         * @param handle 目标句柄
         * @return true 仍在运行；句柄无效或已退出时返回 false
         */
        [[nodiscard]] static bool isRunning(const Handle &handle) noexcept;

        /**
         * @brief 取子进程的退出码（不阻塞）
         * @param handle 目标句柄
         * @return std::optional<int> 子进程已结束时给出退出码；仍在运行或句柄已被释放时返回
         *         std::nullopt
         * @retval -1 「这个子进程已被别处回收」（POSIX 的 ECHILD）：退出码无从得知，但「已经不在了」
         *         是确定的事实，因此交出 -1 而不是 nullopt——按「取到值才算结束」轮询的调用方
         *         （编排者等 worker 退出）等 nullopt 会永远等下去
         */
        [[nodiscard]] static std::optional<int> pollExitCode(const Handle &handle) noexcept;

        /**
         * @brief 请求子进程体面退出
         * @details POSIX 上发 SIGTERM，由子进程自己把退出做干净（本框架的服务器据此走
         *          stop()/drain()）。Windows 没有信号，走控制台事件：向子进程**自己名下**的进程组发
         *          CTRL_BREAK，装了控制台处理函数的子进程会收到——前提是派生时给了
         *          `LaunchOptions::ownProcessGroup`。
         * @param handle 目标句柄
         * @return true 已发出请求（不代表子进程已经退出，退出与否要观察到退出码才算）
         * @return false 发不出去：Windows 上句柄没带独立进程组、或宿主没有控制台；调用方据此
         *         决定是继续等还是改用 forceTermination()。这里不会静默升级成强杀
         */
        [[nodiscard]] static bool requestTermination(const Handle &handle) noexcept;

        /**
         * @brief 强制终止子进程
         * @details POSIX 上是 SIGKILL、Windows 上是 TerminateProcess：子进程没有机会做任何收尾。
         *          退出码由平台决定，调用方不要把它当成业务结论。
         * @param handle 目标句柄
         * @return true 已终止
         * @return false 句柄无效或平台调用失败（原因见 PlatformError::lastErrorCode()）
         */
        [[nodiscard]] static bool forceTermination(const Handle &handle) noexcept;
    };
} // namespace AsynGyanis::Platform
