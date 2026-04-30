/**
 * @file Timer.h
 * @brief 可 co_await 的定时器（基于 timerfd + epoll）
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include "EpollAwaiter.h"

#include <chrono>
#include <coroutine>

namespace Core
{
    class EventLoop;

    /**
     * @brief 基于 timerfd 和 epoll 的可等待定时器。
     *
     * 提供协程友好的定时等待功能，通过 co_await Timer::waitFor(duration) 实现非阻塞延迟。
     * 内部使用 timerfd 创建定时器文件描述符，并注册到 EventLoop 的 epoll 实例中。
     */
    class Timer
    {
    public:
        /**
         * @brief Timer 的等待器（awaitable 对象），由 waitFor() 返回。
         *
         * 封装 EpollAwaiter，在定时器 fd 可读（即超时）时恢复协程。
         */
        class Awaiter
        {
        public:
            /**
             * @brief 构造 Awaiter 对象。
             * @param epoll epoll 实例引用，用于注册等待事件
             * @param fd    timerfd 的文件描述符
             */
            Awaiter(Epoll &epoll, int fd) noexcept;

            /**
             * @brief 是否已就绪（定时器是否已经超时）。
             * @return 永远返回 false，因为定时器需要等待，不会立即就绪。
             */
            bool await_ready() const noexcept;

            /**
             * @brief 协程挂起时注册 epoll 等待事件。
             * @param handle 当前协程的句柄，将在超时后被唤醒
             */
            void await_suspend(std::coroutine_handle<> handle) const noexcept;

            /**
             * @brief 协程恢复时执行（无返回值，超时后只是唤醒）。
             */
            void await_resume() const;

        private:
            EpollAwaiter m_awaiter; ///< 内部封装的 epoll 等待器
            int          m_fd;      ///< timerfd 文件描述符
        };

        /**
         * @brief 构造 Timer 对象。
         * @param loop 事件循环引用，用于注册 epoll 事件
         */
        explicit Timer(EventLoop &loop);

        /**
         * @brief 析构函数，关闭 timerfd 并移除 epoll 注册。
         */
        ~Timer();

        /**
         * @brief 创建一个等待器，使得 co_await timer.waitFor(duration) 能够延迟指定时长。
         * @param duration 需要等待的时长
         * @return Awaiter 对象，可用于 co_await
         */
        Awaiter waitFor(std::chrono::milliseconds duration) const;

    private:
        EventLoop &m_loop;        ///< 所属事件循环
        int        m_timerFd{-1}; ///< timerfd 文件描述符，-1 表示无效
    };

}

#endif
