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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
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
     * @note 队列容量最小为 1：容量 0 会让三种溢出策略全部退化为错误语义（详见 kMinimumQueueSize），
     *       因此构造函数会把 0（或更小的入参，经 size_t 转换后即 0）钳到 1，
     *       调用方无需依赖「传了合法容量」这一前提。
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
         * @brief 队列容量的最小可用值
         *
         * @details 容量 0 在三种策略下都是错误语义而非「不限量」：
         *          Drop 会丢弃全部事件；DropOldest 会对空队列 pop（未定义行为）
         *          并把待落地计数从 0 回绕；Block 的等待谓词 size() < 0 永不成立而永久阻塞。
         *          故本类内部自行钳到该下限，配置边界（LoggerConfigLoader）另有一次钳制与诊断。
         */
        static constexpr std::size_t kMinimumQueueSize = 1;

        /**
         * @brief 构造异步 Sink 并启动后台消费线程
         * @param wrappedSink 被包装的真实 Sink
         * @param queueSize 队列容量上限，小于 1 时按 kMinimumQueueSize 钳制
         * @param policy 队列满时溢出策略
         */
        explicit AsyncSink(std::unique_ptr<LogSink> wrappedSink, size_t queueSize = 1024, OverflowPolicy policy = OverflowPolicy::Block);

        /**
         * @brief 析构异步 Sink 并停止后台线程
         * @details 重写 LogSink 的虚析构：转调 stop()，请求停止并唤醒条件变量，
         *          join 后台线程后再刷新下游 Sink，避免对象销毁后仍有线程访问成员。
         */
        ~AsyncSink() override;

        /**
         * @brief 将日志事件写入异步队列
         * @details 重写 LogSink::write()：不直接落地，按溢出策略决定阻塞/丢弃并累计丢弃计数；
         *          已请求停止时不再入队（同样计入丢弃计数），唤醒后台线程后即刻返回。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 阻塞等待所有已受理事件落地后刷新下游 Sink
         * @details 重写 LogSink::flush()：等待条件为「待落地事件数为 0」而非「队列为空」，
         *          因此 worker 已从队列取出、仍在下游 write 中阻塞的事件同样被纳入等待范围；
         *          等待结束后再转发 flush()，调用返回即代表此前的日志已交给下游刷新。
         * @note 已停止（stop() 之后）时不再无限等待，避免 worker 退出后调用方挂死。
         */
        void flush() override;

        /**
         * @brief 停止异步处理线程并尽力排空队列
         * @details 请求 jthread 的 stop_token、唤醒所有等待者并 join；重复调用无副作用。
         */
        void stop();

        /**
         * @brief 返回因队列溢出或停止后写入被丢弃的事件累计数
         * @return uint64_t 丢弃事件总数
         */
        [[nodiscard]] uint64_t droppedEventCount() const noexcept;

    private:
        /**
         * @brief 后台工作循环，持续消费并转发队列日志事件
         * @param stopToken 本线程的停止令牌，request_stop 后退出循环并排空残留事件
         */
        void workerLoop(std::stop_token stopToken);

        std::unique_ptr<LogSink> m_wrappedSink;      ///< 被包装的下游 Sink
        std::queue<LogEvent>     m_queue;            ///< 事件队列
        size_t                   m_maximumQueueSize; ///< 队列容量上限（已钳到 kMinimumQueueSize 以上）
        OverflowPolicy           m_overflowPolicy;   ///< 溢出策略

        size_t m_pendingCount = 0; ///< 已受理但尚未完成落地的事件数，含 worker 正在写出的在途事件

        std::mutex              m_queueMutex;     ///< 保护 m_queue 与 m_pendingCount 的互斥锁
        std::condition_variable m_queueCondition; ///< 队列非空/空间可用条件变量
        std::condition_variable m_flushCondition; ///< 队列排空条件变量

        std::stop_token       m_stopToken;            ///< 停止令牌快照，供 write()/flush() 无锁判断停止状态
        std::jthread          m_workerThread;         ///< 后台消费线程（析构时自动 join，stop() 已先行 join）
        std::once_flag        m_stopOnce;             ///< 保证停止动作只执行一次，并让并发调用者都等到 join 完成
        std::atomic<uint64_t> m_droppedEventCount{0}; ///< 溢出与停止丢弃事件累计计数
    };
} // namespace AsynGyanis::Base
