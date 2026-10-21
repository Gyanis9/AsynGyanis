/**
 * @file SignalWatcher.h
 * @brief 进程级信号的屏蔽、等待与 Windows 控制台事件
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/Platform.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#endif

namespace AsynGyanis::Platform
{
    /**
     * @brief 观察者要交回的信号类别
     */
    enum class SignalEvent : std::uint8_t
    {
        Interrupt,    ///< SIGINT / Ctrl+C
        Terminate,    ///< SIGTERM
        Reload,       ///< SIGHUP
        ConsoleClose, ///< Windows 的关闭、注销或关机事件
        Unknown       ///< 信号集里出现了未分类的号
    };

    /**
     * @brief 一组进程信号；Windows 上没有 POSIX 信号，用「是否接管控制台停机事件」表达
     */
    class ASYN_PLATFORM_API SignalSet
    {
    public:
        /// 空集合
        SignalSet() noexcept;

        /**
         * @brief 停机信号集：POSIX 是 SIGINT/SIGTERM；Windows 标记为接管控制台停机事件
         */
        [[nodiscard]] static SignalSet forShutdown() noexcept;

        /**
         * @brief 重载信号集：POSIX 是 SIGHUP；Windows 为空（没有重载类控制台事件）
         */
        [[nodiscard]] static SignalSet forReload() noexcept;

        /**
         * @brief 往集合里加一个信号（POSIX；Windows 返回 false）
         * @param signalNumber 信号编号
         * @return true 已加入
         */
        bool add(int signalNumber) noexcept;

        /**
         * @brief 集合里是否有这个信号
         * @param signalNumber 信号编号
         * @return true 在集合里
         */
        [[nodiscard]] bool contains(int signalNumber) const noexcept;

        /**
         * @brief 集合是否为空（Windows 上恒为 true，停机语义由 capturesConsoleShutdown 表达）
         */
        [[nodiscard]] bool isEmpty() const noexcept;

        /**
         * @brief 是否接管 Windows 控制台停机事件
         * @return Windows 上停机集合为 true；其余为 false
         */
        [[nodiscard]] bool capturesConsoleShutdown() const noexcept;

        /**
         * @brief 把集合里的信号挡进当前线程的屏蔽字（POSIX）
         * @param signals 目标集合
         * @return true 已屏蔽（或本平台没有屏蔽字）；false 系统调用失败
         */
        [[nodiscard]] static bool block(const SignalSet &signals) noexcept;

        /**
         * @brief 撤掉集合里的信号在当前线程的屏蔽（POSIX）
         * @param signals 目标集合
         * @return true 已撤掉（或本平台没有屏蔽字）；false 系统调用失败
         */
        [[nodiscard]] static bool unblock(const SignalSet &signals) noexcept;

    private:
#if !ASYN_PLATFORM_WIN32
        sigset_t m_set{}; ///< POSIX 信号集
#endif
        bool m_capturesConsoleShutdown{false}; ///< Windows 控制台停机事件

        friend class SignalWatcher;
    };

    /**
     * @brief 信号观察者：屏蔽集合、在独立线程上等待并回调，Windows 用控制台处理器
     *
     * @details POSIX 侧构造时把集合挡进调用线程，等待线程再把停机/重载受管信号全部挡住后
     *          `sigtimedwait` 轮询；这样两个观察者（停机/重载）的构造顺序不影响彼此的线程。
     *          Windows 侧只安装一个控制台处理器，把 CTRL_* 事件映射成 SignalEvent。
     * @note 一个进程只应有一个控制台停机观察者；重复安装由构造函数拒绝（Core 侧另有属主检查）。
     * @note 回调在等待线程或系统控制线程上执行；控制台关闭事件会等回调返回，回调必须自行设上限。
     */
    class ASYN_PLATFORM_API SignalWatcher
    {
    public:
        /// 收到信号时的处理函数；返回 false 表示未接管（Windows 交回系统缺省动作）
        using Handler = std::function<bool(SignalEvent)>;

        /// 等待/回调异常的报告函数；为空时丢弃
        using ErrorHandler = std::function<void(std::string_view)>;

        /**
         * @brief 屏蔽/安装并启动观察
         * @param signals 目标集合
         * @param handler 信号处理函数
         * @param errorHandler 错误报告函数，可为空
         * @throws std::system_error 屏蔽失败、等待线程启动失败或控制台处理器安装失败
         * @throws std::logic_error 本进程已有控制台停机观察者
         */
        SignalWatcher(SignalSet signals, Handler handler, ErrorHandler errorHandler = {});

        /**
         * @brief 停止观察并还原屏蔽/移除控制台处理器
         */
        ~SignalWatcher();

        SignalWatcher(const SignalWatcher &)            = delete;
        SignalWatcher &operator=(const SignalWatcher &) = delete;
        SignalWatcher(SignalWatcher &&)                 = delete;
        SignalWatcher &operator=(SignalWatcher &&)      = delete;

        /**
         * @brief 观察是否真的装上了
         * @return true 已安装；false 表示平台没有这条路径（例如 Windows 上的重载集合）
         */
        [[nodiscard]] bool isInstalled() const noexcept;

    private:
        /**
         * @brief 把事件交给回调并接住异常
         * @param event 事件
         * @return 回调的返回值；没有回调或抛异常时为 false
         */
        bool dispatch(SignalEvent event) noexcept;

        /**
         * @brief 报告一条错误
         * @param message 错误文本
         */
        void reportError(std::string_view message) const noexcept;

#if ASYN_PLATFORM_WIN32
        /**
         * @brief 系统要求的控制台处理器跳板
         * @param controlType 控制台事件类型
         * @return TRUE 已接管；FALSE 交还系统
         */
        static int __stdcall consoleHandler(unsigned long controlType) noexcept;
#endif

        SignalSet    m_signals;            ///< 目标集合（析构时还原屏蔽）
        Handler      m_handler;            ///< 信号处理函数
        ErrorHandler m_errorHandler;       ///< 错误报告函数
        bool         m_isInstalled{false}; ///< 是否真的装上
#if !ASYN_PLATFORM_WIN32
        std::jthread m_waiter; ///< 等待线程；声明在最后，析构时最先停
#endif
    };
} // namespace AsynGyanis::Platform
