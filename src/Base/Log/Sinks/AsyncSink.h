/**
 * @file AsyncSink.h
 * @brief 异步日志输出目标（队列 + 后台消费线程）
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Sinks/LogSink.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace AsynGyanis::Base
{
    /**
     * @brief 包装任意 Sink 提供异步写入能力
     *
     * @details 调用方仅把 LogEvent 压入队列，真正的落地由后台线程完成，用于降低日志 IO
     *          对业务线程的阻塞。队列满时按 OverflowPolicy 阻塞、丢弃新事件或丢弃最旧事件，
     *          丢弃数量通过 droppedEventCount() 暴露给运维监控。
     * @note 析构或 stop() 会尽力排空残留事件，但不保证跨进程崩溃时的日志完整性。
     */
    class AsyncSink : public LogSink
    {
    public:
        /**
         * @brief 队列溢出策略枚举
         */
        enum class OverflowPolicy
        {
            Block,     ///< 队列满时阻塞等待
            Drop,      ///< 队列满时丢弃新到事件
            DropOldest ///< 队列满时丢弃队首最旧事件，保证保留最新日志
        };

        /**
         * @brief 构造异步 Sink 并启动后台消费线程
         * @param wrappedSink 被包装的真实 Sink
         * @param queueSize 队列容量上限
         * @param policy 队列满时溢出策略
         */
        explicit AsyncSink(std::unique_ptr<LogSink> wrappedSink, size_t queueSize = 1024, OverflowPolicy policy = OverflowPolicy::Block);

        /**
         * @brief 析构异步 Sink 并停止后台线程
         * @details 重写 LogSink 的虚析构：转调 stop()，置停止标志并唤醒条件变量，
         *          join 后台线程后再刷新下游 Sink，避免对象销毁后仍有线程访问成员。
         */
        ~AsyncSink() override;

        /**
         * @brief 将日志事件写入异步队列
         * @details 重写 LogSink::write()：不直接落地，按溢出策略决定阻塞/丢弃并累计丢弃计数；
         *          已停止时不再入队，唤醒后台线程后即刻返回。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 阻塞等待队列清空并刷新下游 Sink
         * @details 重写 LogSink::flush()：先等待队列排空，再转发 flush()，
         *          因此调用返回后此前已入队的日志均已落地。
         */
        void flush() override;

        /**
         * @brief 停止异步处理线程并尽力排空队列
         */
        void stop();

        /**
         * @brief 返回因队列溢出被丢弃的事件累计数
         * @return uint64_t 丢弃事件总数
         */
        [[nodiscard]] uint64_t droppedEventCount() const noexcept;

    private:
        /**
         * @brief 后台工作循环，持续消费并转发队列日志事件
         */
        void workerLoop();

        std::unique_ptr<LogSink> m_wrappedSink;      ///< 被包装的下游 Sink
        std::queue<LogEvent>     m_queue;            ///< 事件队列
        size_t                   m_maximumQueueSize; ///< 队列容量上限
        OverflowPolicy           m_overflowPolicy;   ///< 溢出策略

        std::mutex              m_queueMutex;     ///< 保护队列的互斥锁
        std::condition_variable m_queueCondition; ///< 队列非空/空间可用条件变量
        std::condition_variable m_flushCondition; ///< 队列排空条件变量

        std::atomic<bool>     m_running{true};        ///< 是否仍在运行
        std::thread           m_workerThread;         ///< 后台消费线程
        std::atomic<uint64_t> m_droppedEventCount{0}; ///< 溢出丢弃事件累计计数
    };
} // namespace AsynGyanis::Base
