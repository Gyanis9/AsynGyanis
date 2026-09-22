/**
 * @file ThreadPool.h
 * @brief 固定大小线程池，每个线程绑定一个 EventLoop
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Core/Coroutine/Scheduler.h"

#include <memory>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 固定大小线程池，每个工作线程绑定一个 EventLoop 与 Scheduler
     */
    class ThreadPool
    {
    public:
        /**
         * @brief 构造线程池。
         * @param threadCount 线程数，0 表示自动取「本进程实际可用的核数」（容器配额、cpuset 会把它收窄）
         */
        explicit ThreadPool(size_t threadCount = 0);

        /**
         * @brief 析构函数，自动调用 stop() 确保线程安全退出。
         */
        ~ThreadPool();

        ThreadPool(const ThreadPool &) = delete;

        ThreadPool &operator=(const ThreadPool &) = delete;

        /**
         * @brief 启动所有工作线程。
         * @details 允许在 stop() 之后再调用：此时循环会整套换新，因为 EventLoop 的停止请求是
         *          粘性的，复用旧循环只会让线程立刻退出。start() 之后、尚未 stop() 时重复调用是空操作。
         * @warning 换过循环之后先前取到的 `eventLoop(i)` / `scheduler(i)` 引用指向已销毁的对象，
         *          重启前必须重新取。
         */
        void start();

        /**
         * @brief 打开/关闭「启动时把工作线程逐个绑到逻辑核」
         * @details 绑核减少调度迁移，让每枚核上的 L1/L2 与 TLB 不被别的工作线程踩掉，
         *          尾延迟因此更稳；但绑错的核（容器只放行一部分 CPU、或线程数多于核数）
         *          反而会把负载挤在同一枚核上，所以默认关闭，由部署方按机器实际情况打开。
         * @note 线程数多于可用核数时，只有下标在核数以内的那部分被绑，其余线程保持可迁移；
         *       单个线程绑定失败只记一条 WARN，不会让启动失败。
         * @note start() 之后再调用不会生效（线程已经起来），此时只记一条 WARN 说明该改在哪一步调。
         * @param pinThreadsToCores true 表示 start() 时按线程下标绑核
         */
        void setThreadsPinnedToCores(bool pinThreadsToCores) noexcept;

        /**
         * @brief 停止所有工作线程并等待 join。
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
        size_t                                   m_threadCount; ///< 实际线程数量（启动后不变）
        std::vector<std::unique_ptr<EventLoop> > m_eventLoops;  ///< 每个线程独立的 EventLoop
        std::vector<std::jthread>                m_threads;     ///< 工作线程，使用 jthread 自动管理生命周期
        bool m_pinsThreadsToCores{false};                       ///< 是否在 start() 时把工作线程逐个绑到逻辑核
        bool m_hasBeenStopped{false};                           ///< 是否已 stop() 过一轮（下次 start() 要换新循环）
    };

}
