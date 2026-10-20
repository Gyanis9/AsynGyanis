/**
 * @file OtlpLogSink.h
 * @brief 把日志按周期经 OTLP/HTTP 推给采集端的 LogSink
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Log/Sinks/LogSink.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Logging/OtlpLogJson.h"
#include "Net/Tracing/SpanExporter.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 把日志批按周期 `POST <endpoint>` 给采集端（OTLP/HTTP，Content-Type: application/json）
     *
     * @details 语义照链路出口的批语义（不是 metrics 的快照语义）：日志丢了就是丢了，不能靠下个
     *          周期补一份「最新值」。因此有界队列、失败重试、溢出/收尾丢弃三类计数都保留。
     *          与链路出口的一处差异：本 Sink 的 `write()` 把事件自带的日志上下文摘进记录
     *          （为空时回退到当前线程的 TLS），随后才入队；上下文因此与记录同生共死。
     * @note 收尾期限是 shutdownTimeout：析构时停止收新记录，尽力把队列发完，到点剩下的按丢弃计。
     * @note 队列满时丢新到的记录（与链路出口同一取舍）：旧记录已经在队列里排到队首，
     *       丢新的至少让先发生的日志先到。
     * @warning 本 Sink 自己已有有界队列与工作线程，不需要再套一层 `AsyncSink`；再包一层
     *          只会多一段排队延迟。事件上下文由事件携带，跨线程不会因此丢失。
     */
    class ASYN_NET_API OtlpLogSink final : public Base::LogSink
    {
    public:
        /// 出口参数
        struct Configuration
        {
            std::string                        endpoint{};                      ///< 必填，形如 http(s)://collector:4318/v1/logs
            std::chrono::milliseconds          requestTimeout{3000};            ///< 单次请求的整体时限
            std::chrono::milliseconds          exportInterval{5000};            ///< 攒批周期：每隔这么久发一批
            std::chrono::milliseconds          shutdownTimeout{3000};           ///< 收尾期限：到点仍未发完的按丢弃计
            std::size_t                        maximumPendingRecordCount{4096}; ///< 待发记录条数上界
            std::size_t                        maximumBatchRecordCount{256};    ///< 单批最多几条
            std::vector<HttpClientHeaderField> extraHeaders{};                  ///< 附加头部（鉴权令牌这类）
        };

        /**
         * @brief 建出口并起它的循环线程
         * @param resource 这批日志的出处（服务名与版本）
         * @param configuration 出口参数
         * @throws Base::InvalidArgumentException endpoint 为空或畸形、附加头部非法、队列/批次上限为 0、
         *         周期或时限不是正数
         */
        OtlpLogSink(TraceResource resource, Configuration configuration);

        /// 停止收新记录、尽力发完队列、收掉循环线程
        ~OtlpLogSink() override;

        OtlpLogSink(const OtlpLogSink &)            = delete;
        OtlpLogSink &operator=(const OtlpLogSink &) = delete;
        OtlpLogSink(OtlpLogSink &&)                 = delete;
        OtlpLogSink &operator=(OtlpLogSink &&)      = delete;

        /**
         * @brief 把一条事件折成记录并入队（事件上下文优先，TLS 回退）
         * @param event 日志事件
         */
        void write(const Base::LogEvent &event) override;

        /**
         * @brief 同上，接管事件本体
         * @param event 日志事件
         */
        void write(Base::LogEvent &&event) override;

        /**
         * @brief 空实现：日志按周期推送，没有「调用方缓冲」可刷
         * @note 真正的收尾在析构：停止收新记录后把队列发完（受 shutdownTimeout 约束）
         */
        void flush() override;

        /// 已发出去且拿到 2xx 的批数
        [[nodiscard]] std::uint64_t deliveredBatchCount() const noexcept;

        /// 发出去但没拿到 2xx（或正文成形失败）的批数
        [[nodiscard]] std::uint64_t failedBatchCount() const noexcept;

        /// 没能投出去的记录条数（队列满、收尾期限到、正文成形失败的整批）
        [[nodiscard]] std::uint64_t droppedRecordCount() const noexcept;

        /// 队列里还在等的记录条数
        [[nodiscard]] std::size_t pendingRecordCount() const;

        /// 最近一次失败的中文原因；空串表示还没失败过
        [[nodiscard]] std::string lastFailureReason() const;

    private:
        /// @brief 循环线程主体：在本线程上首次驱动发送协程，然后交给 run()
        void runLoopThread();

        /// @brief 发送协程：按周期取批、渲染、POST；失败留在队列里等下个周期重试
        Core::Task<void> pump();

        /// @brief 取一批记录；队列空时交出 false
        bool takeBatch(std::vector<OtlpLogRecord> &batch);

        /// @brief 把一批放回队首（保序）；放不下时整批按丢弃计
        void requeue(const std::vector<OtlpLogRecord> &batch);

        /// @brief 发一批；返回是否拿到 2xx
        Core::Task<bool> sendBatch(const std::vector<OtlpLogRecord> &batch);

        Configuration               m_configuration;          ///< 生效参数
        TraceResource               m_resource;               ///< 服务名与版本
        Core::EventLoop             m_loop;                   ///< 发送用事件循环
        std::unique_ptr<HttpClient> m_client;                 ///< 复用的出站客户端
        mutable std::mutex          m_queueMutex;             ///< 护住队列
        std::deque<OtlpLogRecord>   m_queue;                  ///< 待发记录
        std::atomic<bool>           m_isStopping{false};      ///< 停止收新记录（析构置位）
        mutable std::mutex          m_reasonMutex;            ///< 护住最近失败原因
        std::string                 m_lastFailureReason{};    ///< 最近一次失败的中文原因
        std::atomic<std::uint64_t>  m_deliveredBatchCount{0}; ///< 2xx 批数
        std::atomic<std::uint64_t>  m_failedBatchCount{0};    ///< 未拿到 2xx 的批数
        std::atomic<std::uint64_t>  m_droppedRecordCount{0};  ///< 没投出去的记录条数
        std::jthread                m_workerThread;           ///< 循环线程：最后声明，构造体里才起
    };
} // namespace AsynGyanis::Net
