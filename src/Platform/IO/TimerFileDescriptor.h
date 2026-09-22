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
     * @details Linux 基于 timerfd_create / timerfd_settime 实现；Windows 无 timerfd，用高精度
     *          可等待定时器 + 线程池等待，到期时向 socket 对写端写一字节，使读端变为可读。
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
         * @details Windows 侧在关描述符之前阻塞注销到期等待：回调要往写端写字节，而描述符号一旦
         *          被别的套接字复用，那次残留的写就成了往陌生连接里灌一个字节。
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
         * @details 重复调用直接覆盖上一次未决的到期设定（Windows 侧的 SetWaitableTimer 本身就是
         *          替换待决到期，不需要先解除）。传入非正值等价于调用 cancel()。
         *          登记失败时**没有任何到期会到来**，调用方必须按返回值处理，不能当成本次设定已生效。
         * @param duration 距离到期的时长
         * @return true 设定已生效；false 底层登记失败（描述符无效或系统资源不足），
         *         本次不会到期
         */
        [[nodiscard]] bool arm(std::chrono::milliseconds duration) noexcept;

        /**
         * @brief 取消未决的到期设定
         * @details Linux 写入零值 itimerspec 解除，Windows 调 CancelWaitableTimer 解除，两边都不
         *          阻塞。取消后不再会有新的到期信号，但**若取消那一刻回调已在途中**，写端仍可能落下
         *          一个字节：读端因此可能多醒一次，派发时并无到期项，按新的堆顶重新武装即可。
         * @note 与描述符一起销毁时必须先注销等待再关描述符（见析构），本方法不做那一步。
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
         * @brief 线程池等待回调：定时器被置信号后向 socket 对写端写入一字节使读端变为可读
         * @param context 登记等待时传入的 TimerFileDescriptor 实例指针
         * @param timerOrWaitFired Windows 的到期标志；等可等待定时器对象时它恒为 FALSE，
         *        分不出到期与等待超时，因此本实现不看它
         */
        static VOID CALLBACK timerCallback(PVOID context, BOOLEAN timerOrWaitFired);

        int    m_writeDescriptor{-1};      ///< 写端描述符（定时器回调写入）
        HANDLE m_waitableTimer{nullptr};   ///< 高精度可等待定时器句柄（拿不到高精度档时退化为普通档）
        HANDLE m_waitRegistration{nullptr}; ///< 线程池等待登记句柄，销毁时要先阻塞注销再关上面的句柄
#endif
    };
} // namespace AsynGyanis::Platform
