/**
 * @file OtlpHttpSpanExporter.h
 * @brief 把链路节按 OTLP/HTTP 发给采集端的出口
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Tracing/OtlpTraceJson.h"
#include "Net/Tracing/SpanExporter.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <thread>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    struct TlsPolicy;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /// OTLP/HTTP 的正文媒体类型：本出口发的是 protobuf 的 JSON mapping，不是 protobuf
    inline constexpr std::string_view kOtlpJsonContentType = "application/json";

    /**
     * @brief 把一批节按 `POST <endpoint>` 发给采集端（OTLP/HTTP，Content-Type: application/json）
     *
     * @details 交付是**异步**的：exportSpans() 只把正文放进本出口自己的有界队列就返回，真正的
     *          请求由本类自带的一条事件循环线程发出去。理由是出口线程不该排在一次网络往返后面——
     *          采集端慢或失联时，同步交付会把 Tracer 的出口线程整条钉住，进而让所有生产者排在
     *          一次慢 IO 后面。代价是「收下」在这里只表示「已投递给下游」，网络层的失败由本类的
     *          计数报出（不看 Tracer 那三个数，它们到这里为止）。
     * @note 采集端地址写错在构造期就拒（parseUrl 抛 InvalidArgumentException），不会退化成
     *       「一直发不出去但没人知道」；TLS 策略被 OpenSSL 拒同样是构造期抛。
     * @note 一批失败之后本出口先退避再试下一批（429/503 听响应里的 `Retry-After`，封顶 5 秒；其余按 200
     *       毫秒），并把没发的批次留在队列里。不留这段间隔就会出现「采集端拒绝 100 毫秒、队列里 64 批
     *       全部撞光」——那 64 批本可以在对端缓过来之后送达，却只能按失败计。
     * @see formatOtlpTracesJson(), Tracer
     */
    class ASYN_NET_API OtlpHttpSpanExporter final : public SpanExporter
    {
    public:
        /**
         * @brief 出口参数
         */
        struct Configuration
        {
            std::string                        endpoint{};                   ///< 必填，形如 http(s)://collector:4318/v1/traces
            std::chrono::milliseconds          requestTimeout{3000};         ///< 单次请求的整体时限（连接、握手、发送、收完响应）
            std::size_t                        maximumPendingBatchCount{64}; ///< 待发送队列的批数上界，越界只丢新到的
            std::chrono::milliseconds          shutdownTimeout{3000};        ///< 收尾期限：停止收新批后最多再等这么久把队列发完
            std::vector<HttpClientHeaderField> extraHeaders{};               ///< 附加头部（鉴权令牌这类），按给出的顺序上线
        };

        /**
         * @brief 建出口并起它的循环线程（TLS 用默认档：按系统信任库校验对端）
         * @param configuration 出口参数
         * @throws Base::InvalidArgumentException endpoint 为空或写法畸形、附加头部的名字为空或含 CR/LF、
         *         或占用了 Host/Content-Length/Connection
         * @throws Core::CoreException 默认 TLS 上下文建不出来
         */
        explicit OtlpHttpSpanExporter(Configuration configuration);

        /**
         * @brief 同上，外加一份出站 TLS 策略（https 采集端的校验深度、CA 与套件都归它管）
         * @param configuration 出口参数
         * @param tlsPolicy 出站 TLS 策略；默认构造即「按系统信任库校验对端」
         * @throws Base::InvalidArgumentException 同上面那支
         * @throws Core::CoreException 策略里某一项被当前 OpenSSL 拒绝
         */
        OtlpHttpSpanExporter(Configuration configuration, const Core::TlsPolicy &tlsPolicy);

        /**
         * @brief 析构：停止收新批、尽力把队列里剩下的发完、收掉循环线程
         * @details 收尾期限就是 shutdownTimeout：到点还没发完的按丢弃计数，不无限期等（采集端失联时
         *          进程不该退出无门）。
         */
        ~OtlpHttpSpanExporter() override;

        OtlpHttpSpanExporter(const OtlpHttpSpanExporter &)            = delete;
        OtlpHttpSpanExporter &operator=(const OtlpHttpSpanExporter &) = delete;
        OtlpHttpSpanExporter(OtlpHttpSpanExporter &&)                 = delete;
        OtlpHttpSpanExporter &operator=(OtlpHttpSpanExporter &&)      = delete;

        /**
         * @brief 把一批节渲染成 OTLP 正文并放进待发送队列
         * @details 重写 SpanExporter::exportSpans()。这里的「收下」只到投递为止：正文渲染不出来
         *          （取值进不了 JSON）或队列已满都算没收下；投递成功之后网络怎么失败都不改本次结果，
         *          那一段的损失看 failedBatchCount()。
         * @param resource 这批节的出处
         * @param spans 待送出的节
         * @return true 正文已成形并投进队列
         * @return false 正文没能成形，或队列已满被丢
         */
        bool exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans) override;

        /**
         * @brief 出口名固定为 otlp-http
         * @details 重写 SpanExporter::exporterName()。
         * @return std::string_view "otlp-http"
         */
        [[nodiscard]] std::string_view exporterName() const noexcept override;

        /**
         * @brief 停止收新批并等队列发完（有期限）
         * @details 重写 SpanExporter::shutdown()：Tracer 收尾时最后一次交付之后调用一次。与析构走
         *          同一条路径，重复调用安全。
         */
        void shutdown() noexcept override;

        /// @brief 已发出去且拿到 2xx 的批数
        [[nodiscard]] std::uint64_t deliveredBatchCount() const noexcept;

        /// @brief 发出去但没拿到 2xx 的批数（连不上、超时、被拒都算）
        [[nodiscard]] std::uint64_t failedBatchCount() const noexcept;

        /// @brief 没能投出去的批数：队列满被丢、正文成形失败、收尾期限到还留在队列里的
        [[nodiscard]] std::uint64_t droppedBatchCount() const noexcept;

        /// @brief 队列里还在等的批数
        [[nodiscard]] std::size_t pendingBatchCount() const noexcept;

        /**
         * @brief 最近一次失败的中文原因
         * @return std::string 空串表示还没失败过；只留最近一条，历史上的一律进计数
         */
        [[nodiscard]] std::string lastFailureReason() const;

    private:
        /// @brief 队列里待发的一个正文（连同它的字节，协程帧持有期间视图要有效）
        struct PendingBatch
        {
            std::string body{}; ///< 已渲染好的 OTLP 正文
        };

        /**
         * @brief 等队列有活的可挂起等待器
         * @details 唤醒走 Scheduler::scheduleRemote（跨线程恢复协程的唯一 sanctioned 通道）。
         *          挂起前会再判一次「有没有活」——生产者可能正好落在「判定没活」与「把句柄存下」
         *          之间，那一拍的通知就没人接。
         */
        class WakeupAwaiter
        {
        public:
            explicit WakeupAwaiter(OtlpHttpSpanExporter &exporter) noexcept : m_exporter(exporter)
            {
            }

            /// @brief 已经有活（或该停了）就不挂起
            [[nodiscard]] bool await_ready() const noexcept;

            /**
             * @brief 存下句柄并决定挂不挂
             * @param handle 待挂起的协程句柄
             * @return true 保持挂起，等下一次唤醒
             * @return false 存句柄的这一拍发现活已经来了，当场继续跑
             */
            bool await_suspend(std::coroutine_handle<> handle) noexcept;

            /// @brief 醒来后不需要取回什么：活是从队列里取的
            void await_resume() const noexcept
            {
            }

        private:
            OtlpHttpSpanExporter &m_exporter; ///< 要挂起/唤醒的那条队列的主人
        };

        /// @brief 循环线程主体：在**本线程上**首次驱动发送协程，然后交给 run()
        void runLoopThread();

        /// @brief 发送协程：醒了就一批批发，失败的那一批之后定时退避并停下，剩下的留在队列里等下一次唤醒
        Core::Task<void> pumpBatches();

        /// @brief 非阻塞取一批；空队列交出 false
        bool takeNextBatch(PendingBatch &destination);

        /// @brief 有活可干或该停下来吗（等待器的谓词）
        [[nodiscard]] bool hasWorkOrStopped() const noexcept;

        /// @brief 尝试把句柄存下；交出 false 表示这一拍已经有活了
        bool parkUnlessWorkArrived(std::coroutine_handle<> handle) noexcept;

        /// @brief 叫醒可能挂着的发送协程
        void wakePump() noexcept;

        Configuration               m_configuration;          ///< 生效的出口参数
        ParsedUrl                   m_target;                 ///< 构造期就拆好的采集端地址
        Core::EventLoop             m_loop;                   ///< 发送用的事件循环（活在本类的循环线程上）
        std::unique_ptr<HttpClient> m_client;                 ///< 复用的出站客户端（一条采集端连接足矣）
        mutable std::mutex          m_stateMutex;             ///< 护住队列、 parked 句柄与收尾标记
        std::deque<PendingBatch>    m_queue{};                ///< 待发送队列
        std::coroutine_handle<>     m_parkedPump{};           ///< 挂起来的发送协程；空句柄表示没挂
        bool                        m_isAccepting{true};      ///< 是否还收新批（shutdown 后不再收）
        std::condition_variable     m_drainedCondition;       ///< 等队列发完
        bool                        m_isDrained{false};       ///< 发送协程已把队列清空并退出
        std::atomic<std::uint64_t>  m_deliveredBatchCount{0}; ///< 2xx 的批数
        std::atomic<std::uint64_t>  m_failedBatchCount{0};    ///< 没拿到 2xx 的批数
        std::atomic<std::uint64_t>  m_droppedBatchCount{0};   ///< 没投出去的批数
        mutable std::mutex          m_reasonMutex;            ///< 护住失败原因文本
        std::string                 m_lastFailureReason{};    ///< 最近一次失败的中文原因
        std::jthread                m_workerThread{};         ///< 循环线程：最后声明，构造函数的体里才起
    };
} // namespace AsynGyanis::Net
