/**
 * @file EventLoop.h
 * @brief 每线程事件循环 — 基于 epoll 驱动，协程调度核心
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Core/EventLoop/Epoll.h"
#include "Core/EventLoop/TimerQueue.h"
#include "Platform/IO/EventNotifier.h"
#include "Core/Coroutine/Scheduler.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace AsynGyanis::Core
{
    /**
     * @brief 事件循环类
     *
     * 封装 epoll 事件监控和协程调度器，运行在单一线程中。
     * 支持跨线程唤醒与优雅停止。每个线程通常拥有一个 EventLoop 实例。
     *
     * @note **一个实例一个运行生命周期**：stop() 置下的停止请求是粘性的，
     *       之后再调用 run() 都会立刻返回（不会阻塞、也不会重新开始跑）。
     *       这个取舍是刻意的——它保证「先 stop() 后 run()」的时序不会丢失停止请求，
     *       而 start() 之后立刻 stop() 正是常见写法。需要重新运行请新建 EventLoop 实例。
     */
    class IoWatcher;
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

        EventLoop(const EventLoop &) = delete;

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
         * @brief 获取定时器队列（非常量引用）
         * @return TimerQueue& 本循环唯一的定时器队列；Timer 只是它的轻量句柄
         */
        [[nodiscard]] TimerQueue &timerQueue() noexcept;

        /**
         * @brief 登记一个存活的事件注册对象（由 IoWatcher 的构造调用）
         * @param watcher 目标对象（非拥有）
         */
        void registerWatcher(const IoWatcher *watcher);

        /**
         * @brief 注销一个事件注册对象（由 IoWatcher 的析构调用）
         * @param watcher 目标对象（非拥有）
         */
        void unregisterWatcher(const IoWatcher *watcher);

        /**
         * @brief 该事件注册对象是否仍然存活
         * @details 事件是**批量**从内核取回来的：先处理的那条事件有可能销毁后一条事件所属的对象
         *          （会话收口时顺手关掉另一条连接就是这条路径），因此派发前必须确认接收对象还在。
         *          少了这一步，后一条事件就是往已释放对象里写成员并 resume 垃圾句柄。
         * @param watcher 目标对象
         * @return true 仍在登记表里，可以安全派发
         * @note 登记与注销都可能发生在别的线程上（收尾时从外部线程关闭连接会析构它的注册对象），
         *       因此内部加锁；派发路径上的判定也在同一把锁内，代价是一次未争用的互斥锁
         */
        [[nodiscard]] bool isWatcherAlive(const IoWatcher *watcher) const;

        /**
         * @brief 检查事件循环是否正在运行
         * @return true 表示正处于 run() 循环中，false 表示已停止或尚未启动
         */
        [[nodiscard]] bool isRunning() const noexcept;

    private:
        /// 存活登记表：IoWatcher 构造/析构时登记与注销，事件派发前据此确认接收对象还活着
        mutable std::mutex                  m_liveWatcherMutex;
        std::unordered_set<const IoWatcher *> m_liveWatchers;

        Epoll                   m_epoll;          ///< epoll 事件管理器
        Scheduler               m_scheduler;      ///< 协程调度器，管理待运行的任务队列
        Platform::EventNotifier m_wakeup;         ///< 跨线程唤醒器
        int                     m_wakeupSentinel; ///< 唤醒哨兵值，用于识别唤醒事件（可选的内部标记）
        std::atomic<bool>       m_running;        ///< 循环是否正在运行中（原子标记）
        std::atomic<bool>       m_stopRequested;  ///< 是否已请求停止（原子标记，线程安全）
        /// 定时器队列。声明在最后 = 最先销毁：驱动协程与循环唯一的 timerfd 先于其余部件退出，
        /// 收尾时不会再向调度器投递等待者
        TimerQueue m_timerQueue;
    };
}
