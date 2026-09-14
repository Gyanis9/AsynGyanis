/**
 * @file Process.h
 * @brief 子进程的启动、观察与终止：多进程 worker 模型的平台底座
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
     *          请求它退出或强杀。请求退出在 POSIX 上走 SIGTERM，Windows 没有信号，
     *          因此该方法在 Windows 上返回 false（调用方据此改用强杀或自行约定退出机制）。
     *
     * @warning spawn() 在 POSIX 上是 fork + exec：fork 之后、exec 之前子进程只能调用
     *          async-signal-safe 的函数，且**父进程此刻不能有其它线程**（多线程下 fork 出的子进程
     *          只带着调用线程，锁状态与运行库状态都可能不自洽）。因此它只该在启动服务器之前、
     *          进程还是单线程时调用——多进程 worker 模型正是在那一刻起的。
     * @note 失败不抛异常：与 Platform 层「错误码 + 返回值」的惯例一致（失败时句柄无效，
     *       原因见 PlatformError::lastErrorCode()），异常由上层折成 Base::SystemException。
     */
    class Process
    {
    public:
        /**
         * @brief 子进程句柄
         *
         * @details 持有平台侧句柄，析构时释放它（Windows 关句柄，POSIX 顺手回收已退出的子进程
         *          以免留下僵尸），但**不会**终止仍在运行的进程——终止是显式动作
         *          （requestTermination()/forceTermination()），不做成析构的副作用。
         */
        class Handle
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
             */
            void close() noexcept;

        private:
            friend class Process;

            /// 直接构造：只允许 Process 产生有效句柄
#if ASYN_PLATFORM_WIN32
            explicit Handle(void *processHandle, unsigned long processId) noexcept;
#else
            explicit Handle(int processId) noexcept;
#endif

#if ASYN_PLATFORM_WIN32
            void         *m_processHandle{nullptr}; ///< 进程句柄；空表示无效
            unsigned long m_processId{0};           ///< 进程号，仅用于日志
#else
            int m_processId{-1}; ///< 进程号；负数表示无效
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
        };

        /**
         * @brief 启动一个子进程
         * @param options 启动参数
         * @return Handle 子进程句柄；启动失败时 isValid() 为 false，原因见 PlatformError::lastErrorCode()
         * @note POSIX 上子进程 exec 失败时以 127 退出（这是本层的约定，调用方据此区分「exec 没起来」
         *       与业务自己的退出码；POSIX 的 shell 惯例与之相同）
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
         * @return std::optional<int> 已退出时给出退出码；仍在运行、句柄无效或退出码已被别处回收
         *         时返回 std::nullopt
         */
        [[nodiscard]] static std::optional<int> pollExitCode(const Handle &handle) noexcept;

        /**
         * @brief 请求子进程体面退出
         * @details POSIX 上发 SIGTERM，由子进程自己把退出做干净（本框架的服务器据此走
         *          stop()/drain()）。
         * @param handle 目标句柄
         * @return true 已发出请求
         * @return false 平台不支持（Windows 没有信号）或句柄无效——调用方应改用 forceTermination()
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
