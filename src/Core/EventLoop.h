/**
 * @file EventLoop.h
 * @brief 每线程事件循环 — 基于 epoll 驱动，协程调度核心
 * @copyright Copyright (c) 2026
 */

#ifndef CORE_EVENTLOOP_H
#define CORE_EVENTLOOP_H

#include "Epoll.h"
#include "Scheduler.h"

#include <atomic>

namespace Core
{
    /**
     * @brief 事件循环类
     *
     * 封装 epoll 事件监控和协程调度器，运行在单一线程中。
     * 支持跨线程唤醒、优雅停止与重启。每个线程通常拥有一个 EventLoop 实例。
     */
    class EventLoop
    {
    public:
        /**
         * @brief 构造一个事件循环对象
         *
         * 初始化 epoll 实例、内部调度器以及唤醒机制所需的文件描述符。
         */
        EventLoop();

        /**
         * @brief 销毁事件循环
         */
        ~EventLoop();

        EventLoop(const EventLoop &)            = delete;
        EventLoop &operator=(const EventLoop &) = delete;

        /**
         * @brief 启动事件循环（阻塞当前线程）
         *
         * 该函数会持续调用 epoll_wait 处理就绪事件，并执行调度器的协程任务。
         * 只有当 stop() 被调用或发生未捕获的错误时才会返回。
         */
        void run();

        /**
         * @brief 请求停止事件循环（非阻塞）
         *
         * 设置停止标志，并通过 wake() 唤醒可能阻塞的 epoll_wait，使 run() 方法尽快返回。
         * @note 可被任意线程调用，线程安全。
         */
        void stop();

        /**
         * @brief 唤醒阻塞在 epoll_wait 上的事件循环（非阻塞）
         *
         * 向唤醒文件描述符写入数据，迫使 epoll_wait 立即返回。
         * 通常用于跨线程通知事件循环有新的任务需要处理，或配合 stop() 加速退出。
         */
        void wake() const;

        /**
         * @brief 获取 epoll 监控器（非常量引用）
         * @return Epoll& 内部 epoll 对象，可用于注册或修改文件描述符的监听事件
         */
        [[nodiscard]] Epoll &epoll() noexcept;

        /**
         * @brief 获取协程调度器（非常量引用）
         * @return Scheduler& 内部调度器对象，用于提交和管理协程任务
         */
        [[nodiscard]] Scheduler &scheduler() noexcept;

        /**
         * @brief 检查事件循环是否正在运行
         * @return true 表示正处于 run() 循环中，false 表示已停止或尚未启动
         */
        [[nodiscard]] bool isRunning() const noexcept;

    private:
        Epoll             m_epoll;          ///< epoll 事件管理器
        Scheduler         m_scheduler;      ///< 协程调度器，管理待运行的任务队列
        int               m_wakeupFd;       ///< 唤醒文件描述符（通常是 eventfd 或 pipe），用于跨线程唤醒
        int               m_wakeupSentinel; ///< 唤醒哨兵值，用于识别唤醒事件（可选的内部标记）
        std::atomic<bool> m_running;        ///< 循环是否正在运行中（原子标记）
        std::atomic<bool> m_stopRequested;  ///< 是否已请求停止（原子标记，线程安全）
    };
}

#endif
