/**
 * @file TimerFileDescriptor.h
 * @brief 跨平台定时器描述符，把定时到期表现为描述符可读
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <chrono>

namespace AsynGyanis::Platform
{
    /**
     * @brief 一次性定时器描述符
     *
     * @details Linux 基于 timerfd_create / timerfd_settime 实现；Windows 无 timerfd，
     *          用 TimerQueue 定时器在到期时向 socket 对写端写一字节，使读端变为可读。
     * @note 用法：把 fileDescriptor() 注册进 epoll 监听 EPOLLIN，调用 arm() 设定到期时间，
     *       被唤醒后调用 drain() 清空到期计数。
     */
    class TimerFileDescriptor
    {
    public:
        /**
         * @brief 创建定时器底层句柄
         */
        TimerFileDescriptor();

        /**
         * @brief 取消未决定时器并关闭全部句柄
         */
        ~TimerFileDescriptor();

        TimerFileDescriptor(const TimerFileDescriptor &) = delete;

        TimerFileDescriptor &operator=(const TimerFileDescriptor &) = delete;

        TimerFileDescriptor(TimerFileDescriptor &&) = delete;

        TimerFileDescriptor &operator=(TimerFileDescriptor &&) = delete;

        /**
         * @brief 获取供事件循环监听的可读描述符
         * @return int 描述符，创建失败时为 FileDescriptor::kInvalid
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 定时器是否已成功创建
         * @return true 描述符可用
         * @return false 底层句柄创建失败
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 设定一次性到期时间
         * @details 重复调用会覆盖上一次未决的到期设定；传入非正值等价于调用 cancel()。
         * @param duration 距离到期的时长
         */
        void arm(std::chrono::milliseconds duration) noexcept;

        /**
         * @brief 取消未决的到期设定
         * @details Linux 通过写入零值 itimerspec 解除，Windows 删除 TimerQueue 定时器并
         *          等待回调结束，保证返回后不再有任何到期通知。
         */
        void cancel() noexcept;

        /**
         * @brief 读空到期数据，使描述符重新回到不可读状态
         */
        void drain() const noexcept;

    private:
        int m_fileDescriptor{-1}; ///< 到期时变为可读的描述符（注册到事件循环）

#if ASYN_PLATFORM_WIN32
        /**
         * @brief TimerQueue 到期回调，向 socket 对写端写入一字节使读端变为可读
         * @param context 登记回调时传入的 TimerFileDescriptor 实例指针
         * @param timerOrWaitFired Windows 传入的到期标志，本实现未使用
         */
        static VOID CALLBACK timerCallback(PVOID context, BOOLEAN timerOrWaitFired);

        int    m_writeDescriptor{-1};  ///< 写端描述符（定时器回调写入）
        HANDLE m_timerHandle{nullptr}; ///< TimerQueue 定时器句柄
#endif
    };
} // namespace AsynGyanis::Platform
