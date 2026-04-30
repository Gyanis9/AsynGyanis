/**
 * @file Scheduler.h
 * @brief 协程调度器：线程本地多级就绪队列 + 工作窃取
 * @copyright Copyright (c) 2026
 */

#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <atomic>
#include <coroutine>
#include <deque>
#include <mutex>
#include <vector>

namespace Core
{
    /**
     * @brief 协程调度器
     *
     * 每个 EventLoop 持有一个 Scheduler 实例，负责管理就绪协程的执行。
     * - 本地队列（m_localQueue）：无锁，单消费者（所属 EventLoop 线程），
     *   用于存放本线程调用 schedule() 投递的任务，采用后进先出的栈式调度。
     * - 全局队列（m_globalQueue）：有互斥锁保护，用于跨线程投递任务
     *   （scheduleRemote 调用），采用先进先出的队列调度。
     * - 工作窃取：空闲线程可以通过 stealFrom() 从其他 Scheduler 窃取任务，
     *   实现负载均衡。
     *
     * @note 本类非线程安全，除 scheduleRemote() 和 stealFrom() 外，
     *       其他成员函数应由所属 EventLoop 线程调用。
     */
    class Scheduler
    {
    public:
        /**
         * @brief 默认构造调度器，内部结构为空
         */
        Scheduler() = default;

        /**
         * @brief 绑定跨线程调度唤醒用的文件描述符
         *
         * 当 scheduleRemote() 将任务推入全局队列时，会向该 fd 写入一个字节，
         * 以唤醒可能阻塞在 epoll_wait 中的目标 EventLoop，使其立即处理新任务。
         * @param fd 可写的文件描述符（通常是 eventfd 或 pipe 写端），
         *           传入 -1 表示禁用唤醒功能
         */
        void setWakeupFd(int fd) noexcept;

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
         * 若 m_wakeupFd 有效，则向该 fd 写入 1 字节，提醒目标线程有新任务。
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
         * @brief 尝试从另一个 Scheduler 窃取任务（工作窃取）
         *
         * 空闲线程可以调用其他 Scheduler 的 stealFrom 来获得一个任务，
         * 以平衡负载。此函数会锁定目标调度器的全局队列，并尝试从队首窃取。
         * 窃取成功后，返回的协程句柄将由调用方执行。
         *
         * @param other 被窃取的目标调度器
         * @return 窃取到的协程句柄，若无任务则返回空句柄
         * @note 此函数可在任意线程调用，但通常仅应由空闲的 EventLoop 线程调用。
         */
        static std::coroutine_handle<> stealFrom(Scheduler &other);

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
        std::vector<std::coroutine_handle<>> m_localQueue;     ///< 本地就绪队列（本线程独享，无锁，使用 vector 模拟栈）
        std::deque<std::coroutine_handle<>>  m_globalQueue;    ///< 全局就绪队列（跨线程安全，受 m_globalMutex 保护）
        std::mutex                           m_globalMutex;    ///< 保护全局队列的互斥锁
        std::atomic<size_t>                  m_globalCount{0}; ///< 全局队列长度（原子变量，用于快速判空）
        int                                  m_wakeupFd{-1};   ///< 唤醒文件描述符，-1 表示未启用唤醒
    };
}

#endif
