/**
 * @file OtlpHttpMetricsExporter.h
 * @brief 把进程读数按周期推给 OTLP/HTTP 采集端的出口
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Metrics/OtlpMetricsJson.h"
#include "Net/Tracing/SpanExporter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    struct TlsPolicy;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 按导出周期现取一份进程读数快照，经 `POST <endpoint>` 推给采集端（OTLP/HTTP，Content-Type: application/json）
     *
     * @details 与链路出口（OtlpHttpSpanExporter）刻意不同的两条语义，都是数据形态决定的：
     *          一是**快照语义**——每个周期现取一份「此刻的全量读数」发出去，没有待发队列、失败不重发：
     *          下个周期的快照自带最新值，一个周期丢了没有补的价值，也就没有「批必须送达」那种排空账；
     *          二是**周期驱动**——导出由本类自带的事件循环线程上的定时器按 `exportInterval` 发起，
     *          不依赖任何调用方投递。采集端慢或失联时，生产线不受影响（快照在导出线程上现取，
     *          注册表的读锁只在那一次取样本的瞬间被拿）。
     * @note 读数取不到（provider 交回空批）的周期直接跳过并计 emptySnapshotCount：三路条目全空时
     *       发一份结构完整但没有指标的正文没有意义，那不是「通路坏了」的信号。
     * @see formatOtlpMetricsJson(), Core::ProcessMetricsRegistry::samples()
     */
    class ASYN_NET_API OtlpHttpMetricsExporter final
    {
    public:
        /**
         * @brief 出口参数
         */
        struct Configuration
        {
            std::string                        endpoint{};            ///< 必填，形如 http(s)://collector:4318/v1/metrics
            std::chrono::milliseconds          requestTimeout{3000};  ///< 单次请求的整体时限（连接、握手、发送、收完响应）
            std::chrono::milliseconds          exportInterval{15000}; ///< 导出周期：每隔这么久取一份快照推一次，必须是正数
            std::vector<HttpClientHeaderField> extraHeaders{};        ///< 附加头部（鉴权令牌这类），按给出的顺序上线
        };

        /**
         * @brief 取一期读数批的动作
         * @note 调用发生在出口自己的循环线程上；实现必须快——它跑在导出路径上，阻塞会推迟下一次周期
         */
        using SnapshotProvider = std::function<OtlpMetricsBatch()>;

        /**
         * @brief 建出口并起它的循环线程（TLS 用默认档：按系统信任库校验对端）
         * @param resource 快照的出处（服务名与版本），随每个周期上报
         * @param configuration 出口参数
         * @param snapshotProvider 取快照的动作；空函数对象在构造期就拒
         * @throws Base::InvalidArgumentException endpoint 为空或写法畸形、附加头部的名字为空或含 CR/LF、
         *         占用了 Host/Content-Length/Connection/Content-Type、周期或时限不是正数，或 snapshotProvider 为空
         * @throws Core::CoreException 默认 TLS 上下文建不出来
         */
        OtlpHttpMetricsExporter(TraceResource resource, Configuration configuration, SnapshotProvider snapshotProvider);

        /**
         * @brief 同上，外加一份出站 TLS 策略（https 采集端的校验深度、CA 与套件都归它管）
         * @param resource 快照的出处（服务名与版本）
         * @param configuration 出口参数
         * @param snapshotProvider 取快照的动作
         * @param tlsPolicy 出站 TLS 策略；默认构造即「按系统信任库校验对端」
         * @throws Base::InvalidArgumentException 同上面那支
         * @throws Core::CoreException 策略里某一项被当前 OpenSSL 拒绝
         */
        OtlpHttpMetricsExporter(TraceResource resource, Configuration configuration, SnapshotProvider snapshotProvider, const Core::TlsPolicy &tlsPolicy);

        /**
         * @brief 析构：停导出、收掉循环线程
         * @details 收尾最多等「一次在途导出 + 一个停止察觉切片」这么久：在途请求还在飞时等它落地，
         *          到点仍未落地的那一次按放弃处置（没发出去，也没人补——快照语义没有补发的义务）。
         */
        ~OtlpHttpMetricsExporter();

        OtlpHttpMetricsExporter(const OtlpHttpMetricsExporter &)            = delete;
        OtlpHttpMetricsExporter &operator=(const OtlpHttpMetricsExporter &) = delete;
        OtlpHttpMetricsExporter(OtlpHttpMetricsExporter &&)                 = delete;
        OtlpHttpMetricsExporter &operator=(OtlpHttpMetricsExporter &&)      = delete;

        /**
         * @brief 停止导出并收掉循环（有期限）；重复调用安全
         * @details 析构走同一条路径。返回之后不会再有新的导出发起。
         */
        void shutdown() noexcept;

        /// @brief 已发出去且拿到 2xx 的导出次数
        [[nodiscard]] std::uint64_t deliveredExportCount() const noexcept;

        /// @brief 发出去但没拿到 2xx 的导出次数（连不上、超时、被拒、正文成形失败都算）
        [[nodiscard]] std::uint64_t failedExportCount() const noexcept;

        /// @brief 快照为空被跳过、没有发起请求的周期数
        [[nodiscard]] std::uint64_t emptySnapshotCount() const noexcept;

        /**
         * @brief 最近一次失败的中文原因
         * @return std::string 空串表示还没失败过；只留最近一条，历史上的一律进计数
         */
        [[nodiscard]] std::string lastFailureReason() const;

    private:
        /// @brief 循环线程主体：在**本线程上**首次驱动导出协程，然后交给 run()
        void runLoopThread();

        /// @brief 导出协程：按周期醒来取快照、渲染、POST 一次；醒来时复查停止标记
        Core::Task<void> exportOncePerInterval();

        /// @brief 还接受导出吗（shutdown 之后不再发起新的周期）
        [[nodiscard]] bool isStillExporting() const noexcept;

        Configuration               m_configuration;           ///< 生效的出口参数
        TraceResource               m_resource;                ///< 快照的出处
        SnapshotProvider            m_snapshotProvider;        ///< 取一期读数批的动作
        ParsedUrl                   m_target;                  ///< 构造期就拆好的采集端地址
        Core::EventLoop             m_loop;                    ///< 导出用的事件循环（活在本类的循环线程上）
        std::unique_ptr<HttpClient> m_client;                  ///< 复用的出站客户端（一条采集端连接足矣）
        std::atomic<bool>           m_isExporting{true};       ///< 是否还在做周期导出（shutdown 置掉）
        std::mutex                  m_pumpExitMutex;           ///< 护住「导出协程已退出」的等待
        std::condition_variable     m_pumpExitCondition;       ///< 等导出协程退出
        bool                        m_hasPumpExited{false};    ///< 导出协程已退出（在 m_pumpExitMutex 下读写）
        std::atomic<std::uint64_t>  m_deliveredExportCount{0}; ///< 2xx 的导出次数
        std::atomic<std::uint64_t>  m_failedExportCount{0};    ///< 没拿到 2xx 的导出次数
        std::atomic<std::uint64_t>  m_emptySnapshotCount{0};   ///< 空快照跳过的周期数
        mutable std::mutex          m_reasonMutex;             ///< 护住失败原因文本
        std::string                 m_lastFailureReason{};     ///< 最近一次失败的中文原因
        std::jthread                m_workerThread{};          ///< 循环线程：最后声明，构造函数的体里才起
    };
} // namespace AsynGyanis::Net
