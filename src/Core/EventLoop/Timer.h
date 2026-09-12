/**
 * @file Timer.h
 * @brief 可 co_await 的定时器（基于 timerfd + epoll）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/TimerFileDescriptor.h"

#include <chrono>
#include <coroutine>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 基于 timerfd 和 epoll 的可等待定时器。
     *
     * 提供协程友好的定时等待功能，通过 co_await Timer::waitFor(duration) 实现非阻塞延迟。
     * 内部的 timerfd 在构造时**常驻注册**到 EventLoop 的 epoll（一次注册、反复等待），
     * 因此每次等待都不产生 epoll_ctl；就绪与否则由注册对象的就绪缓存保证不丢。
     */
    class Timer
    {
    public:
        /**
         * @brief Timer 的等待器（awaitable 对象），由 waitFor() 返回。
         *
         * 在定时器 fd 可读（即超时）时恢复协程，并在恢复时读走过期计数。
         */
        class Awaiter
        {
        public:
            /**
             * @brief 构造 Awaiter 对象。
             * @param watcher 定时器 fd 的常驻注册对象
             * @param fileDescriptor timerfd 的文件描述符（恢复时用于读走过期计数）
             */
            Awaiter(IoWatcher &watcher, int fileDescriptor) noexcept;

            /**
             * @brief 是否已就绪（定时器在本次等待之前就已经到期）。
             * @return true 就绪标记已在，等待立即完成
             * @return false 需要挂起等待
             */
            [[nodiscard]] bool await_ready() noexcept;

            /**
             * @brief 协程挂起时登记等待事件。
             * @param handle 当前协程的句柄，将在超时后被唤醒
             * @return true 已登记，可以挂起
             * @return false 注册已失效（定时器已销毁），不挂起
             */
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

            /**
             * @brief 协程恢复时读走过期计数
             * @details 必须读：timerfd 的「可读」是靠读操作清掉的，只等不读会让它一直保持可读，
             *          而边沿触发不会再报第二次——下一次等待就会永远等不到。
             */
            void await_resume();

        private:
            IoWatcher::Awaiter m_awaiter;        ///< 内层等待器（常驻注册）
            int                m_fileDescriptor; ///< timerfd 文件描述符
        };

        /**
         * @brief 构造 Timer 对象。
         * @param loop 事件循环引用，用于常驻注册 timerfd
         * @throws Base::SystemException 创建定时器文件描述符失败（描述符无效），
         *         异常文本带 errno 与其可读描述；这是启动期故障，不可重试；
         *         或描述符有效但注册到 epoll 失败
         */
        explicit Timer(EventLoop &loop);

        /**
         * @brief 析构函数：反注册并唤醒仍挂着的等待者，随后关闭 timerfd。
         */
        ~Timer();

        /**
         * @brief 创建一个等待器，使得 co_await timer.waitFor(duration) 能够延迟指定时长。
         * @param duration 需要等待的时长
         * @return Awaiter 对象，可用于 co_await；定时器若在此之前已到期则立即完成
         */
        Awaiter waitFor(std::chrono::milliseconds duration);

    private:
        EventLoop &                   m_loop;    ///< 所属事件循环
        Platform::TimerFileDescriptor m_timer;   ///< 跨平台定时器文件描述符
        IoWatcher                     m_watcher; ///< timerfd 的常驻 epoll 注册（等待时武装可读）
    };

}
