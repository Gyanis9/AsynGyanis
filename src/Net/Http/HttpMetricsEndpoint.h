/**
 * @file HttpMetricsEndpoint.h
 * @brief 把统计快照渲染成 Prometheus 文本，以及健康检查端点的应答正文
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/HttpServerStats.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// /debug/loops 的 content-type：正文是 JSON
    inline constexpr std::string_view kLoopDiagnosticsContentType = "application/json";
    /// /metrics 的 content-type：Prometheus 文本展示格式 0.0.4；显式声明 UTF-8，因为 HELP 文本是中文
    inline constexpr std::string_view kPrometheusTextContentType = "text/plain; version=0.0.4; charset=utf-8";

    /// /healthz 的应答正文：固定的最小 JSON，探针与人都能一眼看懂
    inline constexpr std::string_view kHealthCheckResponseBody = R"({"status":"ok"})";

    /**
     * @brief 把一次统计快照渲染成 Prometheus 文本格式（exposition format 0.0.4）
     *
     * @details 指标名一律 ASCII，HELP 文本是中文（该格式是 UTF-8，只对反斜杠与换行有转义要求）。
     *          计数器带 `_total` 后缀、直方图带 `_bucket`/`_sum`/`_count`，都按官方命名约定，
     *          否则采集侧推导不出 counter 与 histogram 两类语义。
     * @param stats 统计快照
     * @param metricNamePrefix 指标名前缀，用于同一进程内区分多套服务；空串表示不带前缀
     * @return std::string 可直接作为 200 响应正文的文本，以换行结尾
     * @note 行序固定（计数器、状态码分类、直方图、WebSocket、HTTP/2、发送路径、准入闸门、进程级读数），便于人眼比对两次抓取
     * @warning 快照的口径由**传入的采集端**决定：HTTP/1.1 与 HTTP/2 会话直接计入本服务器的
     *          采集端；HTTP/3 由独立的 QuicServer 服务，只有把同一个采集端交给它
     *          （QuicServer::Configuration::metricsCollector）才会一并出现在这里——只跑 h3 又没接
     *          采集端时会看到全零，那不是「没有流量」，是那条路径没接上。三条通道的口径现已一致：
     *          请求数、状态码类与耗时直方图都算，h3 的耗时同样从「会话收下这条请求」量到「响应排进
     *          待发字节」（见 Http3Session 构造函数的说明）；跨协议对照时唯一要记住的差别是 h3 不产生
     *          「写侧中止」那一族——QUIC 的发送由传输层自行重传，没有那个形态
     * @warning 同一端口由多台服务器共同监听（每循环线程一个）时，还要让它们共用一份采集端
     *          （HttpServer::setMetricsCollector()）：各持一份时抓取会随机命中其中一台，报出的是
     *          那台自己的量，计数器还能在两次抓取之间变小。跨进程没有这条通道——多个 worker 进程
     *          共用一个端口时，本端点报的只是被抓住的那个进程的口径，按进程各自暴露抓取端点再由
     *          采集侧汇总
     * @see HttpServer::enableMetricsEndpoint(), HttpServerStats
     */
    [[nodiscard]] std::string formatPrometheusMetrics(const HttpServerStats &stats, std::string_view metricNamePrefix);

    /**
     * @brief 把进程内事件循环的自观测表渲染成 JSON，作为 /debug/loops 的应答正文
     *
     * @details 判停顿只看「工作相已经持续多久」：等事件的那一相再久也不是停顿（那只是空闲），
     *          已经停下的循环也不参与判定（它不再更新相位，读出来是一个停在原地的旧时刻）。
     * @param observedLoops Core::eventLoopSnapshots() 的返回值，按槽位号升序
     * @param unregisteredLoopCount 因观测槽位已满而没进表的循环条数；非零时表是不全的
     * @param nowMoment 算「这一相已持续多久」的基准时刻，由调用方给，渲染因此可复现可断言
     * @return std::string 紧凑 JSON：顶层给 stallThresholdMicroseconds 与 unregisteredLoopCount，
     *         loops 数组一条循环一行（serial/thread/running/phase/phaseMicroseconds/
     *         completedWorkingSegments/slowestWorkingSegmentMicroseconds/remotePendingCount/stalled）
     * @note stalled 与 Core 那条「工作段超阈值就落 ERROR」用的是同一个阈值（kSlowWorkingSegmentAlertThreshold），
     *       两边口径不同就会一个报警一个不报，那比没有更糟
     * @see Core::eventLoopSnapshots(), HttpServer::enableLoopDiagnosticsEndpoint()
     */
    [[nodiscard]] std::string formatLoopDiagnosticsJson(const std::vector<Core::ObservedEventLoop> &observedLoops, std::size_t unregisteredLoopCount,
                                                        std::chrono::steady_clock::time_point nowMoment);

} // namespace AsynGyanis::Net
