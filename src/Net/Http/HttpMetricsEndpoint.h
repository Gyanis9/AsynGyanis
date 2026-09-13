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
     * @see HttpServer::enableMetricsEndpoint(), HttpServerStats
     */
    [[nodiscard]] std::string formatPrometheusMetrics(const HttpServerStats &stats, std::string_view metricNamePrefix);

} // namespace AsynGyanis::Net
