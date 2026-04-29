/**
 * @file Scheduler.h
 * @brief 协程调度器：线程本地多级就绪队列 + 工作窃取
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <coroutine>
#include <deque>
#include <mutex>
#include <vector>

namespace Core
{
    /**
     * @brief 协程调度器。
     *
     * 每个 EventLoop 持有一个 Scheduler 实例。
     * - 本地队列无锁（单消费者 = 所属 EventLoop 线程）
     * - 全局队列有 mutex 保护（跨线程调度）
     * - 支持工作窃取：空闲线程可从其他 Scheduler 窃取任务
     */
    class Scheduler
    {
    public:
        Scheduler() = default;

        /**
         * @brief 将协程加入本地就绪队列（本线程调用）。
         */
        void schedule(std::coroutine_handle<> handle);

        /**
         * @brief 跨线程调度：将协程推入全局队列。
         */
        void scheduleRemote(std::coroutine_handle<> handle);

        /**
         * @brief 执行一个就绪协程。
         * @return true 表示执行了一个协程
         */
        bool runOne();

        /**
         * @brief 执行所有就绪协程（清空本地队列）。
         */
        void runAll();

        /**
         * @brief 尝试从另一个 Scheduler 窃取任务（工作窃取）。
         * @param other 被窃取的 Scheduler
         * @return 窃取到的协程句柄（无任务时返回空句柄）
         */
        std::coroutine_handle<> stealFrom(Scheduler &other);

        /**
         * @brief 查询是否有待处理的协程。
         */
        [[nodiscard]] bool hasWork() const;

        /**
         * @brief 获取本地就绪队列大小（用于监控）。
         */
        [[nodiscard]] size_t localQueueSize() const;

    private:
        std::vector<std::coroutine_handle<>> m_localQueue;
        std::deque<std::coroutine_handle<>>  m_globalQueue;
        std::mutex                           m_globalMutex;
    };

}

#endif
