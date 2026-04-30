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
     *
     * 线程池创建后需要调用 start() 启动所有线程，stop() 停止并等待退出。
     */
    class ThreadPool
    {
    public:
        /**
         * @brief 构造线程池。
         * @param threadCount 线程数，0 表示使用 hardware_concurrency() 自动确定
         */
        explicit ThreadPool(size_t threadCount = 0);

        /**
         * @brief 析构函数，自动调用 stop() 确保线程安全退出。
         */
        ~ThreadPool();

        ThreadPool(const ThreadPool &)            = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        /**
         * @brief 启动所有工作线程。
         *
         * 为每个线程创建独立的 EventLoop 和 Scheduler，并开始运行事件循环。
         */
        void start();

        /**
         * @brief 停止所有工作线程并等待 join。
         *
         * 通知所有 EventLoop 退出，并等待每个 jthread 结束。
         */
        void stop();

        // ========================================================================
        // 访问器
        // ========================================================================

        /**
         * @brief 获取线程池中的线程数量。
         * @return 线程个数（启动后的实际工作线程数）
         */
        [[nodiscard]] size_t threadCount() const noexcept;

        /**
         * @brief 获取指定索引的工作线程所关联的 EventLoop。
         * @param index 线程索引（0 <= index < threadCount()）
         * @return EventLoop 引用
         */
        [[nodiscard]] EventLoop &eventLoop(size_t index) const;

        /**
         * @brief 获取指定索引的工作线程所关联的 Scheduler。
         * @param index 线程索引（0 <= index < threadCount()）
         * @return Scheduler 引用
         */
        [[nodiscard]] Scheduler &scheduler(size_t index) const;

    private:
        size_t                                  m_threadCount; ///< 实际线程数量（启动后不变）
        std::vector<std::unique_ptr<EventLoop>> m_eventLoops;  ///< 每个线程独立的 EventLoop
        std::vector<std::jthread>               m_threads;     ///< 工作线程，使用 jthread 自动管理生命周期
    };

}

#endif
