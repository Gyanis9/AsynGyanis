/**
 * @file IoContext.h
 * @brief 异步运行时主入口，持有 ThreadPool 并管理全局生命周期
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once


#include "Core/Coroutine/ThreadPool.h"

#include <condition_variable>
#include <mutex>

namespace AsynGyanis::Core
{
    /**
     * @brief 异步运行时主入口，组合 ThreadPool 并管理全局生命周期
     */
    class IoContext
    {
    public:
        /**
         * @brief 构造异步运行时。
         * @param threadCount 工作线程数，0 表示自动检测（取本进程实际可用的核数：容器 CPU 配额与
         *        cpuset 会把它收窄，而不是宿主核数）
         */
        explicit IoContext(size_t threadCount = 0);

        /**
         * @brief 析构函数，自动停止运行时并等待所有线程退出。
         */
        ~IoContext();

        // 禁止拷贝
        IoContext(const IoContext &) = delete;

        IoContext &operator=(const IoContext &) = delete;

        /**
         * @brief 启动线程池并阻塞当前线程，直到 stop() 被调用
         */
        void run();

        /**
         * @brief 通知所有工作线程停止，run() 随之返回
         */
        void stop();

        // ========================================================================
        // 访问器
        // ========================================================================

        /**
         * @brief 获取线程池对象的引用。
         * @return ThreadPool&
         */
        [[nodiscard]] ThreadPool &threadPool() noexcept;

        /**
         * @brief 获取主调度器（通常用于提交需要主线程执行的任务）。
         * @return Scheduler&
         */
        [[nodiscard]] Scheduler &mainScheduler() const;

    private:
        ThreadPool              m_threadPool;     ///< 底层线程池，管理所有工作线程
        std::mutex              m_mutex;          ///< 保护 m_stopped 标志的条件锁
        std::condition_variable m_condition;      ///< 用于等待 stop() 通知的条件变量
        bool                    m_stopped{false}; ///< 是否已请求停止
    };

}
