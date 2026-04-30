/**
 * @file IoContext.h
 * @brief 异步运行时主入口，持有 ThreadPool 并管理全局生命周期
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_IOCONTEXT_H
#define CORE_IOCONTEXT_H

#include "ThreadPool.h"

#include <condition_variable>
#include <mutex>

namespace Core
{
    /**
     * @brief 异步运行时主入口。
     *
     * 组合 ThreadPool，提供 run()/stop() 接口。
     * 析构时自动停止所有 EventLoop。
     *
     * 使用示例：
     * @code
     *   Core::IoContext ctx(4);  // 4 个工作线程
     *   ctx.run();               // 阻塞直到 stop() 被调用
     * @endcode
     */
    class IoContext
    {
    public:
        /**
         * @brief 构造异步运行时。
         * @param threadCount 工作线程数，0 表示自动检测（通常为 CPU 核心数）
         */
        explicit IoContext(size_t threadCount = 0);

        /**
         * @brief 析构函数，自动停止运行时并等待所有线程退出。
         */
        ~IoContext();

        // 禁止拷贝
        IoContext(const IoContext &)            = delete;
        IoContext &operator=(const IoContext &) = delete;

        /**
         * @brief 启动线程池并阻塞当前线程。
         *
         * 调用后，线程池开始执行事件循环，当前线程阻塞等待直到 stop() 被调用。
         * 内部会调用 ThreadPool::start()，并等待停止信号。
         */
        void run();

        /**
         * @brief 通知所有工作线程停止。
         *
         * 唤醒所有 EventLoop 线程，并向调度器发送停止请求。
         * 调用后 run() 方法将返回。
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
        std::condition_variable m_cv;             ///< 用于等待 stop() 通知的条件变量
        bool                    m_stopped{false}; ///< 是否已请求停止
    };

}

#endif
