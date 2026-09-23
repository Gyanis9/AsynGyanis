/**
 * @file HttpMetricsEndpoint.h
 * @brief 把统计快照渲染成 Prometheus 文本，以及健康检查端点的应答正文
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpServerStats.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
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
     * @note 行序固定（计数器、状态码分类、直方图、WebSocket、HTTP/2），便于人眼比对两次抓取
     * @warning 快照的口径由**传入的采集端**决定：HTTP/1.1 与 HTTP/2 会话直接计入本服务器的
     *          采集端；HTTP/3 由独立的 QuicServer 服务，只有把同一个采集端交给它
     *          （QuicServer::Configuration::metricsCollector）才会一并出现在这里——只跑 h3 又没接
     *          采集端时会看到全零，那不是「没有流量」，是那条路径没接上。h3 另有个口径差异：
     *          它不参与耗时直方图（见 Http3Session 构造函数），请求数与状态码类照常计入
     * @warning 同一端口由多台服务器共同监听（每循环线程一个）时，还要让它们共用一份采集端
     *          （HttpServer::setMetricsCollector()）：各持一份时抓取会随机命中其中一台，报出的是
     *          那台自己的量，计数器还能在两次抓取之间变小
     * @see HttpServer::enableMetricsEndpoint(), HttpServerStats
     */
    [[nodiscard]] std::string formatPrometheusMetrics(const HttpServerStats &stats, std::string_view metricNamePrefix);

} // namespace AsynGyanis::Net
