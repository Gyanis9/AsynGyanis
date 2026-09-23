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
#include "Base/Log/LogEvent.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 包装任意 Sink 提供异步写入能力
     *
     * @details 调用方只把 LogEvent 压入队列，落地由后台线程完成，以免日志 IO 阻塞业务线程；
     *          队列满时按 OverflowPolicy 阻塞、丢新事件或丢最旧事件，丢弃量经
     *          droppedEventCount() 暴露给运维监控。
     * @note 析构或 stop() 尽力排空残留事件，不保证跨进程崩溃时的日志完整性。
     * @note 队列容量钳制在 [kMinimumQueueSize, kMaximumQueueSize] 之间，构造函数自己完成，
     *       调用方无需保证传入合法容量。
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
         * @details 容量 0 在三种策略下都是错误语义而非「不限量」：Drop 全丢；DropOldest 对空队列
         *          pop（未定义行为）并让计数回绕；Block 的谓词 size() < 0 永不成立而永久阻塞。
         *          本类自行钳到该下限，配置边界另有一次钳制与诊断。
         */
        static constexpr std::size_t kMinimumQueueSize = 1;

        /**
         * @brief 队列可占内存的预算
         *
         * @details 队列是「业务线程与落地线程之间的短暂缓冲」，不是应用的堆外存：下游卡住时，
         *          容量决定的是这条线在被溢出策略接管之前最多占住多少内存。
         */
        static constexpr std::size_t kMaximumQueueMemoryBudget = 64U * 1024U * 1024U;

        /**
         * @brief 队列容量的上界，由内存预算与单条事件的大小算出
         *
         * @details 不写死条数：LogEvent 以后加字段，这里会自动收紧。本类自行钳到该上限，
         *          配置边界另有一次钳制与诊断。
         */
        static constexpr std::size_t kMaximumQueueSize = kMaximumQueueMemoryBudget / sizeof(LogEvent);

        static_assert(kMaximumQueueSize >= kMinimumQueueSize, "单条日志事件不得大于队列的整份内存预算");

        /**
         * @brief 构造异步 Sink 并启动后台消费线程
         * @param wrappedSink 被包装的真实 Sink
         * @param queueSize 队列容量上限，钳制到 [kMinimumQueueSize, kMaximumQueueSize]
         * @param policy 队列满时溢出策略
         */
        explicit AsyncSink(std::unique_ptr<LogSink> wrappedSink, size_t queueSize = 1024, OverflowPolicy policy = OverflowPolicy::Block);

        /**
         * @brief 析构异步 Sink 并停止后台线程
         * @details 重写 LogSink 的虚析构：转调 stop()，请求停止、唤醒条件变量、join 后台线程
         *          再刷新下游 Sink，避免对象销毁后仍有线程访问成员。
         */
        ~AsyncSink() override;

        /**
         * @brief 将日志事件复制进异步队列
         * @details 重写 LogSink::write(const LogEvent &)：先复制一份再走接管那条路。
         *          调用方交出的是左值，复制这一步省不掉。
         * @param event 日志事件
         */
        void write(const LogEvent &event) override;

        /**
         * @brief 将日志事件移进异步队列，不复制
         * @details 重写 LogSink::write(LogEvent &&)：不直接落地，按溢出策略阻塞或丢弃并累计丢弃
         *          计数，已请求停止时不再入队（同样计入丢弃数）。事件本体被搬进队列，因此一条
         *          日志在生产线程上不再为它多取一块堆。
         * @param event 日志事件；返回后处于有效但未指定的状态
         */
        void write(LogEvent &&event) override;

        /**
         * @brief 阻塞等待「本次进入时已受理的那些事件」全部落地，然后刷新下游 Sink
         * @details 重写 LogSink::flush()：等待条件是**已结数追平进场时的受理水位**，而不是「队列空」，
         *          也不是「待落地数为 0」。后者在有持续生产者的进程里永远不会成立（实测另一条线程按
         *          每秒数万条写入时，flush() 等满 1.9 秒仍未返回，而该等的量只有容量 64 × 50µs ≈ 3ms），
         *          调用方因此被后来者饿死。水位之下含 worker 已取出、仍在下游 write 中的在途事件，
         *          也含被 DropOldest 淘汰和被等级过滤丢弃的那些——它们不再需要任何人等待。
         *          转发下游 flush() 在队列锁外进行，别的线程不会被这一次刷新排在后面。
         * @note stop() 之后不再无限等待，避免 worker 退出后调用方挂死。
         */
        void flush() override;

        /**
         * @brief 停止异步处理线程并尽力排空队列
         * @details 请求 stop_token、唤醒所有等待者并 join；重复调用无副作用。
         */
        void stop();

        /**
         * @brief 返回被丢弃的事件累计数
         * @details 三种情形计入：队列溢出按策略丢弃、已请求停止后写入、以及被包装 sink 的
         *          level 过滤挡下（那一层过滤没有别人会问，见 workerLoop 里的说明）
         * @return uint64_t 丢弃事件总数
         */
        [[nodiscard]] uint64_t droppedEventCount() const noexcept;

    private:
        /**
         * @brief 后台工作循环，持续消费并转发队列日志事件
         * @param stopToken 本线程的停止令牌，request_stop 后退出循环并排空残留事件
         */
        void workerLoop(const std::stop_token &stopToken);

        /**
         * @brief 把事件搬进队尾槽位，必要时先回收已消费的前缀
         * @details 队列刻意不用 std::queue：MSVC 的 std::deque 对超过 16 字节的元素按「一块一元素」
         *          分块，实测每投递一条日志就为 136 字节的事件本体多取一块堆。槽位数组按几何增长，
         *          队列排空后保留容量，因此稳态下入队不碰堆。
         * @param event 待入队的事件本体；返回后处于有效但未指定的状态
         */
        void appendSlot(LogEvent &&event);

        /**
         * @brief 搬出队首事件并把它从队列中核销
         * @return LogEvent 队首事件本体
         */
        [[nodiscard]] LogEvent takeFrontSlot();

        /**
         * @brief 核销队首槽位：只推进队首下标，队列恰好排空时下标与槽位一起归零
         * @details 留着的容量给下一轮入队复用，因此不必在每次取出事件时收缩数组
         */
        void discardFrontSlot();

        /**
         * @brief 当前在队的事件数
         * @return std::size_t 已占用的槽位数，不含队首之前待回收的空槽
         */
        [[nodiscard]] std::size_t queuedEventCount() const noexcept;

        /**
         * @brief 核销一条已受理事件的落地义务，并在有人等待时唤醒 flush 等待者
         * @details 事件离开队列的每一条路都要走这里一次：worker 写出后、被下游等级过滤丢掉时、
         *          被 DropOldest 淘汰时。少一次就有 flush() 永远等不到的账，多一次则让 flush()
         *          在账还没清时就返回。必须在持有队列锁时调用。
         */
        void settleAcceptedEvent();

        std::unique_ptr<LogSink> m_wrappedSink;      ///< 被包装的下游 Sink
        std::vector<LogEvent>    m_slots;            ///< 事件槽位数组；m_headIndex 之前的槽位已消费、待回收
        std::size_t              m_headIndex = 0;    ///< 队首事件所在槽位下标

        size_t                   m_maximumQueueSize; ///< 队列容量上限（已钳到 kMinimumQueueSize 以上）
        OverflowPolicy           m_overflowPolicy;   ///< 溢出策略

        size_t m_acceptedCount = 0; ///< 已受理（进了队列）的事件累计数，flush() 据此定自己的等待水位
        size_t m_settledCount  = 0; ///< 已了结的事件累计数：落地、被过滤丢弃、被淘汰都算，追平受理数即无在途
        size_t m_flushWaiterCount = 0; ///< 正在等 flush 的线程数，为 0 时逐条核销不必碰条件变量

        std::mutex              m_queueMutex;     ///< 保护 m_slots/m_headIndex 与受理/已结计数的互斥锁
        /// 「队列非空」条件：只有 worker 在此等待，入队一侧 notify_one。与腾位条件分开是因为
        /// 一条条件变量上挂着两类谓词时，notify_one 可能叫到谓词不成立的那一类，唤醒被当场吞掉
        std::condition_variable m_workCondition;
        /// 「腾出了空位」条件：只有 Block 策略下等位的最初写入者在此等待，出队一侧 notify_one
        std::condition_variable m_spaceCondition;
        std::condition_variable m_flushCondition; ///< 队列排空条件变量

        std::stop_token       m_stopToken;            ///< 停止令牌快照，供 write()/flush() 无锁判断停止状态
        std::jthread          m_workerThread;         ///< 后台消费线程（析构时自动 join，stop() 已先行 join）
        std::once_flag        m_stopOnce;             ///< 保证停止动作只执行一次，并让并发调用者都等到 join 完成
        std::atomic<uint64_t> m_droppedEventCount{0}; ///< 溢出与停止丢弃事件累计计数
    };
} // namespace AsynGyanis::Base
