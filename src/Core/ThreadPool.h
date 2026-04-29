/**
 * @file ThreadPool.h
 * @brief 固定大小线程池，每个线程绑定一个 EventLoop
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_THREADPOOL_H
#define CORE_THREADPOOL_H

#include "EventLoop.h"
#include "Scheduler.h"

#include <memory>
#include <thread>
#include <vector>

namespace Core
{
    /**
     * @brief 固定大小线程池。
     *
     * 每个工作线程绑定一个 EventLoop 和 Scheduler。
     * 使用 std::jthread 实现自动 join 和协作取消。
     */
    class ThreadPool
    {
    public:
        /**
         * @brief 构造线程池。
         * @param threadCount 线程数，0 表示使用 hardware_concurrency()
         */
        explicit ThreadPool(size_t threadCount = 0);

        ~ThreadPool();

        ThreadPool(const ThreadPool &)            = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        /**
         * @brief 启动所有工作线程。
         */
        void start();

        /**
         * @brief 停止所有工作线程并等待 join。
         */
        void stop();

        // ========================================================================
        // 访问器
        // ========================================================================

        [[nodiscard]] size_t     threadCount() const noexcept;
        [[nodiscard]] EventLoop &eventLoop(size_t index) const;
        [[nodiscard]] Scheduler &scheduler(size_t index) const;

    private:
        size_t                                  m_threadCount;
        std::vector<std::unique_ptr<EventLoop>> m_eventLoops;
        std::vector<std::jthread>               m_threads;
    };

}

#endif
