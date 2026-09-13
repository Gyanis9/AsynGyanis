/**
 * @file Scheduler.h
 * @brief 协程调度器：线程本地多级就绪队列 + 工作窃取
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <atomic>
#include <coroutine>
#include <deque>
#include <mutex>
#include <vector>

namespace AsynGyanis::Platform
{
    class EventNotifier;
}

namespace AsynGyanis::Core
{
    /**
     * @brief 协程调度器
     *
     * 每个 EventLoop 持有一个 Scheduler 实例，负责管理就绪协程的执行。
     * - 本地队列（m_localQueue）：无锁，单消费者（所属 EventLoop 线程），
     *   用于存放本线程调用 schedule() 投递的任务，采用后进先出的栈式调度。
     * - 全局队列（m_globalQueue）：有互斥锁保护，用于跨线程投递任务
     *   （scheduleRemote 调用），采用先进先出的队列调度。
     * - **不做工作窃取**：全局队列里的任务恰恰是「必须回到这个循环上执行」的那些
     *   （跨线程完成回调要 resume 在发起者循环、套接字与 TLS 通道按循环归属），
     *   把它们偷到别的循环执行会破坏亲和性并引入数据竞争。跨循环的负载均衡发生在
     *   接受层（每循环一个监听器 + SO_REUSEPORT），不发生在就绪队列层。
     *
     * @note 本类非线程安全，除 scheduleRemote() 外，其他成员函数应由所属 EventLoop 线程调用。
     */
    class Scheduler
    {
    public:
        /**
         * @brief 默认构造调度器，内部结构为空
         */
        Scheduler() = default;

        /**
         * @brief 绑定跨线程调度唤醒器
         *
         * 当 scheduleRemote() 将任务推入全局队列时，会通过唤醒器通知
         * 目标 EventLoop 立即处理新任务。
         * @param notifier 唤醒器指针，传入 nullptr 表示禁用唤醒功能
         */
        void setWakeupNotifier(Platform::EventNotifier *notifier) noexcept;

        /**
         * @brief 将协程加入本地就绪队列（本线程调用）
         *
         * 该函数是无锁的，但仅允许所属 EventLoop 线程调用。
         * 协程会被追加到 m_localQueue 末尾，随后被 runOne() / runAll() 执行。
         * @param handle 准备调度的协程句柄（必须非空）
         */
        void schedule(std::coroutine_handle<> handle);

        /**
         * @brief 跨线程调度：将协程推入全局队列（线程安全）
         *
         * 任意线程均可调用此函数，将一个协程投递到本调度器的全局队列。
         * 若已设置唤醒器，则通过 notify() 提醒目标线程有新任务。
         * @param handle 准备调度的协程句柄（必须非空）
         */
        void scheduleRemote(std::coroutine_handle<> handle);

        /**
         * @brief 执行一个就绪协程
         *
         * 执行策略：
         * - 优先从本地队列尾部弹出一个协程（栈式顺序）。
         * - 若本地队列为空，则尝试从全局队首获取一个协程（FIFO 顺序）。
         * - 获得的协程将立即 resume()。
         * @return true 表示成功执行了一个协程，false 表示无任务可执行
         */
        bool runOne();

        /**
         * @brief 执行所有就绪协程（清空本地队列）
         *
         * 反复调用 runOne() 直到本地队列和全局队列均无任务。
         * 该函数通常用于事件循环在阻塞前彻底清空任务。
         */
        void runAll();

        /**
         * @brief 查询是否有待处理的协程
         *
         * 检查本地队列非空，或全局队列计数非零。
         * @return true 表示至少有一个就绪协程
         */
        [[nodiscard]] bool hasWork() const;

        /**
         * @brief 获取本地就绪队列大小（用于监控/调试）
         * @return 本地队列中的协程数量
         */
        [[nodiscard]] size_t localQueueSize() const;

    private:
        std::vector<std::coroutine_handle<> > m_localQueue;      ///< 本地就绪队列（本线程独享，无锁，使用 vector 模拟栈）
        std::deque<std::coroutine_handle<> >  m_globalQueue;     ///< 全局就绪队列（跨线程安全，受 m_globalMutex 保护）
        std::mutex                            m_globalMutex;     ///< 保护全局队列的互斥锁
        std::atomic<size_t>                   m_globalCount{0};  ///< 全局队列长度（原子变量，用于快速判空）
        Platform::EventNotifier *             m_wakeup{nullptr}; ///< 唤醒器指针，nullptr 表示未启用唤醒
    };
}
