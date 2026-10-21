/**
 * @file CrashHandler.h
 * @brief 崩溃信号的平台骨架：处置安装、备用信号栈、落场子进程与 stderr 写出
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/Platform.h"

#include <cstdint>
#include <string_view>

namespace AsynGyanis::Platform
{
    /**
     * @brief 崩溃处理器认得的致命信号类别（平台无关）
     */
    enum class CrashSignal : std::uint8_t
    {
        SegmentationFault,  ///< SIGSEGV：无效内存访问
        Abort,              ///< SIGABRT：abort 或断言失败
        BusError,           ///< SIGBUS：总线错误；Windows 没有这一类
        ArithmeticFault,    ///< SIGFPE：算术异常
        IllegalInstruction, ///< SIGILL：非法指令
        Unknown             ///< 不在上面几类里的信号
    };

    /**
     * @brief 崩溃现场的进程级接管
     *
     * @details 安装致命信号处置与每线程备用信号栈；信号到来时先调最小现场回调，Linux 再把完整
     *          现场移进 fork 出的子进程并给子进程一个 alarm 兜底，最后恢复默认处置并重发信号，
     *          让进程仍以「信号致死」退出。Windows 没有 fork，完整现场在信号上下文里就地写。
     * @note 一个进程只应安装一次；重复调用返回 false，但仍会为调用线程补挂备用信号栈。
     * @note 回调在信号上下文里执行，不得分配、不得加锁（Linux 的完整现场回调在子进程里，可以分配）。
     */
    class ASYN_PLATFORM_API CrashHandler
    {
    public:
        /// 现场回调；context 是安装时交回的指针，原样传回
        using Reporter = void (*)(CrashSignal signal, void *context) noexcept;

        /**
         * @brief 安装致命信号处置与调用线程的备用信号栈
         * @param minimalReporter 最小现场回调，必须先于任何分配执行；可为空
         * @param fullReporter 完整现场回调；可为空
         * @param context 原样传给两个回调
         * @return true 本次完成了安装；false 此前已经装过
         */
        static bool install(Reporter minimalReporter, Reporter fullReporter, void *context = nullptr) noexcept;

        /**
         * @brief 本进程是否已安装崩溃处置
         * @return true 已安装
         */
        [[nodiscard]] static bool isInstalled() noexcept;

        /**
         * @brief 把一段文本用一次写送到 stderr
         * @param text 待写文本；空串是空操作
         * @note Windows 走 CRT 的 _write（stderr 不是 socket），POSIX 走描述符写；都不重试
         */
        static void writeStandardError(std::string_view text) noexcept;

        /**
         * @brief 把某个崩溃信号的处置恢复成平台默认
         * @param signal 信号类别；Unknown 或本平台没有的类别是空操作
         */
        static void restoreDefaultDisposition(CrashSignal signal) noexcept;

    private:
        CrashHandler() = delete;
    };
} // namespace AsynGyanis::Platform
