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
         * @param threadCount 工作线程数，0 表示自动检测
         */
        explicit IoContext(size_t threadCount = 0);

        ~IoContext();

        IoContext(const IoContext &)            = delete;
        IoContext &operator=(const IoContext &) = delete;

        /**
         * @brief 启动线程池并阻塞当前线程。直到 stop() 被调用后返回。
         */
        void run();

        /**
         * @brief 通知所有工作线程停止。
         */
        void stop();

        // ========================================================================
        // 访问器
        // ========================================================================

        [[nodiscard]] ThreadPool &threadPool() noexcept;
        [[nodiscard]] Scheduler & mainScheduler();

    private:
        ThreadPool              m_threadPool;
        std::mutex              m_mutex;
        std::condition_variable m_cv;
        bool                    m_stopped{false};
    };

}

#endif
